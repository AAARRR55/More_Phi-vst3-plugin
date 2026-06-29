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
    // H-2 FIX: Match the actual APVTS NormalisableRange for the output gain
    // parameter (declared as [-24, +24] in PluginProcessor.cpp:1460).
    // Previously was [-24, +12] — the upper bound was too narrow, causing a
    // minor skew when the agent's internal dB estimate was converted back
    // through the APVTS. For the agent's trim-down-only use case this doesn't
    // affect correctness (the APVTS clamps and the tool handler resolves the
    // true range), but matching the real range avoids confusion.
    constexpr float lo = -24.0f, hi = 24.0f;
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
    // more_phi.set_parameter resolves by parameter_id; the value is ABSOLUTE
    // (not a -delta). Synchronous on the blackboard pump thread.
    //
    // C-4 FIX: use weakTools_ (weak_ptr to the raw tools pointer) so invoke()
    // gracefully returns when teardown has freed the DefaultToolInvoker.
    // M5 NOTE: invoked synchronously here, NOT via MessageManager::callAsync.
    // The pump thread is joined in AgentRuntime::stop() BEFORE the registry
    // destroys agents, so there is no deferred callback to outlive `this` —
    // the original use-after-free window only existed with callAsync. Keeping
    // it synchronous also matches the contract the unit tests assert and avoids
    // a MessageManager dependency in the agent layer. `alive_` is still checked
    // defensively in case a future caller ever defers this path.
    if (! alive_->load(std::memory_order_acquire))
        return;
    auto tools = weakTools_.lock();
    if (! tools)
        return;

    const juce::String paramName = config_.outputGainParamName;
    auto result = tools->invoke("more_phi.set_parameter",
        { { "parameter_id", paramName.toStdString() },
          { "value", targetNormalized } }, id());

    const bool applied = (! result.contains("error"));

    if (applied)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        currentOutputGainDb_ = newGainDb;   // commit our tracked estimate
    }

    if (ctx_->blackboard)
        ctx_->blackboard->publish(id(), "realtime.correction_applied",
            { { "param", paramName.toStdString() },
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
