// src/AI/Agents/Agents/QualitySafetyAgent.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <atomic>
#include <vector>

namespace more_phi::agents {

// Semantic gatekeeper. Composes with (does not replace) the mechanical PermissionKernel.
class QualitySafetyAgent : public IAgent
{
public:
    struct Config
    {
        double maxLufs = -14.0;
        double maxTruePeakDb = -1.0;
    };

    struct Proposal
    {
        std::vector<nlohmann::json> proposedActions;
        nlohmann::json target = nlohmann::json::object();
        juce::String runId;
    };
    struct Verdict
    {
        bool approved = false;
        juce::String reason;
        nlohmann::json measurements = nlohmann::json::object();
    };

    AgentRole role() const noexcept override { return AgentRole::QualitySafety; }
    juce::String id() const noexcept override { return "quality-1"; }
    std::vector<juce::String> allowedTools() const override
    {
        return { "analysis.compare_render", "get_mastering_state",
                 "audit_plugin_profile", "restore_safe_plugin_snapshot" };
    }
    std::vector<juce::String> subscribedEventTypes() const override
    { return { "optimization.proposal", "creative.suggestion", "realtime.correction_applied" }; }

    void setConfig(Config c) { config_ = c; }
    void prepare(const AgentContext& ctx) override { ctx_ = &ctx; }

    Verdict evaluate(const Proposal& proposal);

    void onEvent(const juce::String& type,
                 const nlohmann::json& payload,
                 const juce::String& source,
                 const juce::String& runId) override;

    AgentResult execute(const AgentTask& task) override;
    AgentState state() const noexcept override { return state_.load(); }
    void stop() override { state_.store(AgentState::Stopped); }

private:
    const AgentContext* ctx_ = nullptr;
    Config config_;
    std::atomic<AgentState> state_{ AgentState::Idle };
};

} // namespace more_phi::agents
