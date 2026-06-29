/*
 * More-Phi — examples/IntegrationExamples.cpp
 * Standalone reference examples for the multi-agent ecosystem.
 *
 * This file is NOT compiled into the main plugin; it demonstrates
 * integration patterns for external tools, MCP clients, and
 * orchestrator lifecycle management.
 *
 * Build note: requires JUCE, nlohmann::json, and the More-Phi headers.
 */

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

// More-Phi ecosystem headers
#include "AI/InstanceIdentity.h"
#include "AI/MCPServer.h"
#include "AI/Agents/AgentRuntime.h"
#include "AI/Orchestrator/EcosystemConfig.h"
#include "AI/Orchestrator/SecurityValidator.h"
#include "Plugin/PluginProcessor.h"

#include <iostream>
#include <thread>
#include <chrono>

namespace more_phi::orchestrator {

//==============================================================================
// Conceptual orchestrator that wires Config -> Security -> Agents -> MCP
// In production this would be a real class; here it shows the assembly pattern.
//==============================================================================
class AgentOrchestrator
{
public:
    AgentOrchestrator() = default;
    ~AgentOrchestrator() { stop(); }

    // Initialize from an ecosystem config and the plugin processor
    bool initialize(MorePhiProcessor& processor, const EcosystemConfig& config)
    {
        config_     = config;
        processor_  = &processor;
        validator_  = std::make_unique<SecurityValidator>(config_);

        // Derive identity from config (port & token)
        InstanceIdentity id = InstanceIdentity::generate(config_.mcp.port);
        id.bearerToken = config_.mcp.authToken;

        // Wire MCP server identity
        processor_->getMCPServer().setIdentity(id);

        if (config_.mcp.enableServer)
        {
            processor_->getMCPServer().startServer(config_.mcp.port);
            if (!processor_->getMCPServer().isRunning())
            {
                juce::Logger::writeToLog("[Orchestrator] MCP server failed to start on port "
                                         + juce::String(config_.mcp.port));
                return false;
            }
        }

        // TODO: instantiate AgentRuntime when the tool invoker / blackboard are available
        // runtime_ = std::make_unique<agents::AgentRuntime>(...);
        // runtime_->start(config_.agents.numWorkers);

        juce::Logger::writeToLog("[Orchestrator] Initialized successfully");
        return true;
    }

    void start()
    {
        if (runtime_)
            runtime_->start(config_.agents.numWorkers);
    }

    void stop()
    {
        if (processor_ && processor_->getMCPServer().isRunning())
            processor_->getMCPServer().stopServer();

        if (runtime_)
            runtime_->stop();
    }

    nlohmann::json describeSystemState() const
    {
        nlohmann::json state;
        state["mcpHealthy"]      = processor_ ? processor_->getMCPServer().isHealthy() : false;
        state["mcpClients"]      = processor_ ? processor_->getMCPServer().getConnectedClients() : 0;
        state["mcpErrorCount"]   = processor_ ? processor_->getMCPServer().getErrorCount() : 0;
        state["configValid"]     = config_.validate();
        state["securityEvents"]  = validator_ ? validator_->getTotalEventsLogged() : 0;
        if (runtime_)
            state["agentState"] = runtime_->describeState();
        return state;
    }

    SecurityValidator* getValidator() const noexcept { return validator_.get(); }

private:
    EcosystemConfig config_;
    MorePhiProcessor* processor_ = nullptr;
    std::unique_ptr<SecurityValidator> validator_;
    std::unique_ptr<agents::AgentRuntime> runtime_; // placeholder
};

} // namespace more_phi::orchestrator

using namespace more_phi;
using namespace more_phi::orchestrator;
using namespace std::chrono_literals;

//==============================================================================
// Example 1: Full startup sequence
//==============================================================================
/**
 * Demonstrates the complete initialization chain:
 *   1. Create default config (or load from file).
 *   2. Build AgentOrchestrator.
 *   3. Start MCP server and AgentRuntime.
 *   4. Query system health.
 *
 * Error handling: every step returns a bool; on failure we log and bail.
 */
