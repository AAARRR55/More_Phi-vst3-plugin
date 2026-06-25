/*
 * More-Phi — Core/InterpolationEngine.h
 * 1D linear + 2D inverse-distance weighted interpolation.
 * SIMD-optimized for real-time audio safety.
 *
 * noexcept guarantee: All interpolation functions are noexcept because:
 * - They only perform arithmetic on pre-allocated data
 * - std::vector::operator[] and size() are noexcept
 * - std::fill on vector iterators is noexcept (no allocation)
 * - All SIMD intrinsics are noexcept
 */
#pragma once

#include "SnapshotBank.h"
#include "VoronoiMorphEngine.h"
#include <juce_graphics/juce_graphics.h>
#include <vector>
#include <array>
#include <cmath>

namespace more_phi {

class InterpolationEngine
{
public:
    // Clock-layout positions for 12 snapshots
    // W6 FIX: Returns BY VALUE. The previous const-ref return referenced a
    // function-local static (unit radius) or a thread_local static (scaled
    // radius); holding that reference across a second call silently
    // invalidated it. Returning 12 Points (96 B) by value is cheap and removes
    // the footgun. The unit-radius positions are still computed once (cached
    // in an internal static) and copied out.
    static std::array<juce::Point<float>, 12> getClockPositions(float radius = 1.0f);

    // 1D: faderPos ∈ [0,1] → linearly segments across occupied snapshots
    // noexcept: Only arithmetic on pre-allocated output vector
    static void compute1D(float faderPos,
                          const SnapshotBank& bank,
                          std::vector<float>& output) noexcept;

    // 2D: cursorXY ∈ [-1,1] → inverse-distance weighted blend
    // noexcept: Only arithmetic on pre-allocated output vector
    static void compute2D(float cursorX, float cursorY,
                          const SnapshotBank& bank,
                          std::vector<float>& output) noexcept;

    // 2D Voronoi/NNI: Natural Neighbor Interpolation via Delaunay triangulation.
    // Only the snapshots whose Voronoi cells are adjacent to the cursor contribute.
    // Falls back to compute2D (IDW) when <3 occupied slots or cursor outside hull.
    // noexcept: No allocations; pure arithmetic on pre-allocated vectors.
    static void compute2D_Voronoi(float cursorX, float cursorY,
                                  const SnapshotBank& bank,
                                  const VoronoiMorphEngine& engine,
                                  std::vector<float>& output) noexcept;

    // SIMD batch interpolation - processes 8 floats at once (AVX), 4 (SSE), or 4 (NEON)
    // noexcept: Pure pointer arithmetic, no allocations
    static void interpolateBatch_SIMD(
        const float* srcA, const float* srcB,
        float* dest, float t, size_t count) noexcept;

    // Check SIMD support at runtime
    static bool hasAVXSupport();
    static bool hasSSESupport();

    // Reports which SIMD path was compiled into this translation unit
    // (independent of runtime CPU capabilities).
    static const char* getCompiledSIMDPath() noexcept;

private:
    static constexpr float kEpsilon = 1e-6f;
    static constexpr float kEpsilonSq = kEpsilon * kEpsilon;  // AUDIT-FIX (C7): single source for IDW divide-guard
    // AUDIT-FIX (C7): removed dead `kIDWPower` constant — IDW uses a hardcoded
    // 1/dist² form (no pow()), and no code referenced this. Re-add as a real
    // knob only if product surfaces a configurable-power need.

    // Scalar fallback
    // noexcept: Pure pointer arithmetic, no allocations
    static void interpolateBatch_Scalar(
        const float* srcA, const float* srcB,
        float* dest, float t, size_t count) noexcept;
};

} // namespace more_phi
