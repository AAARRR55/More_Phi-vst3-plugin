#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Core/MeterWindowAccumulator.h"

using Catch::Approx;

TEST_CASE("MeterWindowAccumulator reports empty windows explicitly", "[MeterWindow][analysis]")
{
    more_phi::MeterWindowAccumulator accumulator;

    const auto stats = accumulator.computeWindow(3.0f);

    REQUIRE_FALSE(stats.success);
    REQUIRE(stats.sampleCount == 0);
    REQUIRE(stats.requestedSeconds == Approx(3.0f));
}

TEST_CASE("MeterWindowAccumulator computes rolling percentile statistics", "[MeterWindow][analysis]")
{
    more_phi::MeterWindowAccumulator accumulator;

    for (int i = 0; i < 5; ++i)
    {
        more_phi::MeterWindowAccumulator::MeterSample sample;
        sample.timestampSeconds = static_cast<double>(i) * 0.1;
        sample.rms = static_cast<float>(i + 1);
        sample.lufsMomentary = -20.0f + static_cast<float>(i);
        sample.lra = static_cast<float>(i) * 0.5f;
        sample.truePeakDBTP = -10.0f + static_cast<float>(i);
        sample.spectralCentroidHz = 1000.0f + 100.0f * static_cast<float>(i);
        sample.spectralTiltDBPerOctave = -2.0f + 0.5f * static_cast<float>(i);
        sample.stereoWidth = 0.2f * static_cast<float>(i);
        sample.midBandCorrelation = -0.5f + 0.25f * static_cast<float>(i);
        accumulator.pushSample(sample);
    }

    const auto fullWindow = accumulator.computeWindow(1.0f);
    REQUIRE(fullWindow.success);
    REQUIRE(fullWindow.sampleCount == 5);
    REQUIRE(fullWindow.actualSeconds == Approx(0.4f));

    const auto& rms = fullWindow.metrics[more_phi::MeterWindowAccumulator::rms];
    CHECK(rms.min == Approx(1.0f));
    CHECK(rms.max == Approx(5.0f));
    CHECK(rms.mean == Approx(3.0f));
    CHECK(rms.p10 == Approx(1.4f));
    CHECK(rms.p50 == Approx(3.0f));
    CHECK(rms.p90 == Approx(4.6f));

    const auto recentWindow = accumulator.computeWindow(0.15f);
    REQUIRE(recentWindow.success);
    REQUIRE(recentWindow.sampleCount == 2);
    CHECK(recentWindow.metrics[more_phi::MeterWindowAccumulator::rms].mean == Approx(4.5f));
}