void example_fullStartup(MorePhiProcessor& processor)
{
    std::cout << "\n=== Example 1: Full Startup ===\n";

    // 1. Load configuration from disk, or fall back to identity-based defaults
    EcosystemConfig config;
    const juce::File configPath = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                    .getChildFile("MorePhi/ecosystem.json");

    if (!config.loadFromFile(configPath))
    {
        std::cout << "Config load failed: " << config.getLastError().toStdString() << "\n";
        std::cout << "Falling back to identity-based defaults...\n";

        config = EcosystemConfig::createFromIdentity(processor.getInstanceIdentity());
    }

    // 2. Validate before use (defensive: catches runtime edits or corrupted files)
    if (!config.validate())
    {
        std::cerr << "Config validation failed: " << config.getLastError().toStdString() << "\n";
        return;
    }

    // 3. Instantiate and initialize the orchestrator
    auto orchestrator = std::make_unique<AgentOrchestrator>();
    if (!orchestrator->initialize(processor, config))
    {
        std::cerr << "Orchestrator initialization failed (see JUCE log).\n";
        return;
    }

    // 4. Start background workers
    orchestrator->start();

    // 5. Describe system state for monitoring / UI dashboards
    const nlohmann::json state = orchestrator->describeSystemState();
    std::cout << "System state:\n" << state.dump(2) << "\n";

    // 6. Verify MCP health before continuing
    if (!state.value("mcpHealthy", false))
    {
        std::cerr << "WARNING: MCP server is not healthy. Check port binding and auth.\n";
    }

    // In a real application, orchestrator would be stored as a member
    // and kept alive for the plugin lifetime. Here we just demonstrate flow.
    std::cout << "Startup complete.\n";
}

//==============================================================================
// Example 2: Submit a user goal and poll for completion
//==============================================================================
/**
 * Shows the agent goal-submission pattern:
 *   1. Build a natural-language intent.
 *   2. Submit via AgentOrchestrator (delegated to AgentRuntime).
 *   3. Poll the result blackboard until the task completes or times out.
 *
 * Error handling: empty runId means submission failed; peekResult may
 * return std::nullopt while the task is still running.
 */
void example_submitGoal(AgentOrchestrator& orch)
{
    std::cout << "\n=== Example 2: Submit Goal ===\n";

    // Access the runtime indirectly. In a real build, AgentOrchestrator
    // would expose submitGoal() directly; here we show the plumbing.
    // juce::String runId = orch.submitGoal("make this track louder and brighter");

    // For illustration, construct a mock goal that an AI assistant would parse:
    const juce::String userIntent = "make this track louder and brighter";
    std::cout << "Submitting goal: " << userIntent.toStdString() << "\n";

    // -------------------------------------------------------------------------
    // In production the runtime is inside the orchestrator; the call is:
    //   juce::String runId = runtime_->submitGoal(userIntent);
    // -------------------------------------------------------------------------
    const juce::String runId = "mock-run-id-12345"; // placeholder

    if (runId.isEmpty())
    {
        std::cerr << "Goal submission failed: no runId returned.\n";
        return;
    }

    std::cout << "Goal accepted with runId: " << runId.toStdString() << "\n";

    // Poll for result with a timeout (non-blocking on the audio thread)
    constexpr int maxPolls = 100;
    for (int i = 0; i < maxPolls; ++i)
    {
        std::this_thread::sleep_for(100ms);

        // ------------------------------------------------------------------
        // In production:
        //   auto result = runtime_->peekResult(runId);
        //   if (result.has_value()) { ... }
        // ------------------------------------------------------------------
        const bool mockDone = (i > 10); // simulate completion after 1 second
        if (mockDone)
        {
            std::cout << "Goal completed after " << i << " polls.\n";
            break;
        }
    }

    std::cout << "Goal lifecycle finished.\n";
}

//==============================================================================
// Example 3: External MCP client request / response
//==============================================================================
/**
 * Demonstrates how an external process (e.g., a Python script or another
 * plugin) connects to the embedded MCP server and sends a JSON-RPC request.
 *
 * This is a standalone client example; it does not depend on the plugin
 * internals beyond the port and auth token.
 */
