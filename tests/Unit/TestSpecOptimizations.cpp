/*
 * More-Phi — tests/Unit/TestSpecOptimizations.cpp
 * Unit coverage for the three [SPEC] optimization extensions implemented in
 * AI_MCP_VST3_OPTIMIZED_INTEGRATION_SPEC.md:
 *   1. heartbeat method (rate-limit-free liveness probe)
 *   2. per-key (scope-tagged) cache invalidation
 *   3. adaptive rate limiting tied to the permission autonomy level
 */
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>

#include "AI/TokenOptimizer.h"
#include "AI/ToolResultCache.h"
#include "AI/MCPToolHandler.h"
#include "AI/MCPServer.h"
#include "AI/InstanceIdentity.h"
#include "Plugin/PluginProcessor.h"

namespace {

nlohmann::json parseOrEmpty(const juce::String& s)
{
    try { return nlohmann::json::parse(s.toStdString()); }
    catch (...) { return nlohmann::json::object(); }
}

juce::var emptyParams()
{
    return juce::var(new juce::DynamicObject());
}

} // namespace

// ════════════════════════════════════════════════════════════════════════════
// 1. Heartbeat
// ════════════════════════════════════════════════════════════════════════════

TEST_CASE("heartbeat tool is advertised in tools/list", "[unit][ai][spec][heartbeat]")
{
    const auto listed = nlohmann::json::parse(more_phi::MCPToolHandler::getToolList().toStdString());
    REQUIRE(listed.contains("tools"));

    bool found = false;
    for (const auto& tool : listed["tools"])
    {
        if (tool.value("name", std::string{}) == "heartbeat")
        {
            found = true;
            REQUIRE(tool.contains("inputSchema"));
            REQUIRE(tool["inputSchema"].is_object());
        }
    }
    REQUIRE(found);
}

TEST_CASE("heartbeat returns health fields without consuming a rate-limit slot",
          "[unit][ai][spec][heartbeat]")
{
    more_phi::MorePhiProcessor processor;
    auto identity = more_phi::InstanceIdentity::generate(0);
    more_phi::MCPServer server(processor);
    server.setIdentity(identity);

    // Drive the heartbeat path through the public request entry point so we
    // exercise the same code the wire protocol does. Auth must already be
    // established for heartbeat to be accepted, mirroring the runtime contract.
    const juce::String initReq = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"bearer_token":")"
        + identity.bearerToken + R"("}})";
    bool authenticated = false;
    {
        juce::String r = server.processRequestForTesting(initReq, authenticated);
        REQUIRE_FALSE(r.isEmpty());
    }
    REQUIRE(authenticated);

    // Saturate the rate-limit window so any slot-consuming method would be
    // rejected; heartbeat must still succeed because it bypasses the gate.
    auto& optimizer = processor.getTokenOptimizer();
    optimizer.setRateLimit(1);
    REQUIRE(optimizer.tryConsumeRequestSlot());      // now exhausted
    REQUIRE_FALSE(optimizer.canMakeRequest());

    const juce::String hbReq = R"({"jsonrpc":"2.0","id":2,"method":"heartbeat","params":{"client_clock_ms":123}})";
    const juce::String resp = server.processRequestForTesting(hbReq, authenticated);
    REQUIRE_FALSE(resp.isEmpty());

    const auto parsed = parseOrEmpty(resp);
    REQUIRE(parsed["jsonrpc"] == "2.0");
    REQUIRE(parsed["id"] == 2);
    REQUIRE(parsed.contains("result"));
    const auto& result = parsed["result"];
    REQUIRE(result.contains("server_time_ms"));
    REQUIRE(result.contains("uptime_ms"));
    REQUIRE(result.contains("queue_depth_approx"));
    REQUIRE(result.contains("connected_clients"));
    REQUIRE(result.contains("healthy"));
}

TEST_CASE("heartbeat before initialize is rejected as unauthorized",
          "[unit][ai][spec][heartbeat]")
{
    more_phi::MorePhiProcessor processor;
    auto identity = more_phi::InstanceIdentity::generate(0);
    more_phi::MCPServer server(processor);
    server.setIdentity(identity);

    bool authenticated = false;
    const juce::String hbReq = R"({"jsonrpc":"2.0","id":1,"method":"heartbeat","params":{}})";
    const juce::String resp = server.processRequestForTesting(hbReq, authenticated);

    const auto parsed = parseOrEmpty(resp);
    REQUIRE(parsed.contains("error"));
    REQUIRE(parsed["error"]["code"] == -32001);
    REQUIRE_FALSE(authenticated);
}

