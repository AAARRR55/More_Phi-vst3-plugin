/*
 * MorphSnap — Core/PhysicsEngine.h
 * Spring-damper elastic mode + Perlin noise drift.
 */
#pragma once

#include <cmath>

namespace morphsnap {

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
    static void updateElastic(ElasticState& state,
                              float targetX, float targetY,
                              ElasticPreset preset, float dt);

    // Perlin noise drift: outputs (outX, outY)
    static void updateDrift(float& outX, float& outY,
                            float time, float speed, float distance,
                            float chaos, DriftMode mode,
                            float anchorX, float anchorY, float gravity);

    // Simple Perlin noise implementation (2D gradient noise)
    static float perlin(float x, float y);

private:
    static float perlinOctaves(float x, float y, int octaves);
    static float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
    static float lerp(float a, float b, float t) { return a + t * (b - a); }
    static float grad(int hash, float x, float y);
};

} // namespace morphsnap
