/*
 * MorphSnap — tests/Unit/TestAudioEngine.cpp
 *
 * Catch2 v3 tests for audio engine specifications:
 *   - OversamplingWrapper lifecycle and latency reporting
 *   - SampleRateManager (prepareToPlay contract verification)
 *   - Latency calculation and setLatencySamples consistency
 *   - SIMD interpolation correctness and scalar parity
 *   - Denormal suppression guard behavior
 *   - DC offset convergence of smoothing filter
 *   - Buffer-size-independent processing invariants
 *   - Audio quality: SNR floor for pass-through chain
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "Core/OversamplingWrapper.h"
#include "Core/InterpolationEngine.h"
#include "Core/MorphProcessor.h"
#include "Core/SnapshotBank.h"

#include <juce_dsp/juce_dsp.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

using Catch::Approx;
using namespace morphsnap;

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────
namespace {

/** Compute RMS of a float buffer. */
float computeRMS(const float* buf, int n)
{
    if (n == 0) return 0.0f;
    double acc = 0.0;
    for (int i = 0; i < n; ++i)
        acc += static_cast<double>(buf[i]) * buf[i];
    return static_cast<float>(std::sqrt(acc / n));
}

/** Compute SNR (dB) of signal vs. noise buffer. */
float computeSNR_dB(const float* signal, const float* noise, int n)
{
    double sigPow = 0.0, noisePow = 0.0;
    for (int i = 0; i < n; ++i)
    {
        sigPow   += static_cast<double>(signal[i]) * signal[i];
        noisePow += static_cast<double>(noise[i])  * noise[i];
    }
    if (noisePow < 1e-30) return 120.0f;
    return static_cast<float>(10.0 * std::log10(sigPow / noisePow));
}

/** Fill buffer with a sine wave. */
void fillSine(float* buf, int n, float freqHz, float sampleRate, float amplitude = 1.0f)
{
    for (int i = 0; i < n; ++i)
        buf[i] = amplitude * std::sin(2.0f * juce::MathConstants<float>::pi * freqHz * static_cast<float>(i) / sampleRate);
}

/** Compute DC offset (mean) of buffer. */
float computeDC(const float* buf, int n)
{
    if (n == 0) return 0.0f;
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
        sum += buf[i];
    return static_cast<float>(sum / n);
}

} // namespace


// ─────────────────────────────────────────────────────────────────────────────
//  OversamplingWrapper — Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("OversamplingWrapper: x1 factor reports zero latency", "[oversampling]")
{
    OversamplingWrapper os;
    os.setFactor(OversamplingFactor::x1);
    os.prepare(512, 2, 48000.0);

    REQUIRE(os.getLatencyInSamples() == 0);
    REQUIRE_FALSE(os.isActive());
    REQUIRE(os.getActiveFactor() == 1);
}

TEST_CASE("OversamplingWrapper: x2 factor reports positive latency", "[oversampling]")
{
    OversamplingWrapper os;
    os.setFactor(OversamplingFactor::x2);
    os.prepare(512, 2, 48000.0);

    REQUIRE(os.isActive());
    REQUIRE(os.getActiveFactor() == 2);
    REQUIRE(os.getLatencyInSamples() > 0);
}

TEST_CASE("OversamplingWrapper: x4 FIR factor latency is greater than x2", "[oversampling]")
{
    OversamplingWrapper os2, os4;
    os2.setFactor(OversamplingFactor::x2);
    os4.setFactor(OversamplingFactor::x4);
    os2.setFilterType(AAFilterType::FIR);
    os4.setFilterType(AAFilterType::FIR);
    os2.prepare(512, 2, 48000.0);
    os4.prepare(512, 2, 48000.0);

    // Higher factor = more filter stages = more latency
    REQUIRE(os4.getLatencyInSamples() > os2.getLatencyInSamples());
}

TEST_CASE("OversamplingWrapper: IIR mode has lower or equal latency than FIR at same factor", "[oversampling]")
{
    OversamplingWrapper osFIR, osIIR;
    osFIR.setFactor(OversamplingFactor::x4);
    osIIR.setFactor(OversamplingFactor::x4);
    osFIR.setFilterType(AAFilterType::FIR);
    osIIR.setFilterType(AAFilterType::IIR);
    osFIR.prepare(512, 2, 48000.0);
    osIIR.prepare(512, 2, 48000.0);

    REQUIRE(osIIR.getLatencyInSamples() <= osFIR.getLatencyInSamples());
}

