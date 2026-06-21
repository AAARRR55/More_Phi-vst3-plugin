// src/AI/Orchestrator/AgentOrchestrator.h
#pragma once

#include "AI/Agents/AgentRuntime.h"
#include "AI/Agents/Blackboard/BlackboardBridge.h"
#include "AI/Agents/Tooling/DefaultToolInvoker.h"
#include "AI/Agents/Logging/NullAgentLogger.h"

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>

namespace more_phi
{
class MorePhiProcessor;
}

namespace more_phi::orchestrator
{

/**
 * Single-initialization facade that wires PluginProcessor -> AgentRuntime -> MCPServer.
 * Owns the full multi-agent lifecycle: BlackboardBridge, tool invoker, logger,
 * agent runtime, and registration of all 6 built-in agents.
 */
class AgentOrchestrator
{
public:
    explicit AgentOrchestrator (MorePhiProcessor& processor);
    ~AgentOrchestrator();

    /** Start the orchestrator: create bridges, register agents, start scheduler,
     *  and optionally bring up the MCP server. */
    bool start();

    /** Graceful teardown: stop scheduler, agents, and MCP server. */
    void stop();

    /** Submit a high-level user goal to the Conductor.
     *  @return the runId (task id) or empty string if unavailable. */
    juce::String submitUserGoal (const juce::String& intent);

    /** Snapshot of system state as JSON (orchestrator, MCP, agents, scheduler). */
    nlohmann::json describeSystemState() const;

private:
    MorePhiProcessor& processor_;

    std::unique_ptr<agents::BlackboardBridge>     blackboardBridge_;
    std::unique_ptr<agents::DefaultToolInvoker> toolInvoker_;
    agents::NullAgentLogger                       logger_;
    std::unique_ptr<agents::AgentRuntime>         agentRuntime_;
    std::atomic<bool>                             running_ { false };
};

} // namespace more_phi::orchestrator
