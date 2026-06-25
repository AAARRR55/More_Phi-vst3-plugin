/*
 * More-Phi — AI/MCPServer.cpp
 * Multi-instance MCP server with bearer auth, morph identity,
 * and automatic error recovery.
 *
 * JSON responses are built with nlohmann::json for correctness and safety.
 * Incoming requests are parsed with nlohmann::json (M-1 FIX); params are
 * converted back to juce::var so they can flow to MCPToolHandler unchanged.
 */
#include "MCPServer.h"
#include "MCPToolHandler.h"
#include "Plugin/PluginProcessor.h"
#include <nlohmann/json.hpp>
#include <exception>

namespace more_phi {

using json = nlohmann::json;

static bool constantTimeEqual(const char* a, const char* b, size_t len)
{
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i)
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}


MCPServer::MCPServer(MorePhiProcessor& processor)
    : juce::Thread("MorePhi-MCP"), processor_(processor)
{
    // Identity will be fully set via setIdentity() before startServer()
}

MCPServer::~MCPServer()
{
    stopServer();
}

void MCPServer::startServer(int port)
{
    // Use identity port if set, otherwise use the argument
    port_ = (identity_.port > 0) ? identity_.port : port;
    errorCount_.store(0);
    healthy_.store(false);

    if (port_ <= 0)
    {
        recordStartupFailure("No valid MCP port assigned");
        return;
    }

    if (!startThread())
        recordStartupFailure("Failed to start MCP server thread");
}

void MCPServer::recordStartupFailure(const juce::String& details)
{
    healthy_.store(false);
    logError("startup", details);
}

MCPServer::ConnectionThread::ConnectionThread(MCPServer& owner, juce::StreamingSocket* socket)
    : Thread("MCP-Connection"), owner_(owner), socket_(socket)
{
    startedSuccessfully_ = startThread();
    // M8: only count the connection when startThread() actually succeeded,
    // preventing a negative connectedClients_ after destructor decrement.
    if (startedSuccessfully_)
        owner_.connectedClients_++;
}

MCPServer::ConnectionThread::~ConnectionThread()
{
    signalExit();
    // Wait indefinitely to ensure the thread is completely dead before subclass
    // variables are destroyed. Since signalExit() closes the socket, the thread
    // will unblock and exit immediately.
    stopThread(-1);
    // M8: mirror the conditional increment — only decrement if the thread was
    // actually started (constructor only increments on success).
    if (startedSuccessfully_)
        owner_.connectedClients_--;
}

void MCPServer::ConnectionThread::signalExit()
{
    signalThreadShouldExit();
    if (socket_) socket_->close();
}

