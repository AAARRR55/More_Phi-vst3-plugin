// tests/Unit/TestOzoneRoundTrip.cpp
//
// Round-trip identity tests for the chainPlanner adapter layer's plain↔normalized
// conversions, plus behavioural tests for the F3/F4 side-channels introduced in
// the 2026-06-30 audit fixes.
//
// The audit brief's criterion (c): plainParamToNormalized(normalizedParamToPlain(x))
// must equal x for each parameter class. The decode path clamps + maps real units
// into normalized [0,1]; the OzonePlanApplicator re-normalizes back to [0,1] for
// the hosted plugin. These two passes must agree on the range or the round-trip
// lies by a constant factor (the historical F2/F5/F7 bugs).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/catch_approx.hpp>

#include "AI/OzoneParameterMap.h"
#include "AI/SonicMasterDecisionDecoder.h"
#include "AI/OzonePlanApplicator.h"
#include "Core/NeuralMasteringTypes.h"
#include "Core/AdaptiveEQ.h"

#include <cmath>
#include <algorithm>
#include <array>

using namespace more_phi;
using Catch::Approx;
using Catch::Matchers::WithinAbs;

namespace {

// Inverse of OzoneParameterMap::normalizeGain over the default ±12 dB range.
// decode: gainDb = eq[i] * kMaxGainDB  (eq[i] in [-1,1])
// encode: normalized = normalizeGain(gainDb) = (gainDb + 12) / 24
// Round-trip: eq[i] -> gainDb = eq[i]*12 -> normalized = (eq[i]*12 + 12)/24 = (eq[i]+1)/2
//           -> which inverts to eq[i] = normalized*2 - 1. Identity holds.
float denormalizeGainDefault(float normalized)
{
    return normalized * (2.0f * AdaptiveEQ::kMaxGainDB) - AdaptiveEQ::kMaxGainDB;  // [-12, +12]
}

} // namespace

// ── F2: EQ gain round-trip over the decode range (±kMaxGainDB) ────────────────

TEST_CASE("EQ gain round-trip is identity over +/-kMaxGainDB (F2 default-arg fix)",
          "[ozone][roundtrip][f2]")
{
    REQUIRE(AdaptiveEQ::kMaxGainDB == Approx(12.0f).margin(1e-3f));

    for (float eqNorm = -1.0f; eqNorm <= 1.0f; eqNorm += 0.1f)
    {
        // decode: normalized model output -> real dB
        const float gainDb = eqNorm * AdaptiveEQ::kMaxGainDB;
        // encode: real dB -> hosted-plugin normalized (DEFAULT args, the F2 fix)
        const float encoded = OzoneParameterMap::normalizeGain(gainDb);
        // inverse: hosted-plugin normalized -> real dB
        const float roundTrippedDb = denormalizeGainDefault(encoded);
        // Identity: the round-trip must recover the original dB.
        CHECK(roundTrippedDb == Approx(gainDb).margin(1e-5f));
        // And the encoded value over ±12 must reach the extremes 0.0 and 1.0
        // (the prior ±18 default capped at 0.833 / 0.167).
        if (std::abs(eqNorm - 1.0f) < 1e-3f)
            CHECK(encoded == Approx(1.0f).margin(1e-5f));
        if (std::abs(eqNorm + 1.0f) < 1e-3f)
            CHECK(encoded == Approx(0.0f).margin(1e-5f));
    }
}

// ── F5: compressor ratio round-trip over the decode clamp [1, 6] ──────────────

