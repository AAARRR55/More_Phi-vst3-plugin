/*
 * More-Phi — Core/EnvelopeFollower.cpp
 * One-pole RMS envelope follower implementation.
 *
 * Algorithm:
 *   rms   = sqrt(sum(x[i]^2) / N)          per-block RMS
 *   level = rms * sensitivity               apply pre-gain
 *   if level > envelope: envelope = envelope * attackCoeff  + level * (1 - attackCoeff)
 *   else:                envelope = envelope * releaseCoeff + level * (1 - releaseCoeff)
 *   output = clamp(envelope, 0, 1)
 */
#include "EnvelopeFollower.h"
#include <algorithm>
#include <cmath>

namespace more_phi {

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr float kMinAttackMs   =   0.1f;
static constexpr float kMaxAttackMs   = 500.0f;
static constexpr float kMinReleaseMs  =   1.0f;
static constexpr float kMaxReleaseMs  = 5000.0f;
static constexpr float kMinSensitivity=   0.1f;
static constexpr float kMaxSensitivity=  10.0f;

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void EnvelopeFollower::prepare(double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    recomputeCoefficients();
    reset();
}

void EnvelopeFollower::reset() noexcept
{
    envelope_ = 0.0f;
}

// ── Parameter setters ─────────────────────────────────────────────────────────

void EnvelopeFollower::setAttack(float ms) noexcept
{
    attackMs_    = std::clamp(ms, kMinAttackMs, kMaxAttackMs);
    attackCoeff_.store(computeCoeff(attackMs_), std::memory_order_relaxed);
}

void EnvelopeFollower::setRelease(float ms) noexcept
{
    releaseMs_    = std::clamp(ms, kMinReleaseMs, kMaxReleaseMs);
    releaseCoeff_.store(computeCoeff(releaseMs_), std::memory_order_relaxed);
}

void EnvelopeFollower::setSensitivity(float gain) noexcept
{
    sensitivity_.store(std::clamp(gain, kMinSensitivity, kMaxSensitivity), std::memory_order_relaxed);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

float EnvelopeFollower::computeCoeff(float ms) const noexcept
{
    // Standard one-pole time-constant formula
    // coeff → 1  gives slower tracking (longer time constant)
    // coeff → 0  gives instant response
    const double tc = static_cast<double>(ms) * 0.001 * sampleRate_;
    if (tc <= 0.0) return 0.0f;
    return static_cast<float>(std::exp(-1.0 / tc));
}

void EnvelopeFollower::recomputeCoefficients() noexcept
{
    attackCoeff_.store(computeCoeff(attackMs_), std::memory_order_relaxed);
    releaseCoeff_.store(computeCoeff(releaseMs_), std::memory_order_relaxed);
}

// ── Process ───────────────────────────────────────────────────────────────────

float EnvelopeFollower::process(const float* audioData, int numSamples) noexcept
{
    if (numSamples <= 0 || audioData == nullptr)
        return envelope_;

    // Compute per-block RMS
    float sumSq = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        const float s = audioData[i];
        sumSq += s * s;
    }
    const float rms   = std::sqrt(sumSq / static_cast<float>(numSamples));
    const float sens  = sensitivity_.load(std::memory_order_relaxed);
    const float level = std::clamp(rms * sens, 0.0f, 1.0f);

    // One-pole filter with asymmetric attack/release.
    // MOD-3 FIX: attackCoeff_/releaseCoeff_ are per-SAMPLE coefficients, but we
    // apply a single update per BLOCK. Raise the coefficient to numSamples so the
    // effective time constant matches the configured ms — previously it tracked
    // blockSize× too fast (a 10 ms attack became ~0.02 ms at 512 samples/48 kHz).
    const float attackC  = attackCoeff_.load(std::memory_order_relaxed);
    const float releaseC = releaseCoeff_.load(std::memory_order_relaxed);
    const float baseCoeff = (level > envelope_) ? attackC : releaseC;
    const float coeff = std::pow(baseCoeff, static_cast<float>(numSamples));
    envelope_ = envelope_ * coeff + level * (1.0f - coeff);
    envelope_ = std::clamp(envelope_, 0.0f, 1.0f);

    return envelope_;
}

} // namespace more_phi
