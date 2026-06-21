// src/AI/Agents/Agents/RealtimeControlAgent.cpp
#include "AI/Agents/Agents/RealtimeControlAgent.h"

#include "AI/Agents/Blackboard/BlackboardBridge.h"

#include <juce_core/juce_core.h>

namespace more_phi::agents {

namespace {
juce::int64 nowMs() noexcept { return juce::Time::currentTimeMillis(); }
} // namespace

void RealtimeControlAgent::setConfig(Config c) { config_ = c; }

bool RealtimeControlAgent::consumeRateAndBudgetLocked(const juce::String& runId, int paramIndex)
{
    const auto t = nowMs();
    const auto key = runId.toStdString() + ":" + std::to_string(paramIndex);
    auto& bucket = rateBuckets_[key];
    if (bucket.windowStartMs == 0 || (t - bucket.windowStartMs) >= 1000)
    {
        bucket.windowStartMs = t;
        bucket.count = 0;
    }
    if (bucket.count >= config_.maxCorrectionsPerParamPerSecond)
        return false;
    int& budget = runBudgets_[runId.toStdString()];
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

    const int param = config_.outputGainParamIndex;
    bool allowed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        allowed = consumeRateAndBudgetLocked(runId, param);
    }
    if (! allowed)
    {
        // Rate/budget exhausted: announce the back-off for QualitySafety to observe.
        if (ctx_->blackboard)
            ctx_->blackboard->publish(id(), "realtime.correction_skipped",
                { { "reason", "rate_or_budget" }, { "type", type.toStdString() } }, runId);
        return;
    }

    // Compute a small corrective trim. Negative = output down.
    const float stepDb = -config_.clipTrimStepDb;
    auto result = ctx_->tools->invoke("set_parameter",
        { { "index", param }, { "value", stepDb } }, id());

    if (ctx_->blackboard)
        ctx_->blackboard->publish(id(), "realtime.correction_applied",
            { { "param", param }, { "delta_db", stepDb },
              { "type", type.toStdString() }, { "result", result } }, runId);
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
