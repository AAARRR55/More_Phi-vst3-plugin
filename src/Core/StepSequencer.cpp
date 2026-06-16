/*
 * More-Phi — Core/StepSequencer.cpp
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

namespace more_phi {

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
    rngState_       = 0xDEADBEEFu; // reset to deterministic seed
}

// ── PRNG ──────────────────────────────────────────────────────────────────────

float StepSequencer::nextRandom() noexcept
{
    // xorshift32 — identical pattern to GranularMorphEngine::nextRandom()
    rngState_ ^= rngState_ << 13;
    rngState_ ^= rngState_ >> 17;
    rngState_ ^= rngState_ << 5;
    return static_cast<float>(rngState_) * 2.3283064365386963e-10f; // / 2^32
}

// ── Parameter setters ─────────────────────────────────────────────────────────

void StepSequencer::setStepCount(int count) noexcept
{
    const juce::SpinLock::ScopedLockType lock(writeLock_);
    WriteScope ws(*this);
    stepCount_ = std::clamp(count, 1, MAX_STEPS);
    // currentStep_ is audio-internal state; its bounds are handled in process()
    // (modulo/clamp), so we don't touch it here — that would race the audio thread.
}

void StepSequencer::setStepValue(int step, float value) noexcept
{
    if (step < 0 || step >= MAX_STEPS) return;
    const juce::SpinLock::ScopedLockType lock(writeLock_);
    WriteScope ws(*this);
    steps_[static_cast<size_t>(step)] = std::clamp(value, -1.0f, 1.0f);
}

void StepSequencer::setRate(float beatsPerStep) noexcept
{
    const juce::SpinLock::ScopedLockType lock(writeLock_);
    WriteScope ws(*this);
    beatsPerStep_ = std::clamp(beatsPerStep, kMinBeatsPerStep, kMaxBeatsPerStep);
}

void StepSequencer::setSmoothing(float amount) noexcept
{
    const juce::SpinLock::ScopedLockType lock(writeLock_);
    WriteScope ws(*this);
    smoothing_ = std::clamp(amount, 0.0f, 1.0f);
}

// ── Getters ───────────────────────────────────────────────────────────────────

float StepSequencer::getStepValue(int step) const noexcept
{
    if (step < 0 || step >= MAX_STEPS) return 0.0f;
    return steps_[step];
}

// ── Step advancement ──────────────────────────────────────────────────────────

void StepSequencer::advanceStep(int stepCount, Direction dir) noexcept
{
    switch (dir)
    {
        case Direction::Forward:
            currentStep_ = (currentStep_ + 1) % stepCount;
            break;

        case Direction::Backward:
            currentStep_ = (currentStep_ - 1 + stepCount) % stepCount;
            break;

        case Direction::PingPong:
            if (pingPongForward_)
            {
                ++currentStep_;
                if (currentStep_ >= stepCount)
                {
                    currentStep_     = std::max(0, stepCount - 2);
                    pingPongForward_ = false;
                }
            }
            else
            {
                --currentStep_;
                if (currentStep_ < 0)
                {
                    currentStep_     = std::min(1, stepCount - 1);
                    pingPongForward_ = true;
                }
            }
            break;

        case Direction::Random:
            currentStep_ = static_cast<int>(nextRandom() * static_cast<float>(stepCount));
            break;

        default:
            currentStep_ = (currentStep_ + 1) % stepCount;
            break;
    }
}

// ── Process ───────────────────────────────────────────────────────────────────

float StepSequencer::process(float dt, float bpm) noexcept
{
    // MOD-4: snapshot the config under the seqlock so a concurrent setter
    // (setStepValue/setStepCount/setRate/setSmoothing/setDirection from the
    // message thread) can't tear the steps_ array or the scalar config. Writers
    // are serialized by writeLock_; the reader is lock-free with bounded retry.
    std::array<float, MAX_STEPS> localSteps{};
    int       localStepCount    = 0;
    float     localBeatsPerStep = 0.25f;
    float     localSmoothing    = 0.0f;
    Direction localDir          = Direction::Forward;

    for (int attempt = 0; attempt < kMaxReadRetries; ++attempt)
    {
        const uint32_t s1 = seq_.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0u) continue;
        localSteps        = steps_;
        localStepCount    = stepCount_;
        localBeatsPerStep = beatsPerStep_;
        localSmoothing    = smoothing_;
        localDir          = direction_;
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t s2 = seq_.load(std::memory_order_acquire);
        if (s1 == s2) break;
    }

    if (dt <= 0.0f || localStepCount <= 0)
        return smoothedValue_;

    const float safeBpm = bpm > 0.0f ? bpm : 120.0f;

    // Phase increment: (bpm / 60.0) * dt / beatsPerStep
    const float phaseInc = (safeBpm / 60.0f) * dt / localBeatsPerStep;
    phase_ += phaseInc;

    if (phase_ >= 1.0f)
    {
        phase_ = std::fmod(phase_, 1.0f);
        advanceStep(localStepCount, localDir);
    }

    // Update target from current step (clamp guards against a stepCount shrink
    // landing currentStep_ out of range for one block until modulo re-aligns it).
    const int idx = std::clamp(currentStep_, 0, MAX_STEPS - 1);
    targetValue_ = localSteps[static_cast<size_t>(idx)];

    // Apply smoothing: coeff in [0, 0.99] mapped from smoothing_ in [0, 1]
    if (localSmoothing <= 0.0f)
    {
        smoothedValue_ = targetValue_;
    }
    else
    {
        const float coeff = localSmoothing * 0.99f;
        smoothedValue_    = smoothedValue_ * coeff + targetValue_ * (1.0f - coeff);
    }

    return std::clamp(smoothedValue_, -1.0f, 1.0f);
}

} // namespace more_phi
