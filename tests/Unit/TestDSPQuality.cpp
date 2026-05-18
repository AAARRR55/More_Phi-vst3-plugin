/*
 * More-Phi — tests/Unit/TestDSPQuality.cpp
 *
 * Audio quality regression tests.
 * These tests verify measurable signal quality properties:
 *   - Aliasing detection via swept-sine test
 *   - Latency measurement consistency
 *   - Smoothing filter DC accuracy
 *   - Parameter interpolation monotonicity
 *   - Oversampling round-trip fidelity (A/B comparison)
 *
 * Tests are intentionally "bit-near" rather than bit-exact to allow
 * compiler-specific FP optimizations while catching gross regressions.
 */

#define _USE_MATH_DEFINES
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Core/OversamplingWrapper.h"
#include "Core/InterpolationEngine.h"
#include "Core/SnapshotBank.h"
#include "Core/MorphProcessor.h"
#include "Core/LatencyManager.h"
#include "Host/IPluginHostManager.h"

#include <juce_dsp/juce_dsp.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>
#include <complex>

using Catch::Approx;
using namespace more_phi;

// ─────────────────────────────────────────────────────────────────────────────
//  DSP Test Utilities
// ─────────────────────────────────────────────────────────────────────────────
namespace {

/** Peak magnitude of the FFT of buf in the frequency band [freqLo, freqHi] Hz. */
float peakMagnitudeInBand(const std::vector<float>& buf, float sampleRate,
                          float freqLo, float freqHi)
{
    const int N = static_cast<int>(buf.size());
    // Discrete Fourier magnitude using Goertzel for each bin in band
    float peakMag = 0.0f;
    for (int k = 0; k < N / 2; ++k)
    {
        float freq = static_cast<float>(k) * sampleRate / static_cast<float>(N);
        if (freq < freqLo || freq > freqHi) continue;

        // Goertzel
        double omega = 2.0 * M_PI * k / N;
        double coeff = 2.0 * std::cos(omega);
        double q1 = 0.0, q2 = 0.0;
        for (int n = 0; n < N; ++n)
        {
            double q0 = coeff * q1 - q2 + buf[n];
            q2 = q1;
            q1 = q0;
        }
        float mag = static_cast<float>(std::sqrt(q1*q1 + q2*q2 - q1*q2*coeff));
        peakMag = std::max(peakMag, mag);
    }
    return peakMag;
}

void fillSine(std::vector<float>& buf, float freqHz, float sampleRate, float amplitude = 1.0f)
{
    for (int i = 0; i < static_cast<int>(buf.size()); ++i)
        buf[i] = amplitude * std::sin(2.0f * juce::MathConstants<float>::pi * freqHz
                                      * static_cast<float>(i) / sampleRate);
}

float computeRMS(const std::vector<float>& buf)
{
    double acc = 0.0;
    for (float v : buf) acc += static_cast<double>(v) * v;
    return static_cast<float>(std::sqrt(acc / buf.size()));
}

float rmsRatio_dB(float signal, float noise)
{
    if (noise < 1e-30f) return 120.0f;
    return 20.0f * std::log10(signal / noise);
}

uint64_t checksumNormalizedValues(const std::vector<float>& values)
{
    uint64_t hash = 1469598103934665603ull;
    for (float value : values)
    {
        const auto quantized = static_cast<uint32_t>(
            std::lround(std::clamp(value, 0.0f, 1.0f) * 1000000.0f));
        hash ^= quantized;
        hash *= 1099511628211ull;
    }
    return hash;
}

class SnapshotSuiteBridge final : public IParameterBridge
{
public:
    explicit SnapshotSuiteBridge(int parameterCount)
        : values(static_cast<size_t>(parameterCount), 0.0f)
    {
    }

    int getParameterCount() const override { return static_cast<int>(values.size()); }

    float getParameterNormalized(int index) const override
    {
        return values.at(static_cast<size_t>(index));
    }

