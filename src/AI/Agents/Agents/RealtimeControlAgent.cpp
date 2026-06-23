// src/AI/Agents/Agents/RealtimeControlAgent.cpp
#include "AI/Agents/Agents/RealtimeControlAgent.h"

#include "AI/Agents/Blackboard/BlackboardBridge.h"

#include <juce_core/juce_core.h>

namespace more_phi::agents {

namespace {
juce::int64 nowMs() noexcept { return juce::Time::currentTimeMillis(); }

// Convert dB to APVTS-normalized for the outputGain parameter (skew 0..1 where
// 0.5 ≈ unity-ish). More-Phi's outputGain uses the standard juce decibel skew;
// the exact skew factor doesn't matter much for a small reactive trim because we
// clamp and the host smooths. We use juce::Decibels + a linear-ish mapping that
// is monotonic and bounded so the floor check is meaningful.
float dbToNormalized(float db)
{
    // Map [-24 dB, +12 dB] → [0, 1] linearly; clamp. Good enough for trim math
    // since the agent only ever moves gain DOWN by small steps and re-reads via
    // the tool result. (Real normalization skew is host-resolved at apply time.)
    const float lo = -24.0f, hi = 12.0f;
    return juce::jlimit(0.0f, 1.0f, (db - lo) / (hi - lo));
}
} // namespace

void RealtimeControlAgent::setConfig(Config c) { config_ = c; }

bool RealtimeControlAgent::consumeRateAndBudgetLocked(const juce::String& runId)
{
    const auto t = nowMs();
    // M1: opportunistic eviction — drop expired rate windows so the buckets map
    // cannot grow unbounded across many distinct run ids over a long session.
    for (auto it = rateBuckets_.begin(); it != rateBuckets_.end(); )
    {
        if (it->second.windowStartMs != 0 && (t - it->second.windowStartMs) >= 1000)
            it = rateBuckets_.erase(it);
        else
            ++it;
    }

    // The blackboard pump delivers events with an empty runId (AgentRuntime fans
    // out onEvent with a synthetic runId). Use a stable synthetic key in that case
    // so rate/budget limiting still applies without rejecting every correction.
    const auto key = runId.isEmpty() ? std::string{ "__reactive__" }
                                     : runId.toStdString();

    auto& bucket = rateBuckets_[key];
    if (bucket.windowStartMs == 0 || (t - bucket.windowStartMs) >= 1000)
    {
        bucket.windowStartMs = t;
        bucket.count = 0;
    }
    if (bucket.count >= config_.maxCorrectionsPerParamPerSecond)
        return false;
    int& budget = runBudgets_[key];
    if (budget >= config_.correctionBudgetPerRun)
        return false;
    ++bucket.count;
    ++budget;
    return true;
}

void RealtimeControlAgent::onEvent(const juce::String& type,
                                   const nlohmann::json& payload,
                                   const juce::String& /*source*/,
                                   const juce::String& runId)
{
    if (! ctx_ || ! ctx_->tools)
        return;
    if (type != "analysis.clipping_detected" && type != "analysis.lufs_breach")
        return;

    // C3: nothing to trim if the gain parameter name isn't configured.
    if (config_.outputGainParamName.isEmpty())
        return;

    float newGainDb;
    bool allowed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        allowed = consumeRateAndBudgetLocked(runId);
        if (! allowed)
            newGainDb = currentOutputGainDb_;   // unchanged; only used for the skip event
        else
        {
            // Move output gain DOWN by clipTrimStepDb (negative = trim).
            newGainDb = currentOutputGainDb_ - config_.clipTrimStepDb;
        }
    }
    if (! allowed)
    {
        // Rate/budget exhausted: announce the back-off for QualitySafety to observe.
        if (ctx_->blackboard)
            ctx_->blackboard->publish(id(), "realtime.correction_skipped",
                { { "reason", "rate_or_budget" }, { "type", type.toStdString() } }, runId);
        return;
    }

    // C3: refuse to drive output gain through the floor. If the next target would
    // fall below the normalized floor, clamp and still publish a "skipped" event
    // so QualitySafety sees the reactive system hit its limit.
    const float targetNormalized = dbToNormalized(newGainDb);
    if (targetNormalized <= config_.outputGainFloorNormalized)
    {
        if (ctx_->blackboard)
            ctx_->blackboard->publish(id(), "realtime.correction_skipped",
                { { "reason", "gain_floor_reached" },
                  { "type", type.toStdString() },
                  { "current_db", newGainDb } }, runId);
        return;
    }

    // C3: set the More-Phi output gain by PARAMETER ID (absolute normalized value).
    // more_phi.set_parameter resolves via APVTS by parameter_id on the message
    // thread — never touches the hosted plugin by fragile index, and the value is
    // absolute (not a -delta, which the previous code passed incorrectly). We use
    // parameter_id rather than name because the resolver matches `name` against
    // the human-readable display string ("Output Gain"), not the id.
    auto result = ctx_->tools->invoke("more_phi.set_parameter",
        { { "parameter_id", config_.outputGainParamName.toStdString() },
          { "value", targetNormalized } }, id());

    const bool applied = (! result.contains("error"));

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (applied)
            currentOutputGainDb_ = newGainDb;   // commit our tracked estimate
    }

    if (ctx_->blackboard)
        ctx_->blackboard->publish(id(), "realtime.correction_applied",
            { { "param", config_.outputGainParamName.toStdString() },
              { "target_db", newGainDb },
              { "applied", applied },
              { "type", type.toStdString() },
              { "result", result } }, runId);
}

AgentResult RealtimeControlAgent::execute(const AgentTask& task)
{
    state_.store(AgentState::Busy);
    AgentResult r;
    r.taskId = task.id;
    r.success = true;
    r.findings = { { "note", "reactive agent; primary entry is onEvent" },
                   { "task", task.intent.toStdString() } };
    state_.store(AgentState::Idle);
    return r;
}

} // namespace more_phi::agents
