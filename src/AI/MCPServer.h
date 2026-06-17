/*
 * More-Phi — AI/MCPServer.h
 * Embedded MCP JSON-RPC 2.0 server for AI client connectivity.
 * Multi-instance aware: unique port, bearer auth, morph identity.
 * Includes automatic recovery and error resilience.
 */
#pragma once

#include <juce_core/juce_core.h>
#include "InstanceIdentity.h"
#include "AutomationControlPlane.h"
#include <atomic>

namespace more_phi {

class MorePhiProcessor;

class MCPServer : private juce::Thread
{
public:
    /** Thread-per-connection handler for concurrent MCP requests. */
    class ConnectionThread : public juce::Thread
    {
    public:
        ConnectionThread(MCPServer& owner, juce::StreamingSocket* socket);
        ~ConnectionThread() override;

        void run() override;
        void signalExit();
        bool startedSuccessfully() const noexcept { return startedSuccessfully_; }

    private:
        MCPServer& owner_;
        std::unique_ptr<juce::StreamingSocket> socket_;
        bool authenticated_ = false;
        bool startedSuccessfully_ = false;
    };

    explicit MCPServer(MorePhiProcessor& processor);
    ~MCPServer() override;

    /** Start on the port from the assigned identity. */
    void startServer(int port = 30001);
    void stopServer();
    void recordStartupFailure(const juce::String& details);

    bool isRunning() const { return isThreadRunning(); }
    int  getPort() const   { return identity_.port > 0 ? identity_.port : port_; }
    int  getConnectedClients() const { return connectedClients_.load(); }
    
    /** Health status for monitoring */
    bool isHealthy() const { return healthy_.load(); }
    int  getErrorCount() const { return errorCount_.load(); }
    void resetErrorCount() { errorCount_.store(0); }

    /** Get/set the instance identity (must be set before startServer). */
    void setIdentity(const InstanceIdentity& id) { identity_ = id; }
    const InstanceIdentity& getIdentity() const  { return identity_; }

    /** Legacy accessor — returns identity's bearer token. */
    const juce::String& getAuthToken() const { return identity_.bearerToken; }

    /** Access the instance-scoped automation runtime (C13 fix). */
    AutomationRuntime& getAutomationRuntime() noexcept { return automationRuntime_; }

private:
    void run() override;
    bool createServerListener();
    void handleConnection(juce::StreamingSocket* client);
    juce::String processRequest(const juce::String& jsonRequest, bool& authenticated);
    juce::String dispatchTool(const juce::String& method, const juce::var& params);
    bool validateAuth(const juce::var& params);
    
    // Error recovery helpers
    bool attemptRecovery();
    void logError(const juce::String& context, const juce::String& details = {});

    MorePhiProcessor& processor_;
    juce::StreamingSocket serverSocket_;
    int port_ = 30001;
    InstanceIdentity identity_;
    std::atomic<int> connectedClients_{0};
    std::atomic<bool> healthy_{false};
    std::atomic<int> errorCount_{0};
    
    // Recovery configuration
    static constexpr int MAX_CONSECUTIVE_ERRORS = 5;
    static constexpr int RECOVERY_DELAY_MS = 1000;
    static constexpr int MAX_BIND_ATTEMPTS = 3;
    static constexpr int MAX_CONNECTIONS = 4;  // Max concurrent TCP clients

    void removeConnection(ConnectionThread* thread);

    juce::OwnedArray<ConnectionThread> activeConnections_;
    mutable juce::CriticalSection connectionsLock_;

    AutomationRuntime automationRuntime_;  // C13: instance-scoped runtime
};

} // namespace more_phi
