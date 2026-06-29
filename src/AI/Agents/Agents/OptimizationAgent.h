// src/AI/Agents/Agents/OptimizationAgent.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <atomic>

namespace more_phi::agents {

// Drives parameters toward a target metric. Drafts via mastering.plan_preview,
// evaluates N candidates via mastering.render_batch, picks best by error.
// Does NOT apply directly — returns proposedActions for the Conductor to re-dispatch
// (permission-gated) and QualitySafety to verify.
class OptimizationAgent : public IAgent
{
public:
    AgentRole role() const noexcept override { return AgentRole::Optimization; }
    juce::String id() const noexcept override { return "optimization-1"; }
    std::vector<juce::String> allowedTools() const override
    {
        return { "set_parameter", "set_parameters_batch",
                 "set_more_phi_parameter", "set_more_phi_parameters",
                 "mastering.plan_preview", "mastering.render_batch" };
    }
    std::vector<juce::String> subscribedEventTypes() const override
    { return { "analysis.finding", "quality.target_set" }; }

    void prepare(const AgentContext& ctx) override { ctx_ = &ctx; }
    AgentResult execute(const AgentTask& task) override;
    AgentState state() const noexcept override { return state_.load(); }
    void stop() override { state_.store(AgentState::Stopped); }

private:
    const AgentContext* ctx_ = nullptr;
    std::atomic<AgentState> state_{ AgentState::Idle };
};

} // namespace more_phi::agents
