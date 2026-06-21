// End-to-end test for the multi-agent orchestration layer.
//
// Wires a real AgentRuntime with the real Conductor + 6 specialist agents against
// a fake IToolInvoker (no MorePhiProcessor / no MCP socket). Asserts the full
// happy path:
//   1. submitGoal → Conductor executes and decomposes into followUps.
//   2. The runtime honors ONLY the Conductor's followUps (D-isolation).
//   3. Each delegated specialist executes and records a result via peekResult.
//   4. The runtime's describeState() reports the full cast registered.
//
// This is the integration counterpart to the unit tests in TestAgentRuntimeCore,
// TestConductorAgent, TestAnalysisAgent, etc. — it does not re-assert per-agent
// business logic, only the runtime's orchestration contract.

#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include "AI/AutomationControlPlane.h"
#include "AI/Agents/AgentRuntime.h"
#include "AI/Agents/Blackboard/BlackboardBridge.h"
#include "AI/Agents/Tooling/DefaultToolInvoker.h"
#include "AI/Agents/Logging/NullAgentLogger.h"
#include "AI/Agents/Llm/DeterministicFallbackLlmClient.h"
#include "AI/Agents/Conductor/ConductorAgent.h"
#include "AI/Agents/Agents/AnalysisAgent.h"
#include "AI/Agents/Agents/OptimizationAgent.h"
#include "AI/Agents/Agents/CreativeAgent.h"
#include "AI/Agents/Agents/RealtimeControlAgent.h"
#include "AI/Agents/Agents/QualitySafetyAgent.h"
#include "AI/Agents/Agents/MemoryAgent.h"

using namespace more_phi::agents;
using namespace std::chrono_literals;

namespace {

struct RuntimeFixture
{
    RuntimeFixture()
    {
        dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getNonexistentChildFile("morephi_agent_e2e_test", "");
        more_phi::AutomationRuntime::setStoreDirectoryOverrideForTests(dir);
    }
    ~RuntimeFixture()
    {
        more_phi::AutomationRuntime::clearStoreDirectoryOverrideForTests();
        dir.deleteRecursively();
    }
    juce::File dir;
};

// Fake invoker: every tool call returns a benign canned JSON so specialist
// agents (which invoke analysis.* / mastering.* etc.) can complete without a
// real processor. Capability scope is wide so the invoker never blocks.
struct FakeInvoker
{
    nlohmann::json invoke(const juce::String& /*tool*/,
                          const nlohmann::json& /*params*/,
                          const juce::String& /*agentId*/) const
    {
        return { { "ok", true },
                 { "lufs_integrated", -14.0 },
                 { "true_peak_db", -1.0 } };
    }
};

} // namespace

TEST_CASE("AgentRuntime E2E: goal then Conductor decomposes then specialists execute", "[agents][e2e]")
{
    RuntimeFixture fx;
    more_phi::AutomationRuntime runtime;

    more_phi::IntegrationEventBus bus{64};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;
    DeterministicFallbackLlmClient llm;

    FakeInvoker fake;
    DefaultToolInvoker::DispatchFn dispatch = [&](const juce::String& method, const nlohmann::json& params) {
        return juce::String(fake.invoke(method, params, "e2e").dump());
    };
    DefaultToolInvoker::CapabilityFn cap = [](const juce::String&) {
        // Allow every tool the agents reference; this test is about orchestration,
        // not capability enforcement (covered by TestAgentRuntimeCore + per-agent tests).
        return std::vector<juce::String>{
            "analysis.get_summary", "analysis.get_spectrum", "analysis.get_stereo_field",
            "analysis.capture_window", "analysis.compare_render",
            "mastering.plan_preview", "mastering.render_batch", "mastering.render_status",
            "mastering.select_candidate", "hosted_plugin.info", "hosted_plugin.set_parameter",
            "hosted_plugin.set_parameters", "plugin_profile.describe_semantics",
            "workflow.submit", "workflow.execute", "workflow.cancel",
            "memory.list", "memory.store", "memory.recall"
        };
    };
    DefaultToolInvoker invoker{dispatch, cap};

    AgentRuntime agentRuntime(nullptr, nullptr, &runtime, invoker, bb, logger, &llm);

    // Register the full cast (mirrors MorePhiProcessor::startAgentRuntimeIfNeeded).
    agentRuntime.registerAgent(std::make_unique<ConductorAgent>());
    agentRuntime.registerAgent(std::make_unique<AnalysisAgent>());
    agentRuntime.registerAgent(std::make_unique<OptimizationAgent>());
    agentRuntime.registerAgent(std::make_unique<CreativeAgent>());
    agentRuntime.registerAgent(std::make_unique<RealtimeControlAgent>());
    agentRuntime.registerAgent(std::make_unique<QualitySafetyAgent>());
    agentRuntime.registerAgent(std::make_unique<MemoryAgent>());
    agentRuntime.start(2);

    // describeState() must report all 7 agents (and must be safe to call while
    // workers idle — the runtime serializes via its internal mutexes).
    auto state = agentRuntime.describeState();
    REQUIRE(state["agents"].is_array());
    REQUIRE(state["agents"].size() == 7);

    // Submit a streaming-mastering goal — the deterministic fallback always
    // decomposes into analysis + memory + optimization followUps.
    const auto runId = agentRuntime.submitGoal("master this track for streaming", TaskPriority::High, "e2e");
    REQUIRE(runId.isNotEmpty());

    // Wait for the Conductor to complete (its own task id == runId).
    std::optional<AgentResult> conductorResult;
    for (int i = 0; i < 400 && ! conductorResult.has_value(); ++i)
    {
        conductorResult = agentRuntime.peekResult(runId);
        if (! conductorResult.has_value())
            std::this_thread::sleep_for(5ms);
    }
    REQUIRE(conductorResult.has_value());
    REQUIRE(conductorResult->success);
    REQUIRE(conductorResult->findings["delegatedCount"].get<int>() == 3);

    // The Conductor's followUps must have been re-submitted as specialist tasks.
    // Per ConductorAgent.cpp, subtask ids are "<goalId>-<role>".
    const auto base = runId.toStdString();
    std::vector<std::string> subtaskIds = { base + "-analysis", base + "-memory", base + "-optimization" };

    for (const auto& subId : subtaskIds)
    {
        std::optional<AgentResult> subResult;
        for (int i = 0; i < 400 && ! subResult.has_value(); ++i)
        {
            subResult = agentRuntime.peekResult(juce::String(subId));
            if (! subResult.has_value())
                std::this_thread::sleep_for(5ms);
        }
        // Each delegated specialist must have a recorded result.
        // (success may vary per agent's internal logic, but a result MUST exist.)
        REQUIRE(subResult.has_value());
        REQUIRE(subResult->taskId.toStdString() == subId);
    }

    agentRuntime.stop();
}

