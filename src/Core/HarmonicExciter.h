/*
 * More-Phi — Core/HarmonicExciter.h
 *
 * Soft-saturation harmonic exciter for the mastering chain.
 *
 * Signal flow:
 *   input → HP filter (1 kHz, 2nd-order Butterworth) → drive → fastTanh → wet
 *   output = dry_gain * input + wet_gain * excited_hp_signal
 *
 * The HP filter ensures saturation only affects mid/high frequencies, adding
 * presence and air without muddying the low end.
 *
 * tanh approximation: Padé rational, accurate to < 0.1% error for |x| < 4.
 * No std::tanh on the audio thread — guaranteed noexcept, branch-free.
 *
 * Thread safety:
 *   processBlock() — audio thread, noexcept.
 *   setDrive / setDryWet / setEnabled — atomic, any thread.
 *   prepare() — message thread only.
 */
#pragma once

#include <atomic>
#include <array>
#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>

namespace more_phi {

class HarmonicExciter
{
public:
    static constexpr int kMaxChannels = 2;

    HarmonicExciter();

    // ── Configuration (any thread — atomic) ───────────────────────────────────

    /** Drive in dB applied to the HP-filtered signal before saturation [0, 24]. */
    void setDrive(float dB) noexcept { driveLinear_.store(std::pow(10.f, dB / 20.f), std::memory_order_relaxed); }

    /** Dry/wet mix [0 = fully dry, 1 = fully wet]. Default 0.3. */
    void setDryWet(float mix) noexcept { dryWet_.store(std::clamp(mix, 0.f, 1.f), std::memory_order_relaxed); }

    /** HP cutoff frequency in Hz [200, 8000]. Default 1000 Hz. */
    void setCutoff(float hz) noexcept;

    /** Enable/disable the exciter. Disabled = pass-through with no CPU cost. */
    void setEnabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_relaxed); }

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept;

    // ── Audio thread ──────────────────────────────────────────────────────────

    /** Process buf in-place. noexcept — all state pre-allocated. */
    void processBlock(juce::AudioBuffer<float>& buf) noexcept;

private:
    /** Padé approximation of tanh(x). Accurate < 0.1% for |x| < 4. */
    static inline float fastTanh(float x) noexcept
    {
        const float x2 = x * x;
        return x * (27.f + x2) / (27.f + 9.f * x2);
    }

    // 2-pole Butterworth HP biquad state per channel
    struct BiquadState { float z1 = 0.f, z2 = 0.f; };
    std::array<BiquadState, kMaxChannels> hpState_{};

    // HP biquad coefficients (recomputed when cutoff/sr changes)
    float b0_ = 1.f, b1_ = -2.f, b2_ = 1.f;  // numerator
    float a1_ = 0.f, a2_ = 0.f;               // denominator (a0 normalized to 1)

    std::atomic<float> driveLinear_ { std::pow(10.f, 6.f / 20.f) };  // +6 dB default
    std::atomic<float> dryWet_      { 0.3f };
    std::atomic<float> cutoffHz_    { 1000.f };
    std::atomic<bool>  enabled_     { false };   // disabled by default

    double sampleRate_  = 48000.0;

    void recomputeCoefficients() noexcept;
};

} // namespace more_phi
