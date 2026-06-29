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

// AUDIT-FIX (A7): feed a fundamental + H2..H5 at specified dB amplitudes.
// Used to validate that thdPercent is computed (not the stale 0.0f stub).
void feedToneWithHarmonics(RealtimeSpectrumAnalyzer& analyzer,
                           float fundamentalHz,
                           const float harmonicDB[5],
                           int totalSamples)
{
    juce::AudioBuffer<float> buffer(1, kBlockSize);
    int generated = 0;

    while (generated < totalSamples)
    {
        const int samplesThisBlock = std::min(kBlockSize, totalSamples - generated);
        buffer.clear();

        for (int i = 0; i < samplesThisBlock; ++i)
        {
            const float t = static_cast<float>(generated + i);
            float sample = std::sin(2.0f * kPi * fundamentalHz * t / static_cast<float>(kSampleRate));
            for (int h = 2; h <= 5; ++h)
            {
                const float amp = std::pow(10.0f, harmonicDB[h - 2] / 20.0f);
                sample += amp * std::sin(2.0f * kPi * fundamentalHz * h * t / static_cast<float>(kSampleRate));
            }
            buffer.setSample(0, i, sample);
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

// AUDIT-FIX (A7): thdPercent is COMPUTED, not the stale 0.0f stub. A 1 kHz tone
// with H2=-20 dB, H3=-30 dB, H4=-40 dB, H5=-50 dB should report THD ≈ 10.5%
// (sqrt(10^-2 + 10^-3 + 10^-4 + 10^-5) * 100). Allow generous margin — the point
// is to prove the field is populated and in the right order of magnitude, not to
// validate a certified THD meter.
TEST_CASE("RealtimeSpectrumAnalyzer computes THD from harmonics", "[spectrum][thd]")
{
    RealtimeSpectrumAnalyzer analyzer;
    analyzer.prepare(kSampleRate, kBlockSize);

    const float harmonicDB[5] = { -20.0f, -30.0f, -40.0f, -50.0f, -60.0f };
    // ~4 s of audio so the spectrum stabilizes and several analysis frames fire.
    feedToneWithHarmonics(analyzer, 1000.0f, harmonicDB, static_cast<int>(kSampleRate) * 4);

    RealtimeSpectrumAnalyzer::SpectrumSnapshot snapshot;
    REQUIRE(analyzer.getSnapshot(snapshot));
    REQUIRE(snapshot.frameIndex > 0);

    CHECK(snapshot.thdPercent > 1.0f);     // computed, not 0.0
    CHECK(snapshot.thdPercent < 25.0f);    // sane upper bound
}

// AUDIT-FIX (A7): program-level crest converges toward the per-frame crest of a
// pure sine (√2 ≈ 1.414) after the ~1 s EMA window fills. Asserts the field is
// assigned and bounded — previously always 0.0f.
TEST_CASE("RealtimeSpectrumAnalyzer computes program crest factor", "[spectrum][crest]")
{
    RealtimeSpectrumAnalyzer analyzer;
    analyzer.prepare(kSampleRate, kBlockSize);

    feedSine(analyzer, 1000.0f, static_cast<int>(kSampleRate) * 2);

    RealtimeSpectrumAnalyzer::SpectrumSnapshot snapshot;
    REQUIRE(analyzer.getSnapshot(snapshot));
    REQUIRE(snapshot.frameIndex > 0);

    CHECK(snapshot.crestFactorProgram > 1.0f);
    CHECK(snapshot.crestFactorProgram < 2.0f);
}
