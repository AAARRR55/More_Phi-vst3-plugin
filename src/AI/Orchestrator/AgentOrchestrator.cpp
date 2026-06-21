// src/AI/Orchestrator/AgentOrchestrator.cpp
#include "AI/Orchestrator/AgentOrchestrator.h"

#include "Plugin/PluginProcessor.h"
#include "AI/MCPServer.h"
#include "AI/MCPToolHandler.h"

#include "AI/Agents/Conductor/ConductorAgent.h"
#include "AI/Agents/Agents/AnalysisAgent.h"
#include "AI/Agents/Agents/OptimizationAgent.h"
#include "AI/Agents/Agents/CreativeAgent.h"
#include "AI/Agents/Agents/RealtimeControlAgent.h"
#include "AI/Agents/Agents/QualitySafetyAgent.h"

#include <juce_core/juce_core.h>

namespace more_phi::orchestrator
{

AgentOrchestrator::AgentOrchestrator (MorePhiProcessor& processor)
    : processor_ (processor)
{
}

AgentOrchestrator::~AgentOrchestrator()
{
    stop();
}

bool AgentOrchestrator::start()
{
    if (running_.exchange (true))
    {
        juce::Logger::writeToLog ("AgentOrchestrator::start: already running");
        return true;
    }

    try
    {
        // 1. BlackboardBridge over the existing IntegrationEventBus.
        blackboardBridge_ = std::make_unique<agents::BlackboardBridge> (
            processor_.getMCPServer().getAutomationRuntime().events());

        // 2. Tool invoker wrapping MCPToolHandler::handle.
        auto dispatch = [this] (const juce::String& method, const nlohmann::json& params) -> juce::String
        {
            juce::var paramsVar;
            try
            {
                if (params.is_object() || params.is_array())
                    paramsVar = juce::JSON::parse (juce::String (params.dump()));
                else
                    paramsVar = juce::var();
            }
            catch (...)
            {
                paramsVar = juce::var();
            }
            return MCPToolHandler::handle (method, paramsVar,
                                             processor_,
                                             processor_.getInstanceIdentity(),
                                             processor_.getMCPServer().getAutomationRuntime());
        };

        auto capability = [this] (const juce::String& agentId) -> std::vector<juce::String>
        {
            if (! agentRuntime_)
                return {};
            const auto roles = agentRuntime_->registry().registeredRoles();
            for (auto role : roles)
            {
                if (auto* agent = agentRuntime_->registry().find (role))
                {
                    if (agent->id() == agentId)
                        return agent->allowedTools();
                }
            }
            return {};
        };

        toolInvoker_ = std::make_unique<agents::DefaultToolInvoker> (dispatch, capability, 0);

        // 3. AgentRuntime.
        agentRuntime_ = std::make_unique<agents::AgentRuntime> (
            &processor_,
            &processor_.getInstanceIdentity(),
            &processor_.getMCPServer().getAutomationRuntime(),
            *toolInvoker_,
            *blackboardBridge_,
            logger_,
            nullptr   // ILlmClient: agents use deterministic fallback when null
        );

        // 4. Register all 6 built-in agents.
        auto registerAgent = [this] (std::unique_ptr<agents::IAgent> agent)
        {
            if (! agentRuntime_->registerAgent (std::move (agent)))
            {
                juce::Logger::writeToLog ("AgentOrchestrator: failed to register agent");
            }
        };

        registerAgent (std::make_unique<agents::ConductorAgent>());
        registerAgent (std::make_unique<agents::AnalysisAgent>());

        auto optAgent = std::make_unique<agents::OptimizationAgent>();
        registerAgent (std::move (optAgent));

        registerAgent (std::make_unique<agents::CreativeAgent>());

        auto rtAgent = std::make_unique<agents::RealtimeControlAgent>();
        rtAgent->setConfig (agents::RealtimeControlAgent::Config{});
        registerAgent (std::move (rtAgent));

        auto qsAgent = std::make_unique<agents::QualitySafetyAgent>();
        qsAgent->setConfig (agents::QualitySafetyAgent::Config{});
        registerAgent (std::move (qsAgent));

        // 5. Start scheduler and blackboard pump.
        agentRuntime_->start (2);

        // 6. Start MCP server if not already running (graceful degradation: if it fails, agents still work locally).
        if (! processor_.getMCPServer().isRunning())
        {
            processor_.getMCPServer().startServer (processor_.getInstanceIdentity().port);
        }

        juce::Logger::writeToLog ("AgentOrchestrator::start: success");
        return true;
    }
    catch (const std::exception& e)
    {
        juce::Logger::writeToLog ("AgentOrchestrator::start exception: " + juce::String (e.what()));
        stop();
        return false;
    }
}

void AgentOrchestrator::stop()
{
    if (! running_.exchange (false))
        return;

    juce::Logger::writeToLog ("AgentOrchestrator::stop");

    if (agentRuntime_)
        agentRuntime_->stop();

    if (processor_.getMCPServer().isRunning())
        processor_.getMCPServer().stopServer();

    agentRuntime_.reset();
    toolInvoker_.reset();
    blackboardBridge_.reset();
}

juce::String AgentOrchestrator::submitUserGoal (const juce::String& intent)
{
    if (! running_.load() || ! agentRuntime_)
    {
        juce::Logger::writeToLog ("AgentOrchestrator::submitUserGoal: not running");
        return juce::String{};
    }
    return agentRuntime_->submitGoal (intent);
}

nlohmann::json AgentOrchestrator::describeSystemState() const
{
    nlohmann::json state = nlohmann::json::object();
    state["orchestratorRunning"] = running_.load();

    const auto& mcp = processor_.getMCPServer();
    state["mcpServerRunning"] = mcp.isRunning();
    state["mcpHealthy"]       = mcp.isHealthy();
    state["mcpPort"]          = mcp.getPort();

    if (agentRuntime_)
    {
        const auto roles = agentRuntime_->registry().registeredRoles();
        state["agentCount"] = static_cast<int> (roles.size());

        nlohmann::json agentStates = nlohmann::json::array();
        for (auto role : roles)
        {
            if (auto* agent = agentRuntime_->registry().find (role))
            {
                nlohmann::json a;
                a["role"]  = agents::toString (role).toStdString();
                a["id"]    = agent->id().toStdString();
                a["state"] = agents::toString (agent->state()).toStdString();
                agentStates.push_back (a);
            }
        }
        state["agentStates"] = agentStates;

        auto runtimeState = agentRuntime_->describeState();
        if (runtimeState.contains ("scheduler"))
            state["schedulerStats"] = runtimeState["scheduler"];
        else
            state["schedulerStats"] = nlohmann::json::object();
    }
    else
    {
        state["agentCount"]    = 0;
        state["agentStates"]   = nlohmann::json::array();
        state["schedulerStats"] = nlohmann::json::object();
    }

    return state;
}

} // namespace more_phi::orchestrator
