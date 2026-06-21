#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <juce_core/juce_core.h>

#include <chrono>
#include <thread>

#include "AI/Agents/Conductor/ConductorAgent.h"
#include "AI/Agents/AgentRuntime.h"
#include "AI/Agents/Logging/NullAgentLogger.h"
#include "AI/Agents/Llm/DeterministicFallbackLlmClient.h"
#include "AI/AutomationControlPlane.h"

using namespace more_phi::agents;
using Catch::Approx;
using namespace std::chrono_literals;

namespace {

struct ScopedStore
{
    ScopedStore()
    {
        dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getNonexistentChildFile("morephi_conductor_test", "");
        more_phi::AutomationRuntime::setStoreDirectoryOverrideForTests(dir);
    }
    ~ScopedStore()
    {
        more_phi::AutomationRuntime::clearStoreDirectoryOverrideForTests();
        dir.deleteRecursively();
    }
    juce::File dir;
};

// Minimal stub specialist that just succeeds.
class EchoSpecialist : public IAgent
{
public:
    explicit EchoSpecialist(AgentRole r) : role_(r) {}
    AgentRole role() const noexcept override { return role_; }
    juce::String id() const noexcept override { return id_; }
    void setId(const juce::String& id) { id_ = id; }
    std::vector<juce::String> allowedTools() const override { return { "analysis.get_summary" }; }
    void prepare(const AgentContext& ctx) override { ctx_ = &ctx; }
    AgentResult execute(const AgentTask& task) override
    {
        AgentResult r;
        r.taskId = task.id;
        r.success = true;
        r.findings = { { "did", task.intent.toStdString() } };
        return r;
    }
    AgentState state() const noexcept override { return AgentState::Idle; }
    void stop() override {}
    AgentRole role_;
    juce::String id_ = "echo";
    const AgentContext* ctx_ = nullptr;
};

} // namespace

TEST_CASE("Conductor decomposes a streaming-mastering goal and delegates", "[agents][conductor]")
{
    ScopedStore store;
    more_phi::AutomationRuntime runtime;
    more_phi::IntegrationEventBus bus{64};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;
    DeterministicFallbackLlmClient llm;

    DefaultToolInvoker::DispatchFn dispatch = [&](const juce::String& method, const nlohmann::json& /*params*/) {
        return juce::String(nlohmann::json({ { "ok", true }, { "method", method.toStdString() } }).dump());
    };
    DefaultToolInvoker::CapabilityFn cap = [](const juce::String&) {
        return std::vector<juce::String>{ "workflow.submit", "workflow.execute",
                                          "set_parameters_batch", "analysis.get_summary" };
    };
    DefaultToolInvoker invoker{dispatch, cap};

    AgentRuntime rt{nullptr, nullptr, &runtime, invoker, bb, logger, &llm};
    rt.registerAgent(std::make_unique<ConductorAgent>());
    auto analysis = std::make_unique<EchoSpecialist>(AgentRole::Analysis);
    analysis->setId("analysis-1");
    rt.registerAgent(std::move(analysis));
    auto memory = std::make_unique<EchoSpecialist>(AgentRole::Memory);
    memory->setId("memory-1");
    rt.registerAgent(std::move(memory));
    auto opt = std::make_unique<EchoSpecialist>(AgentRole::Optimization);
    opt->setId("opt-1");
    rt.registerAgent(std::move(opt));
    rt.start(2);

    juce::String runId = rt.submitGoal("master for streaming, keep it warm");
    REQUIRE(runId.isNotEmpty());

    // Wait for the conductor task to complete.
    std::optional<AgentResult> result;
    for (int i = 0; i < 400 && ! result.has_value(); ++i)
    {
        result = rt.peekResult(runId);
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(result.has_value());
    REQUIRE(result->success);

    rt.stop();
}
