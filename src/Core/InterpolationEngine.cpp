/*
 * MorphSnap — Core/InterpolationEngine.cpp
 * SIMD-optimized interpolation for real-time audio safety.
 */
#include "InterpolationEngine.h"

// Platform detection for SIMD
#if defined(_MSC_VER)
    #include <intrin.h>
    #define MORPHSNAP_HAS_INTRIN 1
#elif defined(__GNUC__) || defined(__clang__)
    #include <cpuid.h>
    #define MORPHSNAP_HAS_INTRIN 1
#endif

// SIMD headers
#if defined(__AVX2__)
    #include <immintrin.h>
    #define MORPHSNAP_USE_AVX 1
#elif defined(__SSE2__) || (defined(_MSC_VER) && defined(_M_X64))
    #include <emmintrin.h>
    #define MORPHSNAP_USE_SSE 1
#endif

namespace morphsnap {

// ── CPU Feature Detection ─────────────────────────────────────────────────────

bool InterpolationEngine::hasAVXSupport()
{
#if defined(MORPHSNAP_USE_AVX)
    return true;
#else
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
#endif
}

bool InterpolationEngine::hasSSESupport()
{
#if defined(MORPHSNAP_USE_SSE)
    return true;
#else
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
        if (__getcpuid(1, &eax, &ebx, &ecx, &edx))
        {
            hasSSE = (edx & (1 << 26)) != 0;  // SSE2 bit
        }
#endif
        checked = true;
    }
    return hasSSE;
#endif
}

// ── SIMD Interpolation ────────────────────────────────────────────────────────

void InterpolationEngine::interpolateBatch_Scalar(
    const float* srcA, const float* srcB,
    float* dest, float t, size_t count)
{
    const float oneMinusT = 1.0f - t;
    for (size_t i = 0; i < count; ++i)
    {
        dest[i] = srcA[i] * oneMinusT + srcB[i] * t;
    }
}

void InterpolationEngine::interpolateBatch_SIMD(
    const float* srcA, const float* srcB,
    float* dest, float t, size_t count)
{
#if defined(MORPHSNAP_USE_AVX)
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

#elif defined(MORPHSNAP_USE_SSE)
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

std::array<juce::Point<float>, 12> InterpolationEngine::getClockPositions(float radius)
{
    std::array<juce::Point<float>, 12> pos;
    for (int i = 0; i < 12; ++i)
    {
        float angle = juce::MathConstants<float>::twoPi * static_cast<float>(i) / 12.0f
                    - juce::MathConstants<float>::halfPi;  // start at 12 o'clock
        pos[i] = { std::cos(angle) * radius, std::sin(angle) * radius };
    }
    return pos;
}

// ── 1D Interpolation (Fader Mode) ──────────────────────────────────────────────

void InterpolationEngine::compute1D(float faderPos,
                                     const SnapshotBank& bank,
                                     std::vector<float>& output)
{
    // FIXED: Use std::array instead of std::vector to avoid allocation
    // Track occupied slots in a fixed-size array (max 12 slots)
    std::array<int, SnapshotBank::NUM_SLOTS> occupiedSlots{};
    int occupiedCount = 0;

    for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
    {
        if (bank.isOccupied(i))
        {
            occupiedSlots[occupiedCount++] = i;
        }
    }

    if (occupiedCount == 0) return;

    if (occupiedCount == 1)
    {
        const auto& vals = bank.getSlot(occupiedSlots[0]).values;
        const size_t count = juce::jmin(vals.size(), output.size());
        for (size_t p = 0; p < count; ++p)
            output[p] = vals[p];
        return;
    }

    // Map faderPos [0,1] across occupied slots
    const float scaled = faderPos * static_cast<float>(occupiedCount - 1);
    const int loIdx = juce::jlimit(0, occupiedCount - 1,
                                   static_cast<int>(scaled));
    const int hiIdx = juce::jmin(loIdx + 1, occupiedCount - 1);
    const float alpha = scaled - static_cast<float>(loIdx);

    const auto& sLo = bank.getSlot(occupiedSlots[loIdx]).values;
    const auto& sHi = bank.getSlot(occupiedSlots[hiIdx]).values;

    const size_t count = juce::jmin(sLo.size(), sHi.size(), output.size());

    // Use SIMD interpolation for large batches
    if (count >= 8)
    {
        interpolateBatch_SIMD(sLo.data(), sHi.data(), output.data(), alpha, count);
    }
    else
    {
        interpolateBatch_Scalar(sLo.data(), sHi.data(), output.data(), alpha, count);
    }
}

// ── 2D Interpolation (XY Pad Mode) ──────────────────────────────────────────────

void InterpolationEngine::compute2D(float cursorX, float cursorY,
                                     const SnapshotBank& bank,
                                     std::vector<float>& output)
{
    const auto positions = getClockPositions();

    // FIXED: Use std::array for weights to avoid allocation
    std::array<float, 12> weights{};
    float totalWeight = 0.0f;

    for (int i = 0; i < 12; ++i)
    {
        if (!bank.isOccupied(i)) continue;

        const float dx = cursorX - positions[i].x;
        const float dy = cursorY - positions[i].y;
        const float dist = std::sqrt(dx * dx + dy * dy);

        if (dist < kEpsilon)
        {
            // Cursor directly on snapshot — use it exclusively
            const auto& vals = bank.getSlot(i).values;
            const size_t count = juce::jmin(vals.size(), output.size());
            for (size_t p = 0; p < count; ++p) output[p] = vals[p];
            return;
        }

        weights[i] = 1.0f / std::pow(dist, kIDWPower);
        totalWeight += weights[i];
    }

    if (totalWeight < kEpsilon) return;

    std::fill(output.begin(), output.end(), 0.0f);

    for (int i = 0; i < 12; ++i)
    {
        if (!bank.isOccupied(i)) continue;
        const float w = weights[i] / totalWeight;
        const auto& vals = bank.getSlot(i).values;
        const size_t count = juce::jmin(vals.size(), output.size());

        // Weighted accumulation - scalar is fine here (multiple sources)
        for (size_t p = 0; p < count; ++p)
            output[p] += vals[p] * w;
    }
}

} // namespace morphsnap
