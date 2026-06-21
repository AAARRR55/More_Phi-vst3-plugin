// src/AI/Agents/Agents/CreativeAgent.cpp
#include "AI/Agents/Agents/CreativeAgent.h"

namespace more_phi::agents {

AgentResult CreativeAgent::execute(const AgentTask& task)
{
    state_.store(AgentState::Busy);
    AgentResult r;
    r.taskId = task.id;

    if (! ctx_ || ! ctx_->tools)
    {
        r.success = false;
        r.errorCode = "no_tool_invoker";
        state_.store(AgentState::Idle);
        return r;
    }

    // Advisory only. We do not propose write actions (enforced structurally by
    // not populating proposedActions; QualitySafety would gate them anyway).
    auto related = ctx_->tools->invoke("find_related_parameters",
        { { "intent", task.intent.toStdString() } }, id());
    auto hybrids = ctx_->tools->invoke("suggest_intermediate_snapshots", {}, id());

    r.findings = { { "related", related }, { "hybrids", hybrids } };
    r.emitEvents.push_back({ "creative.suggestion", {
        { "intent", task.intent.toStdString() },
        { "advisory", true }
    }});
    r.success = true;

    state_.store(AgentState::Idle);
    return r;
}

} // namespace more_phi::agents
