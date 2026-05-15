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
