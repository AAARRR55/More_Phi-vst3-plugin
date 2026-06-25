/*
 * More-Phi — Core/VoronoiMorphEngine.cpp
 * Delaunay triangulation (Bowyer-Watson) + Natural Neighbor weights (cotangent).
 */

#include "VoronoiMorphEngine.h"
#include <algorithm>
#include <limits>

namespace more_phi {

// ── Geometry helpers ──────────────────────────────────────────────────────────

static float cross2D(juce::Point<float> a, juce::Point<float> b) noexcept
{
    return a.x * b.y - a.y * b.x;
}

static float distSq(juce::Point<float> a, juce::Point<float> b) noexcept
{
    float dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

// ── Circumcircle test ────────────────────────────────────────────────────────

float VoronoiMorphEngine::circumcircleRadiusSq(
    juce::Point<float> a, juce::Point<float> b, juce::Point<float> c) noexcept
{
    // Compute circumcenter via perpendicular bisector intersection,
    // then radius². For the Delaunay condition we only need the
    // in-circle test, but the radius is useful for diagnostics.
    float d = 2.0f * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
    if (std::abs(d) < 1e-12f) return std::numeric_limits<float>::max(); // collinear

    float ax = a.x * a.x + a.y * a.y;
    float bx = b.x * b.x + b.y * b.y;
    float cx = c.x * c.x + c.y * c.y;

    float ux = (ax * (b.y - c.y) + bx * (c.y - a.y) + cx * (a.y - b.y)) / d;
    float uy = (ax * (c.x - b.x) + bx * (a.x - c.x) + cx * (b.x - a.x)) / d;

    float dx = ux - a.x, dy = uy - a.y;
    return dx * dx + dy * dy;
}

bool VoronoiMorphEngine::pointInCircumcircle(
    juce::Point<float> p,
    juce::Point<float> a, juce::Point<float> b, juce::Point<float> c) noexcept
{
    // In-circle test using the determinant method.
    // Returns true if p is inside the circumcircle of triangle (a, b, c).

    float ax = a.x - p.x, ay = a.y - p.y;
    float bx = b.x - p.x, by = b.y - p.y;
    float cx = c.x - p.x, cy = c.y - p.y;

    float det = (ax * ax + ay * ay) * (bx * cy - by * cx)
              - (bx * bx + by * by) * (ax * cy - ay * cx)
              + (cx * cx + cy * cy) * (ax * by - ay * bx);

    // The sign of det relative to the triangle's orientation determines
    // whether p is inside. Compute triangle orientation:
    float signedArea = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::abs(signedArea) < 1e-12f) return false; // degenerate

    // Inside when det and signedArea have the same sign
    return (det > 1e-9f && signedArea > 0.0f) || (det < -1e-9f && signedArea < 0.0f);
}

// ── Cotangent of angle at vertex 'a' in triangle (a,b,c) ─────────────────────

float VoronoiMorphEngine::cotangent(
    juce::Point<float> a, juce::Point<float> b, juce::Point<float> c) noexcept
{
    // cot(A) = (b² + c² - a²) / (4 * area)
    // where A is angle at vertex 'a', side a is opposite 'a' (edge bc),
    // side b is opposite 'b' (edge ac), side c is opposite 'c' (edge ab).
    // Wait — re-derive with consistent notation:
    // We want cot(angle at vertex 'a'). Let:
    //   u = b - a   (vector from a to b)
    //   v = c - a   (vector from a to c)
    // cos(θ) = (u·v) / (|u||v|)
    // sin(θ) = |u×v| / (|u||v|)
    // cot(θ) = cos/sin = (u·v) / |u×v|

    float ux = b.x - a.x, uy = b.y - a.y;
    float vx = c.x - a.x, vy = c.y - a.y;

    float dot = ux * vx + uy * vy;
    float cross = ux * vy - uy * vx;  // |u×v|

    if (std::abs(cross) < 1e-12f) return 0.0f;  // degenerate
    return dot / cross;
}

// ── Barycentric test: which triangle contains p? ─────────────────────────────

int VoronoiMorphEngine::findContainingTriangle(
    juce::Point<float> p,
    const std::array<juce::Point<float>, SnapshotBank::NUM_SLOTS>& pos) const noexcept
{
    for (size_t ti = 0; ti < triangles_.size(); ++ti)
    {
        const auto& t = triangles_[ti];
        // t.a/b/c are local indices into occupiedIdx_; convert to global slot indices
        int ga = occupiedIdx_[static_cast<size_t>(t.a)];
        int gb = occupiedIdx_[static_cast<size_t>(t.b)];
        int gc = occupiedIdx_[static_cast<size_t>(t.c)];
        juce::Point<float> v0 = pos[static_cast<size_t>(ga)];
        juce::Point<float> v1 = pos[static_cast<size_t>(gb)];
        juce::Point<float> v2 = pos[static_cast<size_t>(gc)];

        // Barycentric: compute areas
        float areaTotal = cross2D({v1.x - v0.x, v1.y - v0.y},
                                   {v2.x - v0.x, v2.y - v0.y});
        if (std::abs(areaTotal) < 1e-12f) continue;

        float area0 = cross2D({v1.x - p.x, v1.y - p.y},
                               {v2.x - p.x, v2.y - p.y});
        float area1 = cross2D({v2.x - p.x, v2.y - p.y},
                               {v0.x - p.x, v0.y - p.y});
        float area2 = cross2D({v0.x - p.x, v0.y - p.y},
                               {v1.x - p.x, v1.y - p.y});

        // All areas must have same sign as total
        if (areaTotal > 0)
        {
            if (area0 >= -1e-9f && area1 >= -1e-9f && area2 >= -1e-9f)
                return static_cast<int>(ti);
        }
        else
        {
            if (area0 <= 1e-9f && area1 <= 1e-9f && area2 <= 1e-9f)
                return static_cast<int>(ti);
        }
    }
    return -1; // outside convex hull
}

// ── Delaunay triangulation (Bowyer-Watson) ────────────────────────────────────

void VoronoiMorphEngine::rebuild(
    const std::array<juce::Point<float>, SnapshotBank::NUM_SLOTS>& positions,
    const SnapshotBank& bank)
{
    // M7: document invariant — rebuild must run on message thread, same as
    // getEdges()/getVoronoiCells().
    jassert(juce::MessageManager::isThisTheMessageThread());

    triangles_.clear();
    edges_.clear();
    voronoiCells_.clear();
    triValid_ = false;

    // Collect occupied slot indices
    numOccupied_ = 0;
    for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
    {
        if (bank.isOccupied(i))
            occupiedIdx_[static_cast<size_t>(numOccupied_++)] = i;
    }

    if (numOccupied_ < 3)
    {
        // Not enough points for triangulation; caller should fall back to IDW
        return;
    }

    // Build point list from occupied slots only
    struct Point { float x, y; int origIdx; };
    std::vector<Point> pts(static_cast<size_t>(numOccupied_));
    for (int i = 0; i < numOccupied_; ++i)
    {
        int slot = occupiedIdx_[static_cast<size_t>(i)];
        pts[static_cast<size_t>(i)] = { positions[static_cast<size_t>(slot)].x,
                                        positions[static_cast<size_t>(slot)].y, slot };
    }

    // Super-triangle large enough to contain all points
    // (All points are on unit circle, so radius=2.0 is sufficient)
    constexpr float kSuperR = 3.0f;
    // Virtual super-triangle vertices (not in point list)
    const int superA = numOccupied_;       // virtual index
    const int superB = numOccupied_ + 1;
    const int superC = numOccupied_ + 2;
    juce::Point<float> sA{ 0.0f,  kSuperR };
    juce::Point<float> sB{ -kSuperR * 0.866f, -kSuperR * 0.5f };
    juce::Point<float> sC{  kSuperR * 0.866f, -kSuperR * 0.5f };

    // Helper to get position by index (real or super)
    auto getPos = [&](int idx) -> juce::Point<float> {
        if (idx < numOccupied_)
            return { pts[static_cast<size_t>(idx)].x, pts[static_cast<size_t>(idx)].y };
        if (idx == superA) return sA;
        if (idx == superB) return sB;
        return sC;
    };

    // Start with super-triangle
    triangles_.push_back({ superA, superB, superC });

    // Insert each point incrementally
    for (int pi = 0; pi < numOccupied_; ++pi)
    {
        juce::Point<float> p = { pts[static_cast<size_t>(pi)].x,
                                  pts[static_cast<size_t>(pi)].y };

        // Find all triangles whose circumcircle contains p
        std::vector<int> badTriangles;  // indices into triangles_
        for (size_t ti = 0; ti < triangles_.size(); ++ti)
        {
            const auto& t = triangles_[ti];
            if (pointInCircumcircle(p, getPos(t.a), getPos(t.b), getPos(t.c)))
                badTriangles.push_back(static_cast<int>(ti));
        }

        if (badTriangles.empty())
        {
            // Point is outside all circumcircles — this shouldn't happen
            // with the super-triangle containing all points. If it does,
            // skip this point (degenerate).
            continue;
        }

        // Collect boundary edges of the polygonal hole
        // Each interior edge appears in exactly 2 bad triangles;
        // boundary edges appear in exactly 1.
        struct EdgeKey { int lo, hi; };
        std::vector<EdgeKey> boundary;

        for (int bti : badTriangles)
        {
            const auto& t = triangles_[static_cast<size_t>(bti)];
            int verts[3] = { t.a, t.b, t.c };
            for (int e = 0; e < 3; ++e)
            {
                int v0 = verts[e], v1 = verts[(e + 1) % 3];
                if (v0 > v1) std::swap(v0, v1);
                EdgeKey ek{ v0, v1 };

                // Check if this edge appears in another bad triangle
                bool shared = false;
                for (int bti2 : badTriangles)
                {
                    if (bti2 == bti) continue;
                    const auto& t2 = triangles_[static_cast<size_t>(bti2)];
                    int v2[3] = { t2.a, t2.b, t2.c };
                    for (int e2 = 0; e2 < 3; ++e2)
                    {
                        int w0 = v2[e2], w1 = v2[(e2 + 1) % 3];
                        if (w0 > w1) std::swap(w0, w1);
                        if (ek.lo == w0 && ek.hi == w1)
                        {
                            shared = true;
                            break;
                        }
                    }
                    if (shared) break;
                }
                if (!shared)
                    boundary.push_back(ek);
            }
        }

        // Remove bad triangles (in reverse order to preserve indices)
        std::sort(badTriangles.begin(), badTriangles.end(), std::greater<int>());
        for (int bti : badTriangles)
            triangles_.erase(triangles_.begin() + static_cast<size_t>(bti));

        // Add new triangles connecting p to each boundary edge
        for (const auto& ek : boundary)
            triangles_.push_back({ ek.lo, ek.hi, pi });
    }

    // Remove triangles that use super-triangle vertices
    triangles_.erase(
        std::remove_if(triangles_.begin(), triangles_.end(),
            [this](const Triangle& t) {
                return t.a >= numOccupied_ || t.b >= numOccupied_ || t.c >= numOccupied_;
            }),
        triangles_.end());

    if (triangles_.empty())
    {
        // Degenerate case: all points collinear (shouldn't happen with clock layout
        // unless only 2 points on the same axis). Fall back to IDW.
        return;
    }

    // Build edge list (unique edges from all triangles)
    edges_.clear();
    for (const auto& t : triangles_)
    {
        auto addEdge = [this](int a, int b) {
            int lo = occupiedIdx_[static_cast<size_t>(std::min(a, b))];
            int hi = occupiedIdx_[static_cast<size_t>(std::max(a, b))];
            // Check if already present
            for (const auto& e : edges_)
                if ((e.a == lo && e.b == hi) || (e.a == hi && e.b == lo))
                    return;
            edges_.push_back({ lo, hi });
        };
        addEdge(t.a, t.b);
        addEdge(t.b, t.c);
        addEdge(t.c, t.a);
    }

    // Compute Voronoi cell polygons for UI
    computeVoronoiCells(positions);

    triValid_ = true;
}

// ── Voronoi cell computation (half-plane intersection per slot) ──────────────

void VoronoiMorphEngine::computeVoronoiCells(
    const std::array<juce::Point<float>, SnapshotBank::NUM_SLOTS>& positions)
{
    voronoiCells_.clear();
    if (numOccupied_ < 2) return;

    // For each occupied slot, find all edges in the triangulation incident to it,
    // then compute the Voronoi cell as the intersection of half-planes defined
    // by perpendicular bisectors of those edges. Clip to bounding box.

    constexpr float kClipSize = 2.5f; // clip region: [-kClipSize, +kClipSize]²

    for (int oi = 0; oi < numOccupied_; ++oi)
    {
        int slot = occupiedIdx_[static_cast<size_t>(oi)];
        juce::Point<float> site = positions[static_cast<size_t>(slot)];

        // Start with bounding box polygon
        std::vector<juce::Point<float>> poly = {
            { -kClipSize, -kClipSize },
            {  kClipSize, -kClipSize },
            {  kClipSize,  kClipSize },
            { -kClipSize,  kClipSize }
        };

        // For each other occupied slot, clip by the half-plane closer to this site
        for (int oj = 0; oj < numOccupied_; ++oj)
        {
            if (oi == oj) continue;
            int otherSlot = occupiedIdx_[static_cast<size_t>(oj)];
            juce::Point<float> other = positions[static_cast<size_t>(otherSlot)];

            // Perpendicular bisector: line through midpoint, normal = other - site
            float midX = (site.x + other.x) * 0.5f;
            float midY = (site.y + other.y) * 0.5f;
            float nx = other.x - site.x;  // points toward other
            float ny = other.y - site.y;
            // Half-plane: dot(p - mid, n) <= 0  (closer to site than to other)
            // But we clip to dot(p - mid, n) <= 0

            if (poly.empty()) break;

            std::vector<juce::Point<float>> clipped;
            for (size_t vi = 0; vi < poly.size(); ++vi)
            {
                juce::Point<float> cur = poly[vi];
                juce::Point<float> next = poly[(vi + 1) % poly.size()];

                float dCur = (cur.x - midX) * nx + (cur.y - midY) * ny;
                float dNext = (next.x - midX) * nx + (next.y - midY) * ny;

                bool curInside = (dCur <= 1e-9f);
                bool nextInside = (dNext <= 1e-9f);

                if (curInside)
                    clipped.push_back(cur);

                if (curInside != nextInside)
                {
                    // Edge crosses the bisector — add intersection
                    float t = dCur / (dCur - dNext);
                    juce::Point<float> inter{
                        cur.x + t * (next.x - cur.x),
                        cur.y + t * (next.y - cur.y)
                    };
                    clipped.push_back(inter);
                }
            }
            poly = std::move(clipped);
        }

        if (!poly.empty())
        {
            CellPoly cp;
            cp.slotIndex = slot;
            cp.vertices = std::move(poly);
            voronoiCells_.push_back(std::move(cp));
        }
    }
}

// ── NNI Weight Computation (barycentric within Delaunay triangle) ────────────

void VoronoiMorphEngine::computeWeights(
    float cursorX, float cursorY,
    const std::array<juce::Point<float>, SnapshotBank::NUM_SLOTS>& positions,
    const std::array<float, SnapshotBank::NUM_SLOTS>& masses,
    std::array<float, SnapshotBank::NUM_SLOTS>& weights,
    float& totalWeight) const noexcept
{
    std::fill(weights.begin(), weights.end(), 0.0f);
    totalWeight = 0.0f;

    if (!triValid_ || numOccupied_ < 3)
        return;

    juce::Point<float> cursor{ cursorX, cursorY };

    // Find the triangle containing the cursor
    int triIdx = findContainingTriangle(cursor, positions);
    if (triIdx < 0)
        return; // cursor outside convex hull — caller falls back to IDW

    const auto& ct = triangles_[static_cast<size_t>(triIdx)];

    // Get the three vertices of the containing triangle
    int slot0 = occupiedIdx_[static_cast<size_t>(ct.a)];
    int slot1 = occupiedIdx_[static_cast<size_t>(ct.b)];
    int slot2 = occupiedIdx_[static_cast<size_t>(ct.c)];

    juce::Point<float> v0 = positions[static_cast<size_t>(slot0)];
    juce::Point<float> v1 = positions[static_cast<size_t>(slot1)];
    juce::Point<float> v2 = positions[static_cast<size_t>(slot2)];

    // Check if cursor is directly on a vertex
    constexpr float kEpsilonSq = 1e-10f;
    float d0 = distSq(cursor, v0);
    float d1 = distSq(cursor, v1);
    float d2 = distSq(cursor, v2);

    if (d0 < kEpsilonSq)
    {
        weights[static_cast<size_t>(slot0)] = 1.0f;
        totalWeight = 1.0f;
        return;
    }
    if (d1 < kEpsilonSq)
    {
        weights[static_cast<size_t>(slot1)] = 1.0f;
        totalWeight = 1.0f;
        return;
    }
    if (d2 < kEpsilonSq)
    {
        weights[static_cast<size_t>(slot2)] = 1.0f;
        totalWeight = 1.0f;
        return;
    }

    // Barycentric coordinates: area of sub-triangles opposite each vertex
    // area_i = area of triangle formed by cursor + the two vertices NOT i
    float area0 = std::abs(cross2D({v1.x - cursor.x, v1.y - cursor.y},
                                    {v2.x - cursor.x, v2.y - cursor.y}));
    float area1 = std::abs(cross2D({v2.x - cursor.x, v2.y - cursor.y},
                                    {v0.x - cursor.x, v0.y - cursor.y}));
    float area2 = std::abs(cross2D({v0.x - cursor.x, v0.y - cursor.y},
                                    {v1.x - cursor.x, v1.y - cursor.y}));

    float totalArea = area0 + area1 + area2;
    if (totalArea < 1e-12f)
        return;

    // Barycentric weight * mass for each vertex
    weights[static_cast<size_t>(slot0)] = (area0 / totalArea) * masses[static_cast<size_t>(slot0)];
    weights[static_cast<size_t>(slot1)] = (area1 / totalArea) * masses[static_cast<size_t>(slot1)];
    weights[static_cast<size_t>(slot2)] = (area2 / totalArea) * masses[static_cast<size_t>(slot2)];

    totalWeight = weights[static_cast<size_t>(slot0)]
                + weights[static_cast<size_t>(slot1)]
                + weights[static_cast<size_t>(slot2)];
}

} // namespace more_phi
