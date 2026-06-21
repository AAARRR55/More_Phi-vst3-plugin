#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <juce_core/juce_core.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "AI/Agents/Scheduler/PriorityScheduler.h"

using namespace more_phi::agents;
using Catch::Approx;
using namespace std::chrono_literals;

TEST_CASE("PriorityScheduler runs submitted tasks", "[agents][scheduler]")
{
    PriorityScheduler scheduler;
    scheduler.start(2);

    std::atomic<int> counter{0};
    scheduler.submit([&] { counter.fetch_add(1, std::memory_order_relaxed); }, TaskPriority::Normal);
    scheduler.submit([&] { counter.fetch_add(1, std::memory_order_relaxed); }, TaskPriority::Normal);

    // Spin briefly until both complete or timeout.
    for (int i = 0; i < 200 && counter.load(std::memory_order_relaxed) < 2; ++i)
        std::this_thread::sleep_for(5ms);

    REQUIRE(counter.load(std::memory_order_relaxed) == 2);
    scheduler.stop();
}

TEST_CASE("PriorityScheduler honors priority ordering under single worker", "[agents][scheduler]")
{
    // With one worker and tasks submitted before start, higher priority runs first.
    PriorityScheduler scheduler;

    std::vector<int> order;
    juce::SpinLock orderLock;

    auto record = [&](int tag) {
        juce::SpinLock::ScopedLockType lock(orderLock);
        order.push_back(tag);
    };

    scheduler.submit([&] { record(1); }, TaskPriority::Background);
    scheduler.submit([&] { record(2); }, TaskPriority::High);
    scheduler.submit([&] { record(3); }, TaskPriority::RealtimeCritical);

    scheduler.start(1); // single worker so ordering is deterministic from the queue

    for (int i = 0; i < 200 && order.size() < 3; ++i)
        std::this_thread::sleep_for(5ms);

    REQUIRE(order.size() == 3);
    // RealtimeCritical (3) before High (2) before Background (1)
    REQUIRE(order[0] == 3);
    REQUIRE(order[1] == 2);
    REQUIRE(order[2] == 1);

    scheduler.stop();
}

// ── Task 4: BlackboardBridge ─────────────────────────────────────────────────
#include "AI/AutomationControlPlane.h"
#include "AI/Agents/Blackboard/BlackboardBridge.h"

TEST_CASE("BlackboardBridge publishes and fans out to subscribers", "[agents][blackboard]")
{
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};

    int received = 0;
    juce::String seenType;
    nlohmann::json seenPayload;
    bb.subscribe("analysis-1", { "analysis.finding" },
        [&](const juce::String& type, const nlohmann::json& payload, const juce::String& /*source*/) {
            ++received;
            seenType = type;
            seenPayload = payload;
        });

    bb.publish("analysis-1", "analysis.finding", { { "lufs", -9.2 } });
    bb.poll();

    REQUIRE(received == 1);
    REQUIRE(seenType == "analysis.finding");
    REQUIRE(seenPayload["lufs"].get<double>() == Approx(-9.2));
}

TEST_CASE("BlackboardBridge isolates subscribers by event type", "[agents][blackboard]")
{
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};

    int aHits = 0, bHits = 0;
    bb.subscribe("agent-a", { "alpha" }, [&](auto&&...) { ++aHits; });
    bb.subscribe("agent-b", { "beta" },  [&](auto&&...) { ++bHits; });

    bb.publish("src", "alpha", {});
    bb.publish("src", "beta", {});
    bb.publish("src", "alpha", {});
    bb.poll();

    REQUIRE(aHits == 2);
    REQUIRE(bHits == 1);
}

// ── Task 6: DefaultToolInvoker ───────────────────────────────────────────────
#include "AI/Agents/Tooling/DefaultToolInvoker.h"

TEST_CASE("DefaultToolInvoker enforces capability scope", "[agents][invoker]")
{
    std::vector<juce::String> allowed = { "analysis.get_summary", "analysis.get_spectrum" };
    juce::String dispatchedMethod;
    DefaultToolInvoker::DispatchFn dispatch = [&](const juce::String& method, const nlohmann::json& /*params*/) -> juce::String {
        dispatchedMethod = method;
        return juce::String(nlohmann::json({ { "lufs", -9.0 } }).dump());
    };
    DefaultToolInvoker::CapabilityFn capFor = [&](const juce::String& /*agentId*/) -> std::vector<juce::String> {
        return allowed;
    };

    DefaultToolInvoker invoker(dispatch, capFor);

    auto ok = invoker.invoke("analysis.get_summary", {}, "analysis-1");
    REQUIRE(dispatchedMethod == "analysis.get_summary");
    REQUIRE(ok["lufs"].get<double>() == Approx(-9.0));

    auto blocked = invoker.invoke("set_parameter", { { "index", 0 } }, "analysis-1");
    REQUIRE(blocked.contains("error"));
    REQUIRE(blocked["error"]["code"].get<std::string>() == "capability_denied");
}

