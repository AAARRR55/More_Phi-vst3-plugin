#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

#include "AI/Agents/Agents/RealtimeControlAgent.h"
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
    int setParamCalls = 0;
    nlohmann::json invoke(const juce::String& tool, const nlohmann::json& params, const juce::String&) override
    {
        lastTool = tool;
        lastParams = params;
        if (tool == "set_parameter")
            ++setParamCalls;
        return { { "ok", true } };
    }
};
} // namespace

TEST_CASE("RealtimeControlAgent enqueues a correction on clipping", "[agents][realtime]")
{
    FakeInvoker fake;
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;
    AgentContext ctx;
    ctx.tools = &fake;
    ctx.blackboard = &bb;
    ctx.logger = &logger;

    RealtimeControlAgent agent;
    RealtimeControlAgent::Config cfg;
    cfg.maxCorrectionsPerParamPerSecond = 4;
    cfg.correctionBudgetPerRun = 16;
    agent.setConfig(cfg);
    agent.prepare(ctx);

    // Inject a clipping event via onEvent (as the blackboard pump would).
    agent.onEvent("analysis.clipping_detected",
        { { "true_peak_db", 0.2 }, { "channel", "R" } }, "analysis-1", "run-1");

    REQUIRE(fake.lastTool == "set_parameter");
    REQUIRE(fake.setParamCalls == 1);
    REQUIRE(fake.lastParams.contains("index"));
}

TEST_CASE("RealtimeControlAgent rate-cap suppresses excess corrections", "[agents][realtime]")
{
    FakeInvoker fake;
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;
    AgentContext ctx;
    ctx.tools = &fake;
    ctx.blackboard = &bb;
    ctx.logger = &logger;

    RealtimeControlAgent agent;
    RealtimeControlAgent::Config cfg;
    cfg.maxCorrectionsPerParamPerSecond = 2;   // tight
    cfg.correctionBudgetPerRun = 16;
    agent.setConfig(cfg);
    agent.prepare(ctx);

    for (int i = 0; i < 5; ++i)
        agent.onEvent("analysis.clipping_detected", { { "true_peak_db", 0.3 } }, "a", "run-2");

    // Only the first 2 within the same param/second window may apply.
    REQUIRE(fake.setParamCalls == 2);
}
