// src/AI/Agents/Conductor/ConductorAgent.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <atomic>

namespace more_phi::agents {

// The Conductor turns a user goal into a coordinated multi-agent plan.
// It uses WorkflowOrchestrator for the auditable run record and an ILlmClient
// (or deterministic fallback) to decompose intent into specialist subtasks.
class ConductorAgent : public IAgent
{
public:
    AgentRole role() const noexcept override { return AgentRole::Conductor; }
    juce::String id() const noexcept override { return "conductor-1"; }
    std::vector<juce::String> allowedTools() const override
    {
        return { "workflow.submit", "workflow.execute", "workflow.cancel",
                 "hosted_plugin.info", "analysis.get_summary" };
    }

    void prepare(const AgentContext& ctx) override { ctx_ = &ctx; }

    AgentResult execute(const AgentTask& task) override;
    AgentState state() const noexcept override { return state_.load(); }
    void stop() override;
    // H-4 FIX: Callback for submitting follow-ups asynchronously. Wired by
    // AgentRuntime. When the async LLM decomposition completes, the conductor
    // calls this on the scheduler worker to enqueue specialist subtasks.
    void setSubmitCallback(std::function<void(std::vector<AgentTask>)> cb) { submitCallback_ = std::move(cb); }
    // H-4: Called periodically by the runtime's pump to check if any pending
    // async decompositions have completed. If so, submits follow-ups via callback.
    void checkPendingDecompositions();

private:
    nlohmann::json decomposeGoal(const juce::String& intent);

    const AgentContext* ctx_ = nullptr;
    std::atomic<AgentState> state_{ AgentState::Idle };

    // H-4: In-flight async decompositions. Keyed by task.id.
    std::mutex pendingMutex_;
    std::unordered_map<std::string, std::pair<AgentTask, std::future<nlohmann::json>>> pendingDecompositions_;
    std::function<void(std::vector<AgentTask>)> submitCallback_;
};

} // namespace more_phi::agents
