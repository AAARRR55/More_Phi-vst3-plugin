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
    prepare(sampleRate, 0);
}

void EnvelopeFollower::prepare(double sampleRate, int blockSize) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    blockSize_.store(blockSize, std::memory_order_relaxed);
    recomputeCoefficients();
    reset();
}

void EnvelopeFollower::reset() noexcept
{
    envelope_.store(0.0f, std::memory_order_relaxed);
}

// ── Parameter setters ─────────────────────────────────────────────────────────

void EnvelopeFollower::setAttack(float ms) noexcept
{
    attackMs_ = std::clamp(ms, kMinAttackMs, kMaxAttackMs);
    const float c = computeCoeff(attackMs_);
    attackCoeff_.store(c, std::memory_order_relaxed);
    logAttackCoeff_.store(c > 0.0f ? std::log(c) : -100.0f, std::memory_order_relaxed);
    const int bs = blockSize_.load(std::memory_order_relaxed);
    if (bs > 0)
        attackCoeffPerBlock_.store(std::pow(c, static_cast<float>(bs)), std::memory_order_relaxed);
}

void EnvelopeFollower::setRelease(float ms) noexcept
{
    releaseMs_ = std::clamp(ms, kMinReleaseMs, kMaxReleaseMs);
    const float c = computeCoeff(releaseMs_);
    releaseCoeff_.store(c, std::memory_order_relaxed);
    logReleaseCoeff_.store(c > 0.0f ? std::log(c) : -100.0f, std::memory_order_relaxed);
    const int bs = blockSize_.load(std::memory_order_relaxed);
    if (bs > 0)
        releaseCoeffPerBlock_.store(std::pow(c, static_cast<float>(bs)), std::memory_order_relaxed);
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
    const float attackC  = computeCoeff(attackMs_);
    const float releaseC = computeCoeff(releaseMs_);
    attackCoeff_.store(attackC, std::memory_order_relaxed);
    releaseCoeff_.store(releaseC, std::memory_order_relaxed);
    logAttackCoeff_.store(attackC > 0.0f ? std::log(attackC) : -100.0f, std::memory_order_relaxed);
    logReleaseCoeff_.store(releaseC > 0.0f ? std::log(releaseC) : -100.0f, std::memory_order_relaxed);
    if (blockSize_ > 0)
    {
        attackCoeffPerBlock_.store(std::pow(attackC,  static_cast<float>(blockSize_)), std::memory_order_relaxed);
        releaseCoeffPerBlock_.store(std::pow(releaseC, static_cast<float>(blockSize_)), std::memory_order_relaxed);
    }
}

// ── Process ───────────────────────────────────────────────────────────────────

float EnvelopeFollower::process(const float* audioData, int numSamples) noexcept
{
    float env = envelope_.load(std::memory_order_relaxed);
    if (numSamples <= 0 || audioData == nullptr)
        return env;

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
    // C15 FIX: avoid std::pow on the audio thread. Per-block coefficients are
    // pre-computed in prepare()/setAttack()/setRelease(). If numSamples differs
    // from the prepared block size, fall back to exp/log (still faster than pow).
    float coeff;
    if (numSamples == blockSize_ && blockSize_ > 0)
    {
        coeff = (level > env) ? attackCoeffPerBlock_.load(std::memory_order_relaxed)
                              : releaseCoeffPerBlock_.load(std::memory_order_relaxed);
    }
    else
    {
        const float logBase = (level > env) ? logAttackCoeff_.load(std::memory_order_relaxed)
                                            : logReleaseCoeff_.load(std::memory_order_relaxed);
        coeff = std::exp(static_cast<float>(numSamples) * logBase);
    }
    env = env * coeff + level * (1.0f - coeff);
    env = std::clamp(env, 0.0f, 1.0f);

    envelope_.store(env, std::memory_order_relaxed);
    return env;
}

} // namespace more_phi
