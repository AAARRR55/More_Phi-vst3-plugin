// src/AI/Agents/OptimizationAgent.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <atomic>

namespace more_phi::agents {

// Drives parameters toward a target metric. Drafts via mastering.plan_preview,
// evaluates N candidates via mastering.render_batch, picks best by error.
// Does NOT apply directly — returns proposedActions for the Conductor to re-dispatch
// (permission-gated) and QualitySafety to verify.
//
// O1 (2026-06-29): neural-first path. When the task is a mastering goal,
// execute() prefers mastering.neural_apply (one-shot analyze+apply via the embedded
// ONNX model) over the heuristic batch path. Falls back to the heuristic path only
// when the neural model is unavailable or the apply is a no-op.
class OptimizationAgent : public IAgent
{
public:
    AgentRole role() const noexcept override { return AgentRole::Optimization; }
    juce::String id() const noexcept override { return "optimization-1"; }
    std::vector<juce::String> allowedTools() const override
    {
        return { "set_parameter", "set_parameters_batch",
                 "set_more_phi_parameter", "set_more_phi_parameters",
                 "mastering.plan_preview", "mastering.render_batch",
                 // O1: neural mastering path — one-shot analyze+apply + dry-run preview.
                 "mastering.neural_apply", "sonicmaster_decision" };
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
