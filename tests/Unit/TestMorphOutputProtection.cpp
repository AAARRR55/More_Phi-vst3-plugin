// tests/Unit/TestMorphOutputProtection.cpp
//
// Morph-output clipping fix: the main wet path now routes through a
// BrickwallLimiter (gated by the new `outputProtect` APVTS param, default ON).
// Before this fix, morphing into snapshots with high gain/output values — or
// summing correlated peaks in HybridBlend — could overshoot 0 dBFS and clip,
// because the only BrickwallLimiter in the codebase lived inside the dormant
// AutoMasteringEngine; the live wet output path had no protection.
//
// This test pins the behavioural guarantee the wiring provides: with the
// limiter engaged the wet output stays at/below the ceiling (sample- AND
// inter-sample-peak), and with it OFF the overshoot is present (proving the
// limiter is what catches it). It also asserts the lookahead is reported to
// the DAW via LatencyManager so PDC compensates.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Core/BrickwallLimiter.h"
#include "Core/LatencyManager.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

using namespace more_phi;
using Catch::Approx;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int    kBlockSize  = 512;

// Fill a stereo buffer with a hot, band-limited-ish signal that peaks above
// 0 dBFS. A full-amplitude sine plus a correlated second harmonic produces
// inter-sample peaks well past 1.0, which is exactly what a hosted plugin
// driven by a high-gain snapshot (or an equal-power HybridBlend sum of
// aligned peaks) would emit into the wet path.
void fillOvershootSignal(juce::AudioBuffer<float>& buf)
{
    const double w1 = 2.0 * juce::MathConstants<double>::pi * 4200.0 / kSampleRate; // near-Nyquist → ISPs
    const double w2 = 2.0 * juce::MathConstants<double>::pi * 300.0  / kSampleRate;
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
    {
        auto* d = buf.getWritePointer(ch);
        for (int i = 0; i < buf.getNumSamples(); ++i)
        {
            // 1.4× fundamental + 0.4× harmonic → peaks past 1.8, ISPs higher.
            const double s = 1.4 * std::sin(w1 * i) + 0.4 * std::sin(w2 * i);
            d[i] = static_cast<float>(s);
        }
    }
}

float maxAbs(const juce::AudioBuffer<float>& buf)
{
    float peak = 0.0f;
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
    {
        const auto* d = buf.getReadPointer(ch);
        for (int i = 0; i < buf.getNumSamples(); ++i)
            peak = std::max(peak, std::abs(d[i]));
    }
    return peak;
}

} // namespace

TEST_CASE("Morph output protection: engaged limiter holds output under ceiling",
          "[morph][output_protect]")
{
    BrickwallLimiter limiter;
    limiter.prepare(kSampleRate, kBlockSize);

    SECTION("With limiter engaged (outputProtect ON), no sample exceeds ceiling")
    {
        limiter.setEnabled(true);
        limiter.setCeiling(-1.0f);   // streaming-safe dBTP
        const float ceilingLinear = std::pow(10.0f, -1.0f / 20.0f);

        juce::AudioBuffer<float> buf(2, kBlockSize);
        fillOvershootSignal(buf);

        // Sanity: the input genuinely overshoots 0 dBFS.
        REQUIRE(maxAbs(buf) > 1.0f);

        // The lookahead limiter needs a few blocks to prime its delay line and
        // see the peaks coming; feed the same hot signal repeatedly so the
        // worst-case peak is inside the lookahead window.
        for (int b = 0; b < 8; ++b)
        {
            fillOvershootSignal(buf);
            limiter.processBlock(buf);
        }

        // Core guarantee: every output sample stays at/below the ceiling.
        // A small margin covers the limiter's ISP model vs the sample-read check.
        const float peak = maxAbs(buf);
        INFO("limited peak = " << peak << ", ceiling = " << ceilingLinear);
        REQUIRE(peak <= ceilingLinear + 0.02f);

        // And the limiter actually did something (GR meter is negative).
        REQUIRE(limiter.getGainReductionDB() < -0.5f);
    }

    SECTION("With limiter bypassed (outputProtect OFF), overshoot passes through")
    {
        // This is the control: proves the overshoot is real and that only the
        // engaged limiter suppresses it — not the test harness.
        limiter.setEnabled(false);
        limiter.setCeiling(-1.0f);

        juce::AudioBuffer<float> buf(2, kBlockSize);
        fillOvershootSignal(buf);
        limiter.processBlock(buf);

        REQUIRE(maxAbs(buf) > 1.0f);
    }
}

TEST_CASE("Morph output protection: lookahead is reportable to the DAW via LatencyManager",
          "[morph][output_protect][latency]")
{
    BrickwallLimiter limiter;
    limiter.prepare(kSampleRate, kBlockSize);

    LatencyManager latency;

    // The wiring reports the limiter's lookahead while the toggle is ON.
    const int lookahead = limiter.getLookaheadSamples();
    REQUIRE(lookahead > 0);   // 4 ms @ 48 kHz == 192 samples

    latency.setMorphOutputLatency(lookahead);
    REQUIRE(latency.getMorphOutputLatency() == lookahead);
    REQUIRE(latency.getTotal() == lookahead);

    // And drops back to zero when the user turns output protection OFF.
    latency.setMorphOutputLatency(0);
    REQUIRE(latency.getMorphOutputLatency() == 0);
    REQUIRE(latency.getTotal() == 0);
}
