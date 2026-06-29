// tests/Unit/TestSonicMasterDecisionDecoder.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "AI/SonicMasterDecisionDecoder.h"
#include "AI/MasteringTargetCurves.h"
#include "Core/NeuralMasteringTypes.h"

#include <cmath>
#include <limits>
#include <array>
#include <algorithm>

namespace {

// Build a decision vector where every slot encodes its own index /10 so each
// field can be asserted independently after decode.
void fillIndexScaled(float (&decision)[more_phi::kSonicMasterDecisionWidth])
{
    for (std::size_t i = 0; i < more_phi::kSonicMasterDecisionWidth; ++i)
        decision[i] = static_cast<float>(i) * 0.1f;
}

} // namespace

TEST_CASE("decodeSonicMasterDecision maps EQ gains to eq[0..7] scaled into AdaptiveEQ range",
          "[SonicMaster][Decoder]")
{
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    fillIndexScaled(decision);
    // Set sane, in-range EQ gains (dB) so the clamp path is not exercised here.
    for (std::size_t i = 0; i < more_phi::kSonicMasterEqGainCount; ++i)
        decision[i] = static_cast<float>(i) - 3.5f; // -3.5 .. +3.5 dB

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));

    REQUIRE(plan.valid);
    REQUIRE(plan.appliedMask.eq);
    // kAdaptiveEqMaxGainDb == 12. Decoded target = gain_db / kAdaptiveEqMaxGainDb.
    for (std::size_t i = 0; i < more_phi::kSonicMasterEqGainCount; ++i)
    {
        const float expected = (static_cast<float>(i) - 3.5f) / more_phi::kAdaptiveEqMaxGainDb;
        CHECK_THAT(plan.projectedTargets.eq[i],
                   Catch::Matchers::WithinAbs(expected, 1e-4f));
    }
}

TEST_CASE("decodeSonicMasterDecision clamps out-of-range EQ to +/-kMaxGainDB",
          "[SonicMaster][Decoder]")
{
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    decision[0] = 50.0f;  // far over +12 dB
    decision[1] = -50.0f; // far under -12 dB

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));

    CHECK_THAT(plan.projectedTargets.eq[0], Catch::Matchers::WithinAbs(1.0f, 1e-4f));  // +12/12
    CHECK_THAT(plan.projectedTargets.eq[1], Catch::Matchers::WithinAbs(-1.0f, 1e-4f)); // -12/12
}

TEST_CASE("decodeSonicMasterDecision maps target LUFS into loudness band",
          "[SonicMaster][Decoder]")
{
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    decision[more_phi::kSonicMasterTargetLufsIdx] = -14.0f;

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));

    REQUIRE(plan.appliedMask.loudness);
    // AutoMasteringEngine::applyValidatedPlan maps loudness[0] as
    // -14 + value*6 clamped to [-23,-8]. So a target of -14 LUFS must decode
    // to value 0.0 (so the formula yields exactly -14).
    CHECK_THAT(plan.projectedTargets.loudness[0], Catch::Matchers::WithinAbs(0.0f, 1e-4f));
}

TEST_CASE("decodeSonicMasterDecision clamps LUFS to engine's [-23,-8] range",
          "[SonicMaster][Decoder]")
{
    // AUDIT-4 regression: an extreme target of -6 LUFS is beyond the engine's
    // output clamp. The decoder must clamp it to -8 (value = 1.0) so the
    // round-trip math matches what applyValidatedPlan actually delivers — not
    // value=1.33, which would silently drop to -8 and lie to telemetry.
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    decision[more_phi::kSonicMasterTargetLufsIdx] = -6.0f;

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));
    CHECK_THAT(plan.projectedTargets.loudness[0], Catch::Matchers::WithinAbs(1.0f, 1e-4f));

    decision[more_phi::kSonicMasterTargetLufsIdx] = -30.0f;
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));
    // -23 -> (-23+14)/6 = -1.5
    CHECK_THAT(plan.projectedTargets.loudness[0], Catch::Matchers::WithinAbs(-1.5f, 1e-4f));
}