void MCPServer::ConnectionThread::run()
{
    juce::String buffer;
    char chunk[4096];
    bool writeError = false;
    int readErrors = 0;
    constexpr int MAX_READ_ERRORS = 3;
    constexpr int MAX_REQUEST_BYTES = 256 * 1024;
    constexpr int IDLE_TIMEOUT_MS = 30000;
    int64_t lastActivityMs = juce::Time::currentTimeMillis();

    while (!threadShouldExit() && !writeError)
    {
        try
        {
            const int ready = socket_->waitUntilReady(true, 500);
            if (ready < 0)
            {
                readErrors++;
                if (readErrors >= MAX_READ_ERRORS) break;
                continue;
            }
            if (ready == 0)
            {
                readErrors = 0;
                if (juce::Time::currentTimeMillis() - lastActivityMs > IDLE_TIMEOUT_MS)
                {
                    owner_.logError("connection", "Idle timeout exceeded; closing connection");
                    break;
                }
                continue;
            }

            const int bytesRead = socket_->read(chunk, sizeof(chunk) - 1, false);
            if (bytesRead == 0) break;   // Graceful close
            if (bytesRead < 0)
            {
                owner_.logError("connection", "Socket read error; closing connection");
                break;
            }

            readErrors = 0;
            lastActivityMs = juce::Time::currentTimeMillis();
            chunk[bytesRead] = '\0';
            buffer += juce::String::fromUTF8(chunk, bytesRead);

            if (buffer.getNumBytesAsUTF8() > MAX_REQUEST_BYTES)
            {
                owner_.logError("request", "Request exceeded max size; closing connection");
                juce::String response = juce::String(
                    json{{"jsonrpc","2.0"},{"error",{{"code",-32600},{"message","Request too large"}}},{"id",nullptr}}.dump());
                response += "\n";
                socket_->write(response.toRawUTF8(), static_cast<int>(response.getNumBytesAsUTF8()));
                writeError = true;
                break;
            }

            while (buffer.isNotEmpty())
            {
                int nlPos = buffer.indexOf("\n");
                juce::String message;

                if (nlPos >= 0)
                {
                    message = buffer.substring(0, nlPos).trim();
                    buffer = buffer.substring(nlPos + 1);
                }
                else
                {
                    try {
                        auto testParse = json::parse(buffer.toStdString());
                        juce::ignoreUnused(testParse);
                        message = buffer.trim();
                        buffer.clear();
                    } catch (...) {
                        break;
                    }
                }

                if (message.isEmpty()) continue;

                juce::String response;
                try {
                    response = owner_.processRequest(message, authenticated_);
                } catch (const std::exception& e) {
                    owner_.logError("request", juce::String("Exception processing request: ") + e.what());
                    response = juce::String(json{{"jsonrpc","2.0"},{"error",{{"code",-32603},{"message","Internal error"}}},{"id",nullptr}}.dump());
                }

                if (response.isEmpty())
                    continue; // C-15 FIX: notification — no response required

                response += "\n";
                {
                    const char* ptr = response.toRawUTF8();
                    int remaining = static_cast<int>(response.getNumBytesAsUTF8());
                    while (remaining > 0)
                    {
                        const int n = socket_->write(ptr, remaining);
                        if (n < 0) { writeError = true; break; }
                        ptr += n;
                        remaining -= n;
                    }
                }

                lastActivityMs = juce::Time::currentTimeMillis();
                if (threadShouldExit()) break;
            }
        }
        catch (...)
        {
            owner_.logError("connection", "Unhandled exception in connection thread");
            const auto response = juce::String(json{
                {"jsonrpc", "2.0"},
                {"error", {{"code", -32603}, {"message", "Connection thread internal error"}}},
                {"id", nullptr}
            }.dump()) + "\n";
            if (socket_ != nullptr && socket_->isConnected())
            {
                try {
                    socket_->write(response.toRawUTF8(), static_cast<int>(response.getNumBytesAsUTF8()));
                } catch (...) {
                    // M-3 FIX: swallow write errors to prevent nested exceptions
                }
            }
            break;
        }
    }
}

void MCPServer::stopServer()
{
    healthy_.store(false);
    signalThreadShouldExit();
    serverSocket_.close();

    // Signal all connections to exit
    {
        const juce::ScopedLock lock(connectionsLock_);
        for (auto* conn : activeConnections_)
            conn->signalExit();
    }

    // Wait indefinitely to ensure the listener thread has completely exited.
    // Since serverSocket_.close() unblocks waitForNextConnection(), the thread
    // will exit immediately.
    stopThread(-1);

    // Final cleanup of remaining connections
    const juce::ScopedLock lock(connectionsLock_);
    activeConnections_.clear();
}