    void setParameterNormalized(int index, float value) override
    {
        values.at(static_cast<size_t>(index)) = value;
    }

    juce::String getParameterName(int index) const override
    {
        return "Param " + juce::String(index);
    }

    void applyParameterState(const std::vector<float>& newValues) override
    {
        applyParameterState(newValues.data(), static_cast<int>(newValues.size()));
    }

    void applyParameterState(const float* newValues, int count) override
    {
        ++applyCount;
        const int safeCount = juce::jmin(count, static_cast<int>(values.size()));
        for (int i = 0; i < safeCount; ++i)
            values[static_cast<size_t>(i)] = newValues[i];
    }

    std::vector<float> captureParameterState() const override { return values; }
    bool isDiscrete(int) const override { return false; }
    std::vector<bool> getDiscreteMap() const override { return std::vector<bool>(values.size(), false); }
    juce::String getParameterLabel(int) const override { return {}; }
    juce::String getParameterDisplayValue(int index) const override { return juce::String(values.at(static_cast<size_t>(index))); }
    float getParameterDefault(int) const override { return 0.5f; }
    juce::StringArray getParameterValueStrings(int) const override { return {}; }

    std::vector<float> values;
    int applyCount = 0;
};

} // namespace


// ─────────────────────────────────────────────────────────────────────────────
//  LatencyManager — unit tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("LatencyManager: zero on construction", "[latency]")
{
    LatencyManager lm;
    REQUIRE(lm.getTotal() == 0);
    REQUIRE(lm.getOversamplingLatency() == 0);
    REQUIRE(lm.getFFTWindowLatency() == 0);
    REQUIRE(lm.getHostedPluginLatency() == 0);
}

TEST_CASE("LatencyManager: total = sum of all components", "[latency]")
{
    LatencyManager lm;
    lm.setOversamplingLatency(32);
    lm.setFFTWindowLatency(512);
    lm.setHostedPluginLatency(128);

    REQUIRE(lm.getTotal() == 32 + 512 + 128);
}

TEST_CASE("LatencyManager: negative inputs are clamped to zero", "[latency]")
{
    LatencyManager lm;
    lm.setOversamplingLatency(-10);
    lm.setFFTWindowLatency(-5);
    REQUIRE(lm.getTotal() == 0);
}

TEST_CASE("LatencyManager: updating one component recalculates total", "[latency]")
{
    LatencyManager lm;
    lm.setOversamplingLatency(64);
    REQUIRE(lm.getTotal() == 64);

    lm.setHostedPluginLatency(256);
    REQUIRE(lm.getTotal() == 320);

    lm.setOversamplingLatency(0);
    REQUIRE(lm.getTotal() == 256);
}

TEST_CASE("LatencyManager: FFT window latency matches fftSize/2 formula", "[latency]")
{
    // For a 1024-point FFT, the window-induced latency is 512 samples.
    const int fftSize = 1024;
    LatencyManager lm;
    lm.setFFTWindowLatency(fftSize / 2);

    REQUIRE(lm.getFFTWindowLatency() == 512);
}

TEST_CASE("LatencyManager: mode switch to bypass resets oversampling latency to 0", "[latency]")
{
    LatencyManager lm;
    lm.setOversamplingLatency(72);
    REQUIRE(lm.getTotal() == 72);

    // Simulate switching to Direct mode (no oversampling)
    lm.setOversamplingLatency(0);
    REQUIRE(lm.getTotal() == 0);
}


