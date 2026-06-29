#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include "AI/Orchestrator/EcosystemConfig.h"
#include "AI/InstanceIdentity.h"

using namespace more_phi::orchestrator;

TEST_CASE("EcosystemConfig createDefaults returns valid config", "[orchestrator][config]")
{
    auto cfg = EcosystemConfig::createDefaults();
    REQUIRE(cfg.validate());
    REQUIRE(cfg.getLastError().isEmpty());
}

TEST_CASE("EcosystemConfig validate passes for defaults", "[orchestrator][config]")
{
    auto cfg = EcosystemConfig::createDefaults();
    REQUIRE(cfg.validate());
}

TEST_CASE("EcosystemConfig toJson round-trip through loadFromJson", "[orchestrator][config]")
{
    auto original = EcosystemConfig::createDefaults();
    auto j = original.toJson();

    EcosystemConfig restored;
    REQUIRE(restored.loadFromJson(j));

    REQUIRE(restored.mcp.port == original.mcp.port);
    REQUIRE(restored.mcp.authToken == original.mcp.authToken);
    REQUIRE(restored.mcp.maxConnections == original.mcp.maxConnections);
    REQUIRE(restored.mcp.idleTimeoutMs == original.mcp.idleTimeoutMs);
    REQUIRE(restored.mcp.maxRequestBytes == original.mcp.maxRequestBytes);
    REQUIRE(restored.mcp.enableServer == original.mcp.enableServer);

    REQUIRE(restored.agents.numWorkers == original.agents.numWorkers);
    REQUIRE(restored.agents.blackboardPollIntervalMs == original.agents.blackboardPollIntervalMs);
    REQUIRE(restored.agents.maxCallsPerAgentPerSecond == original.agents.maxCallsPerAgentPerSecond);
    REQUIRE(restored.agents.enableAllAgents == original.agents.enableAllAgents);

    REQUIRE(restored.security.maxJsonDepth == original.security.maxJsonDepth);
    REQUIRE(restored.security.maxJsonSize == original.security.maxJsonSize);
    REQUIRE(restored.security.rateLimitPerMinute == original.security.rateLimitPerMinute);
    REQUIRE(restored.security.allowedMethods.size() == original.security.allowedMethods.size());

    REQUIRE(restored.plugin.defaultMorphX == original.plugin.defaultMorphX);
    REQUIRE(restored.plugin.defaultMorphY == original.plugin.defaultMorphY);
    REQUIRE(restored.plugin.enableAudioDomainMorph == original.plugin.enableAudioDomainMorph);
    REQUIRE(restored.plugin.defaultPhysicsMode == original.plugin.defaultPhysicsMode);
}

TEST_CASE("EcosystemConfig validation fails for invalid port", "[orchestrator][config]")
{
    auto cfg = EcosystemConfig::createDefaults();
    cfg.mcp.port = 80; // well-known port, below 1024
    REQUIRE_FALSE(cfg.validate());
    REQUIRE(cfg.getLastError().contains("port"));
}

TEST_CASE("EcosystemConfig validation fails for empty authToken", "[orchestrator][config]")
{
    auto cfg = EcosystemConfig::createDefaults();
    cfg.mcp.authToken = "";
    REQUIRE_FALSE(cfg.validate());
    REQUIRE(cfg.getLastError().contains("authToken"));
}

TEST_CASE("EcosystemConfig validation fails for out-of-range morph values", "[orchestrator][config]")
{
    auto cfg = EcosystemConfig::createDefaults();
    cfg.plugin.defaultMorphX = 1.5f;
    REQUIRE_FALSE(cfg.validate());
    REQUIRE(cfg.getLastError().contains("defaultMorphX"));

    cfg = EcosystemConfig::createDefaults();
    cfg.plugin.defaultMorphY = -0.1f;
    REQUIRE_FALSE(cfg.validate());
    REQUIRE(cfg.getLastError().contains("defaultMorphY"));

    cfg = EcosystemConfig::createDefaults();
    cfg.plugin.defaultPhysicsMode = 3;
    REQUIRE_FALSE(cfg.validate());
    REQUIRE(cfg.getLastError().contains("defaultPhysicsMode"));
}

TEST_CASE("EcosystemConfig loadFromFile with non-existent file returns false", "[orchestrator][config]")
{
    EcosystemConfig cfg;
    juce::File nonExistent = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                 .getChildFile("definitely-does-not-exist.json");
    REQUIRE_FALSE(cfg.loadFromFile(nonExistent));
    REQUIRE(cfg.getLastError().contains("does not exist"));
}

TEST_CASE("EcosystemConfig createFromIdentity uses identity port and token", "[orchestrator][config]")
{
    auto identity = more_phi::InstanceIdentity::generate(54321);
    auto cfg = EcosystemConfig::createFromIdentity(identity);

    REQUIRE(cfg.mcp.port == 54321);
    REQUIRE(cfg.mcp.authToken == identity.bearerToken);
    REQUIRE(cfg.validate());
}
