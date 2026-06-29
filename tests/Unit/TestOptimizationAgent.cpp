#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

#include "AI/Agents/Agents/OptimizationAgent.h"
#include "AI/Agents/AgentContext.h"
#include "AI/Agents/Blackboard/BlackboardBridge.h"
#include "AI/Agents/Logging/NullAgentLogger.h"
#include "AI/AutomationControlPlane.h"

using namespace more_phi::agents;

namespace {

class FakeInvoker : public IToolInvoker
{
public:
    // O1 regression hook: controls the mastering.neural_apply response shape so the
    // neural-first vs heuristic-fallback branches can both be exercised. Defaults
    // false so the existing heuristic test (which expects proposedActions >= 1)
    // still takes the heuristic path via the neural no-op fallback.
    bool neuralSucceeds = false;
    juce::String lastTool;
    nlohmann::json lastParams;
    nlohmann::json invoke(const juce::String& tool, const nlohmann::json& params, const juce::String&) override
    {
        lastTool = tool;
        lastParams = params;
        if (tool == "mastering.neural_apply")
        {
            // OptimizationAgent reads "available" + "applied" to choose the path.
            if (neuralSucceeds)
                return { {"available", true}, {"applied", true},
                         {"mapping_status", nlohmann::json::object()},
                         {"live_measurements", { {"lufs_integrated", -14.0} }} };
            return { {"available", true}, {"applied", false}, {"state", "apply_no_op"} };
        }
        if (tool == "mastering.plan_preview")
            return { { "plan", { { "output_gain_db", -2.0 } } } };
        if (tool == "mastering.render_batch")
        {
            // Return N candidates with increasing error; the agent should pick candidate 0.
            nlohmann::json cands = nlohmann::json::array();
            for (int i = 0; i < 4; ++i)
                cands.push_back({ { "id", i }, { "lufs_error", 0.5 + 0.1 * i }, { "params", { { "out", -1.0 - 0.5 * i } } } });
            return { { "candidates", cands } };
        }
        return { { "ok", true } };
    }
};

} // namespace

TEST_CASE("OptimizationAgent picks the best candidate by metric", "[agents][optimization]")
{
    FakeInvoker fake;
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;

    AgentContext ctx;
    ctx.tools = &fake;
    ctx.blackboard = &bb;
    ctx.logger = &logger;

    OptimizationAgent agent;
    agent.prepare(ctx);

    AgentTask task;
    task.id = "o1";
    task.intent = "optimize toward streaming target";
    task.payload = { { "target", { { "lufsIntegrated", -14.0 }, { "truePeakMaxDb", -1.0 } } } };
    AgentResult r = agent.execute(task);

    REQUIRE(r.success);
    REQUIRE(r.proposedActions.size() >= 1);
    REQUIRE(r.proposedActions[0]["tool"].get<std::string>() == "set_parameters_batch");
    bool hasProposal = false;
    for (const auto& e : r.emitEvents)
        if (e.type == "optimization.proposal") hasProposal = true;
    REQUIRE(hasProposal);
}

TEST_CASE("OptimizationAgent takes the neural-first path when the model applies", "[agents][optimization]")
{
    // O1 regression: the highest-impact behavior change. Previously the FakeInvoker
    // returned a canned shape with no "available"/"applied" keys, so both read as
    // false and OptimizationAgent ALWAYS fell through to the heuristic path — the
    // neural-first branch was never exercised. Assert the neural branch is taken on
    // success and that proposedActions stays empty (the neural apply wrote directly,
    // nothing for the Conductor to re-dispatch).
    FakeInvoker fake;
    fake.neuralSucceeds = true;
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;

    AgentContext ctx;
    ctx.tools = &fake;
    ctx.blackboard = &bb;
    ctx.logger = &logger;

    OptimizationAgent agent;
    agent.prepare(ctx);

    AgentTask task;
    task.id = "n1";
    task.intent = "master this track for streaming";   // isMasteringTask() == true
    AgentResult r = agent.execute(task);

    REQUIRE(r.success);
    REQUIRE(r.findings.value("path", std::string{}) == "neural");
    REQUIRE(r.proposedActions.empty());                 // neural apply wrote directly
    REQUIRE(fake.lastTool == "mastering.neural_apply"); // took the neural branch first
}

TEST_CASE("OptimizationAgent falls back to heuristic when the neural apply no-ops", "[agents][optimization]")
{
    // O1 fallback: when mastering.neural_apply returns available=true but applied=false
    // (no hosted plugin / unmapped / model no-op), the agent must fall through to the
    // heuristic batch path and return a proposedAction for the Conductor to re-dispatch.
    FakeInvoker fake;
    fake.neuralSucceeds = false;
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;

    AgentContext ctx;
    ctx.tools = &fake;
    ctx.blackboard = &bb;
    ctx.logger = &logger;

    OptimizationAgent agent;
    agent.prepare(ctx);

    AgentTask task;
    task.id = "n2";
    task.intent = "make it louder for streaming";      // isMasteringTask() == true
    AgentResult r = agent.execute(task);

    REQUIRE(r.success);
    REQUIRE(r.findings.value("path", std::string{}) == "heuristic");
    REQUIRE(r.proposedActions.size() >= 1);             // Conductor re-dispatches this
    REQUIRE(r.findings.contains("neural_fallback_reason"));   // fallback reason recorded
}