// ════════════════════════════════════════════════════════════════════════════
// 2. Per-key (scope-tagged) cache invalidation
// ════════════════════════════════════════════════════════════════════════════

TEST_CASE("ToolResultCache scope classifier maps tools to the right scope",
          "[unit][ai][spec][cache]")
{
    using Scope = more_phi::ToolResultCache::Scope;
    REQUIRE(more_phi::ToolResultCache::scopeForTool("list_parameters")             == Scope::Parameters);
    REQUIRE(more_phi::ToolResultCache::scopeForTool("get_parameter")               == Scope::Parameters);
    REQUIRE(more_phi::ToolResultCache::scopeForTool("hosted_plugin.parameters")    == Scope::Parameters);
    REQUIRE(more_phi::ToolResultCache::scopeForTool("more_phi.parameters")         == Scope::Parameters);
    REQUIRE(more_phi::ToolResultCache::scopeForTool("diagnose_parameter_pipeline") == Scope::Parameters);

    REQUIRE(more_phi::ToolResultCache::scopeForTool("analysis.get_summary")    == Scope::Analysis);
    REQUIRE(more_phi::ToolResultCache::scopeForTool("analysis.get_spectrum")   == Scope::Analysis);

    REQUIRE(more_phi::ToolResultCache::scopeForTool("get_morph_state")                     == Scope::Morph);
    REQUIRE(more_phi::ToolResultCache::scopeForTool("plugin_profile.describe_semantics")  == Scope::Profile);
    REQUIRE(more_phi::ToolResultCache::scopeForTool("describe_plugin_semantic_map")        == Scope::Profile);
    REQUIRE(more_phi::ToolResultCache::scopeForTool("get_plugin_info")                     == Scope::PluginInfo);
    REQUIRE(more_phi::ToolResultCache::scopeForTool("automation.history")                  == Scope::Control);
    REQUIRE(more_phi::ToolResultCache::scopeForTool("workflow.list")                       == Scope::Control);
    REQUIRE(more_phi::ToolResultCache::scopeForTool("unknown_read_tool")                   == Scope::Parameters);
}

TEST_CASE("invalidateScopes evicts only the targeted scope and preserves the rest",
          "[unit][ai][spec][cache]")
{
    more_phi::ToolResultCache cache(32);

    juce::var empty = emptyParams();
    const uint64_t gen = 7;

    // Populate three scopes.
    cache.put("list_parameters",   empty, gen, nlohmann::json{{"params","v1"}});
    cache.put("analysis.get_summary", empty, gen, nlohmann::json{{"lufs",-14.0}});
    cache.put("plugin_profile.describe_semantics", empty, gen, nlohmann::json{{"controls", 4}});

    REQUIRE(cache.get("list_parameters",  empty, gen).has_value());
    REQUIRE(cache.get("analysis.get_summary", empty, gen).has_value());
    REQUIRE(cache.get("plugin_profile.describe_semantics", empty, gen).has_value());

    // A parameter write should evict only the parameter-describing scope.
    using Scope = more_phi::ToolResultCache::Scope;
    const size_t evicted = cache.invalidateScopes({Scope::Parameters});
    REQUIRE(evicted == 1);

    REQUIRE_FALSE(cache.get("list_parameters", empty, gen).has_value());     // evicted
    REQUIRE(cache.get("analysis.get_summary", empty, gen).has_value());       // preserved
    REQUIRE(cache.get("plugin_profile.describe_semantics", empty, gen).has_value()); // preserved
}

TEST_CASE("invalidateScopes is a no-op for an empty scope set",
          "[unit][ai][spec][cache]")
{
    more_phi::ToolResultCache cache(8);
    juce::var empty = emptyParams();
    cache.put("list_parameters", empty, 1, nlohmann::json{{"v", 1}});

    REQUIRE(cache.invalidateScopes({}) == 0);
    REQUIRE(cache.get("list_parameters", empty, 1).has_value());
}

