/*
 * MorphSnap — AI/MCPServer.cpp
 * Fixed: blocking reads with newline-delimited JSON-RPC framing.
 */
#include "MCPServer.h"
#include "MCPToolHandler.h"
#include "Plugin/PluginProcessor.h"

namespace morphsnap {

MCPServer::MCPServer(MorphSnapProcessor& processor)
    : juce::Thread("MorphSnap-MCP"), processor_(processor)
{
    // Generate a random auth token
    juce::Random rng;
    authToken_ = juce::String::toHexString(rng.nextInt64())
               + juce::String::toHexString(rng.nextInt64());
}

MCPServer::~MCPServer()
{
    stopServer();
}

void MCPServer::startServer(int port)
{
    port_ = port;
    startThread();
}

void MCPServer::stopServer()
{
    signalThreadShouldExit();
    serverSocket_.close();
    stopThread(3000);
}

void MCPServer::run()
{
    if (!serverSocket_.createListener(port_, "127.0.0.1"))
    {
        DBG("MCP Server: failed to bind port " + juce::String(port_));
        return;
    }

    DBG("MCP Server listening on port " + juce::String(port_));

    while (!threadShouldExit())
    {
        auto* client = serverSocket_.waitForNextConnection();
        if (client && !threadShouldExit())
            handleConnection(client);
    }
}

void MCPServer::handleConnection(juce::StreamingSocket* client)
{
    connectedClients_++;
    std::unique_ptr<juce::StreamingSocket> conn(client);

    juce::String buffer;
    char chunk[4096];

    bool writeError = false;

    while (!threadShouldExit() && conn->isConnected() && !writeError)
    {
        // Blocking read with a small timeout so we can check threadShouldExit
        const int ready = conn->waitUntilReady(true, 500);  // 500ms timeout
        if (ready < 0) break;   // Error
        if (ready == 0) continue; // Timeout, check loop condition

        const int bytesRead = conn->read(chunk, sizeof(chunk) - 1, false);
        if (bytesRead <= 0) break;  // Client disconnected

        chunk[bytesRead] = '\0';
        buffer += juce::String::fromUTF8(chunk, bytesRead);

        // Process complete JSON messages (newline-delimited OR single object)
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
                    break;  // Incomplete JSON, wait for more data

                message = buffer.trim();
                buffer.clear();
            }

            if (message.isEmpty()) continue;

            juce::String response = processRequest(message);
            response += "\n";

            const int written = conn->write(
                response.toRawUTF8(),
                static_cast<int>(response.getNumBytesAsUTF8()));

            if (written < 0) { writeError = true; break; }
        }
    }
    connectedClients_--;
}

juce::String MCPServer::processRequest(const juce::String& jsonRequest)
{
    auto parsed = juce::JSON::parse(jsonRequest);
    if (parsed.isVoid())
        return R"({"jsonrpc":"2.0","error":{"code":-32700,"message":"Parse error"},"id":null})";

    auto method = parsed.getProperty("method", "").toString();
    auto params = parsed.getProperty("params", juce::var());
    auto id     = parsed.getProperty("id", juce::var());

    juce::String result = dispatchTool(method, params);

    return "{\"jsonrpc\":\"2.0\",\"result\":" + result + ",\"id\":" +
           juce::JSON::toString(id) + "}";
}

juce::String MCPServer::dispatchTool(const juce::String& method, const juce::var& params)
{
    return MCPToolHandler::handle(method, params, processor_);
}

} // namespace morphsnap
