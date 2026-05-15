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

namespace more_phi {

class LUFSMeter
{
public:
    static constexpr int   kMaxChannels        = 2;
    static constexpr float kAbsoluteGateLUFS   = -70.0f;   // ITU-R BS.1770-4 absolute gate
    static constexpr float kRelativeGateOffset = -10.0f;   // relative gate: ungated_LUFS - 10
    static constexpr int   kHistoryBlocks      = 600;       // 60 s at 10 blocks/s

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
     * @param channels     Array of channel pointers (L=0, R=1)
     * @param numChannels  Active channel count (clamped to kMaxChannels)
     * @param numSamples   Samples per channel in this block
     */
    void processBlock(const float* const* channels,
                      int numChannels,
                      int numSamples) noexcept;

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

    // ── 100 ms integration ────────────────────────────────────────────────────
    double sampleRate_      { 48000.0 };
    int    blockSizeSamples_{ 4800 };       // 100ms in samples
    int    blockAccum_      { 0 };          // samples accumulated
    float  blockSumSq_[kMaxChannels]{};    // mean-square accumulator per ch

    // ── Block history (circular) ──────────────────────────────────────────────
    std::array<float, kHistoryBlocks> blockLUFS_{};   // per-block loudness (LUFS)
    int historyHead_  { 0 };
    int historyCount_ { 0 };

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

    static BiquadCoeffs makeHighShelf(float fc, float dBgain, double sr) noexcept;
    static BiquadCoeffs makeHighPass2 (float fc, float Q,      double sr) noexcept;
};

} // namespace more_phi
