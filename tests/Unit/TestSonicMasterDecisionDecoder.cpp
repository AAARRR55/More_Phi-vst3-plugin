// tests/Unit/TestSonicMasterDecisionDecoder.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "AI/SonicMasterDecisionDecoder.h"
#include "Core/NeuralMasteringTypes.h"

#include <cmath>
#include <limits>

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
    // applyValidatedPlan maps dynamics[band] = value -> threshold = -20 + value*8,
    // ratio = 2.5 + value*1.5. So threshold=-20 -> value 0, ratio=2.5 -> value 0.
    CHECK_THAT(plan.projectedTargets.dynamics[0], Catch::Matchers::WithinAbs(0.0f, 1e-3f));
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
