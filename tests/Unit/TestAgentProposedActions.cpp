// tests/Unit/TestAgentProposedActions.cpp
//
// F1/AUDIT: pins that AgentRuntime now DISPATCHES proposedActions (previously
// computed by specialists and silently dropped). A custom specialist returns a
// proposedAction; the runtime must invoke the tool through the IToolInvoker,
// record the outcome in telemetry, and publish an event.
#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

#include "AI/Agents/AgentRuntime.h"
#include "AI/Agents/Blackboard/BlackboardBridge.h"
#include "AI/Agents/Logging/NullAgentLogger.h"
#include "AI/AutomationControlPlane.h"

using namespace more_phi::agents;
using namespace std::chrono_literals;

namespace {

// Records every invoke() so the test can prove dispatch happened.
class RecordingInvoker : public IToolInvoker
{
public:
    std::atomic<int> invokeCount{0};
    juce::String lastTool;
    nlohmann::json lastParams;

    nlohmann::json invoke(const juce::String& tool, const nlohmann::json& params,
                          const juce::String& /*agentId*/) override
    {
        lastTool = tool;
        lastParams = params;
        invokeCount.fetch_add(1, std::memory_order_relaxed);
        return { { "ok", true } };
    }
};

// A specialist that returns exactly one proposedAction for the runtime to
// dispatch. Mirrors the shape OptimizationAgent now emits (F8 schema).
class ProposingAgent : public IAgent
{
public:
    AgentRole role() const noexcept override { return AgentRole::Custom; }
    juce::String id() const noexcept override { return "proposer-1"; }
    std::vector<juce::String> allowedTools() const override { return { "set_parameters_batch" }; }
    void prepare(const AgentContext&) override {}
    AgentResult execute(const AgentTask& task) override
    {
        AgentResult r;
        r.taskId = task.id;
        r.success = true;
        r.proposedActions.push_back({
            { "tool", "set_parameters_batch" },
            { "params", nlohmann::json::array({
                { { "index", 0 }, { "value", 0.5f } }
            }) }
        });
        return r;
    }
    AgentState state() const noexcept override { return AgentState::Idle; }
    void stop() override {}
};

} // namespace

TEST_CASE("AgentRuntime dispatches proposedActions through the tool invoker", "[agents][proposedActions]")
{
    more_phi::AutomationRuntime runtime;
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;
    RecordingInvoker invoker;

    std::atomic<int> dispatchEvents{0};
    bb.subscribe("runtime", { "agents.proposed_action_dispatched" },
        [&](const juce::String&, const nlohmann::json& payload, const juce::String&) {
            if (payload.value("ok", false))
                dispatchEvents.fetch_add(1, std::memory_order_relaxed);
        });

    AgentRuntime agentRuntime(nullptr, nullptr, &runtime, invoker, bb, logger, nullptr);
    agentRuntime.registerAgent(std::make_unique<ProposingAgent>());
    agentRuntime.start(1);

    AgentTask task;
    task.id = "propose-root";
    task.targetRole = AgentRole::Custom;
    task.intent = "propose an edit";
    task.priority = TaskPriority::Normal;
    const auto assigned = agentRuntime.submitTask(task);

    std::optional<AgentResult> result;
    for (int i = 0; i < 200 && ! result.has_value(); ++i)
    {
        result = agentRuntime.peekResult(assigned);
        if (! result.has_value())
            std::this_thread::sleep_for(5ms);
    }
    REQUIRE(result.has_value());

    // F1: the proposedAction must have been dispatched exactly once.
    REQUIRE(invoker.invokeCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(invoker.lastTool == "set_parameters_batch");

    // Telemetry records the dispatch outcome.
    REQUIRE(result->telemetry.contains("proposedActionsDispatch"));
    const auto dispatchLog = result->telemetry["proposedActionsDispatch"];
    REQUIRE(dispatchLog.is_array());
    REQUIRE(dispatchLog.size() == 1);
    REQUIRE(dispatchLog[0].value("outcome", std::string{}) == "dispatched");

    // Pump the blackboard so the dispatch event fans out.
    for (int i = 0; i < 50 && dispatchEvents.load(std::memory_order_relaxed) == 0; ++i)
    {
        bb.poll();
        std::this_thread::sleep_for(2ms);
    }
    REQUIRE(dispatchEvents.load(std::memory_order_relaxed) == 1);

    agentRuntime.stop();
}
