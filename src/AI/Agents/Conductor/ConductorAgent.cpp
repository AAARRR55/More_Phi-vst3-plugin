// src/AI/Agents/Conductor/ConductorAgent.cpp
#include "AI/Agents/Conductor/ConductorAgent.h"

#include "AI/AutomationControlPlane.h"
#include "AI/Agents/Blackboard/BlackboardBridge.h"
#include "AI/Agents/Llm/DeterministicFallbackLlmClient.h"

#include <juce_core/juce_core.h>

#include <future>

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

// Build follow-up tasks from the decomposed plan.
std::vector<AgentTask> buildSubtasks(const AgentTask& original, const nlohmann::json& plan)
{
    std::vector<AgentTask> subtasks;
    for (const auto& step : plan["steps"])
    {
        const auto role = roleFromName(step.value("agent", ""));
        if (role == AgentRole::Custom)
            continue;
        AgentTask sub;
        sub.id = original.id + "-" + step.value("agent", std::string());
        sub.runId = original.id;
        sub.targetRole = role;
        sub.intent = juce::String(step.value("intent", ""));
        sub.priority = followUpPriority(role);
        sub.origin = "conductor-1";
        if (step.contains("payload"))
            sub.payload = step["payload"];
        subtasks.push_back(std::move(sub));
    }
    return subtasks;
}

bool isErrorPlan(const nlohmann::json& plan)
{
    return plan.contains("error");
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
            const auto foundCount = resp.toolCalls.size();
            for (const auto& tc : resp.toolCalls)
            {
                if (tc.contains("arguments") && tc["arguments"].contains("steps"))
                    return tc["arguments"];
            }
            if (ctx_->logger)
                ctx_->logger->log(id(), "warn",
                    "decomposeGoal: examined " + juce::String(static_cast<int>(foundCount))
                    + " tool call(s) but none contained a valid 'steps' argument; "
                    "falling back to deterministic decomposition",
                    { { "foundCount", foundCount } });
        }
    }
    return DeterministicFallbackLlmClient::decomposeIntent(intent);
}

void ConductorAgent::stop()
{
    state_.store(AgentState::Stopped);
    // Cancel any pending async decompositions (the futures become unavailable).
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pendingDecompositions_.clear();
}

void ConductorAgent::checkPendingDecompositions()
{
    // H-4: Called periodically (e.g. from the blackboard pump thread). Checks
    // if any async LLM decomposition has completed and submits the follow-ups
    // via submitCallback_.
    if (! submitCallback_)
    {
        // No callback wired yet — nothing to do.
        return;
    }

    std::lock_guard<std::mutex> lock(pendingMutex_);
    for (auto it = pendingDecompositions_.begin(); it != pendingDecompositions_.end(); )
    {
        auto& [task, future] = it->second;
        if (future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            auto plan = future.get();
            it = pendingDecompositions_.erase(it);

            if (isErrorPlan(plan) || ! plan.contains("steps") || ! plan["steps"].is_array())
            {
                if (ctx_ && ctx_->logger)
                    ctx_->logger->log(id(), "warn", "async decomposition failed or had no steps",
                                      { { "taskId", task.id.toStdString() }, { "plan", plan } });
                state_.store(AgentState::Idle);
                continue;
            }

            auto subtasks = buildSubtasks(task, plan);
            if (! subtasks.empty())
                submitCallback_(std::move(subtasks));

            if (ctx_ && ctx_->blackboard)
                ctx_->blackboard->publish(id(), "conductor.plan",
                    { { "runId", task.id.toStdString() }, { "plan", plan } });

            state_.store(AgentState::Idle);
        }
        else
        {
            ++it;
        }
    }
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

    // 2. H-4 FIX: When an LLM is wired, defer the (potentially-blocking HTTP)
    //    decomposition to a std::async thread so the scheduler worker is not
    //    blocked. Returns immediately with a "deferred" result. The follow-ups
    //    are submitted asynchronously via checkPendingDecompositions() when the
    //    future is ready, using submitCallback_ (wired by AgentRuntime).
    if (ctx_ && ctx_->llm)
    {
        auto planFuture = std::async(std::launch::async, [this, intent = task.intent]() -> nlohmann::json {
            try { return decomposeGoal(intent); }
            catch (...) { return nlohmann::json{ { "error", "decompose_async_exception" } }; }
        });

        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            pendingDecompositions_[task.id.toStdString()] = { task, std::move(planFuture) };
        }

        r.success = true;
        r.telemetry["decomposedVia"] = "async-llm";
        r.telemetry["async"] = true;
        state_.store(AgentState::Idle);
        return r;
    }

    // 3. Synchronous path (no LLM — deterministic fallback is instant).
    auto plan = decomposeGoal(task.intent);
    if (! plan.contains("steps") || ! plan["steps"].is_array())
    {
        r.success = false;
        r.errorCode = "decompose_failed";
        state_.store(AgentState::Idle);
        return r;
    }

    // 4. Build and delegate follow-ups.
    auto subtasks = buildSubtasks(task, plan);
    r.followUps = std::move(subtasks);
    r.findings["plan"] = plan;
    r.findings["delegatedCount"] = static_cast<int>(r.followUps.size());
    r.emitEvents.push_back({ "conductor.plan", { { "runId", runId.toStdString() }, { "plan", plan } } });

    r.success = true;
    r.telemetry["decomposedVia"] = plan.value("source", "deterministic-fallback");
    state_.store(AgentState::Idle);
    return r;
}

} // namespace more_phi::agents