void example_mcpClientRequest(MCPServer& server)
{
    std::cout << "\n=== Example 3: MCP Client Request ===\n";

    const int port       = server.getPort();
    const juce::String token = server.getAuthToken();

    // 1. Open TCP connection to the MCP server
    juce::StreamingSocket clientSocket;
    if (!clientSocket.connect("127.0.0.1", port, 5000))
    {
        std::cerr << "Client failed to connect to port " << port << "\n";
        return;
    }

    std::cout << "Client connected to MCP server on port " << port << "\n";

    // 2. Build a JSON-RPC 2.0 request with authentication
    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["method"]  = "tools/list";
    request["params"]  = { {"auth_token", token.toStdString()} };
    request["id"]      = 1;

    const std::string payload = request.dump() + "\n";

    // 3. Send the request
    const int sent = clientSocket.write(payload.data(), static_cast<int>(payload.size()));
    if (sent != static_cast<int>(payload.size()))
    {
        std::cerr << "Client failed to send complete request (sent " << sent << " bytes).\n";
        return;
    }

    std::cout << "Client sent " << sent << " bytes.\n";

    // 4. Read the response (blocking with a 5-second timeout)
    char buffer[4096] = {};
    const int received = clientSocket.read(buffer, sizeof(buffer) - 1, true, 5000);
    if (received <= 0)
    {
        std::cerr << "Client received no response (timeout or disconnect).\n";
        return;
    }

    buffer[received] = '\0';
    std::cout << "Client received " << received << " bytes:\n" << buffer << "\n";

    // 5. Parse and validate the response
    try
    {
        nlohmann::json response = nlohmann::json::parse(buffer);
        if (response.contains("error"))
        {
            std::cerr << "Server returned error: " << response.at("error").dump() << "\n";
        }
        else if (response.contains("result"))
        {
            std::cout << "Server result: " << response.at("result").dump(2) << "\n";
        }
    }
    catch (const nlohmann::json::exception& e)
    {
        std::cerr << "Client failed to parse response: " << e.what() << "\n";
    }

    clientSocket.close();
}

//==============================================================================
// Example 4: Graceful shutdown
//==============================================================================
/**
 * Shows how to stop the orchestrator, drain pending work, and handle
 * errors during teardown without leaving sockets or threads dangling.
 */
void example_gracefulShutdown(AgentOrchestrator& orch)
{
    std::cout << "\n=== Example 4: Graceful Shutdown ===\n";

    // 1. Capture final state before shutdown (for diagnostics)
    const nlohmann::json finalState = orch.describeSystemState();
    std::cout << "Final state before shutdown:\n" << finalState.dump(2) << "\n";

    // 2. Stop the orchestrator (stops MCP server, agent workers, and blackboard pump)
    try
    {
        orch.stop();
        std::cout << "Orchestrator stopped cleanly.\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << "Exception during shutdown: " << e.what() << "\n";
        // In production: log to crash reporter, do NOT rethrow from destructors
    }

    // 3. Verify cleanup
    // ------------------------------------------------------------------
    // In production:
    //   if (processor.getMCPServer().isRunning())
    //       std::cerr << "WARNING: MCP server still running after stop!\n";
    // ------------------------------------------------------------------
    std::cout << "Shutdown sequence complete.\n";
}

//==============================================================================
// Example 5: Security pipeline
//==============================================================================
/**
 * Demonstrates the full security pipeline on an incoming request:
 *   1. Extract client ID from socket.
 *   2. Check rate limit.
 *   3. Validate auth token (constant-time).
 *   4. Validate JSON structure (depth, size, fields, method whitelist).
 *   5. Sanitize parameters.
 *
 * Any step may reject the request with a descriptive error.
 */
