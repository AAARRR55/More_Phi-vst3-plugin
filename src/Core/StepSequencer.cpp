/*
 * MorphSnap — Core/StepSequencer.cpp
 * Clock-driven step sequencer implementation.
 *
 * Clock model:
 *   phaseIncrement = (bpm / 60.0) * beatsPerStep * dt
 *   When phase_ wraps past 1.0 → advanceStep()
 *
 * Smoothing coefficient:
 *   A linear mapping of smoothing ∈ [0, 1] to a one-pole coefficient
 *   in [0, 0.99]. At smoothing=0 the output is the raw step value;
 *   at smoothing=1 the filter decays with τ ≈ 100 ms (regardless of rate).
 */
#include "StepSequencer.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>  // std::rand, RAND_MAX

namespace morphsnap {

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr float kMinBeatsPerStep = 0.0078125f; // 1/128th note
static constexpr float kMaxBeatsPerStep = 16.0f;      // 4 bars per step

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void StepSequencer::prepare(double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    reset();
}

void StepSequencer::reset() noexcept
{
    phase_          = 0.0f;
    currentStep_    = 0;
    pingPongForward_= true;
    smoothedValue_  = 0.0f;
    targetValue_    = steps_[0];
}

// ── Parameter setters ─────────────────────────────────────────────────────────

void StepSequencer::setStepCount(int count) noexcept
{
    stepCount_   = std::clamp(count, 1, MAX_STEPS);
    currentStep_ = std::min(currentStep_, stepCount_ - 1);
}

void StepSequencer::setStepValue(int step, float value) noexcept
{
    if (step < 0 || step >= MAX_STEPS) return;
    steps_[step] = std::clamp(value, -1.0f, 1.0f);
}

void StepSequencer::setRate(float beatsPerStep) noexcept
{
    beatsPerStep_ = std::clamp(beatsPerStep, kMinBeatsPerStep, kMaxBeatsPerStep);
}

void StepSequencer::setSmoothing(float amount) noexcept
{
    smoothing_ = std::clamp(amount, 0.0f, 1.0f);
}

// ── Getters ───────────────────────────────────────────────────────────────────

float StepSequencer::getStepValue(int step) const noexcept
{
    if (step < 0 || step >= MAX_STEPS) return 0.0f;
    return steps_[step];
}

// ── Step advancement ──────────────────────────────────────────────────────────

void StepSequencer::advanceStep() noexcept
{
    switch (direction_)
    {
        case Direction::Forward:
            currentStep_ = (currentStep_ + 1) % stepCount_;
            break;

        case Direction::Backward:
            currentStep_ = (currentStep_ - 1 + stepCount_) % stepCount_;
            break;

        case Direction::PingPong:
            if (pingPongForward_)
            {
                ++currentStep_;
                if (currentStep_ >= stepCount_)
                {
                    currentStep_     = std::max(0, stepCount_ - 2);
                    pingPongForward_ = false;
                }
            }
            else
            {
                --currentStep_;
                if (currentStep_ < 0)
                {
                    currentStep_     = std::min(1, stepCount_ - 1);
                    pingPongForward_ = true;
                }
            }
            break;

        case Direction::Random:
            currentStep_ = std::rand() % stepCount_;
            break;

        default:
            currentStep_ = (currentStep_ + 1) % stepCount_;
            break;
    }
}

// ── Process ───────────────────────────────────────────────────────────────────

float StepSequencer::process(float dt, float bpm) noexcept
{
    if (dt <= 0.0f || stepCount_ <= 0)
        return smoothedValue_;

    const float safeBpm = bpm > 0.0f ? bpm : 120.0f;

    // Phase increment: beats-per-second × beats-per-step × dt-in-seconds
    // (bpm/60) = beats/second; multiply by beatsPerStep → steps/second * dt = step-fraction
    // Actually: phase advances at (beats/sec) * dt per beat per step, so:
    //   phaseInc = (bpm / 60.0) * dt / beatsPerStep
    const float phaseInc = (safeBpm / 60.0f) * dt / beatsPerStep_;
    phase_ += phaseInc;

    if (phase_ >= 1.0f)
    {
        phase_ = std::fmod(phase_, 1.0f);
        advanceStep();
    }

    // Update target from current step
    targetValue_ = steps_[currentStep_];

    // Apply smoothing: coeff in [0, 0.99] mapped from smoothing_ in [0, 1]
    if (smoothing_ <= 0.0f)
    {
        smoothedValue_ = targetValue_;
    }
    else
    {
        const float coeff = smoothing_ * 0.99f;
        smoothedValue_    = smoothedValue_ * coeff + targetValue_ * (1.0f - coeff);
    }

    return std::clamp(smoothedValue_, -1.0f, 1.0f);
}

} // namespace morphsnap