void MCPServer::run()
{
    int bindAttempts = 0;
    while (!threadShouldExit())
    {
        if (!createServerListener())
        {
            bindAttempts++;
            logError("bind", "Failed to bind port " + juce::String(port_));
            if (bindAttempts >= MAX_BIND_ATTEMPTS)
            {
                healthy_.store(false);
                return;
            }
            wait(RECOVERY_DELAY_MS);
            continue;
        }

        bindAttempts = 0;
        healthy_.store(true);
        int consecutiveErrors = 0;

        while (!threadShouldExit())
        {
            // 1. Periodic cleanup of disconnected threads
            {
                const juce::ScopedLock lock(connectionsLock_);
                for (int i = activeConnections_.size(); --i >= 0;)
                {
                    if (!activeConnections_[i]->isThreadRunning())
                        activeConnections_.remove(i);
                }
            }

            try
            {
                const int readyResult = serverSocket_.waitUntilReady(true, 500);
                if (threadShouldExit()) break;
                if (readyResult < 0) break;
                if (readyResult == 0) continue;

                auto* client = serverSocket_.waitForNextConnection();
                if (client)
                {
                    if (!client->isLocal())
                    {
                        delete client;
                        logError("connection", "Rejected non-local MCP client");
                        continue;
                    }

                    const juce::ScopedLock lock(connectionsLock_);
                    // M-6 FIX: Enforce max connection limit to prevent resource exhaustion.
                    if (activeConnections_.size() >= MAX_CONNECTIONS)
                    {
                        delete client;  // Reject connection
                        logError("connection",
                            "Rejected: max connections reached (" + juce::String(MAX_CONNECTIONS) + ")");
                        continue;
                    }

                    // H-14 FIX: Check connection thread started successfully before adding
                    auto* conn = new ConnectionThread(*this, client);
                    if (!conn->startedSuccessfully())
                    {
                        delete conn;
                        logError("connection", "Failed to start connection thread");
                        continue;
                    }
                    activeConnections_.add(conn);
                    consecutiveErrors = 0;
                }
            }
            catch (...)
            {
                if (++consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) break;
            }
        }
        serverSocket_.close();
        if (!threadShouldExit()) wait(RECOVERY_DELAY_MS);
    }
    healthy_.store(false);
}

bool MCPServer::createServerListener()
{
    // H-16 FIX: Remove TOCTOU probe; retry binding directly with next candidate.
    for (int attempt = 0; attempt < MAX_BIND_ATTEMPTS; ++attempt)
    {
        if (serverSocket_.createListener(port_, "127.0.0.1"))
            return true;

        ++port_;
        if (identity_.port > 0)
            identity_.port = port_;
    }
    return false;
}

