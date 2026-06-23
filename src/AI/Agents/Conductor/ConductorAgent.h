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
    void stop() override { state_.store(AgentState::Stopped); }

private:
    nlohmann::json decomposeGoal(const juce::String& intent);

    const AgentContext* ctx_ = nullptr;
    std::atomic<AgentState> state_{ AgentState::Idle };
};

} // namespace more_phi::agents
