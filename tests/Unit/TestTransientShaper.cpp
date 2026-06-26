// tests/Unit/TestTransientShaper.cpp
//
// Phase 3 (Ozone "Impact"): the transient shaper emphasizes or reduces
// transients relative to sustain via a fast/slow envelope ratio. These tests
// pin the four invariants: disabled == unity, steady signal == near-unity,
// transient emphasis makes attacks louder vs sustain, and the output is bounded.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Core/TransientShaper.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr double kSr = 48000.0;

// Build a stereo buffer of N samples.
juce::AudioBuffer<float> makeBuffer(int samples)
{
    juce::AudioBuffer<float> buf(2, samples);
    buf.clear();
    return buf;
}

// RMS over a sample range (channel 0).
float rms(const juce::AudioBuffer<float>& buf, int start, int length)
{
    const float* d = buf.getReadPointer(0);
    double sum = 0.0;
    for (int i = 0; i < length; ++i) sum += static_cast<double>(d[start + i]) * d[start + i];
    return static_cast<float>(std::sqrt(sum / length));
}

} // namespace

TEST_CASE("TransientShaper disabled is unity (no change)",
          "[TransientShaper]")
{
    more_phi::TransientShaper ts;
    ts.prepare(kSr, 512);
    ts.setEnabled(false);      // disabled
    ts.setAmount(1.0f);        // would shape if enabled

    auto buf = makeBuffer(2048);
    for (int i = 0; i < buf.getNumSamples(); ++i)
        buf.setSample(0, i, 0.5f * static_cast<float>(std::sin(2.0 * 3.14159 * 220.0 * i / kSr)));

    std::vector<float> before(buf.getNumSamples());
    for (int i = 0; i < buf.getNumSamples(); ++i) before[i] = buf.getSample(0, i);

    ts.processBlock(buf);

    for (int i = 0; i < buf.getNumSamples(); ++i)
        CHECK(buf.getSample(0, i) == Catch::Approx(before[i]).margin(1e-6f));
}

TEST_CASE("TransientShaper unity when amount is zero",
          "[TransientShaper]")
{
    more_phi::TransientShaper ts;
    ts.prepare(kSr, 512);
    ts.setEnabled(true);
    ts.setAmount(0.0f);        // zero amount → no shaping

    auto buf = makeBuffer(2048);
    for (int i = 0; i < buf.getNumSamples(); ++i)
        buf.setSample(0, i, 0.3f);
    const float beforeRms = rms(buf, 0, buf.getNumSamples());

    ts.processBlock(buf);
    const float afterRms = rms(buf, 0, buf.getNumSamples());

    // Zero amount + unity output gain → signal unchanged in RMS.
    CHECK(afterRms == Catch::Approx(beforeRms).margin(1e-3f));
}

TEST_CASE("TransientShaper positive amount lifts transients above sustain",
          "[TransientShaper]")
{
    // A signal with a sharp attack followed by a long sustain: the attack
    // portion's energy should be relatively louder after emphasis. We compare
    // the ratio (attackRMS / sustainRMS) before and after — emphasis raises it.
    more_phi::TransientShaper ts;
    ts.prepare(kSr, 512);
    ts.setEnabled(true);
    ts.setAmount(0.9f);

    auto buf = makeBuffer(8000);
    // Sustain body: steady low-level tone.
    for (int i = 2000; i < 8000; ++i)
        buf.setSample(0, i, 0.1f * static_cast<float>(std::sin(2.0 * 3.14159 * 220.0 * i / kSr)));
    // Sharp attack burst in the first 2000 samples: high amplitude.
    for (int i = 0; i < 2000; ++i)
        buf.setSample(0, i, 0.7f * static_cast<float>(std::sin(2.0 * 3.14159 * 880.0 * i / kSr)));

    const float attackBefore = rms(buf, 0, 2000);
    const float sustainBefore = rms(buf, 6000, 2000);
    const float ratioBefore = attackBefore / sustainBefore;

    ts.processBlock(buf);

    const float attackAfter = rms(buf, 0, 2000);
    const float sustainAfter = rms(buf, 6000, 2000);
    const float ratioAfter = attackAfter / sustainAfter;

    // Emphasis (amount > 0) raises the attack/sustain ratio.
    CHECK(ratioAfter > ratioBefore);
}

TEST_CASE("TransientShaper output is bounded within gain limits",
          "[TransientShaper]")
{
    // Even with max amount and a high-crest input, no sample should exceed the
    // kGainMax*kOutputGain ceiling times the input. Sanity: bounded, no NaN/inf.
    more_phi::TransientShaper ts;
    ts.prepare(kSr, 512);
    ts.setEnabled(true);
    ts.setAmount(1.0f);

    auto buf = makeBuffer(4096);
    for (int i = 0; i < buf.getNumSamples(); ++i)
        buf.setSample(0, i, (i % 64 == 0) ? 1.0f : 0.0f);  // impulsive

    ts.processBlock(buf);

    for (int i = 0; i < buf.getNumSamples(); ++i)
    {
        const float v = buf.getSample(0, i);
        CHECK(std::isfinite(v));
        // kGainMax=4.0, outputGain=1.0 → max gain 4× the input (1.0) = 4.0.
        CHECK(std::abs(v) <= 4.5f);
    }
}

TEST_CASE("TransientShaper steady signal stays near unity",
          "[TransientShaper]")
{
    // A steady sine (no transient content) should pass through nearly unchanged
    // because envFast ≈ envSlow → ratio ≈ 1 → gain ≈ 1. The slow envelope's
    // 150 ms time constant means the baseline needs ~0.5 s to fully establish,
    // so we read the settled tail of a long buffer, not the startup ramp.
    more_phi::TransientShaper ts;
    ts.prepare(kSr, 512);
    ts.setEnabled(true);
    ts.setAmount(0.5f);

    constexpr int kLen = 48000;  // 1 s — well past the 150 ms slow settle
    auto buf = makeBuffer(kLen);
    for (int i = 0; i < buf.getNumSamples(); ++i)
        buf.setSample(0, i, 0.4f * static_cast<float>(std::sin(2.0 * 3.14159 * 440.0 * i / kSr)));
    // Read the settled tail (last 0.25 s).
    const int tailStart = kLen - 12000;
    const float beforeRms = rms(buf, tailStart, 12000);

    ts.processBlock(buf);
    const float afterRms = rms(buf, tailStart, 12000);

    // Steady signal → ratio ~1 → minimal gain change (within ~10%).
    CHECK(afterRms == Catch::Approx(beforeRms).margin(0.10f * beforeRms));
}
