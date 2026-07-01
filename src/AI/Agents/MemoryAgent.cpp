// src/AI/Agents/MemoryAgent.cpp
#include "AI/Agents/MemoryAgent.h"

#include "AI/AutomationControlPlane.h"

namespace more_phi::agents {

AgentResult MemoryAgent::execute(const AgentTask& task)
{
    state_.store(AgentState::Busy);
    AgentResult r;
    r.taskId = task.id;

    if (! ctx_ || ! ctx_->runtime)
    {
        r.success = false;
        r.errorCode = "no_runtime";
        state_.store(AgentState::Idle);
        return r;
    }

    const auto runId = task.payload.value("runId", std::string());
    const bool ok    = task.payload.value("success", false);
    const double score = task.payload.value("score", 0.0);

    if (! runId.empty())
    {
        // Record exactly ONE workflow-level outcome (transaction-level outcomes are
        // already recorded by dispatchWithAutomationTransaction; we do not duplicate).
        more_phi::ActionOutcome outcome;
        outcome.workflowRunId = juce::String(runId);
        outcome.userAccepted = ok;
        outcome.outcomeScore = static_cast<float>(score);
        outcome.source = id().toStdString();
        outcome.feedbackStatus = ok ? "accepted" : "rejected";
        ctx_->runtime->memory().recordOutcome(outcome);
    }

    // Surface relevant priors.
    nlohmann::json sessionCtx = nlohmann::json::object();
    auto recalled = ctx_->runtime->memory().intentContext(sessionCtx, 10);
    r.findings = { { "intentContext", recalled } };
    r.emitEvents.push_back({ "memory.recall_ready", { { "count", recalled.size() } } });
    r.success = true;

    state_.store(AgentState::Idle);
    return r;
}

void MemoryAgent::onEvent(const juce::String& type,
                          const nlohmann::json& payload,
                          const juce::String& /*source*/,
                          const juce::String& runId)
{
    if (type == "conductor.complete" && ctx_ && ctx_->runtime)
    {
        more_phi::ActionOutcome outcome;
        outcome.workflowRunId = runId;
        outcome.userAccepted = payload.value("success", false);
        outcome.outcomeScore = static_cast<float>(payload.value("score", 0.0));
        outcome.source = "memory-on-conductor-complete";
        outcome.feedbackStatus = outcome.userAccepted ? "accepted" : "rejected";
        ctx_->runtime->memory().recordOutcome(outcome);
    }
}

} // namespace more_phi::agents
