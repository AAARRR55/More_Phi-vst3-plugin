// src/AI/Agents/Agents/RealtimeControlAgent.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace more_phi::agents {

// Reactive corrections via the existing lock-free queue. "RealtimeCritical"
// priority means "jump the AGENT queue", NOT "run on the audio thread" (Decision D2).
class RealtimeControlAgent : public IAgent
{
public:
    struct Config
    {
        int maxCorrectionsPerParamPerSecond = 4;   // anti-oscillation
        int correctionBudgetPerRun = 16;           // hard cap before QualitySafety veto
        float clipTrimStepDb = 1.5f;               // how far to trim per clip event
        int outputGainParamIndex = 0;              // tunable; real wiring sets this
    };

    AgentRole role() const noexcept override { return AgentRole::RealtimeControl; }
    juce::String id() const noexcept override { return "realtime-1"; }
    std::vector<juce::String> allowedTools() const override
    {
        return { "set_parameter", "set_morph_position", "more_phi.set_parameter" };
    }
    std::vector<juce::String> subscribedEventTypes() const override
    { return { "analysis.clipping_detected", "analysis.lufs_breach" }; }

    void setConfig(Config c);
    void prepare(const AgentContext& ctx) override { ctx_ = &ctx; }

    // Driven by the blackboard pump.
    void onEvent(const juce::String& type,
                 const nlohmann::json& payload,
                 const juce::String& source,
                 const juce::String& runId) override;

    // Sync execute (for direct task submission).
    AgentResult execute(const AgentTask& task) override;
    AgentState state() const noexcept override { return state_.load(); }
    void stop() override { state_.store(AgentState::Stopped); }

private:
    bool consumeRateAndBudgetLocked(const juce::String& runId, int paramIndex);

    const AgentContext* ctx_ = nullptr;
    Config config_;
    std::atomic<AgentState> state_{ AgentState::Idle };

    struct RateBucket { juce::int64 windowStartMs = 0; int count = 0; };
    mutable std::mutex mutex_;
    std::unordered_map<std::string, RateBucket> rateBuckets_;
    std::unordered_map<std::string, int> runBudgets_;
};

} // namespace more_phi::agents
