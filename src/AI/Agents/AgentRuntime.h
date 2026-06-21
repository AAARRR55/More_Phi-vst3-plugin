// src/AI/Agents/AgentRuntime.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"
#include "AI/Agents/AgentRegistry.h"
#include "AI/Agents/Scheduler/PriorityScheduler.h"
#include "AI/Agents/Blackboard/BlackboardBridge.h"
#include "AI/Agents/Tooling/DefaultToolInvoker.h"
#include "AI/Agents/Logging/NullAgentLogger.h"

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace more_phi {
class MorePhiProcessor;
struct InstanceIdentity;
class AutomationRuntime;
class IntegrationEventBus;
} // namespace more_phi

namespace more_phi::agents {

class IAgentLogger;
class ILlmClient;

class AgentRuntime
{
public:
    // Full constructor (production): wires real processor/identity/runtime.
    // In tests the first three may be nullptr with a fake IToolInvoker.
    AgentRuntime(MorePhiProcessor* processor,
                 const InstanceIdentity* identity,
                 AutomationRuntime* runtime,
                 IToolInvoker& tools,
                 BlackboardBridge& blackboard,
                 IAgentLogger& logger,
                 ILlmClient* llm);

    ~AgentRuntime();

    void start(unsigned numWorkers = 2);
    void stop();

    bool registerAgent(std::unique_ptr<IAgent> agent);
    AgentRegistry& registry() noexcept { return registry_; }
    const AgentRegistry& registry() const noexcept { return registry_; }
    BlackboardBridge& blackboard() noexcept { return blackboard_; }

    // Top-level: user goal -> Conductor decomposes -> specialists execute.
    // Returns the runId (== the Conductor task id). Empty if no Conductor registered.
    juce::String submitGoal(const juce::String& userIntent,
                            TaskPriority priority = TaskPriority::High,
                            const juce::String& origin = "user");

    // Direct entry (event-driven, or MCP agents.run_task).
    // Returns the assigned task id (empty if the target agent is not registered).
    juce::String submitTask(AgentTask task);

    // Observability.
    nlohmann::json describeState() const;
    std::optional<AgentResult> peekResult(const juce::String& taskId) const;

private:
    void executeOnWorker(IAgent& agent, const AgentTask& task);
    void publishResultEvents(const juce::String& agentId, const AgentResult& r);
    void processFollowUps(const IAgent& source, AgentResult& r);

    // Shared by all agents (references / raw pointers, not owned):
    MorePhiProcessor*       processor_;
    const InstanceIdentity* identity_;
    AutomationRuntime*      runtime_;
    IToolInvoker*           tools_;
    BlackboardBridge&       blackboard_;
    IAgentLogger&           logger_;
    ILlmClient*             llm_;

    AgentRegistry     registry_;
    PriorityScheduler scheduler_;

    mutable std::mutex resultsMutex_;
    std::unordered_map<std::string, AgentResult> results_;

    std::atomic<bool> running_{false};
    long long blackboardPumpIntervalMs_ = 50;
    std::atomic<bool> blackboardPumpRunning_{false};
    std::thread blackboardPumpThread_;
};

} // namespace more_phi::agents
