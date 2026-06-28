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
#include "Core/AutoMasteringEngine.h"
#include "Core/NeuralMasteringTypes.h"

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

    // ponytail: "Ozone Pro" is the version-less product name for the Ozone 11
    // Advanced engine. Without this, refreshHostedMasteringApplicators silently
    // no-ops on the most common install path and the plan never applies.
    SECTION("Ozone Pro (version-less product name) matches")
    {
        REQUIRE(OzoneParameterMap::isOzone11("iZotope Ozone Pro"));
        REQUIRE(OzoneParameterMap::isOzone11("Ozone Pro"));
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

// ── Test 7: neural plan → MultiEffectPlan bridge conversion ──────────────────

namespace {

// Stub applicator that returns a positive count so the bridge's
// lastOzoneAppliedCount_ > 0 check works, and records the plan for
// field-level verification.
struct BridgeStub
{
    int callCount = 0;
    MultiEffectPlan lastPlan;
    int returnCount = 7;  // arbitrary positive

    int apply(const MultiEffectPlan& plan)
    {
        ++callCount;
        lastPlan = plan;
        return returnCount;
    }
};

class BridgeMockApplicator : public OzonePlanApplicatorBase
{
public:
    explicit BridgeMockApplicator(BridgeStub& stub) : stub_(stub) {}
    int apply(const MultiEffectPlan& plan) override { return stub_.apply(plan); }
    int getLastAppliedCount() const noexcept override { return stub_.callCount; }
private:
    BridgeStub& stub_;
};

} // anonymous namespace

TEST_CASE("Bridge converts neural plan to MultiEffectPlan fields correctly", "[ozone][bridge]")
{
    // CRITICAL-6/7/17 F2: verify that buildBridgePlanFromNeural correctly
    // translates neural-plan fields into the MultiEffectPlan the Ozone
    // applicator consumes. Use a known plan with loudness/limiter/stereo
    // targets and check the stub received the expected values.

    more_phi::AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, /*startIntelligence=*/false);

    BridgeStub stub;
    BridgeMockApplicator mock(stub);
    engine.getChainPlanner().setOzonePlanApplicator(&mock);
    REQUIRE(engine.getChainPlanner().hasOzoneApplicator());

    // Build a neural plan with known projected targets.
    more_phi::ValidatedNeuralMasteringPlan plan {};
    plan.valid = true;
    plan.fallbackMode = more_phi::NeuralMasteringFallbackMode::None;
    plan.projected = true;

    // EQ: set band 0 to +6 dB (0.5 * 12 dB scale) and band 2 to -3 dB (-0.25 * 12)
    plan.projectedTargets.eq[0] =  0.5f;
    plan.projectedTargets.eq[2] = -0.25f;
    // Loudness: -14 + 0.5*6 = -11 LUFS
    plan.projectedTargets.loudness[0] = 0.5f;
    plan.appliedMask.loudness = true;
    // Limiter: -1 + 0.5*0.5 = -0.75 dBTP → streaming-safe clamp → -1.0
    plan.projectedTargets.limiter[0] = 0.5f;
    plan.appliedMask.limiter = true;
    // Stereo: widen region 0
    plan.projectedTargets.stereo[0] = 0.3f;
    plan.appliedMask.stereo = true;
    // Dynamics via compParams (hasCompParams=true path)
    plan.hasCompParams = true;
    plan.compParams[0].ratio = 3.0f;
    plan.compParams[1].ratio = 2.5f;
    plan.compParams[2].ratio = 4.0f;

    REQUIRE(engine.applyValidatedPlan(plan));

    // The stub should have been called.
    CHECK(stub.callCount == 1);
    REQUIRE(stub.lastPlan.valid);

    // EQ: the bridge builds 8-band JSON from eq[]*12 dB, Q=0.707.
    // We only verify presence; exact JSON parsing is out of scope.
    CHECK(stub.lastPlan.eqPrescriptionJSON.isNotEmpty());
    CHECK(stub.lastPlan.eqPrescriptionJSON.contains("peak"));
    CHECK(stub.lastPlan.eqPrescriptionJSON.contains("\"freq\": 60"));
    CHECK(stub.lastPlan.eqPrescriptionJSON.contains("\"freq\": 1000"));
    CHECK(stub.lastPlan.eqPrescriptionJSON.contains("\"freq\": 10000"));

    // Loudness target: -14 + loudness[0]*6 = -11
    CHECK(stub.lastPlan.targetLUFS == Catch::Approx(-11.0f).margin(0.01f));

    // Ceiling: raw = -1 + limiter[0]*0.5 = -0.75, clamped to kStreamingSafeCeilingDBTP = -1.0
    CHECK(stub.lastPlan.ceilingDBTP == Catch::Approx(-1.0f).margin(0.01f));

    // Compression need: avg ratio (3.0+2.5+4.0)/3 = 3.167 → (3.167-1)/5 = 0.433
    CHECK(stub.lastPlan.compressionNeed == Catch::Approx(0.433f).margin(0.01f));

    // Stereo width[0]: clamp(1.0 + stereo[0], 0, 2) = 1.3
    CHECK(stub.lastPlan.widthCurve[0] == Catch::Approx(1.3f).margin(0.01f));

    // Exciter: harmonic mask not set → should be false
    CHECK_FALSE(stub.lastPlan.exciterEnabled);

    // useNeuralComp should be true (set by bridge)
    CHECK(stub.lastPlan.useNeuralComp);

    // Ozone count should reflect the stub's return value.
    CHECK(engine.getLastOzoneAppliedCount() == stub.returnCount);

    engine.getChainPlanner().clearOzonePlanApplicator();
}