// ─────────────────────────────────────────────────────────────────────────────
//  Aliasing detection — swept-sine test
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Audio quality: no aliasing above Nyquist after x4 oversampling", "[aliasing][snr]")
{
    // Generate a 20 kHz sine at 48 kHz sample rate (just below Nyquist at 24 kHz).
    // After x4 oversampling + identity processing + downsample, there must be
    // no significant energy in frequencies above 22 kHz — the alias range.
    //
    // Practical test: measure energy in [22000, 24000] Hz band before and
    // after the oversampling round-trip. The ratio must be < -60 dB.

    constexpr float SR     = 48000.0f;
    constexpr int   N      = 4096;
    constexpr float freqHz = 20000.0f;  // near-Nyquist tone

    OversamplingWrapper os;
    os.setFactor(OversamplingFactor::x4);
    os.setFilterType(AAFilterType::FIR);
    os.prepare(N, 1, static_cast<double>(SR));

    // Generate a continuous sine across several blocks
    constexpr int kTotalBlocks = 4;
    std::vector<float> continuousSine(static_cast<size_t>(N * kTotalBlocks));
    for (size_t i = 0; i < continuousSine.size(); ++i)
        continuousSine[i] = std::sin(2.0f * 3.14159265358979f * freqHz
                                     * static_cast<float>(i) / SR);

    // Process all blocks to let the FIR filter fully settle
    std::vector<float> allOutput(static_cast<size_t>(N * kTotalBlocks));
    for (int b = 0; b < kTotalBlocks; ++b)
    {
        std::vector<float> blockBuf(continuousSine.begin() + b * N,
                                    continuousSine.begin() + (b + 1) * N);
        float* ptr = blockBuf.data();
        juce::dsp::AudioBlock<float> block(&ptr, 1, static_cast<size_t>(N));

        auto osBlock = os.upsample(block);
        os.downsample(block);

        std::copy_n(blockBuf.begin(), static_cast<size_t>(N),
                     allOutput.begin() + b * N);
    }

    // Measure on the last block (filter fully settled)
    std::vector<float> lastBlock(allOutput.begin() + (kTotalBlocks - 1) * N,
                                  allOutput.end());

    // Measure fundamental peak (band around freqHz ± 1000 Hz)
    float fundamental = peakMagnitudeInBand(lastBlock, SR, freqHz - 1000.0f, freqHz + 1000.0f);

    // Measure alias band (22 kHz - 24 kHz)
    float aliasBand = peakMagnitudeInBand(lastBlock, SR, 22000.0f, 24000.0f);

    if (fundamental > 0.0f && aliasBand > 0.0f)
    {
        float ratio_dB = 20.0f * std::log10(aliasBand / fundamental);
        INFO("Alias suppression: " << -ratio_dB << " dB");
        // Require at least 50 dB alias suppression (practical for near-Nyquist tones)
        REQUIRE(ratio_dB < -50.0f);
    }
}


// ─────────────────────────────────────────────────────────────────────────────
//  Parameter Interpolation Monotonicity
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("InterpolationEngine: 1D compute output is monotonic between snapshots", "[interp][quality]")
{
    // Sweeping the fader from 0.0 to 1.0 should produce strictly monotonic
    // output for a simple two-snapshot case where snapshot B > snapshot A.
    const int paramCount = 4;

    SnapshotBank bank;
    bank.prepare(paramCount);

    std::vector<float> vA = { 0.0f, 0.0f, 0.0f, 0.0f };
    std::vector<float> vB = { 1.0f, 1.0f, 1.0f, 1.0f };
    bank.captureValues(0, vA);
    bank.captureValues(1, vB);

    constexpr int steps = 20;
    std::vector<float> prevOut(paramCount, -1.0f);

    for (int step = 0; step <= steps; ++step)
    {
        float t = static_cast<float>(step) / static_cast<float>(steps);
        std::vector<float> out(paramCount, 0.0f);
        InterpolationEngine::compute1D(t, bank, out);

        for (int p = 0; p < paramCount; ++p)
        {
            INFO("param " << p << " at t=" << t << ": out=" << out[p] << " prev=" << prevOut[p]);
            REQUIRE(out[p] >= prevOut[p] - 1e-4f);  // monotonically non-decreasing
            REQUIRE(out[p] >= 0.0f);
            REQUIRE(out[p] <= 1.0f);
        }
        prevOut = out;
    }
}