TEST_CASE("decodeSonicMasterDecision maps true-peak ceiling into limiter band (telemetry, mask off)",
          "[SonicMaster][Decoder]")
{
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    decision[more_phi::kSonicMasterTruePeakIdx] = -1.0f;

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));

    // The ceiling is decoded into limiter[0] for callers that opt into the
    // high-risk limiter mask, but the default appliedMask.limiter stays OFF so
    // the safety policy (which treats limiter as high-risk) accepts the plan.
    CHECK_FALSE(plan.appliedMask.limiter);
    // applyValidatedPlan: ceiling = -1 + limiter[0]*0.5. A -1 dBTP target must
    // decode to limiter[0] == 0.0 so a caller that enables the mask reproduces it.
    CHECK_THAT(plan.projectedTargets.limiter[0], Catch::Matchers::WithinAbs(0.0f, 1e-4f));
}

TEST_CASE("decodeSonicMasterDecision fills the 3-band compressor block",
          "[SonicMaster][Decoder]")
{
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    // Comp band 0: threshold=-20,ratio=2.5,attack=10,release=100,makeup=0,knee=3
    decision[more_phi::kSonicMasterCompOffset + 0] = -20.0f;
    decision[more_phi::kSonicMasterCompOffset + 1] = 2.5f;
    decision[more_phi::kSonicMasterCompOffset + 2] = 10.0f;
    decision[more_phi::kSonicMasterCompOffset + 3] = 100.0f;
    decision[more_phi::kSonicMasterCompOffset + 4] = 0.0f;
    decision[more_phi::kSonicMasterCompOffset + 5] = 3.0f;

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));

    REQUIRE(plan.appliedMask.dynamics);
    // AUDIT-3: paired layout — dynamics[2*band]=threshold value, [2*band+1]=ratio value.
    // threshold=-20 -> (-20+20)/8 = 0.0; ratio=2.5 -> (2.5-3.5)/2.5 = -0.4
    // (decoder maps ratio over [1,6]: center=3.5, scale=2.5, matching
    // kSonicMasterCompRatioMax=6.0)
    CHECK_THAT(plan.projectedTargets.dynamics[0], Catch::Matchers::WithinAbs(0.0f, 1e-3f));
    CHECK_THAT(plan.projectedTargets.dynamics[1], Catch::Matchers::WithinAbs(-0.4f, 1e-3f));

    // AUDIT-2.1: full real-unit sidecar carries all six params per band.
    REQUIRE(plan.hasCompParams);
    const auto& cp0 = plan.compParams[0];
    CHECK_THAT(cp0.thresholdDb, Catch::Matchers::WithinAbs(-20.0f, 1e-4f));
    CHECK_THAT(cp0.ratio,       Catch::Matchers::WithinAbs(  2.5f, 1e-4f));
    CHECK_THAT(cp0.attackMs,    Catch::Matchers::WithinAbs( 10.0f, 1e-4f));
    CHECK_THAT(cp0.releaseMs,   Catch::Matchers::WithinAbs(100.0f, 1e-4f));
    CHECK_THAT(cp0.makeupDb,    Catch::Matchers::WithinAbs(  0.0f, 1e-4f));
    CHECK_THAT(cp0.kneeDb,      Catch::Matchers::WithinAbs(  3.0f, 1e-4f));
}