TEST_CASE("Compressor ratio round-trip is identity over [1, kSonicMasterCompRatioMax] (F5)",
          "[ozone][roundtrip][f5]")
{
    // The decoder clamps ratio to [kSonicMasterCompRatioMin, kSonicMasterCompRatioMax] = [1, 6].
    // The applicator (F5 fix) now normalizes over the SAME [1, 6] range
    // (kOzoneRatioMin/kOzoneRatioMax), so the round-trip is identity. The prior
    // [1, 20] applicator range disagreed by ~3.3x.
    REQUIRE(kSonicMasterCompRatioMin == Approx(1.0f).margin(1e-5f));
    REQUIRE(kSonicMasterCompRatioMax == Approx(6.0f).margin(1e-5f));
    REQUIRE(OzonePlanApplicator::kOzoneRatioMin == Approx(1.0f).margin(1e-5f));
    REQUIRE(OzonePlanApplicator::kOzoneRatioMax == Approx(kSonicMasterCompRatioMax).margin(1e-5f));

    for (float ratio = 1.0f; ratio <= 6.0f; ratio += 0.5f)
    {
        // applicator encode: real ratio -> hosted normalized over [1, 6]
        const float encoded = std::clamp(
            (ratio - OzonePlanApplicator::kOzoneRatioMin) /
                (OzonePlanApplicator::kOzoneRatioMax - OzonePlanApplicator::kOzoneRatioMin),
            0.0f, 1.0f);
        // inverse: hosted normalized -> real ratio
        const float roundTripped = encoded * (OzonePlanApplicator::kOzoneRatioMax -
                                              OzonePlanApplicator::kOzoneRatioMin)
                                 + OzonePlanApplicator::kOzoneRatioMin;
        CHECK(roundTripped == Approx(ratio).margin(1e-5f));
    }

    // The decoder ceiling (6.0) must encode to exactly 1.0 (was 0.263 under [1,20]).
    const float ceilingEncoded = std::clamp(
        (kSonicMasterCompRatioMax - OzonePlanApplicator::kOzoneRatioMin) /
            (OzonePlanApplicator::kOzoneRatioMax - OzonePlanApplicator::kOzoneRatioMin),
        0.0f, 1.0f);
    CHECK(ceilingEncoded == Approx(1.0f).margin(1e-5f));
}

// ── F7: LUFS target round-trip over the decode range [-23, -8] ────────────────

TEST_CASE("LUFS target round-trip is identity over [-23, -8] (F7 range alignment)",
          "[ozone][roundtrip][f7]")
{
    // The decoder clamps targetLufs to [-23, -8] and maps (lufs+14)/6 into loudness[0].
    // The applicator (F7 fix) now normalizes over the SAME [-23, -8] range, so the
    // full-band round-trip is identity. The prior [-24, -6] range broke the identity
    // at the extremes: -8 LUFS decoded to value +1.0 but re-encoded as 0.889.
    for (float lufs = -23.0f; lufs <= -8.0f; lufs += 1.0f)
    {
        const float encoded = OzoneParameterMap::normalizeLUFS(lufs);  // default [-23,-8] now
        // inverse over the same range
        const float roundTripped = encoded * (-8.0f - (-23.0f)) + (-23.0f);
        CHECK(roundTripped == Approx(lufs).margin(1e-4f));
    }

    // Extremes must reach 0.0 and 1.0 exactly.
    CHECK(OzoneParameterMap::normalizeLUFS(-23.0f) == Approx(0.0f).margin(1e-5f));
    CHECK(OzoneParameterMap::normalizeLUFS( -8.0f) == Approx(1.0f).margin(1e-5f));
}

// ── Ceiling round-trip over [-3, -0.1] (unchanged by fixes; regression guard) ─

TEST_CASE("True-peak ceiling round-trip is identity over [-3, -0.1]",
          "[ozone][roundtrip]")
{
    for (float dbtp = -3.0f; dbtp <= -0.1f; dbtp += 0.1f)
    {
        const float encoded = OzoneParameterMap::normalizeCeiling(dbtp);
        const float roundTripped = encoded * (0.0f - (-3.0f)) + (-3.0f);
        CHECK(roundTripped == Approx(dbtp).margin(1e-4f));
    }
}

// ── F3: decoder sets applyLimiterCeiling when the model ceiling is safer ──────

