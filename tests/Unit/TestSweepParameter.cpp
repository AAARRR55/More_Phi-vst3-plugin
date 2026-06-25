// tests/Unit/TestSweepParameter.cpp
//
// sweep_parameter / AUDIT: the only tool that autonomously sweeps a value range
// on the LIVE hosted plugin. This test drives it against a processor with test
// parameter descriptors (no real VST3) and asserts it returns one measurement
// row per step, each carrying the requested value and an applied_value readback.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "AI/MCPToolHandler.h"
#include "Plugin/PluginProcessor.h"

#include <nlohmann/json.hpp>

using namespace more_phi;
using Catch::Approx;

namespace {

std::vector<ParameterBridge::ParameterDescriptor>
makeDescriptors(const std::vector<juce::String>& names)
{
    std::vector<ParameterBridge::ParameterDescriptor> out;
    out.reserve(names.size());
    for (int i = 0; i < static_cast<int>(names.size()); ++i)
    {
        ParameterBridge::ParameterDescriptor d;
        d.index = i;
        d.name = names[static_cast<size_t>(i)];
        d.value = 0.0f;
        out.push_back(d);
    }
    return out;
}

} // namespace

TEST_CASE("sweep_parameter emits one measurement row per step", "[mcp][sweep]")
{
    MorePhiProcessor processor;
    InstanceIdentity identity;
    processor.getParameterBridge().setParameterDescriptorsForTesting(
        makeDescriptors({ "Gain", "Cutoff" }));

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("index", 0);
    req->setProperty("from", 0.0);
    req->setProperty("to", 1.0);
    req->setProperty("steps", 3);
    req->setProperty("capture_ms", 10);   // keep the test fast

    const auto response = MCPToolHandler::handle("sweep_parameter",
                                                 juce::var(req.get()),
                                                 processor, identity);
    const auto parsed = juce::JSON::parse(response);
    REQUIRE_FALSE(parsed.isVoid());

    REQUIRE(static_cast<bool>(parsed.getProperty("success", false)));
    REQUIRE(static_cast<int>(parsed.getProperty("steps", -1)) == 3);
    REQUIRE(static_cast<int>(parsed.getProperty("captured_steps", -1)) == 3);

    const auto& results = *parsed.getProperty("results", juce::var()).getArray();
    REQUIRE(results.size() == 3);

    // Step 0 = 0.0, step 1 = 0.5, step 2 = 1.0 (inclusive endpoints).
    const float expected[3] = { 0.0f, 0.5f, 1.0f };
    for (int i = 0; i < 3; ++i)
    {
        const auto& row = results[static_cast<size_t>(i)];
        REQUIRE(row.hasProperty("value"));
        REQUIRE(row.hasProperty("applied_value"));
        REQUIRE(row.hasProperty("measurements"));
        CHECK(static_cast<float>(row.getProperty("value", -1.0f)) == Approx(expected[i]));
    }
}

TEST_CASE("sweep_parameter rejects an unresolved parameter", "[mcp][sweep][resolve]")
{
    MorePhiProcessor processor;
    InstanceIdentity identity;
    processor.getParameterBridge().setParameterDescriptorsForTesting(
        makeDescriptors({ "Gain" }));

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("name", "NonexistentParam");
    req->setProperty("from", 0.0);
    req->setProperty("to", 1.0);
    req->setProperty("steps", 3);

    const auto response = MCPToolHandler::handle("sweep_parameter",
                                                 juce::var(req.get()),
                                                 processor, identity);
    const auto parsed = juce::JSON::parse(response);
    REQUIRE_FALSE(parsed.isVoid());
    REQUIRE_FALSE(static_cast<bool>(parsed.getProperty("success", true)));
    REQUIRE_FALSE(static_cast<bool>(parsed.getProperty("resolved", true)));
}
