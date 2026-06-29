/*
 * More-Phi — AI/Orchestrator/EcosystemConfig.h
 * Unified JSON configuration for the multi-agent ecosystem.
 * C++20, MSVC 2022/Clang/GCC compatible.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <cstdint>

#include "AI/InstanceIdentity.h"

namespace more_phi::orchestrator {

//==============================================================================
/**
 * @struct EcosystemConfig
 * @brief Unified configuration for all three ecosystem components:
 *        VST3 plugin, AgentRuntime, and MCP Server.
 *
 * Provides JSON serialization, schema validation, and sensible factory defaults.
 * All numeric ranges are validated; out-of-range values are clamped or rejected.
 */
struct EcosystemConfig
{
    //==========================================================================
    struct MCPConfig
    {
        int          port            = 30001;
        juce::String authToken;
        int          maxConnections  = 4;
        int          idleTimeoutMs   = 30000;
        int          maxRequestBytes = 65536;
        bool         enableServer    = true;

        nlohmann::json toJson() const;
        bool fromJson(const nlohmann::json& j, juce::String& outError);
        bool validate(juce::String& outError) const;
    };

    //==========================================================================
    struct AgentConfig
    {
        int  numWorkers                = 2;
        int  blackboardPollIntervalMs  = 50;
        int  maxCallsPerAgentPerSecond = 10;
        bool enableAllAgents           = true;

        nlohmann::json toJson() const;
        bool fromJson(const nlohmann::json& j, juce::String& outError);
        bool validate(juce::String& outError) const;
    };

    //==========================================================================
    struct SecurityConfig
    {
        int                       maxJsonDepth      = 32;
        int                       maxJsonSize       = 65536;
        std::vector<juce::String> allowedMethods;
        int                       rateLimitPerMinute = 60;

        nlohmann::json toJson() const;
        bool fromJson(const nlohmann::json& j, juce::String& outError);
        bool validate(juce::String& outError) const;
    };

    //==========================================================================
    struct PluginConfig
    {
        float defaultMorphX          = 0.5f;
        float defaultMorphY          = 0.5f;
        bool  enableAudioDomainMorph = false;
        int   defaultPhysicsMode     = 0;   // 0=Direct, 1=Elastic, 2=Drift

        nlohmann::json toJson() const;
        bool fromJson(const nlohmann::json& j, juce::String& outError);
        bool validate(juce::String& outError) const;
    };

    //==========================================================================
    MCPConfig      mcp;
    AgentConfig    agents;
    SecurityConfig security;
    PluginConfig   plugin;

    //==========================================================================
    /** Load from a JSON file on disk. Returns false on parse/validation failure. */
    bool loadFromFile(const juce::File& path);

    /** Load from an in-memory JSON object. Returns false on validation failure. */
    bool loadFromJson(const nlohmann::json& j);

    /** Export current configuration to a JSON object. */
    nlohmann::json toJson() const;

    /** Validate all sub-configs. Returns true if everything is valid. */
    bool validate() const;

    /** Human-readable description of the last validation error. */
    juce::String getLastError() const { return lastError_; }

    //==========================================================================
    /** Factory defaults — suitable for a fresh instance. */
    static EcosystemConfig createDefaults();

    /** Derive defaults from an InstanceIdentity (port & auth token). */
    static EcosystemConfig createFromIdentity(const InstanceIdentity& id);

private:
    mutable juce::String lastError_;

    void setLastError(const juce::String& msg) const { lastError_ = msg; }
};

} // namespace more_phi::orchestrator
