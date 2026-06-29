#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

#include <chrono>
#include <thread>

#include "AI/AutomationControlPlane.h"
#include "AI/Agents/AgentRuntime.h"
#include "AI/Agents/Logging/NullAgentLogger.h"

using namespace more_phi::agents;
using namespace std::chrono_literals;

namespace {

struct ScopedStore
{
    ScopedStore()
    {
        dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getNonexistentChildFile("morephi_agent_iso_test", "");
        more_phi::AutomationRuntime::setStoreDirectoryOverrideForTests(dir);
    }
    ~ScopedStore()
    {
        more_phi::AutomationRuntime::clearStoreDirectoryOverrideForTests();
        dir.deleteRecursively();
    }
    juce::File dir;
};

// A specialist that tries to hand work to another specialist (FORBIDDEN by D-isolation).
class RogueSpecialist : public IAgent
{
public:
    AgentRole role() const noexcept override { return AgentRole::Optimization; }
    juce::String id() const noexcept override { return "rogue-opt"; }
    std::vector<juce::String> allowedTools() const override { return { "set_parameter" }; }
    void prepare(const AgentContext&) override {}
    AgentResult execute(const AgentTask& task) override
    {
        AgentResult r;
        r.taskId = task.id;
        r.success = true;
        // Attempt a forbidden cross-specialist delegation.
        AgentTask hop;
        hop.id = "hop1";
        hop.targetRole = AgentRole::Creative;
        hop.intent = "should be dropped";
        r.followUps.push_back(hop);
        return r;
    }
    AgentState state() const noexcept override { return AgentState::Idle; }
    void stop() override {}
};

// A Creative agent that records whether it ever executes (it should NOT, because
// the rogue's followUp must be dropped, and no Conductor ever runs in this test).
class RecordingCreative : public IAgent
{
public:
    AgentRole role() const noexcept override { return AgentRole::Creative; }
    juce::String id() const noexcept override { return "creative-1"; }
    std::vector<juce::String> allowedTools() const override { return { "capture_snapshot" }; }
    void prepare(const AgentContext&) override {}
    AgentResult execute(const AgentTask& task) override
    {
        ++execCount;
        AgentResult r;
        r.taskId = task.id;
        r.success = true;
        return r;
    }
    AgentState state() const noexcept override { return AgentState::Idle; }
    void stop() override {}

    std::atomic<int> execCount{0};
};

} // namespace

TEST_CASE("Specialist cross-delegation is dropped", "[agents][isolation]")
{
    ScopedStore fx;
    more_phi::IntegrationEventBus bus{32};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;
    DefaultToolInvoker::DispatchFn dispatch = [](const juce::String&, const nlohmann::json&) {
        return juce::String(nlohmann::json({ { "ok", true } }).dump());
    };
    DefaultToolInvoker::CapabilityFn cap = [](const juce::String&) {
        return std::vector<juce::String>{ "set_parameter", "capture_snapshot" };
    };
    DefaultToolInvoker invoker{dispatch, cap};

    AgentRuntime runtime{nullptr, nullptr, nullptr, invoker, bb, logger, nullptr};
    runtime.registerAgent(std::make_unique<RogueSpecialist>());
    auto creative = std::make_unique<RecordingCreative>();
    RecordingCreative* creativePtr = creative.get();
    runtime.registerAgent(std::move(creative));
    runtime.start(2);

    // Drive the rogue specialist directly.
    AgentTask rogue;
    rogue.id = "rogue-task";
    rogue.targetRole = AgentRole::Optimization;
    rogue.intent = "do something";
    juce::String rogueId = runtime.submitTask(rogue);

    for (int i = 0; i < 200; ++i)
    {
        if (runtime.peekResult(rogueId).has_value())
            break;
        std::this_thread::sleep_for(5ms);
    }
    // Allow follow-up processing time + blackboard pump.
    std::this_thread::sleep_for(100ms);

    // The rogue's forbidden followUp must NOT have caused Creative to execute.
    REQUIRE(creativePtr->execCount.load() == 0);

    runtime.stop();
}
