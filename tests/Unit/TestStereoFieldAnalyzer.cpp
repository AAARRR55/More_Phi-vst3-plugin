#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Core/StereoFieldAnalyzer.h"

#include <algorithm>
#include <cmath>

using namespace more_phi;
using Catch::Approx;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 512;
constexpr float kPi = 3.14159265358979323846f;

void feedMidSideSine(StereoFieldAnalyzer& analyzer, float frequency, float sideGain, int totalSamples)
{
    juce::AudioBuffer<float> buffer(2, kBlockSize);
    int generated = 0;

    while (generated < totalSamples)
    {
        const int samplesThisBlock = std::min(kBlockSize, totalSamples - generated);
        buffer.clear();

        for (int i = 0; i < samplesThisBlock; ++i)
        {
            const float phase = 2.0f * kPi * frequency * static_cast<float>(generated + i)
                / static_cast<float>(kSampleRate);
            const float mid = std::sin(phase);
            const float side = sideGain * mid;
            buffer.setSample(0, i, mid + side);
            buffer.setSample(1, i, mid - side);
        }

        analyzer.processBlock(buffer);
        generated += samplesThisBlock;
    }
}

} // namespace

TEST_CASE("StereoFieldAnalyzer publishes an empty snapshot after reset", "[stereo-field][reset]")
{
    StereoFieldAnalyzer analyzer;
    analyzer.prepare(kSampleRate, kBlockSize);

    StereoFieldAnalyzer::StereoFieldSnapshot snapshot;
    REQUIRE(analyzer.getSnapshot(snapshot));

    CHECK(snapshot.sampleRate == Approx(kSampleRate));
    CHECK(snapshot.windowSamples == 4800);
    CHECK(snapshot.frameIndex == 0);
    CHECK(snapshot.stereoWidth == Approx(0.0f));

    for (int band = 0; band < StereoFieldAnalyzer::kNumBands; ++band)
    {
        CHECK(snapshot.correlation[static_cast<size_t>(band)] == Approx(0.0f));
        CHECK(snapshot.msEnergyRatio[static_cast<size_t>(band)] == Approx(0.0f));
    }
}

TEST_CASE("StereoFieldAnalyzer reports positive left-right correlation", "[stereo-field][correlation]")
{
    StereoFieldAnalyzer analyzer;
    analyzer.prepare(kSampleRate, kBlockSize);

    feedMidSideSine(analyzer, 1000.0f, 0.25f, 24000);

    StereoFieldAnalyzer::StereoFieldSnapshot snapshot;
    REQUIRE(analyzer.getSnapshot(snapshot));

    CHECK(snapshot.frameIndex > 0);
    CHECK(snapshot.correlation[2] > 0.85f);
    CHECK(snapshot.msEnergyRatio[2] == Approx(0.0625f).margin(0.035f));
    CHECK(snapshot.stereoWidth == Approx(0.25f).margin(0.08f));
}

TEST_CASE("StereoFieldAnalyzer reports mono content as zero width", "[stereo-field][mono]")
{
    StereoFieldAnalyzer analyzer;
    analyzer.prepare(kSampleRate, kBlockSize);

    feedMidSideSine(analyzer, 1000.0f, 0.0f, 24000);

    StereoFieldAnalyzer::StereoFieldSnapshot snapshot;
    REQUIRE(analyzer.getSnapshot(snapshot));

    CHECK(snapshot.frameIndex > 0);
    CHECK(snapshot.msEnergyRatio[2] < 0.001f);
    CHECK(snapshot.stereoWidth < 0.001f);
}

TEST_CASE("StereoFieldAnalyzer preserves side polarity in correlation", "[stereo-field][polarity]")
{
    StereoFieldAnalyzer analyzer;
    analyzer.prepare(kSampleRate, kBlockSize);

    feedMidSideSine(analyzer, 1000.0f, -2.0f, 24000);

    StereoFieldAnalyzer::StereoFieldSnapshot snapshot;
    REQUIRE(analyzer.getSnapshot(snapshot));

    CHECK(snapshot.frameIndex > 0);
    CHECK(snapshot.correlation[2] < -0.85f);
    CHECK(snapshot.msEnergyRatio[2] == Approx(4.0f).margin(1.5f));
}

TEST_CASE("StereoFieldAnalyzer exposes side-heavy content through M/S ratio", "[stereo-field][analysis]")
{
    StereoFieldAnalyzer analyzer;
    analyzer.prepare(kSampleRate, kBlockSize);

    feedMidSideSine(analyzer, 1000.0f, 2.0f, 24000);

    StereoFieldAnalyzer::StereoFieldSnapshot snapshot;
    REQUIRE(analyzer.getSnapshot(snapshot));

    CHECK(snapshot.frameIndex > 0);
    CHECK(snapshot.msEnergyRatio[2] == Approx(4.0f).margin(1.5f));
    CHECK(snapshot.stereoWidth == Approx(2.0f).margin(0.45f));
}
