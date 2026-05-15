/*
 * More-Phi — Core/PhysicsEngine.h
 * Spring-damper elastic mode + Perlin noise drift.
 *
 * noexcept guarantee: All physics functions are noexcept because:
 * - Pure mathematical operations on primitives (no allocations)
 * - Uses static const lookup tables (no dynamic memory)
 * - All std::cmath functions are noexcept
 */
#pragma once

#include <cmath>

namespace more_phi {

enum class ElasticPreset { Slow, Medium, Heavy };
enum class DriftMode     { Free, Locked, Orbit };

struct ElasticState
{
    float x = 0.0f, y = 0.0f;    // current position
    float vx = 0.0f, vy = 0.0f;  // velocity
};

class PhysicsEngine
{
public:
    // Spring-damper: moves state toward (targetX, targetY) with overshoot
    // noexcept: Pure arithmetic on primitives, no allocations
    static void updateElastic(ElasticState& state,
                              float targetX, float targetY,
                              ElasticPreset preset, float dt) noexcept;

    // Perlin noise drift: outputs (outX, outY)
    // noexcept: Pure arithmetic, uses static lookup table
    static void updateDrift(float& outX, float& outY,
                            float time, float speed, float distance,
                            float chaos, DriftMode mode,
                            float anchorX, float anchorY, float gravity) noexcept;

    // Simple Perlin noise implementation (2D gradient noise)
    // noexcept: Pure arithmetic with static const lookup table
    static float perlin(float x, float y) noexcept;

private:
    // noexcept: Pure arithmetic
    static float perlinOctaves(float x, float y, int octaves) noexcept;
    static float fade(float t) noexcept { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
    static float lerp(float a, float b, float t) noexcept { return a + t * (b - a); }
    static float grad(int hash, float x, float y) noexcept;
};

} // namespace more_phi
