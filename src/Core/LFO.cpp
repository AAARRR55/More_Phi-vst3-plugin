/*
 * More-Phi — Core/LFO.cpp
 * LFO implementation: 6 shapes, tempo-sync, S&H and smoothed-random.
 */
#include "LFO.h"
#include <algorithm>
#include <cmath>

namespace more_phi {

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr float kMinRate   =  0.01f;
static constexpr float kMaxRate   = 50.0f;
static constexpr float kTwoPi    =  6.283185307f;

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void LFO::prepare(double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    reset();
}

void LFO::reset() noexcept
{
    phase_        = 0.0f;
    prevPhase_    = 0.0f;
    shValue_      = 0.0f;
    smoothRand_   = 0.0f;
    randTarget_   = 0.0f;
    currentValue_ = 0.0f;
    rngState_     = 0x12345678u; // reset to deterministic seed
}

// ── Parameter setters ─────────────────────────────────────────────────────────

void LFO::setRate(float hz) noexcept
{
    rate_.store(std::clamp(hz, kMinRate, kMaxRate), std::memory_order_relaxed);
}

void LFO::setPhaseOffset(float phase) noexcept
{
    // Wrap to [0, 1)
    phase = std::fmod(phase, 1.0f);
    if (phase < 0.0f) phase += 1.0f;
    phaseOffset_.store(phase, std::memory_order_relaxed);
}

void LFO::setSyncDivision(int division) noexcept
{
    // Clamp to sensible range: 1 (whole note) … 64 (64th note)
    syncDivision_.store(std::clamp(division, 1, 64), std::memory_order_relaxed);
}

// ── Effective rate resolution ─────────────────────────────────────────────────

float LFO::effectiveRate() const noexcept
{
    if (!tempoSync_.load(std::memory_order_relaxed)) return rate_.load(std::memory_order_relaxed);

    // Tempo sync: one cycle = (division / bpm) * 60 seconds
    // e.g. division=4 (quarter note), bpm=120 → rate = 120/60/4 = 0.5 Hz
    const float bpm = bpm_.load(std::memory_order_relaxed);
    const float safeBpm = bpm > 0.0f ? bpm : 120.0f;
    const int   div = syncDivision_.load(std::memory_order_relaxed);
    const float safeDivision = div > 0 ? static_cast<float>(div) : 4.0f;
    return (safeBpm / 60.0f) / safeDivision;
}

// ── PRNG ──────────────────────────────────────────────────────────────────────

float LFO::nextRandom() noexcept
{
    // xorshift32 — identical pattern to GranularMorphEngine::nextRandom()
    rngState_ ^= rngState_ << 13;
    rngState_ ^= rngState_ >> 17;
    rngState_ ^= rngState_ << 5;
    return static_cast<float>(rngState_) * 2.3283064365386963e-10f; // / 2^32
}

// ── Waveform generators ────────────────────────────────────────────────────────

// phase ∈ [0, 1) → output ∈ [-1, 1]

float LFO::sine(float phase) noexcept
{
    return std::sin(phase * kTwoPi);
}

float LFO::triangle(float phase) noexcept
{
    // Rise from -1 to +1 in first half, fall from +1 to -1 in second half
    if (phase < 0.5f)
        return phase * 4.0f - 1.0f;          // -1 → +1
    return (1.0f - phase) * 4.0f - 1.0f;     // +1 → -1
}

float LFO::saw(float phase) noexcept
{
    // Rising sawtooth: -1 at phase=0, +1 just before wrap
    return phase * 2.0f - 1.0f;
}

float LFO::square(float phase) noexcept
{
    return phase < 0.5f ? 1.0f : -1.0f;
}

// ── Process ───────────────────────────────────────────────────────────────────

float LFO::process(float dt) noexcept
{
    if (dt <= 0.0f)
    {
        currentValue_ = 0.0f;
        return 0.0f;
    }

    // Advance phase
    const float hz       = effectiveRate();
    const float increment= hz * dt;
    prevPhase_           = phase_;
    phase_              += increment;

    // Wrap phase into [0, 1)
    const bool wrapped = (phase_ >= 1.0f);
    if (wrapped)
        phase_ = std::fmod(phase_, 1.0f);

    // Apply offset for read (do not mutate phase_)
    float readPhase = phase_ + phaseOffset_.load(std::memory_order_relaxed);
    if (readPhase >= 1.0f) readPhase -= 1.0f;

    float output = 0.0f;

    switch (static_cast<LFOShape>(shape_.load(std::memory_order_relaxed)))
    {
        case LFOShape::Sine:
            output = sine(readPhase);
            break;

        case LFOShape::Triangle:
            output = triangle(readPhase);
            break;

        case LFOShape::Saw:
            output = saw(readPhase);
            break;

        case LFOShape::Square:
            output = square(readPhase);
            break;

        case LFOShape::SampleAndHold:
            // Latch a new random value each time phase wraps
            if (wrapped)
            {
                shValue_ = nextRandom() * 2.0f - 1.0f;
            }
            output = shValue_;
            break;

        case LFOShape::Random:
        {
            // Smoothed continuous random: interpolate toward a new target
            // each time the phase wraps.
            if (wrapped)
            {
                randTarget_ = nextRandom() * 2.0f - 1.0f;
            }
            // MOD-5 FIX: smooth toward randTarget_ with a rate-INDEPENDENT time
            // constant. The previous `1 - hz*dt*0.5` coupled smoothing to the LFO
            // rate (and was inverted — higher rate gave LESS smoothing) and barely
            // moved per call, leaving the output stuck near the old target. Use a
            // fixed ~50 ms tau per process() call.
            constexpr float kSmoothTauSec = 0.050f;
            const float smoothCoeff = std::exp(-dt / kSmoothTauSec);
            smoothRand_ = smoothRand_ * smoothCoeff + randTarget_ * (1.0f - smoothCoeff);
            output      = smoothRand_;
            break;
        }

        default:
            output = sine(readPhase);
            break;
    }

    output        = std::clamp(output, -1.0f, 1.0f);
    currentValue_ = output;
    return output;
}

} // namespace more_phi
