#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include "AI/Orchestrator/SecurityValidator.h"
#include "AI/Orchestrator/EcosystemConfig.h"

using namespace more_phi::orchestrator;

// NOTE (Finding #14): the dead SecurityValidator::validateAuthToken() method was
// removed — bearer token auth validation is implemented in MCPServer::validateAuth
// (covered by TestMCPServerUnit.cpp) using a fully constant-time comparison with
// no length-side-channel. Tests for the removed method are deleted here.

TEST_CASE("SecurityValidator checkRateLimit allows requests under limit", "[orchestrator][security]")
{
    auto cfg = EcosystemConfig::createDefaults();
    cfg.security.rateLimitPerMinute = 3; // small limit for testing
    SecurityValidator validator(cfg);

    juce::String clientId = "test-client-1";
    REQUIRE(validator.checkRateLimit(clientId));
    REQUIRE(validator.checkRateLimit(clientId));
    REQUIRE(validator.checkRateLimit(clientId));
}

TEST_CASE("SecurityValidator checkRateLimit rejects requests over limit", "[orchestrator][security]")
{
    auto cfg = EcosystemConfig::createDefaults();
    cfg.security.rateLimitPerMinute = 2; // small limit for testing
    SecurityValidator validator(cfg);

    juce::String clientId = "test-client-2";
    REQUIRE(validator.checkRateLimit(clientId));
    REQUIRE(validator.checkRateLimit(clientId));
    REQUIRE_FALSE(validator.checkRateLimit(clientId));
}

TEST_CASE("SecurityValidator validateRequestJson passes for valid simple request", "[orchestrator][security]")
{
    auto cfg = EcosystemConfig::createDefaults();
    SecurityValidator validator(cfg);

    nlohmann::json req = {
        {"jsonrpc", "2.0"},
        {"method", "tools/list"},
        {"id", 1},
        {"params", {}}
    };

    juce::String error;
    REQUIRE(validator.validateRequestJson(req, error));
}

TEST_CASE("SecurityValidator validateRequestJson fails for oversized JSON", "[orchestrator][security]")
{
    auto cfg = EcosystemConfig::createDefaults();
    cfg.security.maxJsonSize = 128; // very small for testing
    SecurityValidator validator(cfg);

    nlohmann::json req = {
        {"jsonrpc", "2.0"},
        {"method", "tools/list"},
        {"id", 1},
        {"params", {{"data", std::string(512, 'x')}}}
    };

    juce::String error;
    REQUIRE_FALSE(validator.validateRequestJson(req, error));
    REQUIRE(error.contains("max size"));
}

TEST_CASE("SecurityValidator sanitizeParams truncates long strings", "[orchestrator][security]")
{
    auto cfg = EcosystemConfig::createDefaults();
    SecurityValidator validator(cfg);

    nlohmann::json params = {
        {"short", "ok"},
        {"long", std::string(5000, 'a')}
    };

    juce::String error;
    REQUIRE(validator.sanitizeParams(params, error));
    REQUIRE(params["short"].get<std::string>() == "ok");
    REQUIRE(params["long"].get<std::string>().length() == 4096);
}

TEST_CASE("SecurityValidator sanitizeParams clamps large numbers", "[orchestrator][security]")
{
    auto cfg = EcosystemConfig::createDefaults();
    SecurityValidator validator(cfg);

    nlohmann::json params = {
        {"small", 42.0},
        {"huge", 1e9},
        {"negative_huge", -1e9}
    };

    juce::String error;
    REQUIRE(validator.sanitizeParams(params, error));
    REQUIRE(params["small"].get<double>() == 42.0);
    REQUIRE(params["huge"].get<double>() == 1e6);
    REQUIRE(params["negative_huge"].get<double>() == -1e6);
}

TEST_CASE("SecurityValidator sanitizeParams removes long keys", "[orchestrator][security]")
{
    auto cfg = EcosystemConfig::createDefaults();
    SecurityValidator validator(cfg);

    std::string longKey(100, 'k');
    nlohmann::json params = {
        {"ok", "value"},
        {longKey, "should be removed"}
    };

    juce::String error;
    REQUIRE(validator.sanitizeParams(params, error));
    REQUIRE(params.contains("ok"));
    REQUIRE_FALSE(params.contains(longKey));
}

TEST_CASE("SecurityValidator getClientId with nullptr returns fallback", "[orchestrator][security]")
{
    auto cfg = EcosystemConfig::createDefaults();
    SecurityValidator validator(cfg);
    REQUIRE(validator.getClientId(nullptr) == "null_socket");
}