void example_securityPipeline(SecurityValidator& validator)
{
    std::cout << "\n=== Example 5: Security Pipeline ===\n";

    // Mock socket for client identification (nullptr is acceptable for the example)
    juce::StreamingSocket* mockSocket = nullptr; // in production: the accepted client socket
    const juce::String clientId = validator.getClientId(mockSocket);
    std::cout << "Client ID: " << clientId.toStdString() << "\n";

    // 1. Rate limit check
    if (!validator.checkRateLimit(clientId))
    {
        std::cerr << "Request rejected: rate limit exceeded for client "
                  << clientId.toStdString() << "\n";
        return;
    }
    std::cout << "Rate limit check passed.\n";

    // 2. Auth token validation (constant-time comparison)
    const juce::String expectedToken = "bearer-secret-abc123";
    const juce::String providedToken = "bearer-secret-abc123"; // in production: from headers
    if (!validator.validateAuthToken(providedToken, expectedToken))
    {
        std::cerr << "Request rejected: invalid auth token.\n";
        validator.logSecurityEvent("AUTH_FAILURE", clientId, "Token mismatch");
        return;
    }
    std::cout << "Auth token validated.\n";

    // 3. Build a sample JSON-RPC request
    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["method"]  = "tools/call";
    request["params"]  = {
        {"tool_name", "set_morph_position"},
        {"x", 0.75},
        {"y", 0.25},
        {"long_string", std::string(5000, 'A')} // will be truncated by sanitizer
    };
    request["id"] = 42;

    // 4. Validate JSON structure
    juce::String error;
    if (!validator.validateRequestJson(request, error))
    {
        std::cerr << "Request rejected: JSON validation failed ("
                  << error.toStdString() << ")\n";
        validator.logSecurityEvent("JSON_VALIDATION_FAILURE", clientId, error);
        return;
    }
    std::cout << "JSON structure validated.\n";

    // 5. Sanitize parameters
    nlohmann::json& params = request["params"];
    if (!validator.sanitizeParams(params, error))
    {
        std::cerr << "Request rejected: parameter sanitization failed ("
                  << error.toStdString() << ")\n";
        validator.logSecurityEvent("SANITIZATION_FAILURE", clientId, error);
        return;
    }
    std::cout << "Parameters sanitized.\n";

    // Show the sanitized payload
    std::cout << "Sanitized request:\n" << request.dump(2) << "\n";

    // 6. Demonstrate a disallowed method rejection
    nlohmann::json badRequest;
    badRequest["jsonrpc"] = "2.0";
    badRequest["method"]  = "system.exec"; // not in whitelist
    badRequest["id"]      = 99;

    if (!validator.validateRequestJson(badRequest, error))
    {
        std::cout << "Correctly rejected disallowed method: " << error.toStdString() << "\n";
    }

    std::cout << "Security pipeline completed successfully.\n";
}

//==============================================================================
// Main entry point (for standalone compilation / testing)
//==============================================================================
/**
 * Uncomment the following main() to build this file as a standalone test
 * executable. It requires linking JUCE and the More-Phi shared code.
 *
 * Build command (example):
 *   g++ -std=c++20 IntegrationExamples.cpp -o integration_examples \
 *       -I../src -I/path/to/juce/modules -I/path/to/nlohmann \
 *       -ljuce_core -lpthread
 */
#if 0
int main(int /*argc*/, char* /*argv*/[])
{
    // JUCE needs to be initialized before using File, Logger, sockets, etc.
    juce::initialiseJuce_GUI();

    // We need a processor instance to demonstrate startup. In a real host
    // this is provided by the DAW; here we use a local scope for testing.
    {
        MorePhiProcessor processor;
        example_fullStartup(processor);

        // The orchestrator would be created inside example_fullStartup;
        // here we reuse a conceptual one for the remaining examples.
        AgentOrchestrator orch;
        if (!orch.initialize(processor, EcosystemConfig::createDefaults()))
        {
            std::cerr << "Failed to initialize orchestrator for examples.\n";
            juce::shutdownJuce_GUI();
            return 1;
        }

        example_submitGoal(orch);
        example_mcpClientRequest(processor.getMCPServer());
        example_securityPipeline(*orch.getValidator());
        example_gracefulShutdown(orch);
    }

    juce::shutdownJuce_GUI();
    return 0;
}
#endif
