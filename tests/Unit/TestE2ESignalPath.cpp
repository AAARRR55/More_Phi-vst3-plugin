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
