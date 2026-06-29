#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

#include "AI/Agents/Agents/QualitySafetyAgent.h"
#include "AI/Agents/AgentContext.h"
#include "AI/Agents/Blackboard/BlackboardBridge.h"
#include "AI/Agents/Logging/NullAgentLogger.h"
#include "AI/AutomationControlPlane.h"

using namespace more_phi::agents;

namespace {
class FakeInvoker : public IToolInvoker
{
public:
    double afterLufs = -13.9;
    double afterTp = -1.1;
    nlohmann::json invoke(const juce::String& tool, const nlohmann::json&, const juce::String&) override
    {
        if (tool == "analysis.compare_render")
            return { { "after", { { "lufs_integrated", afterLufs }, { "true_peak_db", afterTp } } } };
        return { { "ok", true } };
    }
};
} // namespace

TEST_CASE("QualitySafetyAgent approves a proposal within targets", "[agents][quality]")
{
    FakeInvoker fake;
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;
    AgentContext ctx;
    ctx.tools = &fake;
    ctx.blackboard = &bb;
    ctx.logger = &logger;

    QualitySafetyAgent agent;
    QualitySafetyAgent::Config cfg;
    cfg.maxLufs = -14.0;
    cfg.maxTruePeakDb = -1.0;
    agent.setConfig(cfg);
    agent.prepare(ctx);

    QualitySafetyAgent::Proposal p;
    p.proposedActions = { { { "tool", "set_parameters_batch" } } };
    p.target = { { "lufsIntegrated", -14.0 }, { "truePeakMaxDb", -1.0 } };
    auto verdict = agent.evaluate(p);

    REQUIRE(verdict.approved);     // -13.9 > -14 (within, louder allowed up to ceiling), -1.1 < -1.0 (within)
}

TEST_CASE("QualitySafetyAgent rejects a proposal that breaches true-peak", "[agents][quality]")
{
    FakeInvoker fake;
    fake.afterTp = -0.5; // breach
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;
    AgentContext ctx;
    ctx.tools = &fake;
    ctx.blackboard = &bb;
    ctx.logger = &logger;

    QualitySafetyAgent agent;
    QualitySafetyAgent::Config cfg;
    cfg.maxTruePeakDb = -1.0;
    agent.setConfig(cfg);
    agent.prepare(ctx);

    QualitySafetyAgent::Proposal p;
    p.target = { { "truePeakMaxDb", -1.0 } };
    auto verdict = agent.evaluate(p);
    REQUIRE_FALSE(verdict.approved);
    REQUIRE(verdict.reason == "true_peak_breach");
}