TEST_CASE("decodeSonicMasterDecision fills compParams for all 3 bands and clamps",
          "[SonicMaster][Decoder][audit-2.1]")
{
    // AUDIT-2.1: every band's sidecar is populated; out-of-range inputs clamp to
    // the decoder's advertised ranges. All bands get EXPLICIT in-range-or-edge
    // values — we never rely on zero-init clamping, since clamp(0,-40,-6)==-6
    // and that reads as "intentional -6 dB" rather than "unset".
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    // Band 0: in-range sane values.
    decision[more_phi::kSonicMasterCompOffset + 0] = -18.0f;  // thr
    decision[more_phi::kSonicMasterCompOffset + 1] = 3.0f;    // ratio
    decision[more_phi::kSonicMasterCompOffset + 2] = 5.0f;    // attack
    decision[more_phi::kSonicMasterCompOffset + 3] = 80.0f;   // release
    decision[more_phi::kSonicMasterCompOffset + 4] = 2.0f;    // makeup
    decision[more_phi::kSonicMasterCompOffset + 5] = 4.0f;    // knee
    // Band 1: extreme out-of-range (attack too fast, makeup too high, knee negative).
    const std::size_t b1 = more_phi::kSonicMasterCompOffset + more_phi::kSonicMasterCompBandWidth;
    decision[b1 + 0] = -16.0f;
    decision[b1 + 1] = 4.0f;
    decision[b1 + 2] = 0.001f;   // below 0.1 floor -> clamps to 0.1
    decision[b1 + 3] = 50.0f;
    decision[b1 + 4] = 99.0f;    // above 12 ceiling -> clamps to 12
    decision[b1 + 5] = -5.0f;    // below 0 floor -> clamps to 0
    // Band 2: explicit in-range values (NOT zero-init).
    const std::size_t b2 = more_phi::kSonicMasterCompOffset + 2 * more_phi::kSonicMasterCompBandWidth;
    decision[b2 + 0] = -24.0f;
    decision[b2 + 1] = 2.0f;
    decision[b2 + 2] = 20.0f;
    decision[b2 + 3] = 180.0f;
    decision[b2 + 4] = 3.0f;
    decision[b2 + 5] = 6.0f;

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));
    REQUIRE(plan.hasCompParams);

    const auto& cp0 = plan.compParams[0];
    CHECK_THAT(cp0.thresholdDb, Catch::Matchers::WithinAbs(-18.0f, 1e-4f));
    CHECK_THAT(cp0.ratio,       Catch::Matchers::WithinAbs(  3.0f, 1e-4f));
    CHECK_THAT(cp0.attackMs,    Catch::Matchers::WithinAbs(  5.0f, 1e-4f));
    CHECK_THAT(cp0.releaseMs,   Catch::Matchers::WithinAbs( 80.0f, 1e-4f));
    CHECK_THAT(cp0.makeupDb,    Catch::Matchers::WithinAbs(  2.0f, 1e-4f));
    CHECK_THAT(cp0.kneeDb,      Catch::Matchers::WithinAbs(  4.0f, 1e-4f));

    const auto& cp1 = plan.compParams[1];
    CHECK_THAT(cp1.thresholdDb, Catch::Matchers::WithinAbs(-16.0f, 1e-4f));
    CHECK_THAT(cp1.attackMs,    Catch::Matchers::WithinAbs(  0.1f, 1e-4f));  // clamped up
    CHECK_THAT(cp1.makeupDb,    Catch::Matchers::WithinAbs( 12.0f, 1e-4f));  // clamped down
    CHECK_THAT(cp1.kneeDb,      Catch::Matchers::WithinAbs(  0.0f, 1e-4f));  // clamped up

    const auto& cp2 = plan.compParams[2];
    CHECK_THAT(cp2.thresholdDb, Catch::Matchers::WithinAbs(-24.0f, 1e-4f));
    CHECK_THAT(cp2.ratio,       Catch::Matchers::WithinAbs(  2.0f, 1e-4f));
    CHECK_THAT(cp2.attackMs,    Catch::Matchers::WithinAbs( 20.0f, 1e-4f));
    CHECK_THAT(cp2.releaseMs,   Catch::Matchers::WithinAbs(180.0f, 1e-4f));
    CHECK_THAT(cp2.makeupDb,    Catch::Matchers::WithinAbs(  3.0f, 1e-4f));
    CHECK_THAT(cp2.kneeDb,      Catch::Matchers::WithinAbs(  6.0f, 1e-4f));
}

TEST_CASE("decodeSonicMasterDecision decodes threshold and ratio independently",
          "[SonicMaster][Decoder]")
{
    // AUDIT-3 regression: a high threshold + LOW ratio must be expressible.
    // Old code coupled them through one scalar; this would have forced ratio up.
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    decision[more_phi::kSonicMasterCompOffset + 0] = -12.0f;  // high threshold
    decision[more_phi::kSonicMasterCompOffset + 1] = 1.5f;    // gentle ratio
    decision[more_phi::kSonicMasterCompOffset + 6] = -16.0f;  // band 1 threshold
    decision[more_phi::kSonicMasterCompOffset + 7] = 4.0f;    // band 1 ratio

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));

    // Band 0: threshold -12 -> (-12+20)/8 = 1.0; ratio 1.5 -> (1.5-3.5)/2.5 = -0.8
    // (decoder maps ratio over [1,6]: center=3.5, scale=2.5, matching
    // kSonicMasterCompRatioMax=6.0 which the DSP clamps to)
    CHECK_THAT(plan.projectedTargets.dynamics[0], Catch::Matchers::WithinAbs(1.0f, 1e-3f));
    CHECK_THAT(plan.projectedTargets.dynamics[1], Catch::Matchers::WithinAbs(-0.8f, 1e-3f));
    // Band 1: threshold -16 -> (-16+20)/8 = 0.5; ratio 4.0 -> (4.0-3.5)/2.5 = 0.2
    CHECK_THAT(plan.projectedTargets.dynamics[2], Catch::Matchers::WithinAbs(0.5f, 1e-3f));
    CHECK_THAT(plan.projectedTargets.dynamics[3], Catch::Matchers::WithinAbs(0.2f, 1e-3f));
}

