/*
 * More-Phi — Core/StereoImager.h
 *
 * Frequency-dependent stereo width control with mono compatibility guard.
 *
 * Implementation: M/S processing using a 3-crossover Linkwitz-Riley bank.
 * Each frequency region has an independent width coefficient that scales
 * the Side (S) channel.  Width = 1.0 = unmodified.  Width = 0.0 = mono.
 *
 * Default width curve (per mastering blueprint):
 *   Sub  < 120 Hz  : 0.0  (force mono — prevents sub rumble issues)
 *   Low  120–800 Hz: 0.7
 *   Mid  800–8 kHz : 1.0
 *   High > 8 kHz   : 1.3
 *
 * Mono-compatibility guard (message thread, 100 ms window):
 *   If M/S correlation in Mid band < -0.3, reduce side gain in that band
 *   by 6 dB to prevent phase cancellation in mono playback.
 *
 * Thread safety:
 *   processBlock() — audio thread only, noexcept.
 *   setWidth(), setCorrelationGuardEnabled() — safe from any thread (atomic).
 *   prepare() — message thread only.
 */
#pragma once

#include <atomic>
#include <array>
#include <juce_audio_basics/juce_audio_basics.h>

namespace more_phi {

class StereoImager
{
public:
    static constexpr int kNumRegions = 4;  // Sub, Low, Mid, High
    static constexpr int kMaxChannels = 2;

    StereoImager();

    // ── Configuration ─────────────────────────────────────────────────────────

    /**
     * Set stereo width for a frequency region.
     * 0.0 = mono, 1.0 = original, 2.0 = double width.
     * Thread-safe (atomic).
     */
    void setWidth(int regionIndex, float width) noexcept;

    /** Set all four region widths at once. Thread-safe. */
    void setWidthCurve(float sub, float low, float mid, float high) noexcept;

    /** Enable / disable mono-compatibility correlation guard. Thread-safe. */
    void setCorrelationGuardEnabled(bool enabled) noexcept
    {
        correlationGuardEnabled_.store(enabled, std::memory_order_relaxed);
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /** Allocate filter state; call from prepareToPlay. */
    void prepare(double sampleRate, int maxBlockSize);

    /** Reset all filter states and correlation history. */
    void reset() noexcept;

    // ── Audio thread ──────────────────────────────────────────────────────────

    /**
     * Apply frequency-dependent M/S width to buf in-place.
     * Expects stereo (2-channel) AudioBuffer; mono input is a no-op.
     * noexcept — pure arithmetic on pre-allocated state.
     */
    void processBlock(juce::AudioBuffer<float>& buf) noexcept;

private:
    // ── Biquad (same pattern as MultibandSplitter) ────────────────────────────
    struct BiquadCoeffs { float b0{}, b1{}, b2{}, a1{}, a2{}; };
    struct BiquadState  { float z1{}, z2{}; };

    float processBiquad(float x, BiquadState& st, const BiquadCoeffs& c) noexcept
    {
        const float y = c.b0 * x + st.z1;
        st.z1 = c.b1 * x - c.a1 * y + st.z2;
        st.z2 = c.b2 * x - c.a2 * y;
        return y;
    }

    // LR4 helpers operating on M and S channels simultaneously
    float processLR4_LP(float x, int xoverIdx, int ch) noexcept;
    float processLR4_HP(float x, int xoverIdx, int ch) noexcept;

    static void computeButterworth2(float fc, double sr,
                                    BiquadCoeffs& lp, BiquadCoeffs& hp) noexcept;

    // ── Filter bank (3 crossovers → 4 regions) ────────────────────────────────
    struct Crossover
    {
        BiquadCoeffs lp, hp;
        BiquadState  lpS1[kMaxChannels]{}, lpS2[kMaxChannels]{};
        BiquadState  hpS1[kMaxChannels]{}, hpS2[kMaxChannels]{};
    };

    Crossover xo_[3]{};
    float crossFreqs_[3]{ 120.0f, 800.0f, 8000.0f };

    // Width per region (atomic for message-thread writes)
    std::atomic<float> width_[kNumRegions]{};

    // Mono-compatibility guard
    std::atomic<bool> correlationGuardEnabled_{ true };
    float corrAccumMS_ = 0.0f;   // M*S accumulator for mid band
    float corrAccumM_  = 0.0f;   // M² accumulator
    float corrAccumS_  = 0.0f;   // S² accumulator
    int   corrSamples_ = 0;
    int   corrWindowSamples_ = 4800;  // 100 ms
    std::atomic<float> midWidthGuard_{ 1.0f };  // 0.5 if guarding

    double sampleRate_{ 48000.0 };

    void computeAllCoeffs() noexcept;
    void updateCorrelationGuard() noexcept;
};

} // namespace more_phi
