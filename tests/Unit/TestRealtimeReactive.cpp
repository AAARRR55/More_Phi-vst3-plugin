// Integration test for the reactive agent path: an event published on the
// blackboard reaches a subscribed agent's onEvent via the runtime's pump.
// This locks in the AgentRuntime::start() subscription wiring (without it,
// AnalysisAgent → RealtimeControlAgent is dead).

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
#include "AI/Agents/Agents/RealtimeControlAgent.h"

using namespace more_phi::agents;
using namespace std::chrono_literals;

namespace {

struct ScopedStore
{
    ScopedStore()
    {
        dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getNonexistentChildFile("morephi_reactive_test", "");
        more_phi::AutomationRuntime::setStoreDirectoryOverrideForTests(dir);
    }
    ~ScopedStore()
    {
        more_phi::AutomationRuntime::clearStoreDirectoryOverrideForTests();
        dir.deleteRecursively();
    }
    juce::File dir;
};

// Captures the set_parameter tool call so we can assert the reaction happened.
struct Capture
{
    std::atomic<bool> invoked{false};
    std::atomic<int>  param{-1};
    std::atomic<float> value{0.0f};
};

} // namespace

TEST_CASE("Reactive: blackboard clipping event reaches RealtimeControlAgent", "[agents][reactive]")
{
    ScopedStore fx;
    more_phi::AutomationRuntime runtime;
    more_phi::IntegrationEventBus bus{32};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;

    Capture cap;
    DefaultToolInvoker::DispatchFn dispatch = [&](const juce::String& method, const nlohmann::json& params) {
        if (method == "set_parameter")
        {
            cap.invoked.store(true);
            cap.param.store(params.value("index", -1));
            cap.value.store(static_cast<float>(params.value("value", 0.0)));
        }
        return juce::String(nlohmann::json({ {"ok", true} }).dump());
    };
    DefaultToolInvoker::CapabilityFn capFn = [](const juce::String&) {
        return std::vector<juce::String>{ "set_parameter" };
    };
    DefaultToolInvoker invoker{dispatch, capFn};

    AgentRuntime agentRuntime(nullptr, nullptr, &runtime, invoker, bb, logger, nullptr);

    auto rt = std::make_unique<RealtimeControlAgent>();
    RealtimeControlAgent::Config cfg;
    cfg.outputGainParamIndex = 7;
    cfg.clipTrimStepDb = 1.5f;
    rt->setConfig(cfg);
    agentRuntime.registerAgent(std::move(rt));
    agentRuntime.start(1);

    // Publish the clipping event on the blackboard.
    bb.publish("analysis-1", "analysis.clipping_detected",
               { { "true_peak_db", 0.4 } }, "run-1");

    // The runtime's pump thread fans it out to the subscriber. Poll for the reaction.
    for (int i = 0; i < 300 && ! cap.invoked.load(); ++i)
        std::this_thread::sleep_for(2ms);

    REQUIRE(cap.invoked.load());
    REQUIRE(cap.param.load() == 7);          // routed to the configured output-gain param

    agentRuntime.stop();
}

TEST_CASE("Reactive: unrelated event type is ignored by RealtimeControlAgent", "[agents][reactive]")
{
    ScopedStore fx;
    more_phi::AutomationRuntime runtime;
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;

    Capture cap;
    DefaultToolInvoker::DispatchFn dispatch = [&](const juce::String&, const nlohmann::json&) {
        cap.invoked.store(true);
        return juce::String(nlohmann::json({ {"ok", true} }).dump());
    };
    DefaultToolInvoker::CapabilityFn capFn = [](const juce::String&) {
        return std::vector<juce::String>{ "set_parameter" };
    };
    DefaultToolInvoker invoker{dispatch, capFn};

    AgentRuntime agentRuntime(nullptr, nullptr, &runtime, invoker, bb, logger, nullptr);
    auto rt = std::make_unique<RealtimeControlAgent>();
    RealtimeControlAgent::Config cfg; cfg.outputGainParamIndex = 0;
    rt->setConfig(cfg);
    agentRuntime.registerAgent(std::move(rt));
    agentRuntime.start(1);

    // An event the agent did NOT subscribe to must not trigger a correction.
    bb.publish("memory-1", "memory.stored", { { "k", "v" } }, "run-1");

    for (int i = 0; i < 60; ++i)   // brief window; reaction must never fire
        std::this_thread::sleep_for(2ms);

    REQUIRE(! cap.invoked.load());

    agentRuntime.stop();
}
