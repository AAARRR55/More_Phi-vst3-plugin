// src/AI/Agents/CreativeAgent.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <atomic>

namespace more_phi::agents {

// Artistic suggestions. Never auto-applied (requireApprovalRegardlessOfAutonomy).
class CreativeAgent : public IAgent
{
public:
    AgentRole role() const noexcept override { return AgentRole::Creative; }
    juce::String id() const noexcept override { return "creative-1"; }
    std::vector<juce::String> allowedTools() const override
    {
        return { "suggest_intermediate_snapshots", "find_related_parameters",
                 "suggest_morph_settings", "capture_snapshot" };
    }
    std::vector<juce::String> subscribedEventTypes() const override
    { return { "analysis.finding", "optimization.proposal" }; }
    bool requireApprovalRegardlessOfAutonomy() const override { return true; }

    void prepare(const AgentContext& ctx) override { ctx_ = &ctx; }
    AgentResult execute(const AgentTask& task) override;
    AgentState state() const noexcept override { return state_.load(); }
    void stop() override { state_.store(AgentState::Stopped); }

private:
    const AgentContext* ctx_ = nullptr;
    std::atomic<AgentState> state_{ AgentState::Idle };
};

} // namespace more_phi::agents
