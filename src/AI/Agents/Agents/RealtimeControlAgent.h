// src/AI/Agents/Agents/RealtimeControlAgent.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace more_phi::agents {

// Reactive corrections via the existing lock-free queue / APVTS path.
// "RealtimeCritical" priority means "jump the AGENT queue", NOT "run on the audio
// thread" (Decision D2). Corrections target the More-Phi OUTPUT GAIN parameter
// by name (resolved safely on the message thread by more_phi.set_parameter), NOT
// a hardcoded hosted-plugin index (audit C3: index 0 was an arbitrary/unrelated
// control). Correction values are ABSOLUTE normalized targets derived from a
// tracked current-gain estimate — set_parameter semantics are absolute, so the
// previous code that passed a -delta dB as "value" was also wrong.
class RealtimeControlAgent : public IAgent
{
public:
    struct Config
    {
        int   maxCorrectionsPerParamPerSecond = 4;   // anti-oscillation
        int   correctionBudgetPerRun = 16;           // hard cap before QualitySafety veto
        float clipTrimStepDb = 1.5f;                 // how far to trim per clip event
        // C3: which More-Phi parameter to trim. Default is the plugin's own output
        // gain, resolved by name via the tool layer. Empty name disables correction.
        juce::String outputGainParamName = "outputGain";
        // Normalized value (0..1) below which we refuse to trim further — prevents
        // running output gain into the floor indefinitely on sustained clipping.
        float outputGainFloorNormalized = 0.05f;
    };

    AgentRole role() const noexcept override { return AgentRole::RealtimeControl; }
    juce::String id() const noexcept override { return "realtime-1"; }
    std::vector<juce::String> allowedTools() const override
    {
        // more_phi.set_parameter resolves by name on the message thread (safe);
        // set_parameter/hosted_plugin.set_parameter target the hosted plugin by
        // index, which we deliberately do NOT use for reactive trim (C3).
        return { "more_phi.set_parameter" };
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
    bool consumeRateAndBudgetLocked(const juce::String& runId);

    const AgentContext* ctx_ = nullptr;
    Config config_;
    std::atomic<AgentState> state_{ AgentState::Idle };

    struct RateBucket { juce::int64 windowStartMs = 0; int count = 0; };
    mutable std::mutex mutex_;
    std::unordered_map<std::string, RateBucket> rateBuckets_;
    std::unordered_map<std::string, int> runBudgets_;
    // Tracked estimate of the current output gain in dB, so a -delta can be turned
    // into an absolute set_parameter target. Seeded to 0 dB; updated after each
    // successful correction. M1: buckets pruned when windows expire.
    float currentOutputGainDb_ = 0.0f;
};

} // namespace more_phi::agents
