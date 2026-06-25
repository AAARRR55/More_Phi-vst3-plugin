/*
 * More-Phi — tests/Unit/TestMasteringMeters.cpp
 *
 * Order-of-magnitude + known-signal validation for the SonicMaster analysis
 * meters that TestDSPQuality.cpp does NOT cover (it only tests morph/oversampling
 * DSP). The strong LUFS conformance lives in TestLUFSMeter.cpp and the spectrum
 * feature basics in TestSpectrumAnalyzer.cpp; this file covers the AUDIT gaps:
 *
 *   - DC offset is NOT removed in the spectrum path (a known limitation). This
 *     test pins the current behavior so a future DC-removal fix flips it green.
 *   - Crest factor is bounded for a pure sine (≈1.414) — a sanity guard.
 *   - Spectral centroid tracks frequency monotonically (low tone < high tone).
 *
 * Missing-metric inventory (noise floor, SNR, muddiness, harshness, transient):
 * these are intentionally absent today; the audit flags them. If any is added,
 * add a test here. These tests document the gap rather than assert absence.
 */
#include "Core/RealtimeSpectrumAnalyzer.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>

using namespace more_phi;
using Catch::Approx;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int    kBlockSize  = 512;
constexpr float  kPi         = 3.14159265358979323846f;

void feedSineStereo(RealtimeSpectrumAnalyzer& analyzer, float frequency,
                    int totalSamples, float amplitude = 1.0f)
{
    juce::AudioBuffer<float> buffer(2, kBlockSize);
    int generated = 0;
    while (generated < totalSamples)
    {
        const int n = std::min(kBlockSize, totalSamples - generated);
        buffer.clear();
        for (int i = 0; i < n; ++i)
        {
            const float phase = 2.0f * kPi * frequency * static_cast<float>(generated + i)
                              / static_cast<float>(kSampleRate);
            const float s = amplitude * std::sin(phase);
            buffer.setSample(0, i, s);
            buffer.setSample(1, i, s);
        }
        analyzer.processBlock(buffer);
        generated += n;
    }
}

} // anonymous namespace

TEST_CASE("Crest factor of a pure sine is ≈sqrt(2) (sanity)", "[meters][crest]")
{
    RealtimeSpectrumAnalyzer analyzer;
    analyzer.prepare(kSampleRate, kBlockSize);

    // Feed ~0.5s of a 1 kHz full-scale sine.
    feedSineStereo(analyzer, 1000.0f, static_cast<int>(kSampleRate * 0.5));

    RealtimeSpectrumAnalyzer::SpectrumSnapshot snap;
    REQUIRE(analyzer.getSnapshot(snap));
    REQUIRE(snap.frameIndex > 0);

    // Program crest of a sine is peak/RMS = 1.0 / (1/sqrt2) ≈ 1.414. Allow a
    // generous band (windowing + EMA smoothing).
    CHECK(snap.crestFactorProgram == Approx(1.414f).margin(0.25f));
}

TEST_CASE("Spectral centroid is higher for a high tone than a low tone",
          "[meters][centroid]")
{
    RealtimeSpectrumAnalyzer low;
    low.prepare(kSampleRate, kBlockSize);
    feedSineStereo(low, 120.0f, static_cast<int>(kSampleRate * 0.5));
    RealtimeSpectrumAnalyzer::SpectrumSnapshot lowSnap;
    REQUIRE(low.getSnapshot(lowSnap));

    RealtimeSpectrumAnalyzer high;
    high.prepare(kSampleRate, kBlockSize);
    feedSineStereo(high, 6000.0f, static_cast<int>(kSampleRate * 0.5));
    RealtimeSpectrumAnalyzer::SpectrumSnapshot highSnap;
    REQUIRE(high.getSnapshot(highSnap));

    REQUIRE(lowSnap.frameIndex > 0);
    REQUIRE(highSnap.frameIndex > 0);
    CHECK(highSnap.spectralCentroid > lowSnap.spectralCentroid);
    // Monotonic sanity: the low tone's centroid should be near its bin (~120 Hz),
    // the high tone's near ~6 kHz. Loose margins because centroid is energy-weighted
    // across the whole spectrum (Hann leakage, harmonic content is zero for a pure
    // sine but quantization noise spreads).
    CHECK(lowSnap.spectralCentroid  < 1000.0f);
    CHECK(highSnap.spectralCentroid > 2000.0f);
}

// AUDIT gap: the spectrum path does NOT remove DC offset. A pure-DC signal
// (constant 0.5) should ideally be ignored or attributed only to bin 0; today it
// inflates low-frequency energy and the crest/centroid become meaningless. This
// test pins the CURRENT behavior so a future DC-removal fix is detectable. It
// does not assert correctness — it documents the limitation.
TEST_CASE("DC offset is NOT removed in the spectrum path (known limitation)",
          "[meters][dc][audit-gap]")
{
    RealtimeSpectrumAnalyzer analyzer;
    analyzer.prepare(kSampleRate, kBlockSize);

    // Feed a constant DC offset of 0.5 for 0.5s (no AC content at all).
    juce::AudioBuffer<float> buffer(2, kBlockSize);
    int generated = 0;
    const int total = static_cast<int>(kSampleRate * 0.5);
    while (generated < total)
    {
        const int n = std::min(kBlockSize, total - generated);
        buffer.clear();
        for (int i = 0; i < n; ++i)
        {
            buffer.setSample(0, i, 0.5f);
            buffer.setSample(1, i, 0.5f);
        }
        analyzer.processBlock(buffer);
        generated += n;
    }

    RealtimeSpectrumAnalyzer::SpectrumSnapshot snap;
    REQUIRE(analyzer.getSnapshot(snap));
    REQUIRE(snap.frameIndex > 0);

    // A DC-only signal has peak == RMS, so crest ≈ 1.0. Today this passes because
    // there is no DC removal — the DC shows up as energy. When DC removal is added,
    // this assertion should be revisited (the snapshot may report a near-zero /
    // invalid crest instead). INFO documents the expectation.
    INFO("DC-only crest factor: " << snap.crestFactorProgram
         << " (expected ≈1.0 today; revisit after DC-removal fix)");
    CHECK(snap.crestFactorProgram == Approx(1.0f).margin(0.15f));
}

// Missing-metric inventory. These metrics are flagged as absent in the audit.
// Each SECTION documents the expected behavior so a future implementation has a
// ready test target. They are deliberately non-asserting today.
TEST_CASE("Missing mastering metrics (audit inventory — not yet implemented)",
          "[meters][audit-gap][inventory]")
{
    SECTION("noise floor")
    {
        // Not computed. When added: feed a near-silent signal (-90 dBFS) and
        // assert the noise-floor estimate is within a few dB of -90.
        SUCCEED("noise floor metric not implemented (audit gap)");
    }
    SECTION("muddiness (low-energy ratio)")
    {
        // Not computed. When added: feed a signal boosted in the 200-500 Hz
        // region and assert the muddiness score rises vs a flat signal.
        SUCCEED("muddiness metric not implemented (audit gap)");
    }
    SECTION("harshness (2-5 kHz energy)")
    {
        // Not computed. When added: feed a signal with excess 2-5 kHz content
        // and assert the harshness score rises.
        SUCCEED("harshness metric not implemented (audit gap)");
    }
    SECTION("transient / attack")
    {
        // Not computed (TransientDetector exists but only drives morph alpha,
        // not the mastering decision). When added: feed a percussive signal and
        // assert the transient metric is non-zero.
        SUCCEED("transient/attack metric not implemented (audit gap)");
    }
}
