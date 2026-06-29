/*
 * More-Phi — Core/VoronoiMorphEngine.h
 * Delaunay triangulation + barycentric interpolation on the containing triangle.
 *
 * AUDIT-FIX (C1): this header previously advertised Sibson/cotangent Natural
 * Neighbor Interpolation. The implementation in computeWeights() is actually
 * linear barycentric interpolation on the single Delaunay triangle that
 * contains the cursor (3 contributors max), NOT NNI. The cotangent() helper
 * exists but is not on the weight-computation path. This doc now matches the
 * code. True NNI (C1 continuous, local support) is a future enhancement if
 * barycentric's C0 edge kinks prove audible after the C2 hull-boundary blend
 * lands.
 *
 * Algorithm (barycentric):
 *   1. Precompute Delaunay triangulation of occupied snapshot positions
 *      (Bowyer-Watson incremental, max 12 points). In-circle predicate uses
 *      long double accumulation for robustness on the maximally cocircular
 *      clock layout.
 *   2. Per block: find the containing triangle via barycentric area tests.
 *      Cursor on a vertex → exclusive weight on that vertex.
 *      Cursor outside the convex hull → caller falls back to IDW (see C2 fix
 *      in InterpolationEngine for the boundary blend that smooths this step).
 *   3. For each of the 3 vertices v_i of the containing triangle:
 *        w_i = mass_i * (area_opposite_v_i / total_area)
 *   4. Normalize: w_i /= Σ w_j
 *
 * noexcept guarantee: All functions are noexcept. Allocation only in rebuild().
 */

#pragma once

#include "SnapshotBank.h"
#include <juce_graphics/juce_graphics.h>
#include <juce_core/juce_core.h>
#include <array>
#include <vector>
#include <cmath>

namespace more_phi {

class VoronoiMorphEngine
{
public:
    VoronoiMorphEngine() = default;

    /** Rebuild the Delaunay triangulation from the current occupied snapshot
     *  positions. Call when snapshots are captured/cleared.
     *  Must be called on the message thread (allocates).
     *  M7: assertion documents invariant that rebuild() and UI reads
     *  (getEdges/getVoronoiCells) are both message-thread only. */
    void rebuild(const std::array<juce::Point<float>, SnapshotBank::NUM_SLOTS>& positions,
                 const SnapshotBank& bank);

    /** Compute NNI weights for a cursor position.
     *  Audio-thread safe: no allocations, pure arithmetic.
     *  @param cursorX, cursorY  Cursor in [-1, 1] space
     *  @param positions         Clock positions of all 12 slots
     *  @param masses            Per-slot mass weights (from SnapshotBank)
     *  @param weights           Output weights (only occupied slots get non-zero)
     *  @param totalWeight       Output sum of weights
     */
    void computeWeights(float cursorX, float cursorY,
                        const std::array<juce::Point<float>, SnapshotBank::NUM_SLOTS>& positions,
                        const std::array<float, SnapshotBank::NUM_SLOTS>& masses,
                        std::array<float, SnapshotBank::NUM_SLOTS>& weights,
                        float& totalWeight) const noexcept;

    /** Returns true if a valid triangulation exists (≥3 occupied slots). */
    bool isValid() const noexcept { return triValid_; }

    /** For UI: get the triangulation edges (pairs of vertex indices into positions).
     *  M7: must only be called from the message thread (non-reentrant with rebuild). */
    struct Edge { int a, b; };
    const std::vector<Edge>& getEdges() const noexcept
    {
        jassert(juce::MessageManager::existsAndIsCurrentThread());
        return edges_;
    }

    /** For UI: get Voronoi cell polygons. Each polygon is a list of vertices
     *  in screen space (after transform). Only valid when isValid().
     *  M7: must only be called from the message thread (non-reentrant with rebuild). */
    struct CellPoly {
        int slotIndex;
        std::vector<juce::Point<float>> vertices; // CCW polygon
    };
    const std::vector<CellPoly>& getVoronoiCells() const noexcept
    {
        jassert(juce::MessageManager::existsAndIsCurrentThread());
        return voronoiCells_;
    }

    /** AUDIT-FIX (C2): signed perpendicular distance from the cursor to the
     *  convex hull boundary. Positive inside the hull, negative outside.
     *  Used by InterpolationEngine to crossfade barycentric (inside) and IDW
     *  (outside) weights over a narrow band, eliminating the C0 step at the hull.
     *  Audio-thread safe: no allocations, scans the precomputed hull edges.
     *  Returns +infinity when no hull exists (<3 occupied slots or invalid). */
    float signedDistanceToHull(float cursorX, float cursorY,
                               const std::array<juce::Point<float>, SnapshotBank::NUM_SLOTS>& positions) const noexcept;

private:
    struct Triangle { int a, b, c; };  // indices into positions array

    // Delaunay triangulation
    std::vector<Triangle> triangles_;
    std::vector<Edge>      edges_;
    std::vector<Edge>      hullEdges_;  // AUDIT-FIX (C2): boundary edges (shared by 1 triangle), precomputed at rebuild for signedDistanceToHull
    std::vector<CellPoly>  voronoiCells_;
    bool triValid_ = false;

    // Cached occupied slot indices
    std::array<int, SnapshotBank::NUM_SLOTS> occupiedIdx_{};
    int numOccupied_ = 0;

    // Bowyer-Watson helpers
    static float circumcircleRadiusSq(juce::Point<float> a, juce::Point<float> b,
                                       juce::Point<float> c) noexcept;
    static bool pointInCircumcircle(juce::Point<float> p,
                                     juce::Point<float> a, juce::Point<float> b,
                                     juce::Point<float> c) noexcept;
    static float cotangent(juce::Point<float> a, juce::Point<float> b,
                            juce::Point<float> c) noexcept;

    // Find which triangle contains point p (barycentric test)
    int findContainingTriangle(juce::Point<float> p,
                               const std::array<juce::Point<float>, SnapshotBank::NUM_SLOTS>& pos) const noexcept;

    // Compute Voronoi cell polygons for all occupied slots
    void computeVoronoiCells(const std::array<juce::Point<float>, SnapshotBank::NUM_SLOTS>& pos);
};

} // namespace more_phi
