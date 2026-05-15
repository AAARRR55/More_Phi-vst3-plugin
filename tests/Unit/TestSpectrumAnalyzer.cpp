#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Core/RealtimeSpectrumAnalyzer.h"

#include <algorithm>
#include <cmath>

using namespace more_phi;
using Catch::Approx;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 512;
constexpr float kPi = 3.14159265358979323846f;

void feedSine(RealtimeSpectrumAnalyzer& analyzer, float frequency, int totalSamples)
{
    juce::AudioBuffer<float> buffer(1, kBlockSize);
    int generated = 0;

    while (generated < totalSamples)
    {
        const int samplesThisBlock = std::min(kBlockSize, totalSamples - generated);
        buffer.clear();

        for (int i = 0; i < samplesThisBlock; ++i)
        {
            const float phase = 2.0f * kPi * frequency * static_cast<float>(generated + i)
                / static_cast<float>(kSampleRate);
            buffer.setSample(0, i, std::sin(phase));
        }

        analyzer.processBlock(buffer);
        generated += samplesThisBlock;
    }
}

void feedConstantStereo(RealtimeSpectrumAnalyzer& analyzer, float left, float right, int totalSamples)
{
    juce::AudioBuffer<float> buffer(2, kBlockSize);
    int generated = 0;

    while (generated < totalSamples)
    {
        const int samplesThisBlock = std::min(kBlockSize, totalSamples - generated);
        buffer.clear();

        for (int i = 0; i < samplesThisBlock; ++i)
        {
            buffer.setSample(0, i, left);
            buffer.setSample(1, i, right);
        }

        analyzer.processBlock(buffer);
        generated += samplesThisBlock;
    }
}

} // namespace

TEST_CASE("RealtimeSpectrumAnalyzer publishes an empty snapshot after reset", "[spectrum][reset]")
{
    RealtimeSpectrumAnalyzer analyzer;
    analyzer.prepare(kSampleRate, kBlockSize);

    RealtimeSpectrumAnalyzer::SpectrumSnapshot snapshot;
    REQUIRE(analyzer.getSnapshot(snapshot));

    CHECK(snapshot.binCount == RealtimeSpectrumAnalyzer::kMaxBins);
    CHECK(snapshot.sampleRate == Approx(kSampleRate));
    CHECK(snapshot.fftSize == 2048);
    CHECK(snapshot.frameIndex == 0);
    CHECK(snapshot.spectralCentroid == Approx(0.0f));
    CHECK(snapshot.spectralRolloff == Approx(0.0f));
    CHECK(snapshot.crestFactor == Approx(0.0f));

    for (float magnitude : snapshot.magnitudeDB)
        CHECK(magnitude == Approx(-120.0f));
}

TEST_CASE("RealtimeSpectrumAnalyzer estimates a sine wave centroid", "[spectrum][centroid]")
{
    RealtimeSpectrumAnalyzer analyzer;
    analyzer.prepare(kSampleRate, kBlockSize);

    feedSine(analyzer, 1000.0f, 4096);

    RealtimeSpectrumAnalyzer::SpectrumSnapshot snapshot;
    REQUIRE(analyzer.getSnapshot(snapshot));

    CHECK(snapshot.frameIndex > 0);
    CHECK(snapshot.spectralCentroid == Approx(1000.0f).margin(80.0f));
    CHECK(snapshot.spectralRolloff == Approx(1000.0f).margin(200.0f));
    CHECK(snapshot.crestFactor > 1.0f);
    CHECK(snapshot.crestFactor < 3.0f);
}

TEST_CASE("RealtimeSpectrumAnalyzer downmixes stereo before analysis", "[spectrum][stereo]")
{
    RealtimeSpectrumAnalyzer analyzer;
    analyzer.prepare(kSampleRate, kBlockSize);

    feedConstantStereo(analyzer, 1.0f, -1.0f, 4096);

    RealtimeSpectrumAnalyzer::SpectrumSnapshot snapshot;
    REQUIRE(analyzer.getSnapshot(snapshot));

    CHECK(snapshot.frameIndex > 0);
    CHECK(snapshot.spectralCentroid == Approx(0.0f));
    CHECK(snapshot.crestFactor == Approx(0.0f));

    for (float magnitude : snapshot.magnitudeDB)
        CHECK(magnitude == Approx(-120.0f));
}
