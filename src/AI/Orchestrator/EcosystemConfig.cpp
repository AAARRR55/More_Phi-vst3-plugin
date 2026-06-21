/*
 * More-Phi — AI/Orchestrator/EcosystemConfig.cpp
 * Implementation of unified ecosystem configuration.
 */
#include "AI/Orchestrator/EcosystemConfig.h"
#include <fstream>
#include <limits>

namespace more_phi::orchestrator {

//==============================================================================
// MCPConfig
//==============================================================================
nlohmann::json EcosystemConfig::MCPConfig::toJson() const
{
    nlohmann::json j;
    j["port"]            = port;
    j["authToken"]       = authToken.toStdString();
    j["maxConnections"]  = maxConnections;
    j["idleTimeoutMs"]   = idleTimeoutMs;
    j["maxRequestBytes"] = maxRequestBytes;
    j["enableServer"]    = enableServer;
    return j;
}

bool EcosystemConfig::MCPConfig::fromJson(const nlohmann::json& j, juce::String& outError)
{
    try
    {
        if (j.contains("port"))            port = j.at("port").get<int>();
        if (j.contains("authToken"))       authToken = juce::String(j.at("authToken").get<std::string>());
        if (j.contains("maxConnections"))  maxConnections = j.at("maxConnections").get<int>();
        if (j.contains("idleTimeoutMs"))   idleTimeoutMs = j.at("idleTimeoutMs").get<int>();
        if (j.contains("maxRequestBytes"))   maxRequestBytes = j.at("maxRequestBytes").get<int>();
        if (j.contains("enableServer"))    enableServer = j.at("enableServer").get<bool>();
    }
    catch (const nlohmann::json::exception& e)
    {
        outError = "MCPConfig parse error: " + juce::String(e.what());
        return false;
    }
    return true;
}

bool EcosystemConfig::MCPConfig::validate(juce::String& outError) const
{
    if (port < 1024 || port > 65535)
    {
        outError = "MCP port must be in range [1024, 65535] (got " + juce::String(port) + ")";
        return false;
    }
    if (maxConnections < 1 || maxConnections > 256)
    {
        outError = "MCP maxConnections must be in [1, 256] (got " + juce::String(maxConnections) + ")";
        return false;
    }
    if (idleTimeoutMs < 1000 || idleTimeoutMs > 300000)
    {
        outError = "MCP idleTimeoutMs must be in [1000, 300000] (got " + juce::String(idleTimeoutMs) + ")";
        return false;
    }
    if (maxRequestBytes < 1024 || maxRequestBytes > 10485760)
    {
        outError = "MCP maxRequestBytes must be in [1024, 10485760] (got " + juce::String(maxRequestBytes) + ")";
        return false;
    }
    if (authToken.isEmpty())
    {
        outError = "MCP authToken must not be empty";
        return false;
    }
    return true;
}

//==============================================================================
// AgentConfig
//==============================================================================
nlohmann::json EcosystemConfig::AgentConfig::toJson() const
{
    nlohmann::json j;
    j["numWorkers"]                = numWorkers;
    j["blackboardPollIntervalMs"]  = blackboardPollIntervalMs;
    j["maxCallsPerAgentPerSecond"] = maxCallsPerAgentPerSecond;
    j["enableAllAgents"]           = enableAllAgents;
    return j;
}

bool EcosystemConfig::AgentConfig::fromJson(const nlohmann::json& j, juce::String& outError)
{
    try
    {
        if (j.contains("numWorkers"))                numWorkers = j.at("numWorkers").get<int>();
        if (j.contains("blackboardPollIntervalMs"))  blackboardPollIntervalMs = j.at("blackboardPollIntervalMs").get<int>();
        if (j.contains("maxCallsPerAgentPerSecond")) maxCallsPerAgentPerSecond = j.at("maxCallsPerAgentPerSecond").get<int>();
        if (j.contains("enableAllAgents"))           enableAllAgents = j.at("enableAllAgents").get<bool>();
    }
    catch (const nlohmann::json::exception& e)
    {
        outError = "AgentConfig parse error: " + juce::String(e.what());
        return false;
    }
    return true;
}

bool EcosystemConfig::AgentConfig::validate(juce::String& outError) const
{
    if (numWorkers < 1 || numWorkers > 64)
    {
        outError = "Agent numWorkers must be in [1, 64] (got " + juce::String(numWorkers) + ")";
        return false;
    }
    if (blackboardPollIntervalMs < 1 || blackboardPollIntervalMs > 5000)
    {
        outError = "Agent blackboardPollIntervalMs must be in [1, 5000] (got " + juce::String(blackboardPollIntervalMs) + ")";
        return false;
    }
    if (maxCallsPerAgentPerSecond < 1 || maxCallsPerAgentPerSecond > 10000)
    {
        outError = "Agent maxCallsPerAgentPerSecond must be in [1, 10000] (got " + juce::String(maxCallsPerAgentPerSecond) + ")";
        return false;
    }
    return true;
}

//==============================================================================
// SecurityConfig
//==============================================================================
nlohmann::json EcosystemConfig::SecurityConfig::toJson() const
{
    nlohmann::json j;
    j["maxJsonDepth"]      = maxJsonDepth;
    j["maxJsonSize"]       = maxJsonSize;

    nlohmann::json methods = nlohmann::json::array();
    for (const auto& m : allowedMethods)
        methods.push_back(m.toStdString());
    j["allowedMethods"]    = methods;

    j["rateLimitPerMinute"] = rateLimitPerMinute;
    return j;
}

bool EcosystemConfig::SecurityConfig::fromJson(const nlohmann::json& j, juce::String& outError)
{
    try
    {
        if (j.contains("maxJsonDepth"))      maxJsonDepth = j.at("maxJsonDepth").get<int>();
        if (j.contains("maxJsonSize"))     maxJsonSize = j.at("maxJsonSize").get<int>();
        if (j.contains("rateLimitPerMinute")) rateLimitPerMinute = j.at("rateLimitPerMinute").get<int>();

        if (j.contains("allowedMethods") && j.at("allowedMethods").is_array())
        {
            allowedMethods.clear();
            for (const auto& item : j.at("allowedMethods"))
                allowedMethods.emplace_back(item.get<std::string>());
        }
    }
    catch (const nlohmann::json::exception& e)
    {
        outError = "SecurityConfig parse error: " + juce::String(e.what());
        return false;
    }
    return true;
}

bool EcosystemConfig::SecurityConfig::validate(juce::String& outError) const
{
    if (maxJsonDepth < 1 || maxJsonDepth > 128)
    {
        outError = "Security maxJsonDepth must be in [1, 128] (got " + juce::String(maxJsonDepth) + ")";
        return false;
    }
    if (maxJsonSize < 1024 || maxJsonSize > 10485760)
    {
        outError = "Security maxJsonSize must be in [1024, 10485760] (got " + juce::String(maxJsonSize) + ")";
        return false;
    }
    if (allowedMethods.empty())
    {
        outError = "Security allowedMethods must not be empty";
        return false;
    }
    if (rateLimitPerMinute < 1 || rateLimitPerMinute > 10000)
    {
        outError = "Security rateLimitPerMinute must be in [1, 10000] (got " + juce::String(rateLimitPerMinute) + ")";
        return false;
    }
    return true;
}

//==============================================================================
// PluginConfig
//==============================================================================
nlohmann::json EcosystemConfig::PluginConfig::toJson() const
{
    nlohmann::json j;
    j["defaultMorphX"]          = defaultMorphX;
    j["defaultMorphY"]          = defaultMorphY;
    j["enableAudioDomainMorph"] = enableAudioDomainMorph;
    j["defaultPhysicsMode"]     = defaultPhysicsMode;
    return j;
}

bool EcosystemConfig::PluginConfig::fromJson(const nlohmann::json& j, juce::String& outError)
{
    try
    {
        if (j.contains("defaultMorphX"))          defaultMorphX = j.at("defaultMorphX").get<float>();
        if (j.contains("defaultMorphY"))          defaultMorphY = j.at("defaultMorphY").get<float>();
        if (j.contains("enableAudioDomainMorph")) enableAudioDomainMorph = j.at("enableAudioDomainMorph").get<bool>();
        if (j.contains("defaultPhysicsMode"))     defaultPhysicsMode = j.at("defaultPhysicsMode").get<int>();
    }
    catch (const nlohmann::json::exception& e)
    {
        outError = "PluginConfig parse error: " + juce::String(e.what());
        return false;
    }
    return true;
}

bool EcosystemConfig::PluginConfig::validate(juce::String& outError) const
{
    if (defaultMorphX < 0.0f || defaultMorphX > 1.0f)
    {
        outError = "Plugin defaultMorphX must be in [0.0, 1.0] (got " + juce::String(defaultMorphX) + ")";
        return false;
    }
    if (defaultMorphY < 0.0f || defaultMorphY > 1.0f)
    {
        outError = "Plugin defaultMorphY must be in [0.0, 1.0] (got " + juce::String(defaultMorphY) + ")";
        return false;
    }
    if (defaultPhysicsMode < 0 || defaultPhysicsMode > 2)
    {
        outError = "Plugin defaultPhysicsMode must be in [0, 2] (got " + juce::String(defaultPhysicsMode) + ")";
        return false;
    }
    return true;
}

//==============================================================================
// EcosystemConfig (top-level)
//==============================================================================
bool EcosystemConfig::loadFromFile(const juce::File& path)
{
    if (!path.existsAsFile())
    {
        setLastError("Config file does not exist: " + path.getFullPathName());
        return false;
    }

    juce::String content = path.loadFileAsString();
    if (content.isEmpty())
    {
        setLastError("Config file is empty: " + path.getFullPathName());
        return false;
    }

    try
    {
        nlohmann::json j = nlohmann::json::parse(content.toStdString());
        return loadFromJson(j);
    }
    catch (const nlohmann::json::exception& e)
    {
        setLastError("JSON parse error: " + juce::String(e.what()));
        return false;
    }
}

bool EcosystemConfig::loadFromJson(const nlohmann::json& j)
{
    juce::String subError;

    if (j.contains("mcp"))
    {
        if (!mcp.fromJson(j.at("mcp"), subError))
        {
            setLastError("mcp." + subError);
            return false;
        }
    }

    if (j.contains("agents"))
    {
        if (!agents.fromJson(j.at("agents"), subError))
        {
            setLastError("agents." + subError);
            return false;
        }
    }

    if (j.contains("security"))
    {
        if (!security.fromJson(j.at("security"), subError))
        {
            setLastError("security." + subError);
            return false;
        }
    }

    if (j.contains("plugin"))
    {
        if (!plugin.fromJson(j.at("plugin"), subError))
        {
            setLastError("plugin." + subError);
            return false;
        }
    }

    return validate();
}

nlohmann::json EcosystemConfig::toJson() const
{
    nlohmann::json j;
    j["mcp"]      = mcp.toJson();
    j["agents"]   = agents.toJson();
    j["security"] = security.toJson();
    j["plugin"]   = plugin.toJson();
    return j;
}

bool EcosystemConfig::validate() const
{
    juce::String subError;

    if (!mcp.validate(subError))      { setLastError("mcp: " + subError);      return false; }
    if (!agents.validate(subError))   { setLastError("agents: " + subError);   return false; }
    if (!security.validate(subError)) { setLastError("security: " + subError); return false; }
    if (!plugin.validate(subError))   { setLastError("plugin: " + subError);   return false; }

    setLastError({}); // clear on success
    return true;
}

//==============================================================================
// Factory methods
//==============================================================================
EcosystemConfig EcosystemConfig::createDefaults()
{
    EcosystemConfig cfg;

    cfg.mcp.port            = 30001;
    cfg.mcp.authToken       = "changeme-in-production";
    cfg.mcp.maxConnections  = 4;
    cfg.mcp.idleTimeoutMs   = 30000;
    cfg.mcp.maxRequestBytes = 65536;
    cfg.mcp.enableServer    = true;

    cfg.agents.numWorkers                = 2;
    cfg.agents.blackboardPollIntervalMs  = 50;
    cfg.agents.maxCallsPerAgentPerSecond = 10;
    cfg.agents.enableAllAgents           = true;

    cfg.security.maxJsonDepth      = 32;
    cfg.security.maxJsonSize       = 65536;
    cfg.security.allowedMethods    = {
        juce::String("tools/list"),
        juce::String("tools/call"),
        juce::String("initialize"),
        juce::String("initialized")
    };
    cfg.security.rateLimitPerMinute = 60;

    cfg.plugin.defaultMorphX          = 0.5f;
    cfg.plugin.defaultMorphY          = 0.5f;
    cfg.plugin.enableAudioDomainMorph = false;
    cfg.plugin.defaultPhysicsMode     = 0;

    return cfg;
}

EcosystemConfig EcosystemConfig::createFromIdentity(const InstanceIdentity& id)
{
    EcosystemConfig cfg = createDefaults();
    cfg.mcp.port      = id.port;
    cfg.mcp.authToken = id.bearerToken;
    return cfg;
}

} // namespace more_phi::orchestrator
