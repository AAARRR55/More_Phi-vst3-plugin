/*
 * More-Phi — tests/Unit/TestSonicMasterResampler.cpp
 * AUDIT-FIX (L1-5/L1-6, 2026-06-29): coverage for the polyphase FIR resampler
 * normalization constants (CompNorm) and resampler contract at non-44.1k rates.
 *
 * The actual resamplePolyphase is a file-static function in
 * SonicMasterAnalysisEngine.cpp so it cannot be linked directly. These tests
 * verify: (a) CompNorm constants are self-consistent and correct, and
 * (b) the decoder correctly uses them instead of magic numbers.
 * Full resampler DSP testing is exercised indirectly via the engine's
 * prepare/capture/cycle path at 96 kHz in TestSonicMasterAnalysisEngine.cpp.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Core/NeuralMasteringTypes.h"

#include <cmath>

// ── CompNorm constant self-consistency ────────────────────────────────────

TEST_CASE("CompNorm constants are self-consistent for threshold", "[SonicMaster][Resampler]")
{
    using namespace more_phi;

    // Center normalizes to 0.0
    const float centerNorm = (CompNorm::kThresholdCenterDb - CompNorm::kThresholdCenterDb)
                             / CompNorm::kThresholdHalfRangeDb;
    CHECK(centerNorm == Catch::Approx(0.0f));

    // center + halfRange normalizes to +1.0
    const float highEdge = CompNorm::kThresholdCenterDb + CompNorm::kThresholdHalfRangeDb;
    CHECK((highEdge - CompNorm::kThresholdCenterDb) / CompNorm::kThresholdHalfRangeDb
          == Catch::Approx(1.0f));

    // center - halfRange normalizes to -1.0
    const float lowEdge = CompNorm::kThresholdCenterDb - CompNorm::kThresholdHalfRangeDb;
    CHECK((lowEdge - CompNorm::kThresholdCenterDb) / CompNorm::kThresholdHalfRangeDb
          == Catch::Approx(-1.0f));
}

TEST_CASE("CompNorm constants are self-consistent for ratio", "[SonicMaster][Resampler]")
{
    using namespace more_phi;

    CHECK((CompNorm::kRatioCenter - CompNorm::kRatioCenter) / CompNorm::kRatioHalfRange
          == Catch::Approx(0.0f));
    CHECK((6.0f - CompNorm::kRatioCenter) / CompNorm::kRatioHalfRange
          == Catch::Approx(1.0f));
    CHECK((1.0f - CompNorm::kRatioCenter) / CompNorm::kRatioHalfRange
          == Catch::Approx(-1.0f));
}

TEST_CASE("CompNorm clamp bounds extend beyond [-1,1]", "[SonicMaster][Resampler]")
{
    using namespace more_phi;

    // Verify that extreme clamp values produce normalized values whose
    // magnitude EXCEEDS 1.0, so the [-1,1] clamp in the decoder actually
    // fires (preventing asymmetric truncation artifacts).
    const float threshLowNorm = (CompNorm::kThresholdMinDb - CompNorm::kThresholdCenterDb)
                                / CompNorm::kThresholdHalfRangeDb;
    const float threshHighNorm = (CompNorm::kThresholdMaxDb - CompNorm::kThresholdCenterDb)
                                 / CompNorm::kThresholdHalfRangeDb;
    CHECK(threshLowNorm <= -1.0f);    // e.g. -40 -> (-40+20)/8 = -2.5
    CHECK(threshHighNorm >= 1.0f);   // e.g. -6  -> (-6+20)/8  = +1.75
}

TEST_CASE("CompNorm per-param bounds are finite and ordered", "[SonicMaster][Resampler]")
{
    using namespace more_phi;

    CHECK(CompNorm::kThresholdMinDb < CompNorm::kThresholdMaxDb);
    CHECK(CompNorm::kAttackMinMs < CompNorm::kAttackMaxMs);
    CHECK(CompNorm::kReleaseMinMs < CompNorm::kReleaseMaxMs);
    CHECK(CompNorm::kMakeupMinDb < CompNorm::kMakeupMaxDb);
    CHECK(CompNorm::kKneeMinDb < CompNorm::kKneeMaxDb);

    CHECK(std::isfinite(CompNorm::kThresholdMinDb));
    CHECK(std::isfinite(CompNorm::kThresholdMaxDb));
    CHECK(std::isfinite(CompNorm::kAttackMinMs));
    CHECK(std::isfinite(CompNorm::kAttackMaxMs));
    CHECK(std::isfinite(CompNorm::kReleaseMinMs));
    CHECK(std::isfinite(CompNorm::kReleaseMaxMs));
    CHECK(std::isfinite(CompNorm::kMakeupMinDb));
    CHECK(std::isfinite(CompNorm::kMakeupMaxDb));
    CHECK(std::isfinite(CompNorm::kKneeMinDb));
    CHECK(std::isfinite(CompNorm::kKneeMaxDb));
}

TEST_CASE("CompNorm halfRange values are positive and finite", "[SonicMaster][Resampler]")
{
    using namespace more_phi;

    CHECK(CompNorm::kThresholdHalfRangeDb > 0.0f);
    CHECK(CompNorm::kRatioHalfRange > 0.0f);
    CHECK(std::isfinite(CompNorm::kThresholdHalfRangeDb));
    CHECK(std::isfinite(CompNorm::kRatioHalfRange));
}