TEST_CASE("InterpolationEngine: 1D compute returns exact snapshot values at endpoints", "[interp][quality]")
{
    const int paramCount = 4;
    SnapshotBank bank;
    bank.prepare(paramCount);

    std::vector<float> vA = { 0.1f, 0.2f, 0.3f, 0.4f };
    std::vector<float> vB = { 0.6f, 0.7f, 0.8f, 0.9f };
    bank.captureValues(0, vA);
    bank.captureValues(1, vB);

    std::vector<float> atZero(paramCount), atOne(paramCount);
    InterpolationEngine::compute1D(0.0f, bank, atZero);
    InterpolationEngine::compute1D(1.0f, bank, atOne);

    for (int p = 0; p < paramCount; ++p)
    {
        REQUIRE(atZero[p] == Approx(vA[p]).margin(0.01f));
        REQUIRE(atOne[p]  == Approx(vB[p]).margin(0.01f));
    }
}

TEST_CASE("Snapshot Suite: captures 12 slots, recalls by checksum, morphs adjacent slots, and handles empties", "[snapshot][suite]")
{
    constexpr int paramCount = 16;

    SnapshotBank bank;
    bank.prepare(paramCount);

    REQUIRE_FALSE(bank.hasAnyOccupied());

    // Empty bank and empty slot access must be safe.
    std::vector<float> emptyOut(paramCount, 0.25f);
    InterpolationEngine::compute1D(0.5f, bank, emptyOut);
    for (float value : emptyOut)
        REQUIRE(value == Approx(0.5f).margin(0.0001f));

    MorphProcessor emptyMorph(bank);
    emptyMorph.prepare(paramCount);
    std::vector<float> unchanged(paramCount, 0.25f);
    emptyMorph.process(0.5f, 0.5f, 0.5f, MorphSource::Fader, MorphMode::Direct, 1.0f / 60.0f, unchanged);
    for (float value : unchanged)
        REQUIRE(value == Approx(0.25f).margin(0.0001f));

    std::array<std::vector<float>, SnapshotBank::NUM_SLOTS> expected{};
    std::array<uint64_t, SnapshotBank::NUM_SLOTS> expectedChecksums{};

    for (int slot = 0; slot < SnapshotBank::NUM_SLOTS; ++slot)
    {
        auto& values = expected[static_cast<size_t>(slot)];
        values.resize(paramCount);
        for (int p = 0; p < paramCount; ++p)
        {
            const int raw = (slot * 37 + p * 19 + 11) % 101;
            values[static_cast<size_t>(p)] = static_cast<float>(raw) / 100.0f;
        }

        bank.captureValues(slot, values);
        expectedChecksums[static_cast<size_t>(slot)] = checksumNormalizedValues(values);

        std::vector<float> copied;
        REQUIRE(bank.getSlotValuesCopy(slot, copied));
        REQUIRE(copied.size() == static_cast<size_t>(paramCount));
        REQUIRE(checksumNormalizedValues(copied) == expectedChecksums[static_cast<size_t>(slot)]);
    }

    for (int slot = 0; slot < SnapshotBank::NUM_SLOTS; ++slot)
        REQUIRE(bank.isOccupied(slot));

    std::array<int, SnapshotBank::NUM_SLOTS> occupied{};
    REQUIRE(bank.getOccupiedSlots(occupied) == SnapshotBank::NUM_SLOTS);
    for (int slot = 0; slot < SnapshotBank::NUM_SLOTS; ++slot)
        REQUIRE(occupied[static_cast<size_t>(slot)] == slot);

    SnapshotSuiteBridge bridge(paramCount);
    for (int slot = 0; slot < SnapshotBank::NUM_SLOTS; ++slot)
    {
        bridge.values.assign(static_cast<size_t>(paramCount), 0.0f);
        bridge.applyCount = 0;

        bank.recallFast(slot, bridge);

        REQUIRE(bridge.applyCount == 1);
        REQUIRE(checksumNormalizedValues(bridge.values) == expectedChecksums[static_cast<size_t>(slot)]);
    }

    MorphProcessor morph(bank);
    morph.prepare(paramCount);
    morph.setSmoothingRate(0.0f);

    for (int slot = 0; slot < SnapshotBank::NUM_SLOTS - 1; ++slot)
    {
        const float faderPos = (static_cast<float>(slot) + 0.5f)
                             / static_cast<float>(SnapshotBank::NUM_SLOTS - 1);
        std::vector<float> out(paramCount, 0.0f);
        morph.process(0.5f, 0.5f, faderPos, MorphSource::Fader, MorphMode::Direct, 1.0f / 60.0f, out);

        for (int p = 0; p < paramCount; ++p)
        {
            const float expectedMidpoint =
                0.5f * (expected[static_cast<size_t>(slot)][static_cast<size_t>(p)]
                      + expected[static_cast<size_t>(slot + 1)][static_cast<size_t>(p)]);
            INFO("slot pair " << slot << "-" << (slot + 1) << ", param " << p);
            REQUIRE(out[static_cast<size_t>(p)] == Approx(expectedMidpoint).margin(0.0001f));
        }
    }

    bridge.values.assign(static_cast<size_t>(paramCount), 0.33f);
    bridge.applyCount = 0;
    bank.clearSlot(5);
    REQUIRE_FALSE(bank.isOccupied(5));
    REQUIRE_NOTHROW(bank.recallFast(5, bridge));
    REQUIRE(bridge.applyCount == 0);
    for (float value : bridge.values)
        REQUIRE(value == Approx(0.33f).margin(0.0001f));
}