TEST_CASE("Decoder sets applyLimiterCeiling when model ceiling <= streaming-safe (F3)",
          "[SonicMaster][Decoder][f3]")
{
    float decision[kSonicMasterDecisionWidth] {};

    // A conservative model ceiling (-1.5 dBTP, below the -1.0 streaming-safe floor)
    // must enable the applyLimiterCeiling side-channel so the hosted maximizer
    // honours the tighter ceiling.
    decision[kSonicMasterTruePeakIdx] = -1.5f;
    ValidatedNeuralMasteringPlan plan {};
    REQUIRE(decodeSonicMasterDecision(decision, kSonicMasterDecisionWidth, 48000.0, plan));
    CHECK(plan.applyLimiterCeiling);
    // The mask stays OFF (high-risk); only the side-channel fires.
    CHECK_FALSE(plan.appliedMask.limiter);

    // A looser model ceiling (-0.5 dBTP, above streaming-safe) must NOT enable
    // the side-channel — the engine's -1.0 force-clamp is the only guardrail.
    decision[kSonicMasterTruePeakIdx] = -0.5f;
    REQUIRE(decodeSonicMasterDecision(decision, kSonicMasterDecisionWidth, 48000.0, plan));
    CHECK_FALSE(plan.applyLimiterCeiling);
    CHECK_FALSE(plan.appliedMask.limiter);

    // Exactly at the streaming-safe ceiling (-1.0): safer-or-equal, so enabled.
    decision[kSonicMasterTruePeakIdx] = -1.0f;
    REQUIRE(decodeSonicMasterDecision(decision, kSonicMasterDecisionWidth, 48000.0, plan));
    CHECK(plan.applyLimiterCeiling);
}

// ── F4: decoder sets applyHarmonic when the exciter gate is high ──────────────

TEST_CASE("Decoder sets applyHarmonic when exciter gate is high, without raising mask (F4)",
          "[SonicMaster][Decoder][f4]")
{
    float decision[kSonicMasterDecisionWidth] {};

    // Exciter gate above 0.5 + non-trivial drive/mix must enable the side-channel.
    decision[kSonicMasterExciterGateIdx] = 0.8f;   // gate high
    decision[kSonicMasterSatOffset + 0]  = 0.6f;   // band 0 drive
    decision[kSonicMasterSatOffset + 1]  = 0.4f;   // band 0 mix
    decision[kSonicMasterSatOffset + 2]  = 0.5f;   // band 1 drive
    decision[kSonicMasterSatOffset + 3]  = 0.3f;   // band 1 mix

    ValidatedNeuralMasteringPlan plan {};
    REQUIRE(decodeSonicMasterDecision(decision, kSonicMasterDecisionWidth, 48000.0, plan));

    CHECK(plan.applyHarmonic);
    // CRITICAL: appliedMask.harmonic MUST stay false, or the safety policy's
    // HighRiskMask hard-reject drops the entire plan.
    CHECK_FALSE(plan.appliedMask.harmonic);
    // harmonic[0] = gate amount, [1] = avg drive, [2] = avg mix.
    CHECK_THAT(plan.projectedTargets.harmonic[0], WithinAbs(0.8f, 1e-5f));
    CHECK_THAT(plan.projectedTargets.harmonic[1], WithinAbs(0.55f, 1e-5f));  // (0.6 + 0.5) / 2
    CHECK_THAT(plan.projectedTargets.harmonic[2], WithinAbs(0.35f, 1e-5f));  // (0.4 + 0.3) / 2

    // Sub-threshold gate must NOT enable the side-channel.
    decision[kSonicMasterExciterGateIdx] = 0.3f;
    REQUIRE(decodeSonicMasterDecision(decision, kSonicMasterDecisionWidth, 48000.0, plan));
    CHECK_FALSE(plan.applyHarmonic);
    CHECK_FALSE(plan.appliedMask.harmonic);
}

// ── F4: NaN in saturation slots is coerced, not propagated ────────────────────

TEST_CASE("Decoder coerces NaN saturation values without throwing (F4 hardening)",
          "[SonicMaster][Decoder][f4]")
{
    float decision[kSonicMasterDecisionWidth] {};
    decision[kSonicMasterExciterGateIdx] = 0.9f;
    decision[kSonicMasterSatOffset + 0]  = std::nanf("");
    decision[kSonicMasterSatOffset + 1]  = std::numeric_limits<float>::infinity();

    ValidatedNeuralMasteringPlan plan {};
    REQUIRE(decodeSonicMasterDecision(decision, kSonicMasterDecisionWidth, 48000.0, plan));
    // The decoder's finiteOr/clamp must coerce NaN/Inf to 0 before averaging.
    CHECK(plan.applyHarmonic);  // gate still high
    CHECK(std::isfinite(plan.projectedTargets.harmonic[1]));  // drive finite
    CHECK(std::isfinite(plan.projectedTargets.harmonic[2]));  // mix finite
}