TEST_CASE("DefaultToolInvoker enforces per-agent rate budget", "[agents][invoker]")
{
    std::vector<juce::String> allowed = { "analysis.get_summary" };
    DefaultToolInvoker::DispatchFn dispatch = [](const juce::String&, const nlohmann::json&) -> juce::String {
        return juce::String(nlohmann::json({ { "ok", true } }).dump());
    };
    DefaultToolInvoker::CapabilityFn capFor = [&](const juce::String&) { return allowed; };

    DefaultToolInvoker invoker(dispatch, capFor, /*maxCallsPerAgentPerSecond*/ 2);

    auto a = invoker.invoke("analysis.get_summary", {}, "analysis-1");
    auto b = invoker.invoke("analysis.get_summary", {}, "analysis-1");
    auto c = invoker.invoke("analysis.get_summary", {}, "analysis-1");

    REQUIRE_FALSE(a.contains("error"));
    REQUIRE_FALSE(b.contains("error"));
    REQUIRE(c.contains("error"));
    REQUIRE(c["error"]["code"].get<std::string>() == "rate_limited");
}

// ── Task 7: AgentRegistry ────────────────────────────────────────────────────
#include "AI/Agents/AgentRegistry.h"

namespace {

// Minimal IAgent for registry/scheduler/e2e plumbing tests.
class StubAgent : public IAgent
{
public:
    explicit StubAgent(AgentRole r, std::vector<juce::String> tools = {})
        : role_(r), tools_(std::move(tools)) {}
    AgentRole role() const noexcept override { return role_; }
    juce::String id() const noexcept override { return id_; }
    void setId(const juce::String& id) { id_ = id; }
    std::vector<juce::String> allowedTools() const override { return tools_; }
    void prepare(const AgentContext&) override { prepared_ = true; }
    AgentResult execute(const AgentTask& task) override
    {
        ++execCount;
        AgentResult r;
        r.taskId = task.id;
        r.success = true;
        r.findings = { { "echo", task.intent.toStdString() } };
        return r;
    }
    AgentState state() const noexcept override { return AgentState::Idle; }
    void stop() override {}

    AgentRole role_;
    std::vector<juce::String> tools_;
    juce::String id_ = "stub";
    bool prepared_ = false;
    int execCount = 0;
};

} // namespace

TEST_CASE("AgentRegistry registers, finds, and lists roles", "[agents][registry]")
{
    AgentRegistry registry;
    auto a = std::make_unique<StubAgent>(AgentRole::Analysis);
    auto* raw = a.get();
    a->setId("analysis-1");
    registry.registerAgent(std::move(a));

    REQUIRE(registry.find(AgentRole::Analysis) == raw);
    REQUIRE(registry.find(AgentRole::Optimization) == nullptr);
    auto roles = registry.registeredRoles();
    REQUIRE(roles.size() == 1);
    REQUIRE(roles[0] == AgentRole::Analysis);
}

TEST_CASE("AgentRegistry prepare wires context into every agent", "[agents][registry]")
{
    AgentRegistry registry;
    auto a = std::make_unique<StubAgent>(AgentRole::Analysis);
    auto* raw = a.get();
    registry.registerAgent(std::move(a));

    AgentContext ctx;   // members left null; stub doesn't deref them
    registry.prepareAll(ctx);
    REQUIRE(raw->prepared_);
}

// ── Task 8: AgentRuntime ─────────────────────────────────────────────────────
#include "AI/Agents/AgentRuntime.h"
#include "AI/Agents/Llm/NullLlmClient.h"

namespace {

// A minimal AgentRuntime fixture without a real MorePhiProcessor: inject a
// fake invoker + a real AutomationRuntime on a temp dir.
struct RuntimeFixture
{
    RuntimeFixture()
    {
        dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getNonexistentChildFile("morephi_agent_runtime_test", "");
        more_phi::AutomationRuntime::setStoreDirectoryOverrideForTests(dir);
    }
    ~RuntimeFixture()
    {
        more_phi::AutomationRuntime::clearStoreDirectoryOverrideForTests();
        dir.deleteRecursively();
    }
    juce::File dir;
};

} // namespace

TEST_CASE("AgentRuntime submits a task and returns a result", "[agents][runtime]")
{
    RuntimeFixture fx;
    more_phi::IntegrationEventBus bus{32};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;

    DefaultToolInvoker::DispatchFn dispatch = [](const juce::String&, const nlohmann::json&) {
        return juce::String(nlohmann::json({ { "ok", true } }).dump());
    };
    DefaultToolInvoker::CapabilityFn cap = [](const juce::String&) {
        return std::vector<juce::String>{ "analysis.get_summary" };
    };
    DefaultToolInvoker invoker{dispatch, cap};

    AgentRuntime runtime{nullptr, nullptr, nullptr, invoker, bb, logger, nullptr};

    auto a = std::make_unique<StubAgent>(AgentRole::Analysis, std::vector<juce::String>{"analysis.get_summary"});
    a->setId("analysis-1");
    runtime.registerAgent(std::move(a));
    runtime.start(2);

    AgentTask task;
    task.id = "t1";
    task.targetRole = AgentRole::Analysis;
    task.intent = "ping";
    task.priority = TaskPriority::Normal;
    juce::String assigned = runtime.submitTask(task);

    std::optional<AgentResult> result;
    for (int i = 0; i < 200 && ! result.has_value(); ++i)
    {
        result = runtime.peekResult(assigned);
        std::this_thread::sleep_for(5ms);
    }

    REQUIRE(result.has_value());
    REQUIRE(result->success);
    REQUIRE(result->findings["echo"].get<std::string>() == "ping");

    runtime.stop();
}
