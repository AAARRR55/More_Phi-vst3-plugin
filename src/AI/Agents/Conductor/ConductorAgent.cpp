// src/AI/Agents/Conductor/ConductorAgent.cpp
#include "AI/Agents/Conductor/ConductorAgent.h"

#include "AI/AutomationControlPlane.h"
#include "AI/Agents/Llm/DeterministicFallbackLlmClient.h"

#include <juce_core/juce_core.h>

namespace more_phi::agents {

namespace {
AgentRole roleFromName(const std::string& name)
{
    if (name == "analysis")     return AgentRole::Analysis;
    if (name == "optimization") return AgentRole::Optimization;
    if (name == "creative")     return AgentRole::Creative;
    if (name == "realtime")     return AgentRole::RealtimeControl;
    if (name == "quality")      return AgentRole::QualitySafety;
    if (name == "memory")       return AgentRole::Memory;
    return AgentRole::Custom;
}

TaskPriority followUpPriority(AgentRole r)
{
    return r == AgentRole::RealtimeControl ? TaskPriority::RealtimeCritical
         : r == AgentRole::Optimization    ? TaskPriority::High
         : TaskPriority::Normal;
}
} // namespace

nlohmann::json ConductorAgent::decomposeGoal(const juce::String& intent)
{
    // Try the real LLM if wired; otherwise use the deterministic fallback (Risk R1).
    if (ctx_ && ctx_->llm)
    {
        ILlmClient::CompletionRequest req;
        req.systemPrompt = "You decompose a mastering goal into specialist-agent steps.";
        req.userPrompt = intent;
        auto resp = ctx_->llm->complete(req);
        if (resp.ok && resp.toolCalls.is_array() && ! resp.toolCalls.empty())
        {
            for (const auto& tc : resp.toolCalls)
            {
                if (tc.contains("arguments") && tc["arguments"].contains("steps"))
                    return tc["arguments"];
            }
        }
    }
    return DeterministicFallbackLlmClient::decomposeIntent(intent);
}

AgentResult ConductorAgent::execute(const AgentTask& task)
{
    state_.store(AgentState::Busy);
    AgentResult r;
    r.taskId = task.id;

    // 1. Create an auditable workflow run (reuses WorkflowOrchestrator).
    juce::String runId;
    if (ctx_ && ctx_->runtime)
    {
        auto run = ctx_->runtime->workflows().createRun(task.intent.toStdString(),
            nlohmann::json({ { "origin", task.origin.toStdString() } }));
        runId = run.id;
    }
    r.findings["runId"] = runId.toStdString();

    // 2. Decompose intent into specialist subtasks.
    auto plan = decomposeGoal(task.intent);
    if (! plan.contains("steps") || ! plan["steps"].is_array())
    {
        r.success = false;
        r.errorCode = "decompose_failed";
        state_.store(AgentState::Idle);
        return r;
    }

    std::vector<AgentTask> subtasks;
    for (const auto& step : plan["steps"])
    {
        const auto role = roleFromName(step.value("agent", ""));
        if (role == AgentRole::Custom)
            continue;
        AgentTask sub;
        sub.id = task.id + "-" + step.value("agent", std::string());
        // H4: correlate subtasks back to the originating GOAL task id (task.id) so
        // AgentRuntime can tell when the run is truly complete and rewrite the goal
        // result. (The workflow runId is still captured in findings["runId"] above
        // for auditability; it is not the correlation key.)
        sub.runId = task.id;
        sub.targetRole = role;
        sub.intent = juce::String(step.value("intent", ""));
        sub.priority = followUpPriority(role);
        sub.origin = id();
        if (step.contains("payload"))
            sub.payload = step["payload"];
        subtasks.push_back(sub);
    }

    // 3. Delegate: emit followUps (only Conductor's are honored by the runtime).
    r.followUps = std::move(subtasks);
    r.findings["plan"] = plan;
    r.findings["delegatedCount"] = static_cast<int>(r.followUps.size());
    r.emitEvents.push_back({ "conductor.plan", { { "runId", runId.toStdString() }, { "plan", plan } } });

    // 4. The specialist results arrive asynchronously (via followUps). The conductor
    //    task is "complete" once delegation is issued; a future conductor-with-barrier
    //    would await sub-results. For Phase 2 we mark success = delegation issued.
    r.success = true;
    r.telemetry["decomposedVia"] = plan.value("source", "deterministic-fallback");
    state_.store(AgentState::Idle);
    return r;
}

} // namespace more_phi::agents
