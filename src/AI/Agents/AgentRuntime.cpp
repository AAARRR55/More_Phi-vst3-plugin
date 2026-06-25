// src/AI/Agents/AgentRuntime.cpp
#include "AI/Agents/AgentRuntime.h"

#include <juce_core/juce_core.h>

namespace more_phi::agents {

AgentRuntime::AgentRuntime(MorePhiProcessor* processor,
                           const InstanceIdentity* identity,
                           AutomationRuntime* runtime,
                           IToolInvoker& tools,
                           BlackboardBridge& blackboard,
                           IAgentLogger& logger,
                           ILlmClient* llm)
    : processor_(processor)
    , identity_(identity)
    , runtime_(runtime)
    , tools_(&tools)
    , blackboard_(blackboard)
    , logger_(logger)
    , llm_(llm)
{
}

AgentRuntime::~AgentRuntime()
{
    stop();
}

juce::String AgentRuntime::nextTaskId(const juce::String& prefix)
{
    // M5: counter-suffixed id so concurrent producers (UI + MCP threads) cannot
    // produce colliding ids from the time-tick source alone.
    const auto n = taskIdCounter_.fetch_add(1, std::memory_order_relaxed) + 1;
    return prefix + "-" + juce::String(n);
}

void AgentRuntime::start(unsigned numWorkers)
{
    if (running_.exchange(true))
        return;

    // Wire the shared context into every agent. sharedContext_ is a member so
    // the pointers agents store remain valid for the runtime's lifetime.
    sharedContext_.processor = processor_;
    sharedContext_.identity  = identity_;
    sharedContext_.runtime   = runtime_;
    sharedContext_.tools     = tools_;
    sharedContext_.blackboard= &blackboard_;
    sharedContext_.logger    = &logger_;
    sharedContext_.llm       = llm_;
    // M3: freeze the registry before workers start reading it. Any later
    // registerAgent() would race find()/registeredRoles() on worker + MCP
    // threads.
    registry_.seal();
    registry_.prepareAll(sharedContext_);

    // Subscribe each agent's declared event types to the blackboard exactly once.
    // On restart after stop() the same agent instances are still registered, so a
    // runtime-side flag guards against duplicate subscriptions piling up across
    // start()/stop() cycles.
    for (auto role : registry_.registeredRoles())
    {
        if (auto* a = registry_.find(role))
        {
            if (a->id().isNotEmpty() && subscribedAgentIds_.count(a->id().toStdString()))
                continue;
            IAgent* agent = a;
            blackboard_.subscribe(agent->id(), agent->subscribedEventTypes(),
                [agent, this](const juce::String& type, const nlohmann::json& payload, const juce::String& source) {
                    agent->onEvent(type, payload, source, /*runId*/ {});
                });
            subscribedAgentIds_.insert(a->id().toStdString());
        }
    }

    scheduler_.start(numWorkers);

    // Blackboard pump thread: periodically fans out new events to subscribers.
    blackboardPumpRunning_.store(true);
    blackboardPumpThread_ = std::thread([this] {
        while (blackboardPumpRunning_.load())
        {
            // M3: skip the (potentially large) listRecentSince pull when the bus
            // has published nothing since our cursor — a cheap counter compare
            // instead of a full window scan every 50ms.
            try {
                // M9: directly call poll() without the TOCTOU-prone hasNewEvents()
                // gate — poll()'s internal listRecentSince() is cheap when nothing
                // is new, and the old check risked event loss under burst eviction.
                blackboard_.poll();
            } catch (const std::exception& e) {
                logger_.log("runtime", "error", "blackboard pump threw",
                            { { "exception", e.what() } });
            } catch (...) {
                logger_.log("runtime", "error", "blackboard pump threw unknown exception");
            }
            juce::Thread::sleep(static_cast<int>(blackboardPumpIntervalMs_));
        }
    });
}

void AgentRuntime::stop()
{
    if (! running_.exchange(false))
        return;
    blackboardPumpRunning_.store(false);
    if (blackboardPumpThread_.joinable())
        blackboardPumpThread_.join();
    scheduler_.stop();
    registry_.stopAll();
}

bool AgentRuntime::registerAgent(std::unique_ptr<IAgent> agent)
{
    return registry_.registerAgent(std::move(agent));
}

juce::String AgentRuntime::submitGoal(const juce::String& userIntent,
                                      TaskPriority priority,
                                      const juce::String& origin)
{
    auto* conductor = registry_.find(AgentRole::Conductor);
    if (conductor == nullptr)
    {
        logger_.log("runtime", "warn", "submitGoal with no Conductor registered",
                    { { "intent", userIntent.toStdString() } });
        return {};
    }
    if (! running_.load(std::memory_order_acquire))
    {
        logger_.log("runtime", "warn", "submitGoal while runtime stopped",
                    { { "intent", userIntent.toStdString() } });
        return {};
    }
    AgentTask task;
    task.id = nextTaskId("goal");
    task.targetRole = AgentRole::Conductor;
    task.intent = userIntent;
    task.priority = priority;
    task.origin = origin;
    task.payload = { { "isGoal", true } };
    return submitTask(std::move(task));
}

juce::String AgentRuntime::submitTask(AgentTask task)
{
    if (! running_.load(std::memory_order_acquire))
    {
        logger_.log("runtime", "warn", "submitTask while runtime stopped",
                    { { "role", toString(task.targetRole).toStdString() } });
        return {};
    }
    auto* agent = registry_.find(task.targetRole);
    if (agent == nullptr)
    {
        logger_.log("runtime", "warn", "submitTask to unregistered role",
                    { { "role", toString(task.targetRole).toStdString() },
                      { "taskId", task.id.toStdString() } });
        return {};
    }
    if (task.id.isEmpty())
        task.id = nextTaskId("task");
    IAgent* agentPtr = agent;
    AgentTask moved = std::move(task);

    // H4: if this is a Conductor goal (carries isGoal), register the run so its
    // delegated follow-ups can be correlated back to a terminal outcome.
    if (moved.targetRole == AgentRole::Conductor && moved.payload.value("isGoal", false))
        registerRun(moved.id, moved.id);   // goal task id == run id (ConductorAgent uses task.id as runId)

    scheduler_.submit([this, agentPtr, moved] { executeOnWorker(*agentPtr, moved); },
                      moved.priority);
    return moved.id;
}

void AgentRuntime::executeOnWorker(IAgent& agent, const AgentTask& task)
{
    // M6: guard the ENTIRE body (execute + publish + followups + store) so a
    // throw from publishResultEvents (e.g. bad_alloc during blackboard publish)
    // is logged and the worker survives, rather than being swallowed silently
    // by the pool's outer catch and leaving peekResult() polling forever.
    AgentResult r;
    bool haveResult = false;
    try
    {
        r = agent.execute(task);
        haveResult = true;
    }
    catch (const std::exception& e)
    {
        r.taskId = task.id;
        r.success = false;
        r.errorCode = "agent_exception";
        r.findings = { { "exception", e.what() } };
        haveResult = true;
        logger_.log(agent.id(), "error", "agent threw during execute",
                    { { "taskId", task.id.toStdString() }, { "exception", e.what() } });
    }
    catch (...)
    {
        r.taskId = task.id;
        r.success = false;
        r.errorCode = "agent_exception";
        haveResult = true;
        logger_.log(agent.id(), "error", "agent threw unknown exception during execute",
                    { { "taskId", task.id.toStdString() } });
    }

    try
    {
        if (haveResult)
        {
            if (r.taskId.isEmpty())
                r.taskId = task.id;

            publishResultEvents(agent.id(), r);
            processFollowUps(agent, r);

            storeResultLocked(r.taskId, r);

            // H4: advance run correlation for this task's run (if any).
            noteSubtaskCompletion(task.runId, task.id, r);
        }
    }
    catch (const std::exception& e)
    {
        logger_.log(agent.id(), "error", "post-execute bookkeeping threw",
                    { { "taskId", task.id.toStdString() }, { "exception", e.what() } });
    }
    catch (...)
    {
        logger_.log(agent.id(), "error", "post-execute bookkeeping threw (unknown)",
                    { { "taskId", task.id.toStdString() } });
    }
}

void AgentRuntime::publishResultEvents(const juce::String& agentId, const AgentResult& r)
{
    for (const auto& env : r.emitEvents)
        blackboard_.publish(agentId, env.type, env.payload, /*runId*/ {});
}

void AgentRuntime::processFollowUps(const IAgent& source, AgentResult& r)
{
    if (r.followUps.empty())
        return;
    // D-isolation: only the Conductor may delegate. Move the followUps out so the
    // result stored in the results map does not also carry them (avoids confusion
    // in peekResult consumers).
    std::vector<AgentTask> followUps = std::move(r.followUps);
    r.followUps.clear();

    if (source.role() != AgentRole::Conductor)
    {
        logger_.log(source.id(), "warn", "non-Conductor agent attempted delegation; dropped",
                    { { "count", static_cast<int>(followUps.size()) } });
        blackboard_.publish(source.id(), "agents.delegation_rejected",
                            { { "count", static_cast<int>(followUps.size()) } });
        return;
    }

    // H4: the Conductor's goal task id is the run id (see submitTask). Record how
    // many follow-ups we are about to enqueue so noteSubtaskCompletion knows when
    // the run is truly finished.
    const juce::String goalTaskId = r.taskId;
    {
        std::lock_guard<std::mutex> lock(runsMutex_);
        // Register the run if the goal path didn't already (it did for isGoal,
        // but be defensive: a Conductor task submitted directly also forms a run).
        if (runs_.find(goalTaskId.toStdString()) == runs_.end())
            runs_[goalTaskId.toStdString()] = { goalTaskId, 0, true, nlohmann::json::object() };
        runs_[goalTaskId.toStdString()].outstanding += static_cast<int>(followUps.size());
        // Seed aggregate success with the conductor's own success.
        runs_[goalTaskId.toStdString()].aggregateSuccess =
            runs_[goalTaskId.toStdString()].aggregateSuccess && r.success;
    }

    for (auto& follow : followUps)
    {
        // Correlate each follow-up to the goal so noteSubtaskCompletion can tell
        // when the run completes. ConductorAgent already sets follow.runId to the
        // goal task id; we only fill it in if a producer forgot to.
        if (follow.runId.isEmpty())
            follow.runId = goalTaskId.toStdString();
        submitTask(std::move(follow));
    }
}

void AgentRuntime::registerRun(const juce::String& runId, const juce::String& goalTaskId)
{
    std::lock_guard<std::mutex> lock(runsMutex_);
    if (runs_.find(runId.toStdString()) == runs_.end())
        runs_[runId.toStdString()] = { goalTaskId, 0, true, nlohmann::json::object() };
}

void AgentRuntime::noteSubtaskCompletion(const juce::String& runId,
                                         const juce::String& /*subtaskTaskId*/,
                                         const AgentResult& subResult)
{
    if (runId.isEmpty())
        return;   // top-level task with no enclosing run; nothing to correlate

    RunState snapshot;
    bool runComplete = false;
    {
        std::lock_guard<std::mutex> lock(runsMutex_);
        auto it = runs_.find(runId.toStdString());
        if (it == runs_.end())
        {
            // Run wasn't tracked (e.g. a directly-submitted specialist task whose
            // caller passed a runId). Nothing to aggregate.
            return;
        }
        auto& rs = it->second;
        rs.aggregateSuccess = rs.aggregateSuccess && subResult.success;
        if (! subResult.errorCode.isEmpty())
            rs.findings["last_error_code"] = subResult.errorCode.toStdString();
        if (rs.outstanding > 0)
            --rs.outstanding;
        runComplete = (rs.outstanding == 0);
        if (runComplete)
        {
            snapshot = rs;
            runs_.erase(it);
        }
    }

    if (runComplete)
    {
        // Publish conductor.complete so MemoryAgent (and any other observer) learns
        // the run actually finished (H4 — previously never published, so the memory
        // outcome-recall path was dead).
        blackboard_.publish("conductor", "conductor.complete",
            {
                { "success", snapshot.aggregateSuccess },
                { "runId", runId.toStdString() }
            },
            runId);

        // Rewrite the goal's result so peekResult(goalTaskId) reports the real
        // outcome instead of the conductor's premature "delegation issued" success.
        {
            std::lock_guard<std::mutex> rlock(resultsMutex_);
            auto it = results_.find(runId.toStdString());
            if (it != results_.end())
            {
                it->second.success = snapshot.aggregateSuccess;
                it->second.findings["run_complete"] = true;
                if (! snapshot.aggregateSuccess)
                    it->second.errorCode = "run_failed";
                if (! snapshot.findings.empty())
                    it->second.findings["run"] = snapshot.findings;
            }
        }
    }
}

juce::String AgentRuntime::storeResultLocked(const juce::String& taskId, AgentResult result)
{
    // C2: bounded LRU-ish store. Evict the oldest once we exceed maxResults_ so a
    // long-lived session with a chatty client cannot grow results_ unbounded.
    std::lock_guard<std::mutex> lock(resultsMutex_);
    const auto key = taskId.toStdString();
    auto it = results_.find(key);
    if (it != results_.end())
    {
        it->second = std::move(result);   // refresh in place; keep existing order slot
        return taskId;
    }
    results_.emplace(key, std::move(result));
    resultsOrder_.push_back(key);
    while (resultsOrder_.size() > maxResults_)
    {
        const auto& oldest = resultsOrder_.front();
        results_.erase(oldest);
        resultsOrder_.pop_front();
    }
    return taskId;
}

std::optional<AgentResult> AgentRuntime::peekResult(const juce::String& taskId) const
{
    std::lock_guard<std::mutex> lock(resultsMutex_);
    auto it = results_.find(taskId.toStdString());
    if (it == results_.end())
        return std::nullopt;
    return it->second;
}

nlohmann::json AgentRuntime::blackboardRecent(int limit) const
{
    return blackboard_.recentAgentEvents(limit);
}

nlohmann::json AgentRuntime::describeState() const
{
    nlohmann::json j = nlohmann::json::object();
    j["running"] = running_.load();
    auto roles = registry_.registeredRoles();
    nlohmann::json arr = nlohmann::json::array();
    for (auto role : roles)
    {
        const auto* a = registry_.find(role);
        nlohmann::json slot = nlohmann::json::object();
        slot["role"] = toString(role).toStdString();
        if (a) slot["id"] = a->id().toStdString();
        arr.push_back(slot);
    }
    j["agents"] = arr;
    auto s = scheduler_.stats();
    j["scheduler"] = {
        { "executed", s.executed },
        { "starvationBumps", s.starvationBumps }
    };
    {
        std::lock_guard<std::mutex> lock(resultsMutex_);
        j["resultsDepth"] = results_.size();
        j["resultsCapacity"] = maxResults_;
    }
    return j;
}

} // namespace more_phi::agents
