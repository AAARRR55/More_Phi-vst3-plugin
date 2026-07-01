// src/AI/Agents/RealtimeControlAgent.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace more_phi::agents {

// Reactive output-gain trimmer. Corrections are invoked SYNCHRONOUSLY on the
// blackboard pump thread via ctx_->tools->invoke("more_phi.set_parameter", ...)
// -> DefaultToolInvoker -> MCPToolHandler::handle -> dispatchWithAutomationTransaction
// -> enqueueParameterSet onto the existing LockFreeQueue<ParamCommand> (the same
// chokepoint MCP uses). The delivery path is HEADLESS-SAFE: no
// MessageManager::callAsync is on it (callAsync drops in headless/offline-render
// hosts — see the .cpp M5 note and CLAUDE.md Thread-Communication). The pump
// thread is joined in AgentRuntime::stop() BEFORE the registry destroys agents,
// so there is no deferred callback to outlive `this`.
// "RealtimeCritical" priority means "jump the AGENT queue", NOT "run on the audio
// thread" (Decision D2). Reactive latency is bounded by the AnalysisAgent cadence
// (~100ms) + the blackboard pump interval + the synchronous transaction cost —
// ~100-200ms worst case under a clip storm (the pump runs the full transaction
// synchronously), NOT sub-100ms. The load-bearing guarantee is D2 (queue
// priority, never the audio thread), not a latency target. Corrections target
// the More-Phi OUTPUT GAIN parameter by name (resolved safely on the message
// thread), NOT a hardcoded hosted-plugin index (audit C3: index 0 was an
// arbitrary/unrelated control). Correction values are ABSOLUTE normalized targets
// derived from a tracked current-gain estimate — set_parameter semantics are
// absolute, so the previous code that passed a -delta dB as "value" was also wrong.
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
    void prepare(const AgentContext& ctx) override
    {
        ctx_ = &ctx;
    }

    // Driven by the blackboard pump.
    void onEvent(const juce::String& type,
                 const nlohmann::json& payload,
                 const juce::String& source,
                 const juce::String& runId) override;

    // Sync execute (for direct task submission).
    AgentResult execute(const AgentTask& task) override;
    AgentState state() const noexcept override { return state_.load(); }
    // Clears alive_ defensively. Today's delivery path is synchronous (see the
    // class comment — onEvent invokes the tool on the blackboard pump thread, not
    // via MessageManager::callAsync), so nothing is deferred; alive_ is checked
    // before the tool invoke as a guard against any future caller that defers
    // this path. The runtime calls stop() on every agent during AgentRuntime::stop
    // (workers and the pump are joined first), so this runs before the agent is
    // destroyed.
    void stop() override { alive_->store(false, std::memory_order_release); state_.store(AgentState::Stopped); }

private:
    bool consumeRateAndBudgetLocked(const juce::String& runId);

    const AgentContext* ctx_ = nullptr;
    Config config_;
    std::atomic<AgentState> state_{ AgentState::Idle };
    // C-4 / M5: shared liveness flag. The delivery path is synchronous (class
    // comment), so onEvent checks alive_ directly before invoking the tool; the
    // flag is captured by value anywhere a path could be deferred, and cleared in
    // stop() so a (hypothetical) deferred caller can detect the agent is gone
    // without capturing `this`. The DefaultToolInvoker the tool resolves through
    // is owned independently by the Processor's agentTools_ member, so it outlives
    // this agent regardless.
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

    struct RateBucket { juce::int64 windowStartMs = 0; int count = 0; };
    mutable std::mutex mutex_;
    std::unordered_map<std::string, RateBucket> rateBuckets_;
    std::unordered_map<std::string, int> runBudgets_;
    // Tracked estimate of the current output gain in dB, so a -delta can be turned
    // into an absolute set_parameter target. Seeded to 0 dB; updated after each
    // successful correction (synchronously in onEvent, under mutex_). M1: buckets
    // pruned when windows expire.
    float currentOutputGainDb_ = 0.0f;
};

} // namespace more_phi::agents
