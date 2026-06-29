#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include "Plugin/PluginProcessor.h"
#include "AI/Orchestrator/AgentOrchestrator.h"

using namespace more_phi;
using namespace more_phi::orchestrator;

TEST_CASE("AgentOrchestrator can be constructed", "[orchestrator]")
{
    MorePhiProcessor processor;
    AgentOrchestrator orchestrator(processor);
    (void)orchestrator;
}

TEST_CASE("AgentOrchestrator start returns true and describeSystemState shows running", "[orchestrator]")
{
    MorePhiProcessor processor;
    AgentOrchestrator orchestrator(processor);

    REQUIRE(orchestrator.start());

    auto state = orchestrator.describeSystemState();
    REQUIRE(state.is_object());
    REQUIRE(state.contains("orchestratorRunning"));
    REQUIRE(state["orchestratorRunning"].get<bool>() == true);
    REQUIRE(state.contains("agentCount"));
    REQUIRE(state["agentCount"].get<int>() == 6);
    REQUIRE(state.contains("agentStates"));
    REQUIRE(state["agentStates"].is_array());
    REQUIRE(state["agentStates"].size() == 6);
    REQUIRE(state.contains("schedulerStats"));
}

TEST_CASE("AgentOrchestrator stop is idempotent", "[orchestrator]")
{
    MorePhiProcessor processor;
    AgentOrchestrator orchestrator(processor);

    orchestrator.start();
    orchestrator.stop();
    orchestrator.stop(); // must not crash or throw

    auto state = orchestrator.describeSystemState();
    REQUIRE(state["orchestratorRunning"].get<bool>() == false);
}

TEST_CASE("AgentOrchestrator submitUserGoal returns empty when not running", "[orchestrator]")
{
    MorePhiProcessor processor;
    AgentOrchestrator orchestrator(processor);

    juce::String result = orchestrator.submitUserGoal("test goal");
    REQUIRE(result.isEmpty());
}

TEST_CASE("AgentOrchestrator describeSystemState returns valid JSON before start", "[orchestrator]")
{
    MorePhiProcessor processor;
    AgentOrchestrator orchestrator(processor);

    auto state = orchestrator.describeSystemState();
    REQUIRE(state.is_object());
    REQUIRE(state.contains("orchestratorRunning"));
    REQUIRE(state["orchestratorRunning"].get<bool>() == false);
    REQUIRE(state.contains("mcpServerRunning"));
    REQUIRE(state.contains("mcpHealthy"));
    REQUIRE(state.contains("mcpPort"));
    REQUIRE(state.contains("agentCount"));
    REQUIRE(state["agentCount"].get<int>() == 0);
    REQUIRE(state.contains("agentStates"));
    REQUIRE(state["agentStates"].is_array());
    REQUIRE(state.contains("schedulerStats"));
}
