/*
 * More-Phi — tests/Unit/TestE2ESignalPath.cpp
 *
 * End-to-end signal path runtime tests:
 *   - SnapshotBank capture → MorphProcessor interpolation → output changes
 *   - Fader sweep produces monotonic transition between snapshots
 *   - Empty bank leaves output unchanged
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Core/SnapshotBank.h"
#include "Core/MorphProcessor.h"
#include "Core/InterpolationEngine.h"

#include <vector>
#include <cmath>

using Catch::Approx;
using namespace more_phi;

TEST_CASE("End-to-end signal path: morph output transitions between captured snapshots", "[e2e][signal]")
{
    constexpr int paramCount = 8;

    SnapshotBank bank;
    bank.prepare(paramCount);

    std::vector<float> snapshotA(paramCount);
    std::vector<float> snapshotB(paramCount);
    for (int p = 0; p < paramCount; ++p)
    {
        snapshotA[p] = static_cast<float>(p) * 0.1f;
        snapshotB[p] = 1.0f - snapshotA[p];
    }

    bank.captureValues(0, snapshotA);
    bank.captureValues(6, snapshotB);

    MorphProcessor processor(bank);
    processor.prepare(paramCount);
    processor.setSmoothingRate(0.0f);  // instant convergence

    std::vector<float> out(paramCount, 0.0f);

    // At fader 0.0 we should be close to snapshot A
    processor.process(0.0f, 0.0f, 0.0f, MorphSource::Fader, MorphMode::Direct, 1.0f / 60.0f, out);
    for (int p = 0; p < paramCount; ++p)
        REQUIRE(out[p] == Approx(snapshotA[p]).margin(0.001f));

    // At fader 1.0 we should be close to snapshot B
    processor.process(0.0f, 0.0f, 1.0f, MorphSource::Fader, MorphMode::Direct, 1.0f / 60.0f, out);
    for (int p = 0; p < paramCount; ++p)
        REQUIRE(out[p] == Approx(snapshotB[p]).margin(0.001f));
}

TEST_CASE("End-to-end signal path: fader sweep is monotonic between two snapshots", "[e2e][signal]")
{
    constexpr int paramCount = 4;

    SnapshotBank bank;
    bank.prepare(paramCount);

    std::vector<float> snapshotA = {0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<float> snapshotB = {1.0f, 1.0f, 1.0f, 1.0f};

    bank.captureValues(0, snapshotA);
    bank.captureValues(1, snapshotB);

    MorphProcessor processor(bank);
    processor.prepare(paramCount);
    processor.setSmoothingRate(0.0f);

    std::vector<float> prev(paramCount, -1.0f);

    for (int step = 0; step <= 20; ++step)
    {
        float fader = static_cast<float>(step) / 20.0f;
        std::vector<float> out(paramCount, 0.0f);
        processor.process(0.0f, 0.0f, fader, MorphSource::Fader, MorphMode::Direct, 1.0f / 60.0f, out);

        for (int p = 0; p < paramCount; ++p)
        {
            INFO("param " << p << " at fader=" << fader);
            REQUIRE(out[p] >= prev[p] - 1e-4f);
            REQUIRE(out[p] >= 0.0f);
            REQUIRE(out[p] <= 1.0f);
        }
        prev = out;
    }
}

// AUDIT-FIX (T2): dense finite/bounded sweep across the full morph field.
// Endpoint tests (above) check t=0/t=1; this sweeps every fader step at
// multiple XY cursor positions and asserts no NaN/Inf ever appears and every
// output stays in [0,1]. Catches interpolation-path regressions (divide-by-zero
// at coincident points, subnormal accumulation, out-of-range blend weights)
// that endpoint-only tests miss. A full MorePhiProcessor::processBlock audio
// assertion would additionally cover the gain-staging/capture-tap orchestration
// but needs a hosted-plugin harness; this is the completable morph-path slice.
TEST_CASE("End-to-end signal path: dense sweep stays finite and in-range across the morph field", "[e2e][signal][denormals]")
{
    constexpr int paramCount = 16;
    constexpr float kRefDt = 512.0f / 44100.0f;

    SnapshotBank bank;
    bank.prepare(paramCount);

    // Two distinct snapshots so interpolation produces non-trivial output.
    std::vector<float> snapshotA(paramCount), snapshotB(paramCount);
    for (int p = 0; p < paramCount; ++p)
    {
        snapshotA[p] = static_cast<float>(p) / static_cast<float>(paramCount - 1);
        snapshotB[p] = 1.0f - snapshotA[p];
    }
    bank.captureValues(0, snapshotA);
    bank.captureValues(1, snapshotB);

    MorphProcessor processor(bank);
    processor.prepare(paramCount);
    processor.setSmoothingRate(0.0f);  // instant — isolate the interpolation math

    std::vector<float> out(paramCount, 0.0f);

    // Sweep fader [0,1] x a few cursor positions. The interpolation engine must
    // remain finite and in [0,1] everywhere — no NaN from a divide-by-zero at a
    // coincident cursor, no Inf from a runaway weight, no clamp failure.
    for (int fStep = 0; fStep <= 50; ++fStep)
    {
        const float fader = static_cast<float>(fStep) / 50.0f;
        for (int xyStep = 0; xyStep <= 10; ++xyStep)
        {
            const float pos = static_cast<float>(xyStep) / 10.0f;
            processor.process(pos, pos, fader, MorphSource::Fader, MorphMode::Direct, kRefDt, out);

            for (int p = 0; p < paramCount; ++p)
            {
                INFO("param " << p << " fader=" << fader << " pos=" << pos);
                REQUIRE_FALSE(std::isnan(out[p]));
                REQUIRE_FALSE(std::isinf(out[p]));
                REQUIRE(out[p] >= 0.0f);
                REQUIRE(out[p] <= 1.0f);
            }
        }
    }
}
