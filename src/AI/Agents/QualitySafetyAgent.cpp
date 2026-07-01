// src/AI/Agents/QualitySafetyAgent.cpp
#include "AI/Agents/QualitySafetyAgent.h"

#include "AI/Agents/BlackboardBridge.h"

namespace more_phi::agents {

QualitySafetyAgent::Verdict QualitySafetyAgent::evaluate(const Proposal& proposal)
{
    Verdict v;
    if (! ctx_ || ! ctx_->tools)
    {
        v.reason = "no_tool_invoker";
        return v;
    }

    // Re-measure the predicted/after state via compare_render.
    auto compared = ctx_->tools->invoke("analysis.compare_render",
        { { "actions", proposal.proposedActions }, { "target", proposal.target } }, id());

    const auto after = compared.value("after", nlohmann::json::object());
    const double lufs = after.value("lufs_integrated", -99.0);
    const double tp   = after.value("true_peak_db", -99.0);
    v.measurements = { { "lufs_integrated", lufs }, { "true_peak_db", tp } };

    // Streaming targets. LUFS aim has a tolerance band (±1 LU is standard streaming
    // practice around the -14 target); only loudness well above the aim is a breach.
    // True-peak is a hard ceiling.
    if (lufs > config_.maxLufs + 1.0)
    {
        v.reason = "lufs_breach";
        return v;
    }
    if (tp > config_.maxTruePeakDb + 0.05)
    {
        v.reason = "true_peak_breach";
        return v;
    }

    v.approved = true;
    v.reason = "within_targets";
    return v;
}

void QualitySafetyAgent::onEvent(const juce::String& type,
                                 const nlohmann::json& payload,
                                 const juce::String& /*source*/,
                                 const juce::String& runId)
{
    if (type == "optimization.proposal" || type == "creative.suggestion")
    {
        Proposal p;
        p.runId = runId;
        if (payload.contains("actions") && !payload["actions"].is_null())
            p.proposedActions = payload["actions"];

        if (p.proposedActions.empty())
        {
            if (ctx_ && ctx_->blackboard)
                ctx_->blackboard->publish(id(), "quality.no_actions",
                    { { "type", type.toStdString() }, { "note", "proposal had no actions to evaluate" } }, runId);
            return;
        }

        auto verdict = evaluate(p);
        if (ctx_ && ctx_->blackboard)
        {
            ctx_->blackboard->publish(id(), "quality.verdict",
                { { "approved", verdict.approved }, { "reason", verdict.reason.toStdString() },
                  { "type", type.toStdString() } }, runId);
            if (verdict.approved)
                ctx_->blackboard->publish(id(), "quality.target_set",
                    { { "from", type.toStdString() } }, runId);
        }
    }
    // realtime.correction_applied is observed silently here (semantic watchdog);
    // a budget breach surfaces as realtime.correction_skipped, which the realtime
    // agent emits directly.
}

AgentResult QualitySafetyAgent::execute(const AgentTask& task)
{
    state_.store(AgentState::Busy);
    AgentResult r;
    r.taskId = task.id;
    r.success = true;
    r.findings = { { "note", "reactive gatekeeper; primary entry is onEvent" } };
    state_.store(AgentState::Idle);
    return r;
}

} // namespace more_phi::agents
