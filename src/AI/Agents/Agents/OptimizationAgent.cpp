// src/AI/Agents/Agents/OptimizationAgent.cpp
#include "AI/Agents/Agents/OptimizationAgent.h"

#include <algorithm>
#include <limits>

namespace more_phi::agents {

AgentResult OptimizationAgent::execute(const AgentTask& task)
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

    const auto target = task.payload.value("target", nlohmann::json::object());

    // 1. Draft a plan.
    auto draft = ctx_->tools->invoke("mastering.plan_preview",
        { { "target", target } }, id());

    // 2. Evaluate a batch of candidates.
    const int batchSize = 4;
    auto batch = ctx_->tools->invoke("mastering.render_batch",
        { { "target", target }, { "count", batchSize }, { "dry_run", true } }, id());

    // 3. Pick the candidate with the lowest lufs_error.
    int bestIdx = -1;
    double bestError = std::numeric_limits<double>::infinity();
    nlohmann::json bestParams = nlohmann::json::object();
    if (batch.contains("candidates") && batch["candidates"].is_array())
    {
        for (const auto& c : batch["candidates"])
        {
            const double err = c.value("lufs_error", std::numeric_limits<double>::infinity());
            if (err < bestError)
            {
                bestError = err;
                bestIdx = c.value("id", -1);
                bestParams = c.value("params", nlohmann::json::object());
            }
        }
    }

    r.findings = {
        { "draft", draft },
        { "evaluatedCount", batch.contains("candidates") ? static_cast<int>(batch["candidates"].size()) : 0 },
        { "bestCandidateId", bestIdx },
        { "bestError", bestError }
    };

    // 4. Return the chosen parameter delta as a proposedAction for the Conductor to re-dispatch.
    if (! bestParams.empty())
    {
        nlohmann::json action = {
            { "tool", "set_parameters_batch" },
            { "params", { { "values", bestParams } } }
        };
        r.proposedActions.push_back(action);
    }

    r.emitEvents.push_back({ "optimization.proposal", {
        { "bestCandidateId", bestIdx }, { "bestError", bestError },
        { "proposedActionCount", static_cast<int>(r.proposedActions.size()) }
    }});

    r.success = true;
    state_.store(AgentState::Idle);
    return r;
}

} // namespace more_phi::agents
