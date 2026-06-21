#include <catch2/catch_test_macros.hpp>
#include "AI/SonicMasterHttpInferenceSource.h"
#include "AI/SonicMasterDecisionDecoder.h"
#include <cmath>
#include <vector>

TEST_CASE("SonicMasterHttpInferenceSource talks to a running server", "[SonicMaster][Http][Live]")
{
    more_phi::SonicMasterHttpInferenceSource src;
    if (!src.isAvailable())
    {
        WARN("inference server not reachable on 127.0.0.1:8765 — skipping live HTTP test");
        return;
    }

    // 6s bass-heavy-ish mono duplicated to stereo.
    constexpr std::size_t n = more_phi::kSonicMasterSegmentFrames;
    std::vector<float> l(n), r(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i) / 44100.0;
        const float v = 0.25f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 55.0 * t));
        l[i] = v; r[i] = v;
    }
    std::vector<float> interleaved(2 * n);
    more_phi::buildInferRequestBody(l.data(), r.data(), n, interleaved.data());

    float decision[more_phi::kSonicMasterDecisionWidth] = {};
    REQUIRE(src.infer(interleaved.data(), decision, more_phi::kSonicMasterDecisionWidth));

    // Decode + assert sane mastering values.
    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));
    CHECK(plan.valid);
    // The model targets streaming loudness (~-14 LUFS) and a sub-0 dBTP ceiling.
    // Loudness/limiter are decoded as offsets from -14 / -1 (see decoder), so the
    // decoded loudness[0] should land near 0 (target ≈ -14) and limiter[0] near 0.
    CHECK(std::abs(plan.projectedTargets.loudness[0]) < 2.0f);
    CHECK(std::abs(plan.projectedTargets.limiter[0])  < 2.0f);
}
