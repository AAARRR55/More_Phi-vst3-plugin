/*
 * MorphSnap — Core/StepSequencer.h
 * Clock-driven step sequencer producing a modulation signal in [-1, 1].
 *
 * noexcept guarantee: process() is noexcept because:
 * - All buffers are std::array (fixed, pre-allocated)
 * - Pure arithmetic with no allocations
 * - std::rand for Random direction, no heap interaction
 *
 * Clock model: phase_ advances by (bpm / 60.0) * beatsPerStep * dt each block.
 * When phase_ ≥ 1.0 the sequencer steps to the next cell and phase_ wraps.
 *
 * Smoothing: 0 = hard step (instant), 1 = fully smoothed (one-pole IIR toward
 * target value; coefficient baked from smoothing amount and effective step rate).
 */
#pragma once

#include <array>
#include <cmath>

namespace morphsnap {

class StepSequencer
{
public:
    static constexpr int MAX_STEPS = 32;

    // ── Direction modes ────────────────────────────────────────────────────────

    enum class Direction
    {
        Forward  = 0,
        Backward,
        PingPong,
        Random
    };

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /** Prepare for a given sample rate. Call from prepareToPlay. */
    void prepare(double sampleRate) noexcept;

    /** Reset to step 0, phase 0, and silence. */
    void reset() noexcept;

    // ── Audio-thread API ──────────────────────────────────────────────────────

    /**
     * Advance the clock by dt seconds at the given BPM, return current value [-1, 1].
     * noexcept: Pure arithmetic on fixed-size arrays, no allocations.
     */
    float process(float dt, float bpm) noexcept;

    /**
     * Return the value produced by the last process() call.
     * noexcept: Trivial float read.
     */
    float getCurrentValue() const noexcept { return smoothedValue_; }

    // ── Parameter setters (message thread) ────────────────────────────────────

    /** Number of active steps [1, MAX_STEPS]. */
    void setStepCount(int count) noexcept;

    /** Set the value of one step [-1.0, 1.0]. index must be in [0, MAX_STEPS). */
    void setStepValue(int step, float value) noexcept;

    /**
     * Duration of each step in beats (quarter notes).
     * 1.0 = one beat per step (quarter), 0.25 = 16th note, 2.0 = half note.
     */
    void setRate(float beatsPerStep) noexcept;

    /**
     * Smoothing amount [0.0, 1.0].
     * 0 = hard step transitions, 1 = maximum low-pass smoothing.
     */
    void setSmoothing(float amount) noexcept;

    /** Set traversal direction. */
    void setDirection(Direction dir) noexcept { direction_ = dir; }

    // ── Getters ───────────────────────────────────────────────────────────────

    int       getStepCount()   const noexcept { return stepCount_; }
    float     getStepValue(int step) const noexcept;
    float     getRate()        const noexcept { return beatsPerStep_; }
    float     getSmoothing()   const noexcept { return smoothing_; }
    Direction getDirection()   const noexcept { return direction_; }
    int       getCurrentStep() const noexcept { return currentStep_; }

private:
    // ── State ─────────────────────────────────────────────────────────────────

    std::array<float, MAX_STEPS> steps_{};
    int       stepCount_      = 16;
    int       currentStep_    = 0;
    float     phase_          = 0.0f;  // [0.0, 1.0) — fraction through current step
    float     beatsPerStep_   = 0.25f; // default: 16th note
    float     smoothing_      = 0.0f;
    float     smoothedValue_  = 0.0f;
    float     targetValue_    = 0.0f;
    Direction direction_      = Direction::Forward;
    bool      pingPongForward_= true;
    double    sampleRate_     = 48000.0;

    // ── Helpers ───────────────────────────────────────────────────────────────

    /** Advance currentStep_ according to the direction mode. */
    void advanceStep() noexcept;
};

} // namespace morphsnap
