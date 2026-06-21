#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

#include "AI/Agents/Agents/MemoryAgent.h"
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
    { return { { "ok", true } }; }
};

struct ScopedStore
{
    ScopedStore()
    {
        dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getNonexistentChildFile("morephi_memory_test", "");
        more_phi::AutomationRuntime::setStoreDirectoryOverrideForTests(dir);
    }
    ~ScopedStore()
    {
        more_phi::AutomationRuntime::clearStoreDirectoryOverrideForTests();
        dir.deleteRecursively();
    }
    juce::File dir;
};
} // namespace

TEST_CASE("MemoryAgent writes a workflow-level outcome on task with runId", "[agents][memory]")
{
    ScopedStore store;
    more_phi::AutomationRuntime runtime;
    FakeInvoker fake;
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;

    AgentContext ctx;
    ctx.tools = &fake;
    ctx.runtime = &runtime;
    ctx.blackboard = &bb;
    ctx.logger = &logger;

    MemoryAgent agent;
    agent.prepare(ctx);

    AgentTask task;
    task.id = "m1";
    task.intent = "record outcome for run R1";
    task.payload = { { "runId", "R1" }, { "success", true }, { "score", 0.8 } };
    AgentResult r = agent.execute(task);

    REQUIRE(r.success);
    // Exactly one workflow-level outcome recorded for R1.
    auto outcomes = runtime.memory().listOutcomes("R1");
    REQUIRE(outcomes.size() == 1);
}

TEST_CASE("MemoryAgent surfaces prior context via intentContext", "[agents][memory]")
{
    ScopedStore store;
    more_phi::AutomationRuntime runtime;
    FakeInvoker fake;
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;

    AgentContext ctx;
    ctx.tools = &fake;
    ctx.runtime = &runtime;
    ctx.blackboard = &bb;
    ctx.logger = &logger;

    MemoryAgent agent;
    agent.prepare(ctx);

    AgentTask task;
    task.id = "m2";
    task.intent = "recall relevant priors";
    AgentResult r = agent.execute(task);

    REQUIRE(r.success);
    REQUIRE(r.findings.contains("intentContext"));
}
