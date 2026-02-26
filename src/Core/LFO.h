/*
 * MorphSnap — Core/LFO.h
 * Low-frequency oscillator with 6 waveform shapes.
 *
 * noexcept guarantee: All process/math methods are noexcept because:
 * - Pure arithmetic on primitive types (no allocations)
 * - std::fmod, std::sin, std::fabs are noexcept
 * - S&H state is a plain float updated inline
 *
 * Phase convention: phase_ ∈ [0.0, 1.0) (wraps at 1.0, not 2π).
 * All waveform output is in [-1.0, 1.0].
 */
#pragma once

#include "ModulationTypes.h"
#include <cmath>

namespace morphsnap {

class LFO
{
public:
    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /** Initialise for a given sample rate. Call from prepareToPlay. */
    void prepare(double sampleRate) noexcept;

    /** Reset phase and S&H state to defaults. */
    void reset() noexcept;

    // ── Audio-thread API ──────────────────────────────────────────────────────

    /**
     * Advance the LFO by dt seconds and return the new value in [-1, 1].
     * dt = blockSize / sampleRate.
     * noexcept: Pure arithmetic, no allocations.
     */
    float process(float dt) noexcept;

    /**
     * Return the value computed in the last process() call without advancing.
     * Safe to call from any thread after process() has returned.
     * noexcept: Trivial float read.
     */
    float getCurrentValue() const noexcept { return currentValue_; }

    // ── Parameter setters (message thread) ────────────────────────────────────

    void setShape(LFOShape shape) noexcept    { shape_ = shape; }
    void setRate(float hz) noexcept;          // 0.01 – 50 Hz
    void setPhaseOffset(float phase) noexcept; // 0.0 – 1.0
    void setTempoSync(bool synced) noexcept   { tempoSync_ = synced; }
    void setBPM(float bpm) noexcept           { bpm_ = bpm; }
    void setSyncDivision(int division) noexcept; // 1=whole, 2=half, 4=quarter …

    // ── Getters ───────────────────────────────────────────────────────────────

    LFOShape getShape()      const noexcept { return shape_; }
    float    getRate()       const noexcept { return rate_; }
    float    getPhaseOffset()const noexcept { return phaseOffset_; }
    bool     getTempoSync()  const noexcept { return tempoSync_; }
    float    getBPM()        const noexcept { return bpm_; }
    int      getSyncDivision()const noexcept{ return syncDivision_; }

private:
    // ── State ─────────────────────────────────────────────────────────────────

    float       phase_       = 0.0f;
    float       rate_        = 1.0f;       // Hz (after tempo-sync resolution)
    float       phaseOffset_ = 0.0f;       // 0.0 – 1.0
    LFOShape    shape_       = LFOShape::Sine;
    double      sampleRate_  = 48000.0;
    bool        tempoSync_   = false;
    float       bpm_         = 120.0f;
    int         syncDivision_= 4;          // quarter note by default

    // S&H / Random state
    float       shValue_     = 0.0f;       // current latched value (S&H)
    float       prevPhase_   = 0.0f;       // detect wrap for S&H trigger
    float       smoothRand_  = 0.0f;       // smoothed random target (Random shape)
    float       randTarget_  = 0.0f;       // next random target

    float       currentValue_= 0.0f;       // last computed output

    // ── Waveform generators (pure math, noexcept) ─────────────────────────────

    static float sine(float phase) noexcept;
    static float triangle(float phase) noexcept;
    static float saw(float phase) noexcept;
    static float square(float phase) noexcept;

    /** Effective rate in Hz (resolves tempo-sync if enabled). */
    float effectiveRate() const noexcept;
};

} // namespace morphsnap
