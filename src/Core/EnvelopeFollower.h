/*
 * More-Phi — Core/EnvelopeFollower.h
 * RMS-based envelope follower with separate attack/release coefficients.
 *
 * noexcept guarantee: process() is noexcept because:
 * - One-pole IIR filter on pre-allocated primitive state (no allocations)
 * - std::sqrt and std::exp are noexcept
 * - Buffers are raw pointer arithmetic (no bounds checks that throw)
 *
 * Output range: [0.0, 1.0] (rectified, not bipolar).
 */
#pragma once

#include <atomic>
#include <cmath>

namespace more_phi {

class EnvelopeFollower
{
public:
    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /** Set sample rate and block size, recompute filter coefficients. Call from prepareToPlay. */
    void prepare(double sampleRate, int blockSize) noexcept;

    /** Set sample rate and recompute filter coefficients. Call from prepareToPlay.
        Prefer prepare(double, int) so per-block coefficients can be pre-computed. */
    void prepare(double sampleRate) noexcept;

    /** Reset envelope state to silence. */
    void reset() noexcept;

    // ── Audio-thread API ──────────────────────────────────────────────────────

    /**
     * Process a block of audio samples and return the current envelope in [0, 1].
     * audioData may be nullptr only if numSamples == 0.
     * noexcept: Pure pointer arithmetic and one-pole filtering, no allocations.
     */
    float process(const float* audioData, int numSamples) noexcept;

    /**
     * Return the envelope value computed in the last process() call.
     * Safe from any thread after process() has returned.
     * noexcept: Trivial atomic float read.
     */
    float getCurrentValue() const noexcept { return envelope_.load(std::memory_order_relaxed); }

    // ── Parameter setters (message thread) ────────────────────────────────────

    /** Attack time in milliseconds [0.1, 500]. Recomputes coefficient immediately. */
    void setAttack(float ms) noexcept;

    /** Release time in milliseconds [1, 5000]. Recomputes coefficient immediately. */
    void setRelease(float ms) noexcept;

    /**
     * Pre-gain applied to the signal before envelope detection [0.1, 10.0].
     * Higher values make the follower respond to quieter signals.
     */
    void setSensitivity(float gain) noexcept;

    // ── Getters ───────────────────────────────────────────────────────────────

    float getAttack()      const noexcept { return attackMs_; }
    float getRelease()     const noexcept { return releaseMs_; }
    float getSensitivity() const noexcept { return sensitivity_.load(std::memory_order_relaxed); }

private:
    // ── State ─────────────────────────────────────────────────────────────────

    std::atomic<float> envelope_{ 0.0f };  // audio writes, UI reads
    double sampleRate_    = 48000.0;

    // MOD-4: coefficients/sensitivity are written from the message thread
    // (setAttack/setRelease/setSensitivity) and read on the audio thread
    // (process) — atomics prevent torn reads.
    std::atomic<float> attackCoeff_  { 0.0f };
    std::atomic<float> releaseCoeff_ { 0.0f };
    std::atomic<float> sensitivity_  { 1.0f };

    // C15 FIX: pre-computed per-block coefficients so process() never calls
    // std::pow on the audio thread. Updated whenever attack/release or block
    // size changes (always from the message thread). Atomics prevent torn reads
    // on the audio thread.
    std::atomic<int>   blockSize_{ 0 };
    std::atomic<float> attackCoeffPerBlock_  { 0.0f };
    std::atomic<float> releaseCoeffPerBlock_ { 0.0f };

    // Shadow values used to recompute coefficients when sample rate is set
    float  attackMs_  = 10.0f;
    float  releaseMs_ = 100.0f;

    // ── Helpers ───────────────────────────────────────────────────────────────

    /** exp(-1 / (timeMs * 0.001 * sampleRate)) — standard one-pole coefficient. */
    float computeCoeff(float ms) const noexcept;

    void recomputeCoefficients() noexcept;
};

} // namespace more_phi
