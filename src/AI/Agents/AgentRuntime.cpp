// src/AI/Agents/AgentRuntime.cpp
#include "AI/Agents/AgentRuntime.h"

#include <juce_core/juce_core.h>

namespace more_phi::agents {

namespace {
juce::String newTaskId(const juce::String& prefix)
{
    return prefix + "-" + juce::String(juce::Time::getHighResolutionTicks());
}
} // namespace

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
    registry_.prepareAll(sharedContext_);

    scheduler_.start(numWorkers);

    // Blackboard pump thread: periodically fans out new events to subscribers.
    blackboardPumpRunning_.store(true);
    blackboardPumpThread_ = std::thread([this] {
        while (blackboardPumpRunning_.load())
        {
            try { blackboard_.poll(); } catch (...) {}
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
    AgentTask task;
    task.id = newTaskId("goal");
    task.targetRole = AgentRole::Conductor;
    task.intent = userIntent;
    task.priority = priority;
    task.origin = origin;
    task.payload = { { "isGoal", true } };
    return submitTask(std::move(task));
}

juce::String AgentRuntime::submitTask(AgentTask task)
{
    auto* agent = registry_.find(task.targetRole);
    if (agent == nullptr)
    {
        logger_.log("runtime", "warn", "submitTask to unregistered role",
                    { { "role", toString(task.targetRole).toStdString() },
                      { "taskId", task.id.toStdString() } });
        return {};
    }
    if (task.id.isEmpty())
        task.id = newTaskId("task");
    IAgent* agentPtr = agent;
    AgentTask moved = std::move(task);
    scheduler_.submit([this, agentPtr, moved] { executeOnWorker(*agentPtr, moved); },
                      moved.priority);
    return moved.id;
}

void AgentRuntime::executeOnWorker(IAgent& agent, const AgentTask& task)
{
    AgentResult r;
    try
    {
        r = agent.execute(task);
    }
    catch (const std::exception& e)
    {
        r.taskId = task.id;
        r.success = false;
        r.errorCode = "agent_exception";
        r.findings = { { "exception", e.what() } };
        logger_.log(agent.id(), "error", "agent threw during execute",
                    { { "taskId", task.id.toStdString() }, { "exception", e.what() } });
    }
    catch (...)
    {
        r.taskId = task.id;
        r.success = false;
        r.errorCode = "agent_exception";
        logger_.log(agent.id(), "error", "agent threw unknown exception during execute",
                    { { "taskId", task.id.toStdString() } });
    }
    if (r.taskId.isEmpty())
        r.taskId = task.id;

    publishResultEvents(agent.id(), r);
    processFollowUps(agent, r);

    {
        std::lock_guard<std::mutex> lock(resultsMutex_);
        results_[r.taskId.toStdString()] = r;
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
    for (auto& follow : followUps)
        submitTask(std::move(follow));
}

std::optional<AgentResult> AgentRuntime::peekResult(const juce::String& taskId) const
{
    std::lock_guard<std::mutex> lock(resultsMutex_);
    auto it = results_.find(taskId.toStdString());
    if (it == results_.end())
        return std::nullopt;
    return it->second;
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
    return j;
}

} // namespace more_phi::agents