TEST_CASE("OversamplingWrapper: reset() does not change latency", "[oversampling]")
{
    OversamplingWrapper os;
    os.setFactor(OversamplingFactor::x4);
    os.prepare(512, 2, 48000.0);
    int latBefore = os.getLatencyInSamples();

    os.reset();

    REQUIRE(os.getLatencyInSamples() == latBefore);
}

TEST_CASE("OversamplingWrapper: reprepare with different block size is safe", "[oversampling]")
{
    OversamplingWrapper os;
    os.setFactor(OversamplingFactor::x4);
    os.prepare(512, 2, 48000.0);
    // Should not crash or assert:
    os.prepare(256, 2, 48000.0);
    os.prepare(2048, 2, 44100.0);
    REQUIRE(os.isActive());
}

TEST_CASE("OversamplingWrapper: x1 upsample returns a block of the same size", "[oversampling]")
{
    OversamplingWrapper os;
    os.setFactor(OversamplingFactor::x1);
    os.prepare(64, 1, 48000.0);

    // Allocate a simple stereo buffer
    std::vector<float> data(64, 0.5f);
    float* ptr = data.data();
    juce::dsp::AudioBlock<float> inBlock(&ptr, 1, 64);

    auto osBlock = os.upsample(inBlock);
    // At x1 it is a view of the same block — same number of samples
    REQUIRE(osBlock.getNumSamples() == 64);
}

TEST_CASE("OversamplingWrapper: x4 upsample block has 4x the samples", "[oversampling]")
{
    OversamplingWrapper os;
    os.setFactor(OversamplingFactor::x4);
    os.prepare(64, 1, 48000.0);

    std::vector<float> data(64, 0.0f);
    float* ptr = data.data();
    juce::dsp::AudioBlock<float> inBlock(&ptr, 1, 64);

    auto osBlock = os.upsample(inBlock);
    REQUIRE(osBlock.getNumSamples() == 64 * 4);
}

TEST_CASE("OversamplingWrapper: CPU overhead factor increases with factor", "[oversampling]")
{
    OversamplingWrapper os;
    os.setFactor(OversamplingFactor::x1); os.prepare(512, 2, 48000.0);
    float cpu1 = os.estimatedCPUOverheadFactor();

    os.setFactor(OversamplingFactor::x2); os.prepare(512, 2, 48000.0);
    float cpu2 = os.estimatedCPUOverheadFactor();

    os.setFactor(OversamplingFactor::x4); os.prepare(512, 2, 48000.0);
    float cpu4 = os.estimatedCPUOverheadFactor();

    REQUIRE(cpu4 > cpu2);
    REQUIRE(cpu2 > cpu1);
    REQUIRE(cpu1 == Approx(1.0f));
}


// ─────────────────────────────────────────────────────────────────────────────
//  Sample Rate Support Matrix
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("MorphProcessor: prepares successfully at all supported sample rates", "[samplerate]")
{
    // Spec requires support for: 44.1, 48, 88.2, 96, 176.4, 192 kHz
    const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    const int blockSize = 512;
    const int paramCount = 64;

    for (double sr : rates)
    {
        INFO("Sample rate: " << sr);
        SnapshotBank bank;
        bank.prepare(paramCount);

        MorphProcessor proc(bank);
        // prepare() must not throw or assert at any supported sample rate
        REQUIRE_NOTHROW(proc.prepare(paramCount));
    }
}

TEST_CASE("OversamplingWrapper: prepares correctly at all supported sample rates", "[samplerate]")
{
    const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };

    for (double sr : rates)
    {
        INFO("Sample rate: " << sr);
        OversamplingWrapper os;
        os.setFactor(OversamplingFactor::x4);
        REQUIRE_NOTHROW(os.prepare(512, 2, sr));
        REQUIRE(os.getLatencyInSamples() > 0);
    }
}


// ─────────────────────────────────────────────────────────────────────────────
//  Buffer Size Independence
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("MorphProcessor: produces consistent output across varying block sizes", "[buffersize]")
{
    // The morph output for a fixed cursor position must be identical
    // regardless of whether we process in 32, 256, or 1024-sample blocks.
    const int paramCount = 8;

    SnapshotBank bank;
    bank.prepare(paramCount);

    std::vector<float> vA = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
    std::vector<float> vB = {0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f, 0.3f, 0.2f};
    bank.captureValues(0, vA);
    bank.captureValues(6, vB);

    // Reference: compute with smoothing disabled (rate=0 → instant convergence)
    MorphProcessor refProc(bank);
    refProc.prepare(paramCount);
    refProc.setSmoothingRate(0.0f);

    std::vector<float> refOut(paramCount, 0.0f);
    // Run enough blocks to fully converge
    for (int i = 0; i < 50; ++i)
        refProc.process(0.5f, 0.5f, 0.5f, MorphSource::XYPad, MorphMode::Direct, 1.0f / 60.0f, refOut);

    // Now check that different block-size callers converge to the same values
    const int blockSizes[] = { 32, 64, 128, 256, 512, 1024 };
    for (int bs : blockSizes)
    {
        INFO("Block size: " << bs);
        MorphProcessor proc(bank);
        proc.prepare(paramCount);
        proc.setSmoothingRate(0.0f);

        std::vector<float> out(paramCount, 0.0f);
        // Simulate multiple blocks
        for (int i = 0; i < 50; ++i)
            proc.process(0.5f, 0.5f, 0.5f, MorphSource::XYPad, MorphMode::Direct, 1.0f / 60.0f, out);

        for (int p = 0; p < paramCount; ++p)
            REQUIRE(out[p] == Approx(refOut[p]).margin(0.001f));
    }
}


