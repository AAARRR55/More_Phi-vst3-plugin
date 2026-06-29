// src/AI/Agents/AgentRuntime.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"
#include "AI/Agents/AgentRegistry.h"
#include "AI/Agents/Scheduler/PriorityScheduler.h"
#include "AI/Agents/Blackboard/BlackboardBridge.h"
#include "AI/Agents/Tooling/DefaultToolInvoker.h"
#include "AI/Agents/Logging/NullAgentLogger.h"
#include "AI/Agents/Conductor/ConductorAgent.h"

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <set>
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

    // Idempotent: re-starting after stop() is supported (H1). Re-registers the
    // blackboard subscriptions and respawns the worker pool + pump thread.
    void start(unsigned numWorkers = 2);

    // Stops the worker pool and pump thread. Tasks still in the queues are
    // dropped. submit*() after stop() returns empty envelopes so callers can
    // detect the stopped state instead of silently queuing into a dead pool.
    void stop();

    bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    bool registerAgent(std::unique_ptr<IAgent> agent);
    AgentRegistry& registry() noexcept { return registry_; }
    const AgentRegistry& registry() const noexcept { return registry_; }
    BlackboardBridge& blackboard() noexcept { return blackboard_; }

    // Top-level: user goal -> Conductor decomposes -> specialists execute.
    // Returns the runId (== the Conductor task id). Empty if no Conductor registered
    // or the runtime is not running.
    juce::String submitGoal(const juce::String& userIntent,
                            TaskPriority priority = TaskPriority::High,
                            const juce::String& origin = "user");

    // Direct entry (event-driven, or MCP agents.run_task).
    // Returns the assigned task id (empty if the target agent is not registered
    // or the runtime is not running).
    juce::String submitTask(AgentTask task);

    // Observability.
    nlohmann::json describeState() const;
    std::optional<AgentResult> peekResult(const juce::String& taskId) const;

    // H3: curated, capability-scoped view of recent blackboard activity. Unlike
    // reading the raw IntegrationEventBus (which also carries non-agent events
    // like action-ledger artifacts and permission decisions), this returns ONLY
    // agent-published events, capped at `limit`, with payloads trimmed to a safe
    // summary (type/source/runId/sequence) — no raw mastering telemetry or
    // workflow internals leak to an unscoped MCP client.
    nlohmann::json blackboardRecent(int limit = 32) const;

private:
    void executeOnWorker(IAgent& agent, const AgentTask& task);
    void publishResultEvents(const juce::String& agentId, const AgentResult& r);
    void processFollowUps(const IAgent& source, AgentResult& r);

    // H4: conductor outcome correlation. When the Conductor enqueues follow-ups,
    // we remember the originating goal task per runId and count outstanding
    // subtasks. As each completes, we aggregate; when the last one lands we
    // publish conductor.complete (consumed by MemoryAgent) and rewrite the goal
    // result so peekResult(goalTaskId) reflects the real outcome.
    void registerRun(const juce::String& runId, const juce::String& goalTaskId);
    void noteSubtaskCompletion(const juce::String& runId,
                               const juce::String& subtaskTaskId,
                               const AgentResult& subResult);

    juce::String storeResultLocked(const juce::String& taskId, AgentResult result);
    juce::String nextTaskId(const juce::String& prefix);

    // Shared by all agents (references / raw pointers, not owned):
    MorePhiProcessor*       processor_;
    const InstanceIdentity* identity_;
    AutomationRuntime*      runtime_;
    IToolInvoker*           tools_;
    BlackboardBridge&       blackboard_;
    IAgentLogger&           logger_;
    ILlmClient*             llm_;

    // Owned context member: agents hold raw pointers into this, so it MUST
    // outlive every registered agent. Populated in start() before any worker
    // can execute an agent. (Previously this was a stack local in start() —
    // agents received dangling pointers; fixed.)
    AgentContext sharedContext_;

    AgentRegistry     registry_;
    PriorityScheduler scheduler_;

    // C2: bounded results store. results_ is an unordered_map for O(1) lookup by
    // taskId; resultsOrder_ is a FIFO of taskIds so we can evict the oldest when
    // maxResults_ is exceeded (default 1024). Without this, a long-lived plugin
    // session with a chatty MCP client leaked one AgentResult per task forever.
    mutable std::mutex resultsMutex_;
    std::unordered_map<std::string, AgentResult> results_;
    std::deque<std::string> resultsOrder_;
    static constexpr size_t maxResults_ = 1024;

    // M5: monotonic counter so two near-simultaneous submits from different
    // threads cannot collide on juce::Time::getHighResolutionTicks() and
    // produce the same task id (which would silently overwrite a result).
    std::atomic<uint64_t> taskIdCounter_{0};

    // H4: per-run correlation state. Guarded by runsMutex_.
    struct RunState
    {
        juce::String goalTaskId;     // the Conductor's task id for this goal
        int outstanding = 0;         // subtasks not yet completed
        bool aggregateSuccess = true;
        nlohmann::json findings = nlohmann::json::object();
    };
    mutable std::mutex runsMutex_;
    std::unordered_map<std::string, RunState> runs_;

    std::atomic<bool> running_{false};
    long long blackboardPumpIntervalMs_ = 50;
    std::atomic<bool> blackboardPumpRunning_{false};
    std::thread blackboardPumpThread_;

    // Tracks which agent ids have already been wired into the blackboard so that
    // start() after stop() doesn't subscribe the same instance twice.
    std::set<std::string> subscribedAgentIds_;
};

} // namespace more_phi::agents
