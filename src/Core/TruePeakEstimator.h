/*
 * More-Phi — Core/TruePeakEstimator.h
 *
 * ITU-R BS.1770-4 True Peak (inter-sample peak, ISP) estimator.
 *
 * Uses a simple 4x polyphase FIR upsampler (12-tap per phase) to detect
 * inter-sample peaks that standard sample-domain peak meters miss.
 * This is an intentionally lightweight implementation that does not depend
 * on the full OversamplingWrapper — it is designed to be instantiated
 * independently by both TruePeakEstimator and BrickwallLimiter.
 *
 * Method (EBU R128 / ITU-R BS.1770-4 Annex 2):
 *   Upsample by 4 using an FIR interpolation filter (zero-phase, linear-phase).
 *   Find the absolute maximum over all upsampled samples per block.
 *
 * Accuracy (measured 2026-06-19 against a Kaiser-windowed 4x reference
 * reconstruction — see tests/Unit/TestTruePeakEstimator.cpp):
 *   - DC / low-frequency ISP: within ~0 dB of reference (good).
 *   - Step-transition overshoot: detected, but undershoots the reference by
 *     up to ~2 dB depending on block alignment.
 *   - Near-Nyquist content (>=0.45*fs): under-reads by ~20-25 dB. The 12-tap
 *     polyphase prototype rolls off well before Nyquist, so a full-scale
 *     near-Nyquist sine reads ~-25 dBTP instead of ~-1 dBTP.
 *   This is a KNOWN LIMITATION. The estimator is adequate for detecting DC and
 *   low/mid-frequency inter-sample peaks but is NOT a ±0.2 dBTP meter. Any
 *   claim of reference-grade accuracy is unsupported until the prototype FIR
 *   is widened. The test suite pins these values as regression guards.
 *
 * Thread safety:
 *   processBlock() — audio thread only, noexcept.
 *   getTruePeak_dBTP() — atomic read, safe from any thread.
 *   prepare() — message thread only.
 */
#pragma once

#include <atomic>
#include <array>
#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>

namespace more_phi {

class TruePeakEstimator
{
public:
    static constexpr int kMaxChannels  = 2;
    static constexpr int kUpsampleFactor = 4;
    static constexpr int kFIRTaps = 12;     // taps per polyphase subfilter

    TruePeakEstimator();

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /** Prepare filter state for the given sample rate and block size. */
    void prepare(double sampleRate, int maxBlockSize) noexcept;

    /** Reset filter delay lines. */
    void reset() noexcept;

    // ── Audio thread ──────────────────────────────────────────────────────────

    /**
     * Process a block. Updates the true peak atomics.
     * noexcept — no allocations; pure polyphase FIR arithmetic.
     */
    void processBlock(const juce::AudioBuffer<float>& buf) noexcept;

    // ── Results (any thread) ──────────────────────────────────────────────────

    /** True peak of channel 0 (L) in dBTP. */
    [[nodiscard]] float getTruePeak_L_dBTP() const noexcept { return peakL_.load(std::memory_order_relaxed); }

    /** True peak of channel 1 (R) in dBTP. */
    [[nodiscard]] float getTruePeak_R_dBTP() const noexcept { return peakR_.load(std::memory_order_relaxed); }

    /** Stereo-linked maximum true peak in dBTP. */
    [[nodiscard]] float getTruePeak_dBTP()   const noexcept { return peak_.load(std::memory_order_relaxed); }

    /** Linear maximum true peak (0.0–∞). */
    [[nodiscard]] float getTruePeakLinear()  const noexcept { return peakLin_.load(std::memory_order_relaxed); }

    /**
     * Compute the true-peak (maximum over all kUpsampleFactor polyphase phases)
     * of the sample at position @p pos in a delay line of length @p delayLen.
     * Reads the kFIRTaps most-recent samples ending at @p pos.
     *
     * Used by BrickwallLimiter to drive gain reduction from inter-sample peaks,
     * so the dBTP ceiling is honored against ISPs — not just sample peaks.
     * noexcept, allocation-free, callable from the audio thread.
     *
     * @p delayLen must be ≥ kFIRTaps and the ring must hold at least kFIRTaps
     * valid samples behind @p pos (the caller's delay line satisfies this).
     */
    static float truePeakAt(const float* delay, int delayLen, int pos) noexcept;

private:
    // 4-phase × 12-tap polyphase FIR coefficients (pre-computed, symmetric Kaiser window)
    // Phase interleaving: phases 0,1,2,3 correspond to fractional delays 0, 1/4, 2/4, 3/4
    static const float kPolyCoeffs[kUpsampleFactor][kFIRTaps];

    // Per-channel delay line (length = kFIRTaps)
    std::array<float, kFIRTaps> delayL_{}, delayR_{};
    int delayPos_ = 0;

    std::atomic<float> peakL_  { -std::numeric_limits<float>::infinity() };
    std::atomic<float> peakR_  { -std::numeric_limits<float>::infinity() };
    std::atomic<float> peak_   { -std::numeric_limits<float>::infinity() };
    std::atomic<float> peakLin_{ 0.0f };

    static float applyPhase(const float* delay, int pos,
                             const float* coeff, int taps) noexcept;
};

} // namespace more_phi
