/*
 * MorphSnap — Core/InterpolationEngine.h
 * 1D linear + 2D inverse-distance weighted interpolation.
 * SIMD-optimized for real-time audio safety.
 */
#pragma once

#include "SnapshotBank.h"
#include <juce_graphics/juce_graphics.h>
#include <vector>
#include <array>
#include <cmath>

namespace morphsnap {

class InterpolationEngine
{
public:
    // Clock-layout positions for 12 snapshots
    static std::array<juce::Point<float>, 12> getClockPositions(float radius = 0.85f);

    // 1D: faderPos ∈ [0,1] → linearly segments across occupied snapshots
    static void compute1D(float faderPos,
                          const SnapshotBank& bank,
                          std::vector<float>& output);

    // 2D: cursorXY ∈ [-1,1] → inverse-distance weighted blend
    static void compute2D(float cursorX, float cursorY,
                          const SnapshotBank& bank,
                          std::vector<float>& output);

    // SIMD batch interpolation - processes 8 floats at once (AVX) or 4 (SSE)
    static void interpolateBatch_SIMD(
        const float* srcA, const float* srcB,
        float* dest, float t, size_t count);

    // Check SIMD support at runtime
    static bool hasAVXSupport();
    static bool hasSSESupport();

private:
    static constexpr float kEpsilon = 1e-6f;
    static constexpr float kIDWPower = 2.0f;

    // Scalar fallback
    static void interpolateBatch_Scalar(
        const float* srcA, const float* srcB,
        float* dest, float t, size_t count);
};

} // namespace morphsnap
