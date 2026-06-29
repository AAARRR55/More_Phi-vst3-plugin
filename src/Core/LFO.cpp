/*
 * More-Phi — Core/LFO.cpp
 * LFO implementation: 6 shapes, tempo-sync, S&H and smoothed-random.
 */
#include "LFO.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace more_phi {

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr float kMinRate   =  0.01f;
static constexpr float kMaxRate   = 50.0f;
static constexpr float kTwoPi    =  6.283185307f;

// ── Sine lookup table (AUDIT Q3 fix, 2026-06-27) ─────────────────────────────
// Built once (function-local static → first call, off the audio thread) so the
// per-sample sine() never calls std::sin. 2049 entries: index [0..2048] spans
// one full period with a sentinel at the end so linear interp has no wrap
// branch on the common path. Error vs std::sin ≤ ~2e-5 (2048-point LUT).
// ponytail: linear-interp LUT, not a higher-order spline — the LFO is
// sub-audible; 2e-5 is ~95 dB below full scale, inaudible. Upgrade to a
// parabolic interp if this ever drives an audible-rate oscillator.
static constexpr int kSineLUTSize = 2048;

static const float* sineLUT() noexcept
{
    static const std::array<float, kSineLUTSize + 1> table = []()
    {
        std::array<float, kSineLUTSize + 1> t{};
        for (int i = 0; i <= kSineLUTSize; ++i)
            t[static_cast<size_t>(i)] = std::sin(static_cast<float>(i)
                                                 / static_cast<float>(kSineLUTSize)
                                                 * kTwoPi);
        return t;
    }();
    return table.data();
}


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
    currentValue_.store(0.0f, std::memory_order_relaxed);
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
    // LUT-based linear interpolation (AUDIT Q3): no std::sin on the audio thread.
    const float* t = sineLUT();
    float pos = phase * static_cast<float>(kSineLUTSize);
    int   i0  = static_cast<int>(pos);
    float f   = pos - static_cast<float>(i0);
    int   i1  = i0 + 1; // [kSineLUTSize] sentinel covers the wrap
    return t[i0] + f * (t[i1] - t[i0]);
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
        currentValue_.store(0.0f, std::memory_order_relaxed);
        return 0.0f;
    }

    // Advance phase
    const float hz       = effectiveRate();
    const float increment= hz * dt;
    prevPhase_           = phase_;
    phase_              += increment;

    // FIX C11: Detect multiple wraps so S&H/Random update correctly at large block sizes.
    const int wrapCount = static_cast<int>(std::floor(phase_));
    phase_ = std::fmod(phase_, 1.0f);
    if (phase_ < 0.0f) phase_ += 1.0f;

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
            // Latch a new random value each time phase wraps (handles multiple wraps).
            if (wrapCount > 0)
            {
                shValue_ = nextRandom() * 2.0f - 1.0f;
            }
            output = shValue_;
            break;

        case LFOShape::Random:
        {
            // Smoothed continuous random: interpolate toward a new target
            // each time the phase wraps (handles multiple wraps).
            if (wrapCount > 0)
            {
                randTarget_ = nextRandom() * 2.0f - 1.0f;
            }
            // DEEP-DIVE FIX: couple the smoothing time constant to the LFO
            // period so the random waveform preserves its character across the
            // full rate range. A fixed 50 ms tau worked well at ~10 Hz but
            // smeared output into near-DC mush at 20 Hz (smoothing the whole
            // cycle) and was imperceptible at 0.1 Hz. Using 10% of the period
            // keeps the "random stair-step" shape rate-independent.
            const float period = 1.0f / (hz + 1e-6f);
            const float smoothTauSec = period * 0.1f;
            const float smoothCoeff = std::exp(-dt / smoothTauSec);
            smoothRand_ = smoothRand_ * smoothCoeff + randTarget_ * (1.0f - smoothCoeff);
            output      = smoothRand_;
            break;
        }

        default:
            output = sine(readPhase);
            break;
    }

    output        = std::clamp(output, -1.0f, 1.0f);
    currentValue_.store(output, std::memory_order_relaxed);
    return output;
}

} // namespace more_phi