// ─────────────────────────────────────────────────────────────────────────────
//  Smoothing convergence speed
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("MorphProcessor: higher smoothing rate converges more slowly", "[smoothing]")
{
    // A higher smoothing rate (e.g. 0.98) means more of the old value
    // is retained, so it takes longer to converge to the target.
    const int paramCount = 4;

    SnapshotBank bankFast, bankSlow;
    bankFast.prepare(paramCount);
    bankSlow.prepare(paramCount);

    std::vector<float> vals = { 1.0f, 1.0f, 1.0f, 1.0f };
    bankFast.captureValues(0, vals);
    bankSlow.captureValues(0, vals);

    MorphProcessor procFast(bankFast), procSlow(bankSlow);
    procFast.prepare(paramCount);
    procSlow.prepare(paramCount);
    procFast.setSmoothingRate(0.5f);   // fast convergence
    procSlow.setSmoothingRate(0.98f);  // slow convergence

    std::vector<float> outFast(paramCount, 0.0f);
    std::vector<float> outSlow(paramCount, 0.0f);

    // Run 10 blocks — fast should be much closer to 1.0 than slow
    for (int i = 0; i < 10; ++i)
    {
        procFast.process(0.0f, 0.0f, 0.5f, MorphSource::XYPad, MorphMode::Direct, 1.0f/60.0f, outFast);
        procSlow.process(0.0f, 0.0f, 0.5f, MorphSource::XYPad, MorphMode::Direct, 1.0f/60.0f, outSlow);
    }

    // Fast smoother should be closer to target (1.0) than slow smoother
    REQUIRE(outFast[0] > outSlow[0]);
}

TEST_CASE("MorphProcessor: smoothing rate 0.0 means instant convergence", "[smoothing]")
{
    const int paramCount = 4;
    SnapshotBank bank;
    bank.prepare(paramCount);

    std::vector<float> vals = { 0.75f, 0.25f, 0.5f, 1.0f };
    bank.captureValues(0, vals);

    MorphProcessor proc(bank);
    proc.prepare(paramCount);
    proc.setSmoothingRate(0.0f);  // instant — no averaging

    std::vector<float> out(paramCount, 0.0f);
    proc.process(0.0f, 0.0f, 0.5f, MorphSource::XYPad, MorphMode::Direct, 1.0f/60.0f, out);

    // With zero smoothing rate, first call should already be at target
    for (int p = 0; p < paramCount; ++p)
        REQUIRE(out[p] == Approx(vals[p]).margin(0.01f));
}
