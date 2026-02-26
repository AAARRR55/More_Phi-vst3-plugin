/*
 * MorphSnap — Core/EnvelopeFollower.h
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

#include <cmath>

namespace morphsnap {

class EnvelopeFollower
{
public:
    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /** Set sample rate and recompute filter coefficients. Call from prepareToPlay. */
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
     * noexcept: Trivial float read.
     */
    float getCurrentValue() const noexcept { return envelope_; }

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
    float getSensitivity() const noexcept { return sensitivity_; }

private:
    // ── State ─────────────────────────────────────────────────────────────────

    float  envelope_      = 0.0f;
    float  attackCoeff_   = 0.0f;
    float  releaseCoeff_  = 0.0f;
    float  sensitivity_   = 1.0f;
    double sampleRate_    = 48000.0;

    // Shadow values used to recompute coefficients when sample rate is set
    float  attackMs_  = 10.0f;
    float  releaseMs_ = 100.0f;

    // ── Helpers ───────────────────────────────────────────────────────────────

    /** exp(-1 / (timeMs * 0.001 * sampleRate)) — standard one-pole coefficient. */
    float computeCoeff(float ms) const noexcept;

    void recomputeCoefficients() noexcept;
};

} // namespace morphsnap
