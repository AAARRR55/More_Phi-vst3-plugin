/*
 * MorphSnap — AI/MCPServer.cpp
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

namespace morphsnap {

using json = nlohmann::json;

MCPServer::MCPServer(MorphSnapProcessor& processor)
    : juce::Thread("MorphSnap-MCP"), processor_(processor)
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
    startThread();
}

void MCPServer::stopServer()
{
    healthy_.store(false);
    signalThreadShouldExit();
    serverSocket_.close();
    stopThread(3000);
}

void MCPServer::run()
{
    int bindAttempts = 0;
    constexpr int MAX_BIND_ATTEMPTS = 3;

    while (!threadShouldExit())
    {
        // Attempt to create listener with retry
        if (!serverSocket_.createListener(port_, "127.0.0.1"))
        {
            bindAttempts++;
            logError("bind", "Failed to bind port " + juce::String(port_) +
                     " (attempt " + juce::String(bindAttempts) + "/" +
                     juce::String(MAX_BIND_ATTEMPTS) + ")");

            if (bindAttempts >= MAX_BIND_ATTEMPTS)
            {
                DBG("MCP Instance [" + identity_.morphCode + "]: port binding failed after " +
                    juce::String(MAX_BIND_ATTEMPTS) + " attempts, giving up");
                return;
            }

            // Wait before retry
            wait(RECOVERY_DELAY_MS);
            continue;
        }

        // Successfully bound
        bindAttempts = 0;
        healthy_.store(true);

        DBG("MCP Instance [" + identity_.morphCode + "] listening on port " + juce::String(port_));

        // Main accept loop
        int consecutiveErrors = 0;

        while (!threadShouldExit())
        {
            try
            {
                // Poll with timeout so we can respond to threadShouldExit() promptly.
                // waitUntilReady(true, 500) blocks at most 500 ms before returning:
                //   > 0 : socket is readable — a connection is pending
                //   = 0 : timeout expired, no connection yet
                //   < 0 : error
                const int readyResult = serverSocket_.waitUntilReady(true, 500);

                if (threadShouldExit())
                    break;

                if (readyResult < 0)
                {
                    // Socket error — treat as disconnection and attempt recovery
                    logError("accept", "Server socket error during poll, attempting recovery");
                    healthy_.store(false);
                    break;
                }

                if (readyResult == 0)
                    continue;  // Timeout: no connection yet, loop & re-check exit flag

                // A connection is pending — accept it
                auto* client = serverSocket_.waitForNextConnection();
                if (threadShouldExit())
                {
                    delete client;
                    break;
                }

                if (client)
                {
                    handleConnection(client);
                    consecutiveErrors = 0;  // Reset on successful connection
                }
                else if (!serverSocket_.isConnected())
                {
                    // Socket was closed, need to rebind
                    logError("accept", "Server socket disconnected, attempting recovery");
                    healthy_.store(false);
                    break;
                }
            }
            catch (const std::exception& e)
            {
                consecutiveErrors++;
                logError("connection", juce::String("Exception: ") + e.what());

                if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS)
                {
                    logError("connection", "Too many consecutive errors, restarting listener");
                    healthy_.store(false);
                    break;
                }
            }
            catch (...)
            {
                consecutiveErrors++;
                logError("connection", "Unknown exception in accept loop");

                if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS)
                {
                    healthy_.store(false);
                    break;
                }
            }
        }

        // Cleanup before potential retry
        serverSocket_.close();

        if (!threadShouldExit())
        {
            // Wait before attempting to rebind
            wait(RECOVERY_DELAY_MS);
        }
    }

    healthy_.store(false);
}

void MCPServer::handleConnection(juce::StreamingSocket* client)
{
    connectedClients_++;
    std::unique_ptr<juce::StreamingSocket> conn(client);

    juce::String buffer;
    char chunk[4096];
    bool writeError = false;
    bool authenticated = false;  // Per-connection auth state
    int readErrors = 0;
    constexpr int MAX_READ_ERRORS = 3;

    while (!threadShouldExit() && conn->isConnected() && !writeError)
    {
        try
        {
            const int ready = conn->waitUntilReady(true, 500);
            if (ready < 0)
            {
                readErrors++;
                if (readErrors >= MAX_READ_ERRORS)
                    break;
                continue;
            }
            if (ready == 0)
            {
                readErrors = 0;  // Reset on successful poll
                continue;
            }

            const int bytesRead = conn->read(chunk, sizeof(chunk) - 1, false);
            if (bytesRead <= 0)
            {
                // Connection closed or error
                break;
            }

            readErrors = 0;  // Reset on successful read
            chunk[bytesRead] = '\0';
            buffer += juce::String::fromUTF8(chunk, bytesRead);

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
                    if (testParse.isVoid())
                        break;

                    message = buffer.trim();
                    buffer.clear();
                }

                if (message.isEmpty()) continue;

                juce::String response;
                try
                {
                    response = processRequest(message, authenticated);
                }
                catch (const std::exception& e)
                {
                    logError("request", juce::String("Exception processing request: ") + e.what());
                    response = juce::String(
                        json{{"jsonrpc","2.0"},{"error",{{"code",-32603},{"message","Internal error"}}},{"id",nullptr}}.dump());
                }
                catch (...)
                {
                    logError("request", "Unknown exception processing request");
                    response = juce::String(
                        json{{"jsonrpc","2.0"},{"error",{{"code",-32603},{"message","Internal error"}}},{"id",nullptr}}.dump());
                }

                response += "\n";

                const int written = conn->write(
                    response.toRawUTF8(),
                    static_cast<int>(response.getNumBytesAsUTF8()));

                if (written < 0) { writeError = true; break; }
            }
        }
        catch (const std::exception& e)
        {
            logError("handleConnection", juce::String("Exception: ") + e.what());
            break;
        }
        catch (...)
        {
            logError("handleConnection", "Unknown exception");
            break;
        }
    }
    connectedClients_--;
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
            reqId = static_cast<int64_t>(idVar);
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
                {"serverInfo", {{"name","MorphSnap MCP"},{"version","1.0"}}},
                {"instanceId", identity_.instanceId.toStdString()},
                {"morphCode",  identity_.morphCode.toStdString()},
                {"port",       port_}
            };

            return juce::String(
                json{{"jsonrpc","2.0"},{"result",result},{"id",reqId}}.dump());
        }

        return errResponse(-32600, "Unauthorized: invalid bearer_token");
    }

    // ── All other methods require authentication ───────────────────────────────
    if (!authenticated)
        return errResponse(-32600, "Unauthorized: call initialize with bearer_token first");

    // ── Dispatch to tool handler ──────────────────────────────────────────────
    juce::String toolResult;
    try
    {
        toolResult = dispatchTool(method, params);
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
    json resultEmbedded;
    try
    {
        resultEmbedded = json::parse(toolResult.toStdString());
    }
    catch (...)
    {
        // Shouldn't happen — treat as raw string if parse fails
        resultEmbedded = toolResult.toStdString();
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

        // Constant-time comparison: always compare all bytes regardless of
        // where a mismatch occurs, preventing timing-based token enumeration.
        const std::string candidate = token.toStdString();
        const std::string expected  = identity_.bearerToken.toStdString();

        volatile uint8_t diff = static_cast<uint8_t>(candidate.size() ^ expected.size());
        const size_t len = std::min(candidate.size(), expected.size());
        for (size_t i = 0; i < len; ++i)
            diff |= static_cast<uint8_t>(candidate[i] ^ expected[i]);

        return (diff == 0) && (candidate.size() == expected.size());
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

} // namespace morphsnap