TEST_CASE("decodeSonicMasterDecision coerces NaN to neutral without throwing",
          "[SonicMaster][Decoder]")
{
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    for (auto& v : decision) v = std::numeric_limits<float>::quiet_NaN();

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));

    // Every target must be finite; EQ neutral = 0.0, LUFS neutral (=-14) -> 0.0.
    for (std::size_t i = 0; i < more_phi::kSonicMasterEqGainCount; ++i)
        CHECK(std::isfinite(plan.projectedTargets.eq[i]));
    CHECK(std::isfinite(plan.projectedTargets.loudness[0]));
    CHECK(std::isfinite(plan.projectedTargets.limiter[0]));
}

TEST_CASE("decodeSonicMasterDecision rejects null/bad args",
          "[SonicMaster][Decoder]")
{
    more_phi::ValidatedNeuralMasteringPlan plan {};
    CHECK_FALSE(more_phi::decodeSonicMasterDecision(nullptr, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    CHECK_FALSE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 0.0, plan));
    CHECK_FALSE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth - 1, 48000.0, plan));
}

// ── Stage A (2026-06-26): caller target_lufs override ─────────────────────────
// The ONNX graph takes only the waveform (1 input) and cannot condition on a
// target during inference. So the model's loudness slot is a recommendation,
// and a caller (profile / manual / closed-loop correction) must be able to
// override it at decode time. The default (kUseModelTargetLufs) honors the
// model's own value — "apply the recommendation" semantics.
TEST_CASE("decodeSonicMasterDecision honors caller target_lufs override (Stage A)",
          "[SonicMaster][Decoder][StageA]")
{
    // Model recommends -14 LUFS (value 0.0).
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    decision[more_phi::kSonicMasterTargetLufsIdx] = -14.0f;

    // Default: use the model's recommendation.
    {
        more_phi::ValidatedNeuralMasteringPlan plan {};
        REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));
        CHECK_THAT(plan.projectedTargets.loudness[0], Catch::Matchers::WithinAbs(0.0f, 1e-4f));
    }
    // Explicit caller target -11 LUFS overrides: value = (-11+14)/6 = 0.5.
    {
        more_phi::ValidatedNeuralMasteringPlan plan {};
        REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan, -11.0f));
        REQUIRE(plan.appliedMask.loudness);
        CHECK_THAT(plan.projectedTargets.loudness[0], Catch::Matchers::WithinAbs(0.5f, 1e-4f));
        // Still a target, not a measurement.
        CHECK(plan.loudnessIsMeasurement == false);
    }
    // Caller target is clamped to the engine's [-23,-8] range: -6 -> -8 (value 1.0).
    {
        more_phi::ValidatedNeuralMasteringPlan plan {};
        REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan, -6.0f));
        CHECK_THAT(plan.projectedTargets.loudness[0], Catch::Matchers::WithinAbs(1.0f, 1e-4f));
    }
    // Sentinel default == not passing it (honors model).
    {
        more_phi::ValidatedNeuralMasteringPlan a {}, b {};
        REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, a));
        REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, b, more_phi::kUseModelTargetLufs));
        CHECK_THAT(a.projectedTargets.loudness[0], Catch::Matchers::WithinAbs(b.projectedTargets.loudness[0], 1e-6f));
    }
}

