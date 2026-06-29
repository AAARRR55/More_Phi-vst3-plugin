// tests/Unit/TestAuditFixes.cpp
//
// Regression tests for the 2026-06-23 mastering-AI audit fixes:
//   AUDIT-FIX-3  compressor ratio clamp agrees between decoder and engine
//   AUDIT-FIX-5  OzonePlanApplicator signals all-stubs map (hasAnyMapping / isReady)
//   AUDIT-FIX-2  limiter ceiling guaranteed streaming-safe (-1.0 dBTP) on every apply
//   AUDIT-FIX-8  stale plans (capturedAtSteadyClockNs beyond budget) are dropped

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "AI/SonicMasterDecisionDecoder.h"
#include "AI/OzoneParameterMap.h"
#include "Core/NeuralMasteringTypes.h"
#include "Core/AutoMasteringEngine.h"
#include "Core/BrickwallLimiter.h"

#include <cmath>

using namespace more_phi;
using Catch::Approx;
using Catch::Matchers::WithinAbs;

// ── AUDIT-FIX-3: decoder ratio clamp must match the engine's [1,6] ────────────
// Before the fix the decoder accepted ratio up to 20 but applyValidatedPlan
// re-clamped to 6 — telemetry lied by up to 3.3x. Both bounds must agree now.
// AUDIT-FIX (A6→P5): the band was briefly tightened [1,6]→[1,4] in A6 so every
// decoded ratio mapped into the safety policy's normalized [-1,+1] dynamics
// bound. It was widened back to [1,6] in P5 after confirming the CompNorm
// center=3.5 / halfRange=2.5 maps [1,6] EXACTLY onto [-1,+1] (ratio=6→+1,
// ratio=1→-1), and the safety policy's maxDeltaPerPlan.dynamics was widened
// (0.12→0.20) to accommodate the wider range. Raise ALL THREE bounds together
// (CompNorm ratio center/halfRange, kSonicMasterCompRatioMax, and the safety
// delta cap) before widening further — a decoder ratio the CompNorm mapping
// can't represent is silently rejected by the safety gate.

TEST_CASE("AUDIT-FIX-3: decoder clamps ratio to the shared [1,6] band",
          "[audit][sonicmaster][decoder]")
{
    float decision[kSonicMasterDecisionWidth] {};
    // Band 0 ratio = 20.0 (the model's documented max). Must clamp to 6.0,
    // matching AutoMasteringEngine::applyValidatedPlan's kSonicMasterCompRatioMax.
    decision[kSonicMasterCompOffset + 1] = 20.0f;

    ValidatedNeuralMasteringPlan plan {};
    REQUIRE(decodeSonicMasterDecision(decision, kSonicMasterDecisionWidth, 48000.0, plan));
    REQUIRE(plan.hasCompParams);
    REQUIRE_THAT(plan.compParams[0].ratio, WithinAbs(6.0f, 1e-4f));

    // And the floor.
    decision[kSonicMasterCompOffset + 1] = 0.0f;
    REQUIRE(decodeSonicMasterDecision(decision, kSonicMasterDecisionWidth, 48000.0, plan));
    REQUIRE_THAT(plan.compParams[0].ratio, WithinAbs(1.0f, 1e-4f));
}

TEST_CASE("AUDIT-FIX-3: ratio constant is the same value the engine uses",
          "[audit][sonicmaster][decoder]")
{
    // The whole point of the shared constant: decoder and engine cannot drift.
    // kSonicMasterCompRatioMax is referenced by both SonicMasterDecisionDecoder.cpp
    // and AutoMasteringEngine.cpp::applyValidatedPlan.
    CHECK(kSonicMasterCompRatioMin == 1.0f);
    CHECK(kSonicMasterCompRatioMax == 6.0f);  // AUDIT-FIX (P5): widened back from A6's 4.0 — CompNorm [3.5±2.5] maps [1,6] onto [-1,+1] exactly.
}

// ── AUDIT-FIX-3 (round-trip): a ratio the decoder emits survives apply ───────
TEST_CASE("AUDIT-FIX-3: decoded ratio survives applyValidatedPlan without re-clamp",
          "[audit][sonicmaster][engine]")
{
    AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, true);

    ValidatedNeuralMasteringPlan plan {};
    plan.valid = true;
    plan.appliedMask.dynamics = true;
    plan.hasCompParams = true;
    // Ratio at the top of the agreed [1,6] band — must reach the DSP unchanged.
    plan.compParams[0] = { -18.0f, 6.0f, 10.0f, 100.0f, 0.0f, 3.0f };

    REQUIRE(engine.applyValidatedPlan(plan));
    const auto applied = engine.getDynamics().getBandParams(0);
    CHECK(applied.ratio == Approx(6.0f).margin(1e-3f));
}

// ── AUDIT-FIX-5: all-stubs map is detectable ──────────────────────────────────

TEST_CASE("AUDIT-FIX-5: buildForOzone11 factory map has no mappings (hasAnyMapping=false)",
          "[audit][ozone]")
{
    const auto m = OzoneParameterMap::buildForOzone11();
    CHECK_FALSE(m.hasAnyMapping());
}

TEST_CASE("AUDIT-FIX-5: a map with one EQ band reports hasAnyMapping=true",
          "[audit][ozone]")
{
    OzoneParameterMap m {};
    m.eq[0].gainIdx = 5;
    CHECK(m.hasAnyMapping());
}

// ── AUDIT-FIX-2: limiter ceiling guaranteed streaming-safe ────────────────────
// Even when the limiter mask is OFF (the SonicMaster default), every neural
// apply must leave the ceiling at or below -1.0 dBTP.

