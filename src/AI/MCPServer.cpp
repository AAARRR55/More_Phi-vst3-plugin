/*
 * More-Phi — AI/MCPServer.cpp
 * Multi-instance MCP server with bearer auth, morph identity,
 * and automatic error recovery.
 *
 * JSON responses are built with nlohmann::json for correctness and safety.
 * The incoming request is still parsed via juce::JSON::parse so that params
 * can flow to MCPToolHandler as juce::var without further changes to that API.
 */
#include "MCPServer.h"
#include "MCPToolHandler.h"
#include "Plugin/PluginProcessor.h"
#include <nlohmann/json.hpp>
#include <exception>

namespace more_phi {

using json = nlohmann::json;

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
    startThread();
}

MCPServer::ConnectionThread::~ConnectionThread()
{
    signalExit();
    // m-4 FIX: Reduce timeout from 1000ms to 500ms, and force socket close
    // if the thread doesn't exit to avoid blocking plugin unload.
    if (!stopThread(500))
    {
        // Force socket close to unblock any pending read
        if (socket_) socket_->close();
        stopThread(100);  // Brief final wait
    }
}

void MCPServer::ConnectionThread::signalExit()
{
    signalThreadShouldExit();
    if (socket_) socket_->close();
}

void MCPServer::ConnectionThread::run()
{
    owner_.connectedClients_++;
    juce::String buffer;
    char chunk[4096];
    bool writeError = false;
    int readErrors = 0;
    constexpr int MAX_READ_ERRORS = 3;
    constexpr int MAX_REQUEST_BYTES = 256 * 1024;

    while (!threadShouldExit() && socket_->isConnected() && !writeError)
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
                    auto testParse = juce::JSON::parse(buffer);
                    if (testParse.isVoid()) break;
                    message = buffer.trim();
                    buffer.clear();
                }

                if (message.isEmpty()) continue;

                juce::String response;
                try {
                    response = owner_.processRequest(message, authenticated_);
                } catch (const std::exception& e) {
                    owner_.logError("request", juce::String("Exception processing request: ") + e.what());
                    response = juce::String(json{{"jsonrpc","2.0"},{"error",{{"code",-32603},{"message","Internal error"}}},{"id",nullptr}}.dump());
                }

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

                if (threadShouldExit()) break;
            }
        }
        catch (...) { break; }
    }
    owner_.connectedClients_--;
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

    // H-8 FIX: Reduced from 3000ms to 500ms. If called from message thread
    // during plugin shutdown, a 3s block freezes the host. Force-close the
    // socket above already unblocks any blocking accept() call.
    stopThread(500);

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
                    activeConnections_.add(new ConnectionThread(*this, client));
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
    if (serverSocket_.createListener(port_, "127.0.0.1"))
        return true;

    return serverSocket_.createListener(port_, {});
}

juce::String MCPServer::processRequest(const juce::String& jsonRequest, bool& authenticated)
{
    // Parse incoming request with JUCE so params flow to MCPToolHandler as juce::var
    auto parsed = juce::JSON::parse(jsonRequest);
    if (parsed.isVoid())
    {
        return juce::String(
            json{{"jsonrpc","2.0"},{"error",{{"code",-32700},{"message","Parse error"}}},{"id",nullptr}}.dump());
    }

    auto method = parsed.getProperty("method", "").toString();
    auto params = parsed.getProperty("params", juce::var());
    auto idVar  = parsed.getProperty("id",     juce::var());

    // Convert juce::var id to nlohmann::json (int | string | null)
    json reqId = nullptr;
    if (!idVar.isVoid())
    {
        if (idVar.isInt() || idVar.isInt64())
            reqId = static_cast<int64_t>((juce::int64)idVar);
        else if (idVar.isString())
            reqId = idVar.toString().toStdString();
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

        return errResponse(-32600, "Unauthorized: invalid bearer_token");
    }

    // ── All other methods require authentication ───────────────────────────────
    if (!authenticated)
        return errResponse(-32600, "Unauthorized: call initialize with bearer_token first");

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

        const std::string candidate = token.toStdString();
        const std::string expected  = identity_.bearerToken.toStdString();

        // M-11 FIX: Constant-time comparison — always compare to the longer of
        // the two strings so timing doesn't leak the expected token length.
        // If lengths differ, xor the extra bytes with a non-zero constant so
        // the loop always runs the same number of iterations for a given expected length.
        const size_t compareLen = std::max(candidate.size(), expected.size());

        volatile uint8_t diff = 0;
        for (size_t i = 0; i < compareLen; ++i)
        {
            const uint8_t c = (i < candidate.size()) ? static_cast<uint8_t>(candidate[i]) : 0xFF;
            const uint8_t e = (i < expected.size())  ? static_cast<uint8_t>(expected[i])  : 0xFF;
            diff = static_cast<uint8_t>(diff | (c ^ e));
        }

        // Explicit volatile read prevents the compiler from forwarding
        // the last written value without a load.
        const volatile uint8_t result = diff;
        return result == 0;
    }
    catch (...)
    {
        logError("validateAuth", "Exception during auth validation");
        return false;
    }
}

juce::String MCPServer::dispatchTool(const juce::String& method, const juce::var& params)
{
    return MCPToolHandler::handle(method, params, processor_, identity_);
}

void MCPServer::logError(const juce::String& context, const juce::String& details)
{
    errorCount_.fetch_add(1);
    juce::ignoreUnused(context, details);
    DBG("MCP [" + identity_.morphCode + "] ERROR in " + context +
        ": " + details + " (total errors: " + juce::String(errorCount_.load()) + ")");
}

} // namespace more_phi
