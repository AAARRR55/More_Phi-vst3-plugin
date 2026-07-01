// src/AI/Agents/MemoryAgent.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <atomic>

namespace more_phi::agents {

// Workflow-level outcome memory. Does NOT duplicate the transaction-level
// outcomes that dispatchWithAutomationTransaction already records — writes only
// the higher-level workflow outcome tying transactions to intent + feedback.
class MemoryAgent : public IAgent
{
public:
    AgentRole role() const noexcept override { return AgentRole::Memory; }
    juce::String id() const noexcept override { return "memory-1"; }
    std::vector<juce::String> allowedTools() const override
    {
        return { "memory.record_outcome", "memory.update_outcome_feedback",
                 "memory.search", "get_usage_stats", "learn_from_adjustment" };
    }
    std::vector<juce::String> subscribedEventTypes() const override
    { return { "conductor.complete", "quality.verdict" }; }

    void prepare(const AgentContext& ctx) override { ctx_ = &ctx; }
    AgentResult execute(const AgentTask& task) override;

    void onEvent(const juce::String& type,
                 const nlohmann::json& payload,
                 const juce::String& source,
                 const juce::String& runId) override;

    AgentState state() const noexcept override { return state_.load(); }
    void stop() override { state_.store(AgentState::Stopped); }

private:
    const AgentContext* ctx_ = nullptr;
    std::atomic<AgentState> state_{ AgentState::Idle };
};

} // namespace more_phi::agents
