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
    float stiffness, damping;
    switch (preset)
    {
        case ElasticPreset::Slow:   stiffness = 0.5f; damping = 0.3f; break;
        case ElasticPreset::Medium: stiffness = 2.0f; damping = 0.7f; break;
        case ElasticPreset::Heavy:  stiffness = 8.0f; damping = 0.95f; break;
        default:                    stiffness = 2.0f; damping = 0.7f; break;
    }

    // C-D1 FIX: Adaptive sub-stepping for stability with large dt
    const float maxStableDt = 1.0f / (2.0f * std::sqrt(std::max(stiffness, 0.01f)));
    const int numSteps = std::max(1, static_cast<int>(std::ceil(dt / maxStableDt)));
    const float subDt = dt / static_cast<float>(numSteps);

    for (int step = 0; step < numSteps; ++step)
    {
        // Recompute force each sub-step based on current position (not stale)
        const float fx = stiffness * (targetX - s.x) - damping * s.vx;
        const float fy = stiffness * (targetY - s.y) - damping * s.vy;

        // Semi-implicit Euler: update velocity first, then use NEW velocity for position
        s.vx += fx * subDt;
        s.vy += fy * subDt;
        s.x  += s.vx * subDt;
        s.y  += s.vy * subDt;
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
    const int h = hash & 3;
    const float u = h < 2 ? x : y;
    const float v = h < 2 ? y : x;
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

float PhysicsEngine::perlin(float x, float y) noexcept
{
    // Cache floor values — each was previously computed twice (once for int cast,
    // once for fractional part). Now computed once per coordinate.
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const int xi = static_cast<int>(fx) & 255;
    const int yi = static_cast<int>(fy) & 255;
    const float xf = x - fx;
    const float yf = y - fy;
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
    const int octaves = std::clamp(static_cast<int>(chaos * 4.0f) + 1, 1, 4);
    const float nx = perlinOctaves(time * speed, 0.5f, octaves) * distance;
    const float ny = perlinOctaves(0.5f, time * speed, octaves) * distance;

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
            const float angle = time * speed;
            outX = anchorX + std::cos(angle) * distance + nx * 0.2f;
            outY = anchorY + std::sin(angle) * distance + ny * 0.2f;
            break;
        }
    }
}

} // namespace more_phi
