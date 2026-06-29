// src/AI/Agents/Agents/AnalysisAgent.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <atomic>

namespace more_phi::agents {

// Read-only measurement & diagnosis. Never mutates. The system's eyes.
class AnalysisAgent : public IAgent
{
public:
    AgentRole role() const noexcept override { return AgentRole::Analysis; }
    juce::String id() const noexcept override { return "analysis-1"; }
    std::vector<juce::String> allowedTools() const override
    {
        return { "analysis.get_summary", "analysis.get_spectrum", "analysis.get_stereo_field",
                 "ozone.track.analyze", "get_mastering_state",
                 "analysis.capture_window", "analysis.compare_render",
                 // O1 (2026-06-29): dry-run neural mastering preview so the analysis
                 // agent can surface a model decision alongside live measurements.
                 "sonicmaster_decision" };
    }
    std::vector<juce::String> subscribedEventTypes() const override { return { "audio.transport_changed" }; }

    void prepare(const AgentContext& ctx) override { ctx_ = &ctx; }
    AgentResult execute(const AgentTask& task) override;
    AgentState state() const noexcept override { return state_.load(); }
    void stop() override { state_.store(AgentState::Stopped); }

private:
    const AgentContext* ctx_ = nullptr;
    std::atomic<AgentState> state_{ AgentState::Idle };
};

} // namespace more_phi::agents