juce::String MCPServer::processRequest(const juce::String& jsonRequest, bool& authenticated)
{
    // M-1 FIX: Standardize on nlohmann::json for all incoming request parsing.
    json parsed;
    try {
        parsed = json::parse(jsonRequest.toStdString());
    } catch (const json::parse_error&) {
        return juce::String(
            json{{"jsonrpc","2.0"},{"error",{{"code",-32700},{"message","Parse error"}}},{"id",nullptr}}.dump());
    }

    // H-13 FIX: JSON-RPC batch request support.
    if (parsed.is_array())
    {
        json batchResponses = json::array();
        for (const auto& item : parsed)
        {
            if (!item.is_object())
            {
                batchResponses.push_back({{"jsonrpc","2.0"},{"error",{{"code",-32600},{"message","Invalid Request"}}},{"id",nullptr}});
                continue;
            }
            juce::String itemStr = juce::String(item.dump());
            juce::String resp = processRequest(itemStr, authenticated);
            if (resp.isNotEmpty())
            {
                try {
                    batchResponses.push_back(json::parse(resp.toStdString()));
                } catch (...) {
                    // ignore invalid response
                }
            }
        }
        if (batchResponses.empty())
            return {};
        return juce::String(batchResponses.dump());
    }

    if (!parsed.is_object())
    {
        return juce::String(
            json{{"jsonrpc","2.0"},{"error",{{"code",-32600},{"message","Invalid Request"}}},{"id",nullptr}}.dump());
    }

    juce::String method = parsed.contains("method") && parsed["method"].is_string()
                          ? juce::String(parsed["method"].get<std::string>())
                          : juce::String{};

    // Convert params to juce::var for existing tool handler API
    juce::var params;
    if (parsed.contains("params"))
    {
        try {
            params = juce::JSON::parse(juce::String(parsed["params"].dump()));
        } catch (...) {
            params = juce::var();
        }
    }

    // Convert id to nlohmann::json
    json reqId = nullptr;
    if (parsed.contains("id"))
    {
        if (parsed["id"].is_number_integer())
            reqId = parsed["id"].get<int64_t>();
        else if (parsed["id"].is_string())
            reqId = parsed["id"].get<std::string>();
        // else remains null
    }

    // C-15 FIX: Suppress JSON-RPC notification responses.
    if (reqId.is_null())
    {
        return {};
    }

    // M-2 FIX: Validate jsonrpc version field.
    if (!parsed.contains("jsonrpc") || !parsed["jsonrpc"].is_string() || parsed["jsonrpc"] != "2.0")
    {
        return juce::String(
            json{{"jsonrpc","2.0"},{"error",{{"code",-32600},{"message","Invalid Request"}}},{"id",reqId}}.dump());
    }

    // Helper: build a JSON-RPC error response with type-safe escaping
    auto errResponse = [&](int code, const std::string& msg) -> juce::String
    {
        return juce::String(
            json{{"jsonrpc","2.0"},{"error",{{"code",code},{"message",msg}}},{"id",reqId}}.dump());
    };

    // ── initialize ────────────────────────────────────────────────────────────
    if (method == "initialize")
    {
        if (validateAuth(params))
        {
            authenticated = true;

            json result = {
                {"protocolVersion", "2024-11-05"},
                {"serverInfo", {{"name","More-Phi MCP"},{"version","1.0"}}},
                {"capabilities", {{"tools", {{"listChanged", false}}}}},
                {"instanceId", identity_.instanceId.toStdString()},
                {"morphCode",  identity_.morphCode.toStdString()},
                {"port",       port_}
            };

            // MCP spec §3.2: after initialize response, send notifications/initialized.
            // Return both messages as two newline-separated lines; ConnectionThread
            // appends a final '\n', yielding two properly-framed JSON-RPC messages.
            const std::string initResponse =
                json{{"jsonrpc","2.0"},{"result",result},{"id",reqId}}.dump();
            const std::string initNotification =
                json{{"jsonrpc","2.0"},{"method","notifications/initialized"},{"params",json::object()}}.dump();

            return juce::String(initResponse + "\n" + initNotification);
        }

        // L-6 FIX: Use custom error code for auth failures.
        return errResponse(-32001, "Unauthorized: invalid bearer_token");
    }

    // ── All other methods require authentication ───────────────────────────────
    if (!authenticated)
        // L-6 FIX: Use custom error code for auth failures.
        return errResponse(-32001, "Unauthorized: call initialize with bearer_token first");

    // Heartbeat is a liveness probe (spec §4.4): it requires authentication but
    // does NOT consume a rate-limit slot, so an idle client can keep the
    // connection alive without starving real tool traffic. It returns the cheap
    // health fields already tracked by the server.
    if (method == "heartbeat")
    {
        json result = {
            {"server_time_ms",   juce::Time::currentTimeMillis()},
            {"uptime_ms",        uptimeMs()},
            {"queue_depth_approx", static_cast<int64_t>(
                processor_.getPendingParameterCommandCountApprox())},
            {"connected_clients",  connectedClients_.load()},
            {"healthy",            healthy_.load() ? true : false}
        };
        return juce::String(json{{"jsonrpc","2.0"},{"result",result},{"id",reqId}}.dump());
    }

    // Consume a rate-limit slot for each authenticated MCP tool request.
    if (!processor_.getTokenOptimizer().tryConsumeRequestSlot())
        return errResponse(-32000, "Rate limit exceeded");

    // ── Dispatch to tool handler ──────────────────────────────────────────────
    juce::String toolResult;
    const bool isToolsCall = method == "tools/call";
    try
    {
        if (method == "tools/list")
        {
            toolResult = MCPToolHandler::getToolList();
        }
        else if (isToolsCall)
        {
            const auto toolName = params.getProperty("name", "").toString();
            if (toolName.isEmpty())
                return errResponse(-32602, "tools/call missing tool name");

            const auto arguments = params.getProperty("arguments", juce::var(new juce::DynamicObject()));
            toolResult = dispatchTool(toolName, arguments);
        }
        else
        {
            toolResult = dispatchTool(method, params);
        }
    }
    catch (const std::exception& e)
    {
        logError("dispatchTool", juce::String("Exception in ") + method + ": " + e.what());
        #ifdef NDEBUG
        return errResponse(-32603, "Internal error");
        #else
        return errResponse(-32603, std::string("Internal error: ") + e.what());
        #endif
    }
    catch (...)
    {
        logError("dispatchTool", "Unknown exception in " + method);
        return errResponse(-32603, "Internal error");
    }

    // Embed the tool result (valid JSON from MCPToolHandler) into the JSON-RPC envelope.
    // Parsing it back avoids double-escaping and ensures correct structural embedding.
    // M-3 FIX: If parse fails, log the error and return a proper error response instead
    // of silently embedding a raw string that the client can't interpret.
    json resultEmbedded;
    try
    {
        resultEmbedded = json::parse(toolResult.toStdString());
    }
    catch (const json::parse_error& e)
    {
        logError("JSON embed",
            juce::String("Tool handler returned invalid JSON for ") + method
            + ": " + juce::String(e.what()).substring(0, 200));
        // Return a proper error response rather than silently embedding bad data.
        // This helps clients detect and report handler bugs.
        return errResponse(-32603,
            std::string("Tool handler returned invalid JSON: ") + method.toStdString());
    }
    catch (...)
    {
        logError("JSON embed", juce::String("Unexpected error parsing tool result for ") + method);
        return errResponse(-32603, std::string("Internal error processing tool result: ") + method.toStdString());
    }

    if (isToolsCall)
    {
        const bool isError = resultEmbedded.contains("error")
            || (resultEmbedded.contains("success") && resultEmbedded["success"].is_boolean()
                && !resultEmbedded["success"].get<bool>());

        json toolEnvelope{
            {"content", json::array({
                {
                    {"type", "text"},
                    {"text", resultEmbedded.dump()}
                }
            })},
            {"structuredContent", resultEmbedded},
            {"isError", isError}
        };

        return juce::String(
            json{{"jsonrpc","2.0"},{"result",toolEnvelope},{"id",reqId}}.dump());
    }

    return juce::String(
        json{{"jsonrpc","2.0"},{"result",resultEmbedded},{"id",reqId}}.dump());
}

