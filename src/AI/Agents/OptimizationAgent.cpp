// src/AI/Agents/OptimizationAgent.cpp
#include "AI/Agents/OptimizationAgent.h"

#include <algorithm>
#include <limits>

namespace more_phi::agents {

namespace {
// O1 (2026-06-29): heuristic to detect a mastering-oriented task so the neural
// path is preferred. The Conductor decomposes a mastering goal into specialist
// steps whose intent/payload carry mastering signals. Conservative: only treat
// clearly mastering-related tasks as neural-eligible; everything else keeps the
// heuristic batch path (which is the right tool for non-mastering optimization).
bool isMasteringTask(const AgentTask& task)
{
    const auto intent = task.intent.toLowerCase().toStdString();
    if (intent.find("master") != std::string::npos
        || intent.find("loudness") != std::string::npos
        || intent.find("lufs") != std::string::npos
        || intent.find("streaming") != std::string::npos
        || intent.find("eq ") != std::string::npos
        || intent.find("polish") != std::string::npos)
        return true;
    // Payload signals: a target_lufs or explicit neural flag.
    if (task.payload.contains("target_lufs"))
        return true;
    if (task.payload.value("neural", false))
        return true;
    return false;
}
} // namespace

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

    // O1 (2026-06-29): NEURAL-FIRST PATH. For mastering tasks, prefer the embedded
    // ONNX model's one-shot analyze+apply over the heuristic batch. This mirrors the
    // LLM-assistant fix — the agent layer previously could not reach the neural
    // mastering path at all (mastering.neural_apply was absent from allowedTools),
    // so every mastering goal resolved to a heuristic draft. Falls back to the
    // heuristic path below when the neural model is unavailable or the apply is a
    // no-op (no hosted plugin / unmapped / model not loaded).
    if (isMasteringTask(task))
    {
        nlohmann::json neuralParams = { { "target_lufs", task.payload.value("target_lufs", -14.0) } };
        if (task.payload.contains("apply_limiter_ceiling"))
            neuralParams["apply_limiter_ceiling"] = task.payload["apply_limiter_ceiling"];

        auto neural = ctx_->tools->invoke("mastering.neural_apply", neuralParams, id());
        const bool neuralAvailable = neural.value("available", false);
        const bool neuralApplied   = neural.value("applied", false);

        if (neuralAvailable && neuralApplied)
        {
            // The neural apply wrote the hosted plugin directly; there is nothing
            // for the Conductor to re-dispatch, so proposedActions stays empty.
            r.findings = {
                { "path", "neural" },
                { "applied", true },
                { "mapping_status", neural.value("mapping_status", nlohmann::json::object()) },
                { "live_measurements", neural.value("live_measurements", nlohmann::json::object()) }
            };
            r.telemetry["decomposedVia"] = "neural-apply";
            r.emitEvents.push_back({ "optimization.proposal", {
                { "path", "neural" },
                { "applied", true },
                { "proposedActionCount", 0 }
            }});
            r.success = true;
            state_.store(AgentState::Idle);
            return r;
        }
        // Fall through to heuristic: neural model unavailable OR apply was a no-op
        // (no hosted plugin / unmapped). Log the fallback reason in findings.
        r.findings["neural_fallback_reason"] = neural.value("state",
            neuralAvailable ? "apply_no_op" : "model_unavailable");
    }

    // HEURISTIC PATH (original logic preserved).
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

    r.findings["path"] = "heuristic";
    r.findings["draft"] = draft;
    r.findings["evaluatedCount"] = batch.contains("candidates") ? static_cast<int>(batch["candidates"].size()) : 0;
    r.findings["bestCandidateId"] = bestIdx;
    r.findings["bestError"] = bestError;

    // 4. Return the chosen parameter delta as a proposedAction for the Conductor to re-dispatch.
    if (! bestParams.empty())
    {
        nlohmann::json action = {
            { "tool", "set_parameters_batch" },
            { "params", { { "values", bestParams } } }
        };
        r.proposedActions.push_back(action);
    }

    nlohmann::json actionsJson = nlohmann::json::array();
    for (const auto& a : r.proposedActions)
        actionsJson.push_back(a);
    r.emitEvents.push_back({ "optimization.proposal", {
        { "path", "heuristic" },
        { "bestCandidateId", bestIdx }, { "bestError", bestError },
        { "proposedActionCount", static_cast<int>(r.proposedActions.size()) },
        { "actions", actionsJson }
    }});

    r.success = true;
    r.telemetry["decomposedVia"] = "heuristic-batch";
    state_.store(AgentState::Idle);
    return r;
}

} // namespace more_phi::agents