TEST_CASE("set_parameter write invalidates parameters+morph but not analysis/profile",
          "[unit][ai][spec][cache]")
{
    more_phi::ToolResultCache cache(32);
    juce::var empty = emptyParams();
    const uint64_t gen = 1;

    cache.put("list_parameters",              empty, gen, nlohmann::json{{"v", 1}});
    cache.put("get_morph_state",              empty, gen, nlohmann::json{{"x", 0.5}});
    cache.put("analysis.get_summary",         empty, gen, nlohmann::json{{"lufs", -10}});
    cache.put("plugin_profile.describe_semantics", empty, gen, nlohmann::json{{"controls", 9}});

    // A parameter write evicts parameters+morph but preserves analysis/profile.
    using Scope = more_phi::ToolResultCache::Scope;
    cache.invalidateScopes({Scope::Parameters, Scope::Morph});

    REQUIRE_FALSE(cache.get("list_parameters", empty, gen).has_value());
    REQUIRE_FALSE(cache.get("get_morph_state", empty, gen).has_value());
    REQUIRE(cache.get("analysis.get_summary", empty, gen).has_value());
    REQUIRE(cache.get("plugin_profile.describe_semantics", empty, gen).has_value());
}

// ════════════════════════════════════════════════════════════════════════════
// 3. Adaptive rate limiting
// ════════════════════════════════════════════════════════════════════════════

TEST_CASE("autonomy multiplier scales the effective rate limit",
          "[unit][ai][spec][rate]")
{
    more_phi::TokenOptimizer optimizer;
    optimizer.setRateLimit(60);

    REQUIRE(optimizer.getAutonomyRateMultiplier() == 1.0f);
    REQUIRE(optimizer.getEffectiveRateLimit() == 60u);

    optimizer.setAutonomyRateMultiplier(0.5f);   // Manual
    REQUIRE(optimizer.getEffectiveRateLimit() == 30u);

    optimizer.setAutonomyRateMultiplier(1.5f);   // CoPilot
    REQUIRE(optimizer.getEffectiveRateLimit() == 90u);

    optimizer.setAutonomyRateMultiplier(2.0f);   // Autopilot
    REQUIRE(optimizer.getEffectiveRateLimit() == 120u);
}

TEST_CASE("autonomy multiplier rejects zero/negative and clamps absurd values",
          "[unit][ai][spec][rate]")
{
    more_phi::TokenOptimizer optimizer;
    optimizer.setRateLimit(60);

    optimizer.setAutonomyRateMultiplier(0.0f);
    REQUIRE(optimizer.getAutonomyRateMultiplier() == 1.0f);   // rejected → identity

    optimizer.setAutonomyRateMultiplier(-3.0f);
    REQUIRE(optimizer.getAutonomyRateMultiplier() == 1.0f);

    optimizer.setAutonomyRateMultiplier(1000.0f);
    REQUIRE(optimizer.getAutonomyRateMultiplier() == 16.0f);  // clamped
    REQUIRE(optimizer.getEffectiveRateLimit() == 60u * 16u);
}

TEST_CASE("tryConsumeRequestSlot honours the autonomy-scaled limit",
          "[unit][ai][spec][rate]")
{
    more_phi::TokenOptimizer optimizer;
    optimizer.setRateLimit(10);
    optimizer.setAutonomyRateMultiplier(0.5f);   // effective = 5

    for (uint32_t i = 0; i < 5; ++i)
        REQUIRE(optimizer.tryConsumeRequestSlot());

    // 6th request must be rejected at Manual multiplier.
    REQUIRE_FALSE(optimizer.tryConsumeRequestSlot());

    // Promoting to Autopilot (×2 → effective 20) must allow more requests
    // immediately, without waiting for the window to slide.
    optimizer.setAutonomyRateMultiplier(2.0f);
    REQUIRE(optimizer.tryConsumeRequestSlot());
    REQUIRE(optimizer.canMakeRequest());
}

TEST_CASE("permission.set_autonomy propagates the multiplier to the optimizer",
          "[unit][ai][spec][rate]")
{
    more_phi::MorePhiProcessor processor;
    more_phi::InstanceIdentity identity;
    more_phi::AutomationRuntime runtime;

    auto& optimizer = processor.getTokenOptimizer();
    optimizer.setRateLimit(60);
    optimizer.setAutonomyRateMultiplier(1.0f);

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("level", "autopilot");

    const auto response = more_phi::MCPToolHandler::handle(
        "permission.set_autonomy", juce::var(req.get()), processor, identity, runtime);
    const auto parsed = parseOrEmpty(response);

    REQUIRE(parsed.value("success", false) == true);
    REQUIRE(parsed.value("autonomy_level", std::string{}) == "autopilot");
    REQUIRE(optimizer.getAutonomyRateMultiplier() == 2.0f);
    REQUIRE(parsed.value("effective_rate_limit", 0) == 120);
    REQUIRE(optimizer.getEffectiveRateLimit() == 120u);
}
