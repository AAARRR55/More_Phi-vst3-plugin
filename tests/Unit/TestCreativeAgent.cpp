#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

#include "AI/Agents/Agents/CreativeAgent.h"
#include "AI/Agents/AgentContext.h"
#include "AI/Agents/Blackboard/BlackboardBridge.h"
#include "AI/Agents/Logging/NullAgentLogger.h"
#include "AI/AutomationControlPlane.h"

using namespace more_phi::agents;

namespace {
class FakeInvoker : public IToolInvoker
{
public:
    nlohmann::json invoke(const juce::String&, const nlohmann::json&, const juce::String&) override
    {
        return { { "suggestions", { { "warmer", true }, { "brighter", false } } } };
    }
};
} // namespace

TEST_CASE("CreativeAgent requires approval regardless of autonomy", "[agents][creative]")
{
    CreativeAgent agent;
    REQUIRE(agent.requireApprovalRegardlessOfAutonomy());

    FakeInvoker fake;
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;
    AgentContext ctx;
    ctx.tools = &fake;
    ctx.blackboard = &bb;
    ctx.logger = &logger;
    agent.prepare(ctx);

    AgentTask task;
    task.id = "c1";
    task.intent = "suggest alternatives";
    AgentResult r = agent.execute(task);

    REQUIRE(r.success);
    bool hasSuggestion = false;
    for (const auto& e : r.emitEvents)
        if (e.type == "creative.suggestion") hasSuggestion = true;
    REQUIRE(hasSuggestion);
    // Creative MUST NOT auto-apply: no proposedActions with write tools.
    REQUIRE(r.proposedActions.empty());
}
