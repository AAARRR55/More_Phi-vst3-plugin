/*
 * More-Phi — Core/InterpolationEngine.cpp
 * SIMD-optimized interpolation for real-time audio safety.
 */
#include "InterpolationEngine.h"
#include <cmath>

// Platform detection for SIMD — x86/x64 only (not ARM/Apple Silicon)
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #define MORE_PHI_X86 1
    #if defined(_MSC_VER)
        #include <intrin.h>
        #define MORE_PHI_HAS_INTRIN 1
    #elif defined(__GNUC__) || defined(__clang__)
        #include <cpuid.h>
        #define MORE_PHI_HAS_INTRIN 1
    #endif

    // SIMD headers
    #if defined(__AVX2__)
        #include <immintrin.h>
        #define MORE_PHI_USE_AVX 1
    #elif defined(__SSE2__) || (defined(_MSC_VER) && defined(_M_X64))
        #include <emmintrin.h>
        #define MORE_PHI_USE_SSE 1
    #endif
#endif

namespace more_phi {

// ── CPU Feature Detection ─────────────────────────────────────────────────────

bool InterpolationEngine::hasAVXSupport()
{
#if defined(MORE_PHI_USE_AVX)
    return true;
#elif defined(MORE_PHI_X86)
    static bool checked = false;
    static bool hasAVX = false;

    if (!checked)
    {
#if defined(_MSC_VER)
        int cpuInfo[4];
        __cpuid(cpuInfo, 0);
        if (cpuInfo[0] >= 1)
        {
            __cpuidex(cpuInfo, 1, 0);
            hasAVX = (cpuInfo[2] & (1 << 28)) != 0;  // AVX bit
        }
#elif defined(__GNUC__) || defined(__clang__)
        unsigned int eax, ebx, ecx, edx;
        if (__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        {
            hasAVX = (ecx & (1 << 28)) != 0;  // AVX bit
        }
#endif
        checked = true;
    }
    return hasAVX;
#else
    return false;
#endif
}

bool InterpolationEngine::hasSSESupport()
{
#if defined(MORE_PHI_USE_SSE)
    return true;
#elif defined(MORE_PHI_X86)
    static bool checked = false;
    static bool hasSSE = false;

    if (!checked)
    {
#if defined(_MSC_VER)
        int cpuInfo[4];
        __cpuid(cpuInfo, 1);
        hasSSE = (cpuInfo[3] & (1 << 26)) != 0;  // SSE2 bit
#elif defined(__GNUC__) || defined(__clang__)
        unsigned int eax, ebx, ecx, edx;
        if (__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        {
            hasSSE = (edx & (1 << 26)) != 0;  // SSE2 bit
        }
#endif
        checked = true;
    }
    return hasSSE;
#else
    return false;
#endif
}

const char* InterpolationEngine::getCompiledSIMDPath() noexcept
{
#if defined(MORE_PHI_USE_AVX)
    return "AVX2";
#elif defined(MORE_PHI_USE_SSE)
    return "SSE2";
#else
    return "Scalar";
#endif
}

// ── SIMD Interpolation ────────────────────────────────────────────────────────

void InterpolationEngine::interpolateBatch_Scalar(
    const float* srcA, const float* srcB,
    float* dest, float t, size_t count) noexcept
{
    jassert(t >= 0.0f && t <= 1.0f);
    const float oneMinusT = 1.0f - t;
    for (size_t i = 0; i < count; ++i)
    {
        dest[i] = srcA[i] * oneMinusT + srcB[i] * t;
    }
}

void InterpolationEngine::interpolateBatch_SIMD(
    const float* srcA, const float* srcB,
    float* dest, float t, size_t count) noexcept
{
#if defined(MORE_PHI_USE_AVX)
    // AVX2 implementation - 8 floats at once
    __m256 tVec = _mm256_set1_ps(t);
    __m256 oneMinusT = _mm256_set1_ps(1.0f - t);

    size_t simdCount = count - (count % 8);

    for (size_t i = 0; i < simdCount; i += 8)
    {
        __m256 a = _mm256_loadu_ps(srcA + i);
        __m256 b = _mm256_loadu_ps(srcB + i);
        __m256 result = _mm256_add_ps(
            _mm256_mul_ps(a, oneMinusT),
            _mm256_mul_ps(b, tVec));
        _mm256_storeu_ps(dest + i, result);
    }

    // Handle remaining elements
    for (size_t i = simdCount; i < count; ++i)
    {
        dest[i] = srcA[i] * (1.0f - t) + srcB[i] * t;
    }

#elif defined(MORE_PHI_USE_SSE)
    // SSE2 implementation - 4 floats at once
    __m128 tVec = _mm_set1_ps(t);
    __m128 oneMinusT = _mm_set1_ps(1.0f - t);

    size_t simdCount = count - (count % 4);

    for (size_t i = 0; i < simdCount; i += 4)
    {
        __m128 a = _mm_loadu_ps(srcA + i);
        __m128 b = _mm_loadu_ps(srcB + i);
        __m128 result = _mm_add_ps(
            _mm_mul_ps(a, oneMinusT),
            _mm_mul_ps(b, tVec));
        _mm_storeu_ps(dest + i, result);
    }

    // Handle remaining elements
    for (size_t i = simdCount; i < count; ++i)
    {
        dest[i] = srcA[i] * (1.0f - t) + srcB[i] * t;
    }

#else
    // Scalar fallback
    interpolateBatch_Scalar(srcA, srcB, dest, t, count);
#endif
}

// ── Clock Positions ────────────────────────────────────────────────────────────

// W6 FIX: Return by value. Unit-radius positions are still computed once via
// a function-local static and copied out (12 Points = 96 B); scaled-radius
// positions are now computed into a local rather than a thread_local static,
// eliminating the "holding a ref across another call invalidates it" footgun.
// (std::cos/std::sin only run on the very first call for the unit-radius
// cache; subsequent calls are a copy.)
std::array<juce::Point<float>, 12> InterpolationEngine::getClockPositions(float radius)
{
    static const auto kUnitPositions = []()
    {
        std::array<juce::Point<float>, 12> pos;
        for (int i = 0; i < 12; ++i)
        {
            float angle = juce::MathConstants<float>::twoPi * static_cast<float>(i) / 12.0f
                        - juce::MathConstants<float>::halfPi;
            pos[i] = { std::cos(angle), std::sin(angle) };
        }
        return pos;
    }();

    if (std::abs(radius - 1.0f) < 1e-6f)
        return kUnitPositions;

    std::array<juce::Point<float>, 12> scaled;
    for (int i = 0; i < 12; ++i)
        scaled[i] = { kUnitPositions[i].x * radius, kUnitPositions[i].y * radius };
    return scaled;
}

// ── Retry helper ──────────────────────────────────────────────────────────────
// Wraps the lock-free seqlock read with a bounded retry loop and a neutral
// fallback on exhaustion. Both compute1D and compute2D use this pattern.
// noexcept: tryReadLocked is noexcept, DBG and std::fill do not throw.
template<typename Fn>
static void computeWithRetry(const SnapshotBank& bank, Fn&& fn) noexcept
{
    constexpr int kMaxRetries = 5;
    bool lockAcquired = false;

    for (int attempt = 0; attempt < kMaxRetries; ++attempt)
    {
        lockAcquired = bank.tryReadLocked(std::forward<Fn>(fn));
        if (lockAcquired)
            break;
    }

    if (!lockAcquired)
    {
        // Very rare: heavy write contention on the snapshot bank. Hold the
        // previous frame: output still contains the last successful block's
        // morph result, which is glitch-free. Snapping every parameter to 0.5
        // (the old behavior) produced a loud mid-point jump under contention.
        // On the first-ever block the buffer holds prepareToPlay's zeros; that
        // only matters if contention occurs before any successful read, which
        // is effectively impossible.
    }
}

// ── 1D Interpolation (Fader Mode) ──────────────────────────────────────────────

void InterpolationEngine::compute1D(float faderPos,
                                     const SnapshotBank& bank,
                                     std::vector<float>& output) noexcept
{
    // FIX C7: NaN guard — NaN faderPos would otherwise produce UB in jlimit/index math.
    if (!std::isfinite(faderPos))
    {
        std::fill(output.begin(), output.end(), 0.5f);
        return;
    }

    // W-1 FIX (audit): do NOT pre-fill output here. computeWithRetry leaves
    // output untouched on seqlock exhaustion, so a pre-fill would clobber the
    // caller's previous-frame morph result (the "hold previous" invariant the
    // retry helper relies on). The lambda below writes every element on a
    // successful read — including the occupied==0 neutral case (0.5f) — so
    // output is always fully defined when the read succeeds.

    computeWithRetry(bank,
        [&output, faderPos](const auto& slots)
        {
            std::array<int, SnapshotBank::NUM_SLOTS> occupiedSlots{};
            int occupiedCount = 0;

            for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
            {
                if (slots[i].occupied)
                    occupiedSlots[occupiedCount++] = i;
            }

            if (occupiedCount == 0)
            {
                std::fill(output.begin(), output.end(), 0.5f);
                return;
            }

            if (occupiedCount == 1)
            {
                const auto& slot = slots[occupiedSlots[0]];
                const size_t count = juce::jmin(static_cast<size_t>(slot.size()), output.size());
                for (size_t p = 0; p < count; ++p)
                    output[p] = slot.data()[p];
                return;
            }

            // Map faderPos [0,1] across occupied slots
            const float scaled = faderPos * static_cast<float>(occupiedCount - 1);
            const int loIdx = juce::jlimit(0, occupiedCount - 1,
                                           static_cast<int>(scaled));
            const int hiIdx = juce::jmin(loIdx + 1, occupiedCount - 1);
            const float alpha = scaled - static_cast<float>(loIdx);

            const auto& sLo = slots[occupiedSlots[loIdx]];
            const auto& sHi = slots[occupiedSlots[hiIdx]];

            const size_t count = juce::jmin(static_cast<size_t>(sLo.size()),
                                            static_cast<size_t>(sHi.size()),
                                            output.size());

            if (count >= 8)
                interpolateBatch_SIMD(sLo.data(), sHi.data(), output.data(), alpha, count);
            else
                interpolateBatch_Scalar(sLo.data(), sHi.data(), output.data(), alpha, count);
        });
}

// ── 2D Interpolation (XY Pad Mode) ──────────────────────────────────────────────

void InterpolationEngine::compute2D(float cursorX, float cursorY,
                                     const SnapshotBank& bank,
                                     std::vector<float>& output) noexcept
{
    // FIX C8: NaN guard — NaN cursor coordinates would poison all IDW weights.
    if (!std::isfinite(cursorX) || !std::isfinite(cursorY))
    {
        std::fill(output.begin(), output.end(), 0.5f);
        return;
    }

    const auto positions = getClockPositions();

    computeWithRetry(bank,
        [&output, &positions, cursorX, cursorY](const auto& slots)
        {
            std::array<float, SnapshotBank::NUM_SLOTS> weights{};
            float totalWeight = 0.0f;

            // AUDIT-FIX (C7): kEpsilonSq is now the unified class constant.

            for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
            {
                if (!slots[i].occupied)
                    continue;

                const float dx = cursorX - positions[i].x;
                const float dy = cursorY - positions[i].y;
                const float distSq = dx * dx + dy * dy;  // No sqrt needed

                if (distSq < kEpsilonSq)
                {
                    // Cursor directly on snapshot — use it exclusively
                    const auto& slot = slots[i];
                    const size_t count = juce::jmin(static_cast<size_t>(slot.size()), output.size());
                    for (size_t p = 0; p < count; ++p)
                        output[p] = slot.data()[p];
                    return;
                }

                // IDW power=2 with per-slot mass (Gravity Well):
                // w = mass / d² = slots[i].mass / max(distSq, epsilonSq)
                // High mass expands the snapshot's zone of influence; low mass shrinks it.
                // Mass is clamped to [0.1, 3.0] by SnapshotBank::setMass.
                weights[i] = slots[i].mass / std::max(distSq, kEpsilonSq);
                totalWeight += weights[i];
            }

            if (totalWeight < kEpsilon)
                return;

            std::fill(output.begin(), output.end(), 0.0f);

            for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
            {
                if (!slots[i].occupied)
                    continue;

                const float w = weights[i] / totalWeight;
                const auto& slot = slots[i];
                const size_t count = juce::jmin(static_cast<size_t>(slot.size()), output.size());

                // SIMD-accelerated weighted accumulation
#if defined(MORE_PHI_USE_AVX)
                const __m256 wVec = _mm256_set1_ps(w);
                const size_t simdCount = count - (count % 8);
                for (size_t p = 0; p < simdCount; p += 8)
                {
                    __m256 acc = _mm256_loadu_ps(output.data() + p);
                    __m256 src = _mm256_loadu_ps(slot.data() + p);
                    acc = _mm256_add_ps(acc, _mm256_mul_ps(src, wVec));
                    _mm256_storeu_ps(output.data() + p, acc);
                }
                for (size_t p = simdCount; p < count; ++p)
                    output[p] += slot.data()[p] * w;
#elif defined(MORE_PHI_USE_SSE)
                const __m128 wVec = _mm_set1_ps(w);
                const size_t simdCount = count - (count % 4);
                for (size_t p = 0; p < simdCount; p += 4)
                {
                    __m128 acc = _mm_loadu_ps(output.data() + p);
                    __m128 src = _mm_loadu_ps(slot.data() + p);
                    acc = _mm_add_ps(acc, _mm_mul_ps(src, wVec));
                    _mm_storeu_ps(output.data() + p, acc);
                }
                for (size_t p = simdCount; p < count; ++p)
                    output[p] += slot.data()[p] * w;
#else
                for (size_t p = 0; p < count; ++p)
                    output[p] += slot.data()[p] * w;
#endif
            }
        });
}

// ── 2D Voronoi/NNI Interpolation ──────────────────────────────────────────────

void InterpolationEngine::compute2D_Voronoi(float cursorX, float cursorY,
                                             const SnapshotBank& bank,
                                             const VoronoiMorphEngine& engine,
                                             std::vector<float>& output) noexcept
{
    if (!std::isfinite(cursorX) || !std::isfinite(cursorY))
    {
        std::fill(output.begin(), output.end(), 0.5f);
        return;
    }

    // Fall back to IDW when triangulation is invalid (<3 occupied slots)
    if (!engine.isValid())
    {
        compute2D(cursorX, cursorY, bank, output);
        return;
    }

    const auto positions = getClockPositions();

    // Compute NNI weights and blend in a single seqlock read for atomicity.
    // Captures masses + values in one consistent snapshot.
    computeWithRetry(bank,
        [&](const auto& slots)
        {
            // Read masses
            std::array<float, SnapshotBank::NUM_SLOTS> masses{};
            for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
                masses[static_cast<size_t>(i)] = slots[i].mass;

            // Compute NNI weights
            std::array<float, SnapshotBank::NUM_SLOTS> weights{};
            float totalWeight = 0.0f;
            engine.computeWeights(cursorX, cursorY, positions, masses, weights, totalWeight);

            // AUDIT-FIX (C2): crossfade barycentric (inside hull) and IDW (outside)
            // over a narrow signed-distance band. Without this the transition is a
            // hard step — a C0 discontinuity in which/how many snapshots contribute
            // whenever the cursor crosses the convex hull of occupied snapshots.
            // alpha = 1 → pure barycentric; alpha = 0 → pure IDW.
            constexpr float kHullBlendEps = 0.02f;  // ±2% of pad half-width
            const float signedDist = engine.signedDistanceToHull(cursorX, cursorY, positions);
            float alpha = 1.0f;
            if (std::isfinite(signedDist))
            {
                // smoothstep over [-eps, +eps]
                const float t = std::clamp((signedDist + kHullBlendEps) / (2.0f * kHullBlendEps), 0.0f, 1.0f);
                alpha = t * t * (3.0f - 2.0f * t);
            }
            else if (totalWeight < 1e-6f)
            {
                // No hull (degenerate geometry) and no barycentric result → pure IDW.
                alpha = 0.0f;
            }

            const bool needBarycentric = (alpha > 1e-4f) && (totalWeight >= 1e-6f);
            const bool needIDW = (alpha < 1.0f - 1e-4f);

            // Compute IDW weights when needed (inside-band or outside).
            std::array<float, SnapshotBank::NUM_SLOTS> idwWeights{};
            float idwTotal = 0.0f;
            bool onSnapshotIDW = false;
            int onSnapshotIdx = -1;
            if (needIDW)
            {
                for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
                {
                    if (!slots[i].occupied) continue;
                    const float dx = cursorX - positions[i].x;
                    const float dy = cursorY - positions[i].y;
                    const float distSq = dx * dx + dy * dy;

                    if (distSq < kEpsilonSq)
                    {
                        // Cursor directly on a snapshot — exclusive, no blend needed.
                        onSnapshotIDW = true;
                        onSnapshotIdx = i;
                        break;
                    }

                    idwWeights[i] = slots[i].mass / std::max(distSq, kEpsilonSq);
                    idwTotal += idwWeights[i];
                }
            }

            if (onSnapshotIDW)
            {
                const size_t c = juce::jmin(static_cast<size_t>(slots[onSnapshotIdx].size()), output.size());
                for (size_t p = 0; p < c; ++p)
                    output[p] = slots[onSnapshotIdx].data()[p];
                return;
            }

            if (!needBarycentric && idwTotal < 1e-12f) return;  // nothing to contribute
            if (needBarycentric && !needIDW && totalWeight < 1e-6f) return;

            // Emit blended output: alpha * barycentric + (1 - alpha) * IDW.
            std::fill(output.begin(), output.end(), 0.0f);
            for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
            {
                if (!slots[i].occupied) continue;
                float w = 0.0f;
                if (needBarycentric)
                    w += alpha * (weights[static_cast<size_t>(i)] / totalWeight);
                if (needIDW && idwTotal > 1e-12f)
                    w += (1.0f - alpha) * (idwWeights[i] / idwTotal);
                if (w < 1e-8f) continue;

                const auto& slot = slots[i];
                const size_t count = juce::jmin(static_cast<size_t>(slot.size()), output.size());

#if defined(MORE_PHI_USE_AVX)
                const __m256 wVec = _mm256_set1_ps(w);
                size_t simdCount = count - (count % 8);
                for (size_t p = 0; p < simdCount; p += 8)
                {
                    __m256 acc = _mm256_loadu_ps(output.data() + p);
                    __m256 src = _mm256_loadu_ps(slot.data() + p);
                    acc = _mm256_add_ps(acc, _mm256_mul_ps(src, wVec));
                    _mm256_storeu_ps(output.data() + p, acc);
                }
                for (size_t p = simdCount; p < count; ++p)
                    output[p] += slot.data()[p] * w;
#elif defined(MORE_PHI_USE_SSE)
                const __m128 wVec = _mm_set1_ps(w);
                size_t simdCount = count - (count % 4);
                for (size_t p = 0; p < simdCount; p += 4)
                {
                    __m128 acc = _mm_loadu_ps(output.data() + p);
                    __m128 src = _mm_loadu_ps(slot.data() + p);
                    acc = _mm_add_ps(acc, _mm_mul_ps(src, wVec));
                    _mm_storeu_ps(output.data() + p, acc);
                }
                for (size_t p = simdCount; p < count; ++p)
                    output[p] += slot.data()[p] * w;
#else
                for (size_t p = 0; p < count; ++p)
                    output[p] += slot.data()[p] * w;
#endif
            }
        });
}

} // namespace more_phi
