/*
 * MorphSnap — AI/MCPServer.h
 * Embedded MCP JSON-RPC 2.0 server for AI client connectivity.
 */
#pragma once

#include <juce_core/juce_core.h>
#include "Core/LockFreeQueue.h"
#include <atomic>

namespace morphsnap {

class MorphSnapProcessor;

class MCPServer : private juce::Thread
{
public:
    explicit MCPServer(MorphSnapProcessor& processor);
    ~MCPServer() override;

    void startServer(int port = 30001);
    void stopServer();

    bool isRunning() const { return isThreadRunning(); }
    int  getPort() const   { return port_; }
    int  getConnectedClients() const { return connectedClients_.load(); }
    const juce::String& getAuthToken() const { return authToken_; }

private:
    void run() override;
    void handleConnection(juce::StreamingSocket* client);
    juce::String processRequest(const juce::String& jsonRequest);
    juce::String dispatchTool(const juce::String& method, const juce::var& params);

    MorphSnapProcessor& processor_;
    juce::StreamingSocket serverSocket_;
    int port_ = 30001;
    juce::String authToken_;
    std::atomic<int> connectedClients_{0};
};

} // namespace morphsnap