// ── Stage 2 (Ozone §3.2): genre curve + measured tonal-balance residual blend ─
// The model emits an EQ recommendation blind to a target curve. With a curve +
// measured per-band shape supplied, the decoder adds a bounded residual
// (target − measured), clamped to ±6 dB per band and scaled by residualBlend,
// BEFORE the ±12 dB clamp. Default args preserve the original decode.
TEST_CASE("decodeSonicMasterDecision default eqPrior leaves EQ unchanged",
          "[SonicMaster][Decoder][Stage2]")
{
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    decision[0] = 3.0f;  // band 0 = +3 dB

    more_phi::ValidatedNeuralMasteringPlan a {}, b {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, a));
    // Default GenreEqPrior {} → no blend.
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, b,
                                                more_phi::kUseModelTargetLufs, more_phi::GenreEqPrior{}));
    for (std::size_t i = 0; i < more_phi::kSonicMasterEqGainCount; ++i)
        CHECK_THAT(a.projectedTargets.eq[i], Catch::Matchers::WithinAbs(b.projectedTargets.eq[i], 1e-6f));
}

TEST_CASE("decodeSonicMasterDecision blends residual toward the target curve",
          "[SonicMaster][Decoder][Stage2]")
{
    // Model recommends 0 dB on band 0. Use the kHipHopRnB curve which boosts
    // band 0 (+3 dB at 60 Hz) and measure a flat shape (0 dB). The residual at
    // band 0 = (3 − 0) = +3 dB; at residualBlend=1.0 the band-0 gain moves from
    // 0 → +3 dB. Without the prior it stays at 0.
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    decision[0] = 0.0f;

    const auto* curve = more_phi::findTargetCurve("hip_hop_rnb");
    REQUIRE(curve != nullptr);

    std::array<float, more_phi::kSonicMasterEqGainCount> measured{};
    measured.fill(0.0f);  // flat shape

    more_phi::GenreEqPrior prior { curve, measured.data(), 1.0f };

    more_phi::ValidatedNeuralMasteringPlan planWith {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0,
                                                planWith, more_phi::kUseModelTargetLufs, prior));
    // band 0 normalized gain: 0 dB model + 3 dB residual (blend 1.0) = +3 dB / 12.
    const float expectedNorm = 3.0f / more_phi::kAdaptiveEqMaxGainDb;
    CHECK_THAT(planWith.projectedTargets.eq[0], Catch::Matchers::WithinAbs(expectedNorm, 1e-4f));

    // Without the prior, band 0 stays at the model's 0 dB → normalized 0.0.
    more_phi::ValidatedNeuralMasteringPlan planWithout {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, planWithout));
    CHECK_THAT(planWithout.projectedTargets.eq[0], Catch::Matchers::WithinAbs(0.0f, 1e-4f));
}

TEST_CASE("decodeSonicMasterDecision residual is clamped to +-6 dB per band",
          "[SonicMaster][Decoder][Stage2]")
{
    // Curve boosts band 0 by a huge amount (simulate by measuring very negative)
    // — residual must clamp at +6 dB even with blend 1.0, never exceeding the
    // ±12 dB master clamp.
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    decision[0] = 0.0f;

    const auto* curve = more_phi::findTargetCurve("hip_hop_rnb");  // band0 +3
    std::array<float, more_phi::kSonicMasterEqGainCount> measured{};
    measured[0] = -50.0f;  // measured far below → residual would be +53 dB unclamped
    more_phi::GenreEqPrior prior { curve, measured.data(), 1.0f };

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0,
                                                plan, more_phi::kUseModelTargetLufs, prior));
    // Clamped residual = +6 dB → normalized 6/12 = 0.5.
    CHECK_THAT(plan.projectedTargets.eq[0], Catch::Matchers::WithinAbs(0.5f, 1e-4f));
}

TEST_CASE("decodeSonicMasterDecision residualBlend scales the correction",
          "[SonicMaster][Decoder][Stage2]")
{
    // residual = +3 dB, blend 0.5 → +1.5 dB correction on top of model's 0 dB.
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    decision[0] = 0.0f;

    const auto* curve = more_phi::findTargetCurve("hip_hop_rnb");
    std::array<float, more_phi::kSonicMasterEqGainCount> measured{};
    measured.fill(0.0f);
    more_phi::GenreEqPrior prior { curve, measured.data(), 0.5f };

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0,
                                                plan, more_phi::kUseModelTargetLufs, prior));
    // 3 dB * 0.5 blend = 1.5 dB → normalized 1.5/12 = 0.125.
    CHECK_THAT(plan.projectedTargets.eq[0], Catch::Matchers::WithinAbs(0.125f, 1e-4f));
}

