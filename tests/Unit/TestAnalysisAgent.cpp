#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <juce_core/juce_core.h>

#include "AI/Agents/Agents/AnalysisAgent.h"
#include "AI/Agents/AgentContext.h"
#include "AI/Agents/Tooling/DefaultToolInvoker.h"
#include "AI/Agents/Blackboard/BlackboardBridge.h"
#include "AI/Agents/Logging/NullAgentLogger.h"
#include "AI/AutomationControlPlane.h"

using namespace more_phi::agents;
using Catch::Approx;

namespace {

struct FakeInvoker
{
    nlohmann::json summary = { { "lufs_integrated", -9.2 }, { "true_peak_db", -0.3 } };
    nlohmann::json spectrum = { { "tilt_db", 2.0 } };
    nlohmann::json invoke(const juce::String& tool, const nlohmann::json&, const juce::String&) const
    {
        if (tool == "analysis.get_summary")  return summary;
        if (tool == "analysis.get_spectrum") return spectrum;
        return { { "error", { { "code", "unknown_tool" } } } };
    }
};

class FakeToolInvokerAdapter : public IToolInvoker
{
public:
    explicit FakeToolInvokerAdapter(FakeInvoker& f) : f_(f) {}
    nlohmann::json invoke(const juce::String& t, const nlohmann::json& p, const juce::String& a) override
    { return f_.invoke(t, p, a); }
private:
    FakeInvoker& f_;
};

} // namespace

TEST_CASE("AnalysisAgent is read-only and refuses write tools", "[agents][analysis]")
{
    FakeInvoker fake;
    FakeToolInvokerAdapter invoker{fake};
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;

    AgentContext ctx;
    ctx.tools = &invoker;
    ctx.blackboard = &bb;
    ctx.logger = &logger;

    AnalysisAgent agent;
    agent.prepare(ctx);

    REQUIRE(agent.allowedTools() == std::vector<juce::String>{
        "analysis.get_summary", "analysis.get_spectrum", "analysis.get_stereo_field",
        "ozone.track.analyze", "get_mastering_state", "analysis.capture_window",
        "analysis.compare_render", "sonicmaster_decision" });

    AgentTask task;
    task.id = "a1";
    task.intent = "analyze current state";
    AgentResult r = agent.execute(task);

    REQUIRE(r.success);
    REQUIRE(r.findings["lufs_integrated"].get<double>() == Approx(-9.2));
    REQUIRE(r.findings["tilt_db"].get<double>() == Approx(2.0));
    REQUIRE(r.emitEvents.size() >= 1);
    REQUIRE(r.emitEvents[0].type == "analysis.finding");
}

TEST_CASE("AnalysisAgent allowedTools excludes write tools", "[agents][analysis]")
{
    // Capability scope is enforced by DefaultToolInvoker, not the agent itself.
    // Here we verify the agent's allowedTools() does NOT include set_parameter.
    AnalysisAgent agent;
    auto tools = agent.allowedTools();
    bool hasSet = false;
    for (const auto& t : tools)
        if (t == "set_parameter") hasSet = true;
    REQUIRE_FALSE(hasSet);
}
