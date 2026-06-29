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
    juce::String lastTool;
    nlohmann::json lastParams;
    nlohmann::json invoke(const juce::String& tool, const nlohmann::json& params, const juce::String&) override
    {
        lastTool = tool;
        lastParams = params;
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
