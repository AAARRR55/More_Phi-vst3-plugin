/*
 * More-Phi — Core/PhysicsEngine.cpp
 */
#include "PhysicsEngine.h"
#include <cmath>
#include <algorithm>

namespace more_phi {

// ── Elastic Spring-Damper ────────────────────────────────────────────────────

void PhysicsEngine::updateElastic(ElasticState& s,
                                   float targetX, float targetY,
                                   ElasticPreset preset, float dt) noexcept
{
    // H-2 FIX: The preset constants below are the TRUE physical stiffness and
    // damping, tuned at 44100 Hz / 512-sample block (where dt == kRefDt and the
    // previous dtScale compensation was a no-op). Sample-rate independence comes
    // from the adaptive sub-stepping below: it advances the spring by the full
    // physical dt every block regardless of how that dt is sliced, so the spring
    // settles in the same wall-clock time at any sample rate / block size.
    //
    // The previous code ALSO multiplied stiffness & damping by kRefDt/dt, which
    // double-compensated: scaling k and c by s scales BOTH the natural frequency
    // and the damping ratio by sqrt(s), making the spring faster and less bouncy
    // at higher sample rates (a 96 kHz project felt different from 44.1 kHz).
    float stiffness, dampingRatio;
    switch (preset)
    {
        case ElasticPreset::Slow:   stiffness = 0.5f; dampingRatio = 0.35f; break;
        case ElasticPreset::Medium: stiffness = 2.0f; dampingRatio = 0.60f; break;
        // H-3 FIX: Heavy preset was marginally overdamped (ζ≈1.02); numerical
        // errors from the half-step damping factor could push it back into
        // underdamped ringing. Raise ζ well above 1 so the Heavy spring is
        // unambiguously overdamped and settles without oscillation.
        case ElasticPreset::Heavy:  stiffness = 8.0f; dampingRatio = 1.5f; break;
        default:                    stiffness = 2.0f; dampingRatio = 0.60f; break;
    }
    const float damping = 2.0f * dampingRatio * std::sqrt(stiffness);

    // C-D1 FIX: Adaptive sub-stepping for stability with large dt
    const float maxStableDt = 1.0f / (2.0f * std::sqrt(std::max(stiffness, 0.01f)));
    const int numSteps = std::max(1, static_cast<int>(std::ceil(dt / maxStableDt)));
    const float subDt = dt / static_cast<float>(numSteps);

    for (int step = 0; step < numSteps; ++step)
    {
        // H-3 FIX: Fully implicit velocity damping.  The previous half-step
        // factor (1 / (1 + c·dt/2)) weakens the damping enough that the Heavy
        // preset could still ring.  Treating the velocity damping term as
        // fully implicit (1 / (1 + c·dt)) guarantees the spring dissipates
        // energy monotonically on every sub-step; no energy is injected.
        const float dampingFactor = 1.0f / (1.0f + damping * subDt);
        s.vx = (s.vx + stiffness * (targetX - s.x) * subDt) * dampingFactor;
        s.vy = (s.vy + stiffness * (targetY - s.y) * subDt) * dampingFactor;
        constexpr float kMaxVelocity = 10.0f;
        s.vx = std::clamp(s.vx, -kMaxVelocity, kMaxVelocity);
        s.vy = std::clamp(s.vy, -kMaxVelocity, kMaxVelocity);
        s.x += s.vx * subDt;
        s.y += s.vy * subDt;
    }

    // Saturate velocity to prevent NaN/Inf from large dt (e.g. audio engine pause/resume)
    constexpr float kMaxVelocity = 10.0f;
    s.vx = std::clamp(s.vx, -kMaxVelocity, kMaxVelocity);
    s.vy = std::clamp(s.vy, -kMaxVelocity, kMaxVelocity);
    if (!std::isfinite(s.vx)) { s.vx = 0.0f; s.x = targetX; }
    if (!std::isfinite(s.vy)) { s.vy = 0.0f; s.y = targetY; }

    // Kill near-zero velocities so the spring converges to rest rather than
    // jittering indefinitely.  processBlock wraps the entire DSP path with
    // juce::ScopedNoDenormals, so IEEE-754 denormals are already flushed to
    // zero at the CPU level.  This threshold (1e-6) is coarser than the
    // denormal range (~1e-38) and intentionally stops perceptibly-silent motion.
    constexpr float kRestThreshold = 1e-6f;
    if (std::abs(s.vx) < kRestThreshold) s.vx = 0.0f;
    if (std::abs(s.vy) < kRestThreshold) s.vy = 0.0f;
}

// ── Perlin Noise ─────────────────────────────────────────────────────────────

// Permutation table (standard Ken Perlin)
static const int perm[512] = {
    151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,
    69,142,8,99,37,240,21,10,23,190,6,148,247,120,234,75,0,26,197,62,
    94,252,219,203,117,35,11,32,57,177,33,88,237,149,56,87,174,20,125,136,
    171,168,68,175,74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,
    60,211,133,230,220,105,92,41,55,46,245,40,244,102,143,54,65,25,63,161,
    1,216,80,73,209,76,132,187,208,89,18,169,200,196,135,130,116,188,159,86,
    164,100,109,198,173,186,3,64,52,217,226,250,124,123,5,202,38,147,118,126,
    255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,
    119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,129,22,39,253,
    19,98,108,110,79,113,224,232,178,185,112,104,218,246,97,228,251,34,242,193,
    238,210,144,12,191,179,162,241,81,51,145,235,249,14,239,107,49,192,214,31,
    181,199,106,157,184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,
    222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180,
    // repeat
    151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,
    69,142,8,99,37,240,21,10,23,190,6,148,247,120,234,75,0,26,197,62,
    94,252,219,203,117,35,11,32,57,177,33,88,237,149,56,87,174,20,125,136,
    171,168,68,175,74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,
    60,211,133,230,220,105,92,41,55,46,245,40,244,102,143,54,65,25,63,161,
    1,216,80,73,209,76,132,187,208,89,18,169,200,196,135,130,116,188,159,86,
    164,100,109,198,173,186,3,64,52,217,226,250,124,123,5,202,38,147,118,126,
    255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,
    119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,129,22,39,253,
    19,98,108,110,79,113,224,232,178,185,112,104,218,246,97,228,251,34,242,193,
    238,210,144,12,191,179,162,241,81,51,145,235,249,14,239,107,49,192,214,31,
    181,199,106,157,184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,
    222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180
};

float PhysicsEngine::grad(int hash, float x, float y) noexcept
{
    // C8 FIX: Use 8 gradient directions (classic 2D Perlin) instead of only 4
    // diagonal vectors.  The old hash & 3 produced only 4 gradients, creating
    // visible directional bias in the drift noise.
    const int h = hash & 7;
    const float u = h < 4 ? x : y;
    const float v = h < 4 ? y : x;
    return ((h & 1) ? -u : u) + ((h & 2) ? -2.0f * v : 2.0f * v);
}

float PhysicsEngine::perlin(float x, float y) noexcept
{
    // Cache floor values — each was previously computed twice (once for int cast,
    // once for fractional part). Now computed once per coordinate.
    const float wx = std::fmod(x, 256.0f);
    const float wy = std::fmod(y, 256.0f);
    const float fx = std::floor(wx);
    const float fy = std::floor(wy);
    const int xi = static_cast<int>(fx) & 255;
    const int yi = static_cast<int>(fy) & 255;
    // AUDIT-FIX: Use the wrapped coordinate (wx, wy) to compute the fractional
    // part, not the original (x, y). When x is negative, std::fmod produces a
    // negative result; using x - fx with the unwrapped x yields wildly wrong
    // fractional parts (e.g. -1000 - (-232) = -768 instead of 0).
    const float xf = wx - fx;
    const float yf = wy - fy;
    const float u = fade(xf);
    const float v = fade(yf);

    const int aa = perm[perm[xi]     + yi];
    const int ab = perm[perm[xi]     + yi + 1];
    const int ba = perm[perm[xi + 1] + yi];
    const int bb = perm[perm[xi + 1] + yi + 1];

    return lerp(lerp(grad(aa, xf,     yf),     grad(ba, xf - 1, yf),     u),
                lerp(grad(ab, xf,     yf - 1), grad(bb, xf - 1, yf - 1), u),
                v);
}

float PhysicsEngine::perlinOctaves(float x, float y, int octaves) noexcept
{
    float value = 0.0f, amplitude = 1.0f, frequency = 1.0f;
    float maxAmplitude = 0.0f;
    for (int i = 0; i < octaves; ++i)
    {
        value += perlin(x * frequency, y * frequency) * amplitude;
        maxAmplitude += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    if (maxAmplitude < 1e-6f) return 0.0f;
    return value / maxAmplitude;  // normalized to ~[-1, 1]
}

// ── Drift Mode ───────────────────────────────────────────────────────────────

void PhysicsEngine::updateDrift(float& outX, float& outY,
                                 float time, float speed, float distance,
                                 float chaos, DriftMode mode,
                                 float anchorX, float anchorY, float gravity) noexcept
{
    // M-7 FIX: Limit to 4 octaves max. Beyond 4, high-frequency octaves alias
    // at typical audio rates (the Perlin gradient becomes undersampled).
    const float safeSpeed = std::max(speed, 0.0f);
    const float safeDistance = std::max(distance, 0.0f);
    const int octaves = std::clamp(static_cast<int>(chaos * 4.0f) + 1, 1, 4);
    const float nx = perlinOctaves(time * safeSpeed, 0.5f, octaves) * safeDistance;
    const float ny = perlinOctaves(0.5f, time * safeSpeed, octaves) * safeDistance;

    switch (mode)
    {
        case DriftMode::Free:
            outX = nx;
            outY = ny;
            break;

        case DriftMode::Locked:
            outX = anchorX + nx * (1.0f - gravity);
            outY = anchorY + ny * (1.0f - gravity);
            break;

        case DriftMode::Orbit:
        {
            const float kTwoPi = 6.283185307f;
            float angle = time * safeSpeed;
            angle = std::fmod(angle, kTwoPi);
            if (angle < 0.0f) angle += kTwoPi;
            outX = anchorX + std::cos(angle) * safeDistance + nx * 0.2f;
            outY = anchorY + std::sin(angle) * safeDistance + ny * 0.2f;
            break;
        }
    }
}

} // namespace more_phi