TEST_CASE("decodeSonicMasterDecision zero residualBlend disables blend",
          "[SonicMaster][Decoder][Stage2]")
{
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    decision[0] = 0.0f;

    const auto* curve = more_phi::findTargetCurve("hip_hop_rnb");
    std::array<float, more_phi::kSonicMasterEqGainCount> measured{};
    measured.fill(0.0f);
    more_phi::GenreEqPrior prior { curve, measured.data(), 0.0f };  // disabled

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0,
                                                plan, more_phi::kUseModelTargetLufs, prior));
    // blend 0 → no correction, band stays at model's 0 dB.
    CHECK_THAT(plan.projectedTargets.eq[0], Catch::Matchers::WithinAbs(0.0f, 1e-4f));
}

// ── AUDIT-FIX (2026-06-29) tests: stereo 4-region fill + CompNorm usage ──

TEST_CASE("decodeSonicMasterDecision extends stereo regions 2-3 from region 1",
          "[SonicMaster][Decoder]")
{
    // AUDIT-FIX (P7/L1-5): the decoder should fill ALL 4 stereo regions,
    // not just 2. Regions 2 and 3 are copies of region 1 (mid and mid-high).
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    fillIndexScaled(decision);

    // Set specific stereo values: region 0 = 0.3, region 1 = -0.2
    decision[more_phi::kSonicMasterStereoOffset + 0] =  0.3f;
    decision[more_phi::kSonicMasterStereoOffset + 1] = -0.2f;

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));
    REQUIRE(plan.appliedMask.stereo);

    // Region 0 is the directly decoded value
    CHECK_THAT(plan.projectedTargets.stereo[0], Catch::Matchers::WithinAbs(0.3f, 1e-5f));
    // Region 1 is the directly decoded value
    CHECK_THAT(plan.projectedTargets.stereo[1], Catch::Matchers::WithinAbs(-0.2f, 1e-5f));
    // Regions 2-3 are copies of region 1 (P7 extension)
    CHECK_THAT(plan.projectedTargets.stereo[2], Catch::Matchers::WithinAbs(-0.2f, 1e-5f));
    CHECK_THAT(plan.projectedTargets.stereo[3], Catch::Matchers::WithinAbs(-0.2f, 1e-5f));
}

TEST_CASE("decodeSonicMasterDecision uses CompNorm constants for threshold normalization",
          "[SonicMaster][Decoder]")
{
    // AUDIT-FIX (L1-6): the decoder should normalize threshold via
    // (value - CompNorm::kThresholdCenterDb) / CompNorm::kThresholdHalfRangeDb,
    // not (value + 20.0f) / 8.0f (the old magic-number form).
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    fillIndexScaled(decision);

    // Set a specific threshold value: -12 dB
    const float threshold = -12.0f;
    decision[more_phi::kSonicMasterCompOffset + 0] = threshold;

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));
    REQUIRE(plan.hasCompParams);

    // Expected: (-12 - (-20)) / 8 = 8/8 = 1.0  (clamped from +1.0)
    const float expected = (threshold - more_phi::CompNorm::kThresholdCenterDb)
                           / more_phi::CompNorm::kThresholdHalfRangeDb;
    CHECK_THAT(plan.projectedTargets.dynamics[0],
               Catch::Matchers::WithinAbs(std::clamp(expected, -1.0f, 1.0f), 1e-4f));
}

TEST_CASE("decodeSonicMasterDecision uses CompNorm constants for ratio normalization",
          "[SonicMaster][Decoder]")
{
    // AUDIT-FIX (L1-6): the decoder should normalize ratio via
    // (value - CompNorm::kRatioCenter) / CompNorm::kRatioHalfRange.
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    fillIndexScaled(decision);

    // Set a specific ratio: 2.0 (below center 3.5)
    const float ratio = 2.0f;
    decision[more_phi::kSonicMasterCompOffset + 1] = ratio;

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));
    REQUIRE(plan.hasCompParams);

    // Expected: (2.0 - 3.5) / 2.5 = -0.6
    const float expected = (ratio - more_phi::CompNorm::kRatioCenter)
                           / more_phi::CompNorm::kRatioHalfRange;
    CHECK_THAT(plan.projectedTargets.dynamics[1],
               Catch::Matchers::WithinAbs(std::clamp(expected, -1.0f, 1.0f), 1e-4f));
}
