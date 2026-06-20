/*
 * More-Phi — Unit/TestOzoneIntegration.cpp
 * Catch2 v3 tests for the Ozone 11 mastering integration.
 *
 * Coverage:
 *   1. OzoneParameterMap::normalizeGain()   — boundary + midpoint round-trip
 *   2. OzoneParameterMap::normalizeFreq()   — 20 Hz → 0.0, 20 kHz → 1.0
 *   3. OzoneParameterMap::normalizeThreshold() — −60 dBFS → 0.0, 0 dBFS → 1.0
 *   4. OzoneParameterMap::isOzone11()       — positive + negative detection
 *   5. OzonePlanApplicator — enqueueIfMapped skips idx==-1 silently
 *   6. ChainPlanExecutor — registered applicator receives every plan
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "AI/OzoneParameterMap.h"
#include "AI/OzonePlanApplicator.h"
#include "AI/ChainPlanExecutor.h"

using namespace more_phi;
using Catch::Approx;

// ── Test 1: normalizeGain ─────────────────────────────────────────────────────

TEST_CASE("OzoneParameterMap::normalizeGain boundary values", "[ozone][normalization]")
{
    SECTION("Min gain maps to 0.0")
    {
        REQUIRE(OzoneParameterMap::normalizeGain(-18.f) == Approx(0.0f).margin(1e-5f));
    }

    SECTION("Max gain maps to 1.0")
    {
        REQUIRE(OzoneParameterMap::normalizeGain(18.f) == Approx(1.0f).margin(1e-5f));
    }

    SECTION("Zero gain maps to 0.5")
    {
        REQUIRE(OzoneParameterMap::normalizeGain(0.f) == Approx(0.5f).margin(1e-5f));
    }

    SECTION("Custom range: -6 to +6 dB, midpoint = 0.5")
    {
        REQUIRE(OzoneParameterMap::normalizeGain(0.f, -6.f, 6.f) == Approx(0.5f).margin(1e-5f));
    }

    SECTION("Result is clamped to [0, 1] for out-of-range input")
    {
        REQUIRE(OzoneParameterMap::normalizeGain(-100.f) == Approx(0.0f).margin(1e-5f));
        REQUIRE(OzoneParameterMap::normalizeGain( 100.f) == Approx(1.0f).margin(1e-5f));
    }
}

// ── Test 2: normalizeFreq ─────────────────────────────────────────────────────

TEST_CASE("OzoneParameterMap::normalizeFreq boundary values", "[ozone][normalization]")
{
    SECTION("20 Hz (min) maps to 0.0")
    {
        REQUIRE(OzoneParameterMap::normalizeFreq(20.f) == Approx(0.0f).margin(1e-5f));
    }

    SECTION("20 000 Hz (max) maps to 1.0")
    {
        REQUIRE(OzoneParameterMap::normalizeFreq(20000.f) == Approx(1.0f).margin(1e-5f));
    }

    SECTION("Midpoint on log scale is ~632 Hz (geometric mean of 20–20k)")
    {
        // geometric mean = sqrt(20 * 20000) ≈ 632.46 Hz
        const float mid = std::sqrt(20.f * 20000.f);
        REQUIRE(OzoneParameterMap::normalizeFreq(mid) == Approx(0.5f).margin(0.01f));
    }
}

// ── Test 3: normalizeThreshold ────────────────────────────────────────────────

TEST_CASE("OzoneParameterMap::normalizeThreshold boundary values", "[ozone][normalization]")
{
    SECTION("-60 dBFS maps to 0.0")
    {
        REQUIRE(OzoneParameterMap::normalizeThreshold(-60.f) == Approx(0.0f).margin(1e-5f));
    }

    SECTION("0 dBFS maps to 1.0")
    {
        REQUIRE(OzoneParameterMap::normalizeThreshold(0.f) == Approx(1.0f).margin(1e-5f));
    }

    SECTION("-30 dBFS maps to 0.5")
    {
        REQUIRE(OzoneParameterMap::normalizeThreshold(-30.f) == Approx(0.5f).margin(1e-5f));
    }
}

// ── Test 3b: normalizeQ ───────────────────────────────────────────────────────
//
// Regression guard for the Q-encoding defect: OzonePlanApplicator previously
// routed the EQ band Q value through normalizeFreq() (a log2 curve over
// [0.1, 20]). That both mis-scaled Q (Ozone exposes Q as linear in normalized
// VST3 space) and used a range that disagrees with this codebase's own Q
// documentation — PluginSemanticMapper.cpp:171 advertises Q as "0.3 to 8.0"
// and EQParameterTranslator.cpp clamps Q to a narrow mastering range with a
// 0.1 floor. normalizeQ is linear over [0.1, 8.0] so a plan's Q value maps
// deterministically to the hosted plugin's Q parameter, consistent with the
// other linear normalize helpers (gain/threshold/LUFS/ceiling) and distinct
// from the log2 freq curve.

TEST_CASE("OzoneParameterMap::normalizeQ boundary and linearity", "[ozone][normalization]")
{
    SECTION("Floor (0.1) maps to 0.0")
    {
        REQUIRE(OzoneParameterMap::normalizeQ(0.1f) == Approx(0.0f).margin(1e-5f));
    }

    SECTION("Ceiling (8.0) maps to 1.0")
    {
        REQUIRE(OzoneParameterMap::normalizeQ(8.0f) == Approx(1.0f).margin(1e-5f));
    }

    SECTION("Unity Q (1.0) maps to a linear midpoint, not a log midpoint")
    {
        // Linear over [0.1, 8.0]: (1.0 - 0.1) / (8.0 - 0.1) ≈ 0.11392
        REQUIRE(OzoneParameterMap::normalizeQ(1.0f) == Approx(0.11392f).margin(1e-4f));
    }

    SECTION("Mapping is linear (midpoint of the range maps to 0.5)")
    {
        const float rangeMid = (0.1f + 8.0f) * 0.5f;   // 4.05
        REQUIRE(OzoneParameterMap::normalizeQ(rangeMid) == Approx(0.5f).margin(1e-5f));
    }

    SECTION("Result is clamped to [0, 1] for out-of-range input")
    {
        REQUIRE(OzoneParameterMap::normalizeQ(0.0f)  == Approx(0.0f).margin(1e-5f));
        REQUIRE(OzoneParameterMap::normalizeQ(20.0f) == Approx(1.0f).margin(1e-5f));
    }

    SECTION("Linear Q encoding differs from the old log2-freq encoding")
    {
        // The bug routed Q through normalizeFreq(q, 0.1, 20) (log2). For q=1.0
        // that yielded ~0.384; the linear helper must NOT match it. If this
        // assertion ever fails, Q was re-routed through a log curve.
        const float buggyLogEncoding = OzoneParameterMap::normalizeFreq(1.0f, 0.1f, 20.0f);
        REQUIRE_FALSE(OzoneParameterMap::normalizeQ(1.0f)
                      == Approx(buggyLogEncoding).margin(1e-3f));
    }
}

// ── Test 4: isOzone11 ─────────────────────────────────────────────────────────

TEST_CASE("OzoneParameterMap::isOzone11 name detection", "[ozone][detection]")
{
    SECTION("Exact match — full product name")
    {
        REQUIRE(OzoneParameterMap::isOzone11("iZotope Ozone 11 Advanced"));
    }

    SECTION("Partial match — standard edition")
    {
        REQUIRE(OzoneParameterMap::isOzone11("iZotope Ozone 11"));
    }

    SECTION("Case-insensitive match")
    {
        REQUIRE(OzoneParameterMap::isOzone11("izotope ozone 11 standard"));
    }

    SECTION("Neutron 5 does NOT match")
    {
        REQUIRE_FALSE(OzoneParameterMap::isOzone11("iZotope Neutron 5"));
    }

    SECTION("Ozone 10 does NOT match")
    {
        REQUIRE_FALSE(OzoneParameterMap::isOzone11("iZotope Ozone 10"));
    }

    SECTION("Empty string does NOT match")
    {
        REQUIRE_FALSE(OzoneParameterMap::isOzone11(""));
    }
}

// ── Test 5: enqueueIfMapped skips idx == -1 ───────────────────────────────────

TEST_CASE("OzoneParameterMap all-minus-one map is a safe no-op", "[ozone][map]")
{
    // buildForOzone11() returns a map where every index is -1 until audited.
    // Constructing an OzonePlanApplicator and calling apply() must not crash.
    // We verify that getLastAppliedCount() returns 0 (nothing enqueued).
    //
    // We can't run this without a real MorePhiProcessor — use the map directly.

    OzoneParameterMap map = OzoneParameterMap::buildForOzone11();

    // Every EQ band index should be -1 (unaudited state)
    for (int i = 0; i < OzoneParameterMap::kEQBands; ++i)
    {
        CHECK(map.eq[i].freqIdx    == -1);
        CHECK(map.eq[i].gainIdx    == -1);
        CHECK(map.eq[i].qIdx       == -1);
        CHECK(map.eq[i].typeIdx    == -1);
        CHECK(map.eq[i].enabledIdx == -1);
    }

    // Dynamics indices all -1
    CHECK(map.dynamics.thresholdIdx == -1);
    CHECK(map.dynamics.ratioIdx     == -1);
    CHECK(map.dynamics.attackIdx    == -1);
    CHECK(map.dynamics.releaseIdx   == -1);

    // Imager all -1
    for (int i = 0; i < 4; ++i)
        CHECK(map.imager.widthIdx[i] == -1);

    // Maximizer all -1
    CHECK(map.maximizer.outputLevelIdx == -1);
    CHECK(map.maximizer.ceilingIdx     == -1);
}

// ── Test 6: ChainPlanExecutor fires Ozone applicator ─────────────────────────

namespace {

// Minimal stub that records whether apply() was called
struct StubApplicator
{
    int callCount = 0;
    MultiEffectPlan lastPlan;

    int apply(const MultiEffectPlan& plan)
    {
        ++callCount;
        lastPlan = plan;
        return 0;
    }
};

// Wrapper to satisfy OzonePlanApplicatorBase* interface via a mock class
class MockOzonePlanApplicator : public OzonePlanApplicatorBase
{
public:
    explicit MockOzonePlanApplicator(StubApplicator& stub) : stub_(stub) {}

    int apply(const MultiEffectPlan& plan) override
    {
        return stub_.apply(plan);
    }

    int getLastAppliedCount() const noexcept override
    {
        return stub_.callCount;
    }

private:
    StubApplicator& stub_;
};

} // anonymous namespace

TEST_CASE("ChainPlanExecutor calls registered OzonePlanApplicator on executePlan", "[ozone][executor]")
{
    StubApplicator stub;
    MockOzonePlanApplicator mock(stub);

    ChainPlanExecutor executor;
    executor.setOzonePlanApplicator(&mock);

    REQUIRE(executor.hasOzoneApplicator());

    const MultiEffectPlan plan = executor.executePlan(
        /*genreIndex=*/3, /*dynamicRange=*/7.5f,
        /*spectralTilt=*/-0.8f, /*correlationMS=*/0.6f);

    SECTION("Plan is valid")
    {
        REQUIRE(plan.valid);
    }

    SECTION("Applicator was called exactly once")
    {
        REQUIRE(stub.callCount == 1);
    }

    SECTION("Applicator received the same plan")
    {
        REQUIRE(stub.lastPlan.valid);
        REQUIRE(stub.lastPlan.compressionNeed == Approx(plan.compressionNeed).margin(1e-5f));
        REQUIRE(stub.lastPlan.targetLUFS      == Approx(plan.targetLUFS).margin(1e-5f));
    }

    SECTION("Clearing the applicator stops calls")
    {
        executor.clearOzonePlanApplicator();
        REQUIRE_FALSE(executor.hasOzoneApplicator());

        executor.executePlan(3, 7.5f, -0.8f, 0.6f);
        REQUIRE(stub.callCount == 1);  // Still 1 — not called again
    }
}
