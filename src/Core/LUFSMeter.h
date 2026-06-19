/*
 * More-Phi — Core/LUFSMeter.h
 *
 * ITU-R BS.1770-4 integrated loudness meter with:
 *   - K-weighting filter (high-shelf + high-pass biquad stages)
 *   - 100ms block integration
 *   - Absolute and relative gating (per BS.1770-4 §4.2)
 *   - Momentary (400ms), Short-Term (3s), Integrated, and LRA outputs
 *
 * Thread safety:
 *   processBlock() is audio-thread-only, noexcept.
 *   All result getters are atomic reads — safe from any thread.
 *   reset() must be called outside the audio callback (prepareToPlay / start).
 */
#pragma once

#include <atomic>
#include <array>
#include <cmath>
#include <limits>
#include <algorithm>
#include <vector>

namespace more_phi {

class LUFSMeter
{
public:
    static constexpr int   kMaxChannels        = 2;
    static constexpr float kAbsoluteGateLUFS   = -70.0f;   // ITU-R BS.1770-4 absolute gate
    static constexpr float kRelativeGateOffset = -10.0f;   // relative gate: ungated_LUFS - 10
    static constexpr int   kHistoryBlocks      = 18000;     // 30 min at 10 blocks/s
    // LUFS-7: the gated integrated+LRA pass is O(historyCount). Recompute every
    // commit during warmup (short sessions + tests stay exact), then throttle to
    // ~once/sec once the history is long enough for the per-commit cost to grow.
    static constexpr int   kRecomputeWarmup    = 60;        // 6s: always recompute below this
    static constexpr int   kRecomputeInterval  = 10;        // ~1s between recomputes above warmup


    LUFSMeter() = default;

    // ── Lifecycle ────────────────────────────────────────────────────────────

    /**
     * Configure meter for the given sample rate and max block size.
     * Computes K-weighting coefficients. Call from prepareToPlay.
     */
    void prepare(double sampleRate, int maxBlockSize) noexcept;

    /** Reset all filter state and accumulated history. */
    void reset() noexcept;

    // ── Audio thread ─────────────────────────────────────────────────────────

    /**
     * Process a block of interleaved channel pointers.
     * noexcept — pure arithmetic on pre-allocated state.
     *
     * Channel weights: BS.1770-4 §4 specifies loudness factors L/R/C = 1.0,
     * Ls/Rs = 1.41. The weight multiplies each channel's block mean-square
     * (an already-squared power quantity), so a 1.41-weighted surround channel
     * contributes 10*log10(1.41) ≈ +1.5 dB relative to a 1.0-weighted channel
     * of equal power. For the default stereo configuration both channels carry
     * weight 1.0, so stereo output is numerically identical to the previous
     * uniform-weighting implementation. Surround layouts (>2 ch) must call
     * setChannelWeights() to install the correct role weights; otherwise all
     * active channels are weighted 1.0.
     *
     * @param channels     Array of channel pointers (L=0, R=1)
     * @param numChannels  Active channel count (clamped to kMaxChannels)
     * @param numSamples   Samples per channel in this block
     */
    void processBlock(const float* const* channels,
                      int numChannels,
                      int numSamples) noexcept;

    /**
     * Install per-channel loudness weights (BS.1770-4 §4 channel factors).
     * Entries beyond @p count retain their current value. Call from the
     * message thread on layout change. Default is uniform 1.0 (correct for
     * stereo; surround callers pass {1.0,1.0,1.0,1.41,1.41} for 5.0).
     */
    void setChannelWeights(const float* weights, int count) noexcept
    {
        for (int i = 0; i < count && i < kMaxChannels; ++i)
            channelWeights_[static_cast<size_t>(i)] = weights[i];
    }

    // ── Result accessors (any thread) ────────────────────────────────────────

    /** 400 ms window (4 overlapping 100ms blocks). */
    [[nodiscard]] float getMomentary()  const noexcept { return momentary_.load(std::memory_order_relaxed); }

    /** 3 s window (30 overlapping 100ms blocks). */
    [[nodiscard]] float getShortTerm()  const noexcept { return shortTerm_.load(std::memory_order_relaxed); }