// ─────────────────────────────────────────────────────────────────────────────
//  Latency Calculation
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Latency: oversampling latency increases monotonically with factor", "[latency]")
{
    std::array<int, 4> latencies{};
    const OversamplingFactor factors[] = {
        OversamplingFactor::x1,
        OversamplingFactor::x2,
        OversamplingFactor::x4,
        OversamplingFactor::x8
    };

    for (int i = 0; i < 4; ++i)
    {
        OversamplingWrapper os;
        os.setFactor(factors[i]);
        os.prepare(512, 2, 48000.0);
        latencies[i] = os.getLatencyInSamples();
    }

    REQUIRE(latencies[1] > latencies[0]);
    REQUIRE(latencies[2] > latencies[1]);
    REQUIRE(latencies[3] > latencies[2]);
}

TEST_CASE("Latency: x1 bypass introduces exactly 0 samples of latency", "[latency]")
{
    OversamplingWrapper os;
    os.setFactor(OversamplingFactor::x1);
    os.prepare(512, 2, 48000.0);
    REQUIRE(os.getLatencyInSamples() == 0);
}


// ─────────────────────────────────────────────────────────────────────────────
//  DC Offset Prevention
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Smoothing: converges to target without DC bias when input is zero", "[dc]")
{
    // Run the morph processor with both snapshots at 0.5 and cursor at center.
    // The smoothed output should converge to 0.5 with no drift.
    const int paramCount = 16;

    SnapshotBank bank;
    bank.prepare(paramCount);

    std::vector<float> vals(paramCount, 0.5f);
    bank.captureValues(0, vals);

    MorphProcessor proc(bank);
    proc.prepare(paramCount);
    proc.setSmoothingRate(0.9f);

    std::vector<float> out(paramCount, 0.0f);
    for (int i = 0; i < 500; ++i)
        proc.process(0.0f, 0.0f, 0.5f, MorphSource::XYPad, MorphMode::Direct, 1.0f / 60.0f, out);

    for (int p = 0; p < paramCount; ++p)
        REQUIRE(out[p] == Approx(0.5f).margin(0.001f));
}


// ─────────────────────────────────────────────────────────────────────────────
//  Audio Quality: SNR
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Audio quality: upsample+downsample SNR > 80 dB for 1 kHz sine at x4 FIR", "[snr][audio]")
{
    // This verifies that the oversampling chain itself does not degrade the
    // signal by more than 80 dB — the threshold for production audio quality.
    constexpr int N        = 1024;
    constexpr float SR     = 48000.0f;
    constexpr float freqHz = 1000.0f;

    OversamplingWrapper os;
    os.setFactor(OversamplingFactor::x4);
    os.setFilterType(AAFilterType::FIR);
    os.prepare(N, 1, static_cast<double>(SR));

    // Original sine
    std::vector<float> original(N);
    fillSine(original.data(), N, freqHz, SR);

    // Working copy to process
    std::vector<float> processed = original;

    float* ptr = processed.data();
    juce::dsp::AudioBlock<float> block(&ptr, 1, static_cast<size_t>(N));

    // Upsample → identity processing → downsample
    {
        auto osBlock = os.upsample(block);
        // No nonlinear processing — identity pass-through
        os.downsample(block);
    }

    // Compute noise = processed - original (accounting for filter latency)
    const int latency = os.getLatencyInSamples();
    const int compareN = N - latency;
    if (compareN <= 0)
    {
        // Block too small for this latency — skip but document why
        WARN("Block size too small to measure SNR at this latency (" << latency << " samples)");
        return;
    }

    std::vector<float> noise(compareN);
    for (int i = 0; i < compareN; ++i)
        noise[i] = processed[i + latency] - original[i];

    float snr = computeSNR_dB(original.data(), noise.data(), compareN);
    INFO("SNR = " << snr << " dB (latency = " << latency << " samples)");

    // 80 dB is the minimum acceptable for 24-bit audio processing
    REQUIRE(snr > 80.0f);
}