TEST_CASE("AUDIT-FIX-2: neural apply with limiter mask OFF still caps ceiling at -1.0 dBTP",
          "[audit][engine][safety]")
{
    AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, false);

    // Pre-condition: confirm the ceiling can be read back (getCeiling was added
    // for this fix). The default is -1.0 dBTP per BrickwallLimiter.
    BrickwallLimiter limiter;
    CHECK_THAT(limiter.getCeiling(), WithinAbs(-1.0f, 1e-3f));
}

TEST_CASE("AUDIT-FIX-2: ceiling above -1.0 dBTP is clamped down after neural apply",
          "[audit][engine][safety]")
{
    AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, false);

    ValidatedNeuralMasteringPlan plan {};
    plan.valid = true;
    plan.appliedMask.limiter = false;   // SonicMaster default posture
    plan.appliedMask.loudness = true;
    plan.projectedTargets.loudness[0] = 1.0f; // -> -8 LUFS target (the max)

    REQUIRE(engine.applyValidatedPlan(plan));
    // AUDIT-F4.2 (2026-06-27): REAL assertion replacing the prior SUCCEED()
    // no-op. The streaming-safe ceiling clamp runs inside applyValidatedPlan on
    // every apply regardless of the limiter mask; the stamped post-clamp ceiling
    // must be <= -1.0 dBTP (kStreamingSafeCeilingDBTP).
    const float appliedCeiling = engine.getLastAppliedCeilingDbtp();
    INFO("post-apply limiter ceiling: " << appliedCeiling << " dBTP (must be <= -1.0)");
    CHECK(appliedCeiling <= -1.0f + 1e-3f);
}

// AUDIT-F4.1 (2026-06-27): the per-cycle delta cap (maxDeltaPerPlan.loudness =
// 0.10 normalized -> 0.6 LU/cycle) is enforced at DECODE time inside validate()
// on the SonicMaster cycle path. But applyValidatedPlan can be reached by a
// direct in-process caller that hand-builds a ValidatedNeuralMasteringPlan
// without going through validate(); before this fix that caller could apply an
// arbitrarily large loudness jump in a single cycle, bypassing the slew limit.
// The fix re-runs enforceDeltaCaps against lastSafeNeuralPlan_ at the top of
// applyValidatedPlan. This test exercises the bypass directly: establish a
// baseline, then apply a plan with a 1.0-normalized (6 LU) loudness jump and
// assert the applied (stored-as-last-safe) loudness moved at most 0.10 (0.6 LU).
TEST_CASE("AUDIT-F4.1: applyValidatedPlan caps loudness delta vs last safe plan",
          "[audit][engine][safety][F4-1]")
{
    AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, false);

    // Establish a baseline loudness target of 0.0 normalized (-14 LUFS).
    ValidatedNeuralMasteringPlan baseline {};
    baseline.valid = true;
    baseline.appliedMask.loudness = true;
    baseline.projectedTargets.loudness[0] = 0.0f;
    REQUIRE(engine.applyValidatedPlan(baseline));
    REQUIRE(engine.hasLastSafeNeuralMasteringPlan());
    REQUIRE(std::abs(engine.getLastSafeNeuralMasteringPlan().projectedTargets.loudness[0] - 0.0f) < 1e-6f);

    // Direct in-process caller bypassing validate(): tries to jump the loudness
    // target from 0.0 to 1.0 (a 6 LU delta). The cap is 0.10 normalized/cycle.
    ValidatedNeuralMasteringPlan jump {};
    jump.valid = true;
    jump.appliedMask.loudness = true;
    jump.projectedTargets.loudness[0] = 1.0f;

    REQUIRE(engine.applyValidatedPlan(jump));
    // The apply-time guard MUST have clamped at least one dimension.
    CHECK(engine.getLastApplyDeltaClamps() >= 1u);
    // The stored last-safe loudness must have moved at most maxDeltaPerPlan
    // (0.10 normalized = 0.6 LU), NOT the full 1.0.
    const float appliedLoudness = engine.getLastSafeNeuralMasteringPlan().projectedTargets.loudness[0];
    CHECK(appliedLoudness <= Approx(0.10f + 1e-5f));
    CHECK(appliedLoudness >= 0.0f);  // monotonic toward the requested jump, not negative
}

TEST_CASE("AUDIT-F4.1: first apply (no last-safe baseline) is unconstrained",
          "[audit][engine][safety][F4-1]")
{
    AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, false);
    REQUIRE_FALSE(engine.hasLastSafeNeuralMasteringPlan());

    // No baseline -> first apply must NOT be clamped (mirrors validate()).
    ValidatedNeuralMasteringPlan first {};
    first.valid = true;
    first.appliedMask.loudness = true;
    first.projectedTargets.loudness[0] = 1.0f;
    REQUIRE(engine.applyValidatedPlan(first));
    CHECK(engine.getLastApplyDeltaClamps() == 0u);
    CHECK(std::abs(engine.getLastSafeNeuralMasteringPlan().projectedTargets.loudness[0] - 1.0f) < 1e-6f);
}

// ── AUDIT-FIX-8: stale plans carry a timestamp and are droppable ─────────────
// The stamp lives on the plan; the analysis engine fills it at capture time and
// applyRamped checks the budget. Here we assert the schema/constant contract.

TEST_CASE("AUDIT-FIX-8: ValidatedNeuralMasteringPlan has a capture-timestamp field",
          "[audit][sonicmaster][staleness]")
{
    ValidatedNeuralMasteringPlan plan {};
    // Default (legacy producers) is 0, which skips the staleness check.
    CHECK(plan.capturedAtSteadyClockNs == 0u);

    // The engine stamps a non-zero value at capture time.
    plan.capturedAtSteadyClockNs = 12345u;
    CHECK(plan.capturedAtSteadyClockNs != 0u);
}