    /** Gated integrated loudness since last reset(). */
    [[nodiscard]] float getIntegrated() const noexcept { return integrated_.load(std::memory_order_relaxed); }

    /** Loudness Range (LRA): 95th − 10th percentile of gated block distribution. */
    [[nodiscard]] float getLRA()        const noexcept { return lra_.load(std::memory_order_relaxed); }

    // ── LUFS helper ──────────────────────────────────────────────────────────

    static float meanSquareToLUFS(float ms) noexcept
    {
        if (ms < 1e-30f) return -std::numeric_limits<float>::infinity();
        return -0.691f + 10.0f * std::log10(ms);
    }

private:
    // ── Biquad state (Direct Form II Transposed) ─────────────────────────────
    struct BiquadState { float z1 = 0.f, z2 = 0.f; };
    struct BiquadCoeffs { float b0, b1, b2, a1, a2; };

    // K-weighting: pre-filter (stage 1) + high-pass (stage 2), per channel
    BiquadState preState_[kMaxChannels]{};
    BiquadState hpState_[kMaxChannels]{};
    BiquadCoeffs pre_{};
    BiquadCoeffs hp_{};

    // BS.1770-4 per-channel loudness weights. Default 1.0 for every channel
    // (correct for stereo L/R; surround callers set Ls/Rs to 1.41 via
    // setChannelWeights()). The weight multiplies an already-squared
    // mean-square, so 1.41 contributes 10*log10(1.41) ≈ +1.5 dB per surround
    // channel. Restoring the true surround weights here was the LUFS-BUG fix
    // from the 2026-06-19 DSP re-audit — previously every channel was
    // hardcoded 1.0, under-reporting surround loudness by ~1.5 dB on the
    // surround channels.
    std::array<float, kMaxChannels> channelWeights_{ 1.0f, 1.0f };

    // ── 100 ms integration ────────────────────────────────────────────────────
    double sampleRate_      { 48000.0 };
    int    blockSizeSamples_{ 4800 };       // 100ms in samples
    int    blockAccum_      { 0 };          // samples accumulated
    float  blockSumSq_[kMaxChannels]{};    // mean-square accumulator per ch

    // ── Block history (circular) ──────────────────────────────────────────────
    std::array<float, kHistoryBlocks> blockMS_{};      // per-block K-weighted mean-square
    int historyHead_  { 0 };
    int historyCount_ { 0 };
    int commitsSinceRecompute_ { 0 };  // LUFS-7: throttle counter for integrated/LRA

    std::vector<float> gated_;
    std::vector<float> lraGated_;

    // ── Atomic outputs (written on audio thread, read anywhere) ──────────────
    std::atomic<float> momentary_  { -std::numeric_limits<float>::infinity() };
    std::atomic<float> shortTerm_  { -std::numeric_limits<float>::infinity() };
    std::atomic<float> integrated_ { -std::numeric_limits<float>::infinity() };
    std::atomic<float> lra_        { 0.0f };

    // ── Internal helpers ──────────────────────────────────────────────────────
    float processBiquad(float x, BiquadState& st, const BiquadCoeffs& c) noexcept;
    void  computeKWeightingCoeffs() noexcept;
    void  commitBlock(int numChannels) noexcept;
    void  updateLongTermMetrics() noexcept;
    float windowedMeanLUFS(int numBlocks) const noexcept;

#if defined(MORE_PHI_TEST_MODE) && MORE_PHI_TEST_MODE
public:
    // ── Test-only coefficient access ─────────────────────────────────────────
    // Exposes the computed K-weighting biquad coefficients so unit tests can
    // compare them directly against the ITU-R BS.1770-4 Annex 1 Table 1
    // normative values. Not part of the production API; gated behind
    // MORE_PHI_TEST_MODE (defined for the test target only).
    struct KWeightCoeffsView { float b0, b1, b2, a1, a2; };
    KWeightCoeffsView getPreFilterCoeffs() const noexcept { return { pre_.b0, pre_.b1, pre_.b2, pre_.a1, pre_.a2 }; }
    KWeightCoeffsView getRLBCoeffs()      const noexcept { return { hp_.b0,  hp_.b1,  hp_.b2,  hp_.a1,  hp_.a2  }; }
#endif
};

} // namespace more_phi