bool MCPServer::validateAuth(const juce::var& params)
{
    try
    {
        auto token = params.getProperty("bearer_token", "").toString();
        if (token.isEmpty()) return false;

        const juce::String candidateToken = token;
        const juce::String expectedToken = identity_.bearerToken;

        // C-14 FIX: Fixed-length constant-time comparison to prevent timing attacks.
        //
        // Timing note (audit N3, 2026-06-19): the length pre-check below returns
        // early on a length mismatch, which leaks the EXPECTED token length via
        // the time taken to reject. This is acceptable here because the bearer
        // token is a fixed-size format — InstanceIdentity::generate() always
        // produces a 16-byte (32-hex-char) token — so the length is public
        // knowledge, not a secret. The constantTimeEqual() loop then compares
        // only the byte contents without early exit. If the token format ever
        // becomes variable-length, replace the pre-check with a
        // length-padded constant-time compare (e.g. HMAC tag comparison).
        if (candidateToken.length() != expectedToken.length())
            return false;

        return constantTimeEqual(candidateToken.toRawUTF8(), expectedToken.toRawUTF8(), static_cast<size_t>(expectedToken.length()));
    }
    catch (...)
    {
        logError("validateAuth", "Exception during auth validation");
        return false;
    }
}

juce::String MCPServer::dispatchTool(const juce::String& method, const juce::var& params)
{
    return MCPToolHandler::handle(method, params, processor_, identity_, automationRuntime_);
}

void MCPServer::logError(const juce::String& context, const juce::String& details)
{
    errorCount_.fetch_add(1);
    juce::ignoreUnused(context, details);
    DBG("MCP [" + identity_.morphCode + "] ERROR in " + context +
        ": " + details + " (total errors: " + juce::String(errorCount_.load()) + ")");
}

} // namespace more_phi