TEST_CASE("AgentRuntime E2E: specialist that tries to delegate is rejected", "[agents][e2e]")
{
    // D-isolation invariant: only the Conductor may delegate. A specialist that
    // (hypothetically) returned followUps must have them dropped, and the runtime
    // must publish agents.delegation_rejected to the blackboard. We drive this
    // through submitTask with a custom misbehaving agent.
    RuntimeFixture fx;
    more_phi::AutomationRuntime runtime;

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

    AgentRuntime agentRuntime(nullptr, nullptr, &runtime, invoker, bb, logger, nullptr);

    // Subscribe to the rejection event before publishing so we can observe it.
    std::atomic<int> rejections{0};
    bb.subscribe("runtime", { "agents.delegation_rejected" },
        [&](const juce::String&, const nlohmann::json&, const juce::String&) {
            rejections.fetch_add(1, std::memory_order_relaxed);
        });

    // A rogue specialist that tries to delegate.
    class RogueAgent : public IAgent
    {
    public:
        AgentRole role() const noexcept override { return AgentRole::Custom; }
        juce::String id() const noexcept override { return "rogue-1"; }
        std::vector<juce::String> allowedTools() const override { return { "analysis.get_summary" }; }
        void prepare(const AgentContext&) override {}
        AgentResult execute(const AgentTask& task) override
        {
            AgentResult r;
            r.taskId = task.id;
            r.success = true;
            // Attempt to delegate — this MUST be dropped by the runtime.
            AgentTask sub;
            sub.id = "rogue-child";
            sub.targetRole = AgentRole::Analysis;
            r.followUps.push_back(sub);
            return r;
        }
        AgentState state() const noexcept override { return AgentState::Idle; }
        void stop() override {}
    };

    agentRuntime.registerAgent(std::make_unique<RogueAgent>());
    agentRuntime.registerAgent(std::make_unique<AnalysisAgent>());  // so the dropped child has a target
    agentRuntime.start(1);

    AgentTask task;
    task.id = "rogue-root";
    task.targetRole = AgentRole::Custom;
    task.intent = "try to delegate";
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
    // The rogue's result must have its followUps cleared (not honored).
    REQUIRE(result->followUps.empty());

    // Pump the blackboard so the rejection event fans out to our subscriber.
    for (int i = 0; i < 50 && rejections.load(std::memory_order_relaxed) == 0; ++i)
    {
        bb.poll();
        std::this_thread::sleep_for(2ms);
    }
    REQUIRE(rejections.load(std::memory_order_relaxed) == 1);

    // The dropped child task must NOT have a result.
    REQUIRE_FALSE(agentRuntime.peekResult("rogue-child").has_value());

    agentRuntime.stop();
}
