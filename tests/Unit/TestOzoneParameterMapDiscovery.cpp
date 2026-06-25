// tests/Unit/TestOzoneParameterMapDiscovery.cpp
//
// Regression guard for AUDIT finding W1 (2026-06-25): the OzoneParameterMap
// shipped entirely as -1 stubs (buildForOzone11 is all -1 by design), so the
// only way slots become mapped is OzoneParameterMap::buildFromHostedPlugin()
// matching the hosted plugin's parameter names via substring/word-boundary
// heuristics. If the hosted plugin's names don't match the heuristic keys, the
// map stays all-stubs, OzonePlanApplicator::isReady()==false, and EVERY neural
// mastering apply silently no-ops (only a DBG line, never reaching the
// assistant). This test pins the discovery heuristic against an Ozone-shaped
// parameter set so a future change to the heuristics (or the name patterns) is
// caught before the neural chain silently goes inert.
//
// It also pins mappedSlotCount() (new in this audit) which the sonicmaster_decision
// MCP tool surfaces as mapping_status.mapped_slot_count.
#include "AI/OzoneParameterMap.h"
#include "Host/IPluginHostManager.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_core/juce_core.h>
#include <vector>

namespace {

// Minimal IParameterBridge that reports a fixed name list. buildFromHostedPlugin
// only reads getParameterName/getParameterCount, so this is all we need.
// Pattern reused from FakeParameterBridge in TestAIRegressions.cpp.
class NameOnlyBridge final : public more_phi::IParameterBridge
{
public:
    explicit NameOnlyBridge(std::vector<juce::String> names) : names_(std::move(names)) {}

    int getParameterCount() const override { return static_cast<int>(names_.size()); }
    float getParameterNormalized(int) const override { return 0.0f; }
    void setParameterNormalized(int, float) override {}
    juce::String getParameterName(int index) const override
    {
        return (index >= 0 && static_cast<size_t>(index) < names_.size())
                   ? names_[static_cast<size_t>(index)]
                   : juce::String{};
    }
    void applyParameterState(const std::vector<float>&) override {}
    void applyParameterState(const float*, int) override {}
    std::vector<float> captureParameterState() const override { return {}; }
    bool isDiscrete(int) const override { return false; }
    std::vector<bool> getDiscreteMap() const override { return {}; }
    juce::String getParameterLabel(int) const override { return {}; }
    juce::String getParameterDisplayValue(int) const override { return {}; }
    float getParameterDefault(int) const override { return 0.5f; }
    juce::StringArray getParameterValueStrings(int) const override { return {}; }
    juce::String getParameterStableID(int index) const override { return getParameterName(index); }
    int getParameterNumSteps(int) const override { return 0; }

private:
    std::vector<juce::String> names_;
};

} // namespace

TEST_CASE("OzoneParameterMap buildForOzone11 ships as all-stub (W1 baseline)",
          "[Ozone][Mapping]")
{
    // The factory default is intentionally all -1: real mappings only appear via
    // buildFromHostedPlugin against a live hosted plugin. Pinning this makes the
    // "silent no-op when nothing is hosted" condition explicit and catches any
    // accidental hard-coded index table that would short-circuit discovery.
    const auto m = more_phi::OzoneParameterMap::buildForOzone11();
    CHECK_FALSE(m.hasAnyMapping());
    CHECK(m.mappedSlotCount() == 0);
}

TEST_CASE("buildFromHostedPlugin maps Ozone-shaped parameter names", "[Ozone][Mapping]")
{
    // Names mirror what iZotope Ozone exposes and what the heuristic in
    // buildFromHostedPlugin keys on (see OzoneParameterMap.cpp:99-340):
    //   "Eq Band N <Freq|Gain|Q|Type|Enable>", "Dynamics <Threshold|Ratio|...>",
    //   "Imager <Sub|Low|Mid|High> Width", "Maximizer <Output Level|Ceiling>".
    NameOnlyBridge bridge{{
        // EQ band 1 — all five slots
        "Eq Band 1 Frequency", "Eq Band 1 Gain", "Eq Band 1 Q", "Eq Band 1 Type", "Eq Band 1 Enable",
        // EQ band 2 — freq + gain only (partial band, still counts)
        "Eq Band 2 Frequency", "Eq Band 2 Gain",
        // Dynamics — all four
        "Dynamics Threshold", "Dynamics Ratio", "Dynamics Attack", "Dynamics Release",
        // Imager — all four widths
        "Imager Sub Width", "Imager Low Width", "Imager Mid Width", "Imager High Width",
        // Maximizer — both
        "Maximizer Output Level", "Maximizer Ceiling",
        // Unrelated params that must NOT be picked up
        "Output Gain", "Master Bypass",
    }};

    const auto m = more_phi::OzoneParameterMap::buildFromHostedPlugin(bridge);

    // Readiness + coverage.
    REQUIRE(m.hasAnyMapping());
    // 5 (band1) + 2 (band2) + 4 (dynamics) + 4 (imager) + 2 (maximizer) = 17.
    REQUIRE(m.mappedSlotCount() == 17);

    // Indices are positional into the bridge's array; verify a representative
    // few so a regression in extractBandNumber or the slot classifier is caught.
    CHECK(m.eq[0].freqIdx    == 0);
    CHECK(m.eq[0].gainIdx    == 1);
    CHECK(m.eq[0].qIdx       == 2);
    CHECK(m.eq[0].typeIdx    == 3);
    CHECK(m.eq[0].enabledIdx == 4);
    CHECK(m.eq[1].freqIdx    == 5);
    CHECK(m.eq[1].gainIdx    == 6);
    CHECK(m.eq[1].qIdx       == -1);  // band 2 had no Q
    CHECK(m.dynamics.thresholdIdx == 7);
    CHECK(m.dynamics.ratioIdx     == 8);
    CHECK(m.imager.widthIdx[0]    == 11);  // sub
    CHECK(m.imager.widthIdx[3]    == 14);  // high
    CHECK(m.maximizer.outputLevelIdx == 15);
    CHECK(m.maximizer.ceilingIdx     == 16);
}

TEST_CASE("buildFromHostedPlugin with non-Ozone names stays unmapped (W1 silent no-op)",
          "[Ozone][Mapping]")
{
    // This is the W1 failure mode: a hosted plugin whose names don't match the
    // heuristic (e.g. a generic EQ/compressor) leaves the map all-stub. Pinning
    // it documents that discovery is name-dependent, not a guarantee.
    NameOnlyBridge bridge{{"Gain", "Tone", "Mix", "Volume"}};
    const auto m = more_phi::OzoneParameterMap::buildFromHostedPlugin(bridge);
    CHECK_FALSE(m.hasAnyMapping());
    CHECK(m.mappedSlotCount() == 0);
}
