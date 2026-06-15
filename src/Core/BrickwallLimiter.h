/*
 * More-Phi — Core/BrickwallLimiter.h
 *
 * Lookahead brickwall limiter with inter-sample peak (ISP) detection.
 *
 * Architecture:
 *   1. Input samples are written into a ring-buffer delay line (4 ms lookahead).
 *   2. A TruePeakEstimator scans the lookahead window for ISPs.
 *   3. Gain reduction is computed: G = clamp(ceiling_linear / peak, 0, 1).
 *   4. Gain is smoothed: attack = instantaneous, release = logarithmic (50–200 ms).
 *   5. Delayed output samples are multiplied by the smoothed gain.
 *
 * True Peak ceiling default: -1.0 dBTP (streaming safe).
 *
 * Thread safety:
 *   processBlock() — audio thread only, noexcept.
 *   setCeiling(), setRelease() — atomic writes, any thread.
 *   prepare() — message thread only.
 */
#pragma once

#include <atomic>
#include <array>
#include <cmath>
#include <limits>
#include <juce_audio_basics/juce_audio_basics.h>
#include "TruePeakEstimator.h"

namespace more_phi {

class BrickwallLimiter
{
public:
    static constexpr int   kMaxChannels       = 2;
    static constexpr float kDefaultCeilingDBTP = -1.0f;   // dBTP
    static constexpr float kDefaultReleaseMsec =  80.0f;   // ms

    // Lookahead buffer: sized for 10 ms at 192 kHz (maximum supported SR).
    // At 48 kHz 4 ms = 192 samples; buffer headroom allows up to 10 ms.
    static constexpr int kLookaheadBufSize = 2048;

    BrickwallLimiter();

    // ── Configuration (any thread — atomic) ───────────────────────────────────

    /** True Peak ceiling in dBTP. Default: -1.0 dBTP. */
    void setCeiling(float dBTP) noexcept;

    /** Release time in milliseconds [1–500]. Default: 80 ms. */
    void setRelease(float ms) noexcept;

    /** Enable or bypass the limiter. Bypass: output = input, no latency shift. */
    void setEnabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_relaxed); }

    // ── Lifecycle (message thread) ─────────────────────────────────────────────

    /**
     * Prepare delay lines, true peak estimator, and gain smoother.
     * Computes lookahead sample count from sampleRate.
     */
    void prepare(double sampleRate, int maxBlockSize);

    /** Flush delay lines and reset gain smoother to 0 dB. */
    void reset() noexcept;

    // ── Audio thread ──────────────────────────────────────────────────────────

    /**
     * Limit buffer in-place with 4 ms lookahead.
     * noexcept — all state is pre-allocated.
     */
    void processBlock(juce::AudioBuffer<float>& buf) noexcept;

    // ── Metering (any thread) ─────────────────────────────────────────────────

    /** Current gain reduction in dB (0 = none, negative = reducing). */
    [[nodiscard]] float getGainReductionDB() const noexcept
    {
        return gainReductionDB_.load(std::memory_order_relaxed);
    }

    /** Current output true peak in dBTP. */
    [[nodiscard]] float getTruePeak_dBTP() const noexcept
    {
        return truePeak_.getTruePeak_dBTP();
    }

    /** Returns the current lookahead latency in samples at the prepared sample rate. */
    [[nodiscard]] int getLookaheadSamples() const noexcept { return lookaheadSamples_; }

private:
    // ── Delay line (ring buffer) ──────────────────────────────────────────────
    std::array<float, kLookaheadBufSize> delayL_{}, delayR_{};
    int writePos_ = 0;
    int lookaheadSamples_ = 0;

    // B-1 FIX: per-position true-peak cache over the delay line. The true-peak
    // (max over 4 polyphase phases) is computed ONCE per written sample and
    // stored here, so the per-sample lookahead window scan is a cheap
    // max-reduction over cached values rather than a per-sample std::abs of the
    // raw delay line. This makes the dBTP ceiling hold against inter-sample
    // peaks (the original sample-peak scan let ISPs through).
    std::array<float, kLookaheadBufSize> windowPeakL_{}, windowPeakR_{};

    // ── True Peak detector (lookahead window only) ────────────────────────────
    TruePeakEstimator truePeak_;

    // ── Gain smoother ─────────────────────────────────────────────────────────
    float gainSmoothed_    = 1.0f;  // linear gain currently applied
    float releaseCoeff_    = 0.0f;  // exp(-1/(release_ms * 0.001 * sr))
    double sampleRate_     = 48000.0;

    // ── Atomic parameters ─────────────────────────────────────────────────────
    std::atomic<float> ceilingLinear_{ std::pow(10.0f, kDefaultCeilingDBTP / 20.0f) };
    std::atomic<float> releaseMs_    { kDefaultReleaseMsec };
    std::atomic<bool>  enabled_      { true };

    // ── Metering ──────────────────────────────────────────────────────────────
    std::atomic<float> gainReductionDB_{ 0.0f };

    juce::AudioBuffer<float> lookaheadBuf_;   // pre-allocated scan buffer

    void recomputeReleaseCoeff() noexcept;
};

} // namespace more_phi
