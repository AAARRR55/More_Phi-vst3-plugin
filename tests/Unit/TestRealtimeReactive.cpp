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

// Captures the more_phi.set_parameter tool call so we can assert the reaction happened.
struct Capture
{
    std::atomic<bool> invoked{false};
    std::atomic<float> value{0.0f};
    std::atomic<bool> sawParamId{false};
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
    // C3: the reactive agent now trims the More-Phi OUTPUT GAIN via
    // more_phi.set_parameter (by parameter_id), not a hosted-plugin index.
    DefaultToolInvoker::DispatchFn dispatch = [&](const juce::String& method, const nlohmann::json& params) {
        if (method == "more_phi.set_parameter")
        {
            cap.invoked.store(true);
            cap.value.store(static_cast<float>(params.value("value", 0.0)));
            // Record that we received a parameter_id (the configured output gain name).
            cap.sawParamId.store(params.value("parameter_id", std::string{}) == "outputGain");
        }
        return juce::String(nlohmann::json({ {"ok", true} }).dump());
    };
    DefaultToolInvoker::CapabilityFn capFn = [](const juce::String&) {
        return std::vector<juce::String>{ "more_phi.set_parameter" };
    };
    DefaultToolInvoker invoker{dispatch, capFn};

    AgentRuntime agentRuntime(nullptr, nullptr, &runtime, invoker, bb, logger, nullptr);

    auto rt = std::make_unique<RealtimeControlAgent>();
    RealtimeControlAgent::Config cfg;
    cfg.outputGainParamName = "outputGain";   // C3: target by parameter id
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
    REQUIRE(cap.sawParamId.load());        // routed to the configured output-gain param by id
    REQUIRE(cap.value.load() < 1.0f);      // absolute normalized target, trimmed below unity

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
        return std::vector<juce::String>{ "more_phi.set_parameter" };
    };
    DefaultToolInvoker invoker{dispatch, capFn};

    AgentRuntime agentRuntime(nullptr, nullptr, &runtime, invoker, bb, logger, nullptr);
    auto rt = std::make_unique<RealtimeControlAgent>();
    RealtimeControlAgent::Config cfg; cfg.outputGainParamName = "outputGain";
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

namespace {
// Captures the thread id the tool is invoked on, so we can prove the reactive
// correction is delivered SYNCHRONOUSLY on the calling (pump) thread — NOT
// deferred via MessageManager::callAsync, which drops on headless/offline-render
// hosts. Regression for the M5 doc-vs-code footgun: the header used to claim the
// callAsync path; the code invokes synchronously. This pins the synchronous path.
struct ThreadCapturingInvoker : public IToolInvoker
{
    std::atomic<bool>   invoked{false};
    std::atomic<float>  value{0.0f};
    juce::String        lastTool;
    std::thread::id     callerThread{};
    std::atomic<bool>   sameThread{false};
    nlohmann::json invoke(const juce::String& tool, const nlohmann::json& params, const juce::String&) override
    {
        lastTool = tool;
        if (tool == "more_phi.set_parameter")
        {
            invoked.store(true);
            value.store(static_cast<float>(params.value("value", 0.0)));
            sameThread.store(std::this_thread::get_id() == callerThread);
        }
        return { {"ok", true} };
    }
};
} // namespace

TEST_CASE("RealtimeControlAgent delivers corrections synchronously (no callAsync)", "[agents][reactive]")
{
    // If delivery were via MessageManager::callAsync, the tool would be invoked
    // later on the JUCE message thread (a different id) and the assertions below
    // would fail. Driving onEvent directly on this thread proves synchronous
    // delivery — the path that keeps output protection alive under offline render.
    ScopedStore fx;
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;

    ThreadCapturingInvoker fake;
    fake.callerThread = std::this_thread::get_id();

    AgentContext ctx;
    ctx.tools = &fake;
    ctx.blackboard = &bb;
    ctx.logger = &logger;

    RealtimeControlAgent agent;
    RealtimeControlAgent::Config cfg;
    cfg.outputGainParamName = "outputGain";
    cfg.clipTrimStepDb = 1.5f;
    agent.setConfig(cfg);
    agent.prepare(ctx);

    agent.onEvent("analysis.clipping_detected", { {"true_peak_db", 0.4} }, "analysis-1", "run-1");

    REQUIRE(fake.invoked.load());
    REQUIRE(fake.sameThread.load());          // synchronous, same thread — no callAsync hop
    REQUIRE(fake.lastTool == "more_phi.set_parameter");
    REQUIRE(fake.value.load() < 1.0f);        // absolute normalized target, trimmed below unity
    agent.stop();
}