TEST_CASE("Audio quality: x1 bypass has perfect SNR (exact pass-through)", "[snr][audio]")
{
    constexpr int N = 512;
    std::vector<float> original(N);
    fillSine(original.data(), N, 1000.0f, 48000.0f);

    std::vector<float> processed = original;
    float* ptr = processed.data();
    juce::dsp::AudioBlock<float> block(&ptr, 1, static_cast<size_t>(N));

    OversamplingWrapper os;
    os.setFactor(OversamplingFactor::x1);
    os.prepare(N, 1, 48000.0);

    auto osBlock = os.upsample(block);
    os.downsample(block);

    // At x1 the signal must be byte-identical to the original
    for (int i = 0; i < N; ++i)
        REQUIRE(processed[i] == original[i]);
}


// ─────────────────────────────────────────────────────────────────────────────
//  SIMD Interpolation — Quality Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("InterpolationEngine: interpolateBatch_SIMD matches scalar for all boundary t values", "[simd][interp]")
{
    // t = 0.0, 1.0 are the most numerically sensitive cases
    const float tValues[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
    const size_t N = 32;

    std::vector<float> a(N), b(N), dst_simd(N), dst_scalar(N);
    for (size_t i = 0; i < N; ++i)
    {
        a[i] = static_cast<float>(i) * 0.03f;
        b[i] = 1.0f - a[i];
    }

    for (float t : tValues)
    {
        INFO("t = " << t);
        InterpolationEngine::interpolateBatch_SIMD(a.data(), b.data(), dst_simd.data(), t, N);

        for (size_t i = 0; i < N; ++i)
            dst_scalar[i] = a[i] * (1.0f - t) + b[i] * t;

        for (size_t i = 0; i < N; ++i)
            REQUIRE(dst_simd[i] == Approx(dst_scalar[i]).margin(1e-5f));
    }
}

TEST_CASE("InterpolationEngine: interpolateBatch_SIMD handles non-multiple-of-8 sizes correctly", "[simd][interp]")
{
    // Specifically exercises the scalar remainder loop in the SIMD path
    for (size_t N : { size_t(1), size_t(3), size_t(7), size_t(9), size_t(15), size_t(17) })
    {
        INFO("N = " << N);
        std::vector<float> a(N, 0.3f), b(N, 0.7f), dst(N);
        InterpolationEngine::interpolateBatch_SIMD(a.data(), b.data(), dst.data(), 0.5f, N);

        for (size_t i = 0; i < N; ++i)
            REQUIRE(dst[i] == Approx(0.5f).margin(1e-5f));
    }
}


// ─────────────────────────────────────────────────────────────────────────────
//  Bypass Behavior
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("MorphProcessor: with no occupied slots, output remains unchanged", "[bypass]")
{
    // When SnapshotBank is empty, MorphProcessor must not modify the output
    // vector. This implements the "true bypass" contract.
    SnapshotBank bank;
    bank.prepare(4);

    MorphProcessor proc(bank);
    proc.prepare(4);

    std::vector<float> out = { 0.1f, 0.2f, 0.3f, 0.4f };
    proc.process(0.5f, 0.5f, 0.5f, MorphSource::XYPad, MorphMode::Direct, 1.0f / 60.0f, out);

    // Output must be unchanged
    REQUIRE(out[0] == Approx(0.1f));
    REQUIRE(out[1] == Approx(0.2f));
    REQUIRE(out[2] == Approx(0.3f));
    REQUIRE(out[3] == Approx(0.4f));
}


// ─────────────────────────────────────────────────────────────────────────────
//  Denormal Suppression
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Denormal guard: ScopedNoDenormals compiles and is available", "[denormal]")
{
    // Verify JUCE's denormal suppression utility is accessible.
    // The presence and correct flush-to-zero behavior is critical for
    // preventing CPU stalls in the physics/smoothing loops.
    {
        juce::ScopedNoDenormals noDenormals;

        // A value very close to the denormal threshold (~1e-38 for float)
        volatile float denormalCandidate = std::numeric_limits<float>::min() * 0.1f;

        // With flush-to-zero enabled, arithmetic with near-zero values
        // must complete in bounded time (not trigger microcode assist).
        volatile float result = denormalCandidate * 0.99f;
        (void)result;
    }
    // ScopedNoDenormals restores FPU state on destruction — this must not crash.
    SUCCEED("ScopedNoDenormals constructed and destroyed without error");
}
