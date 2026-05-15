#include "SIMDAudio.h"

#include <algorithm>
#include <cmath>

// Suppress unreachable code warnings for platform fallback paths
#ifdef _MSC_VER
#pragma warning(disable: 4702)
#endif

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
#define MORE_PHI_SIMD_X86 1
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#else
#define MORE_PHI_SIMD_X86 0
#endif

namespace more_phi {

bool SIMDAudio::supportsAVX()
{
    static const bool avxSupport = detectAVXSupport();
    return avxSupport;
}

bool SIMDAudio::supportsSSE()
{
    static const bool sseSupport = detectSSESupport();
    return sseSupport;
}

bool SIMDAudio::detectAVXSupport()
{
#if MORE_PHI_SIMD_X86
   #ifdef _MSC_VER
    int cpuInfo[4] = {};
    __cpuid(cpuInfo, 1);
    return (cpuInfo[2] & (1 << 28)) != 0;
   #else
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        return (ecx & (1 << 28)) != 0;
#endif
#endif
    return false;
}

bool SIMDAudio::detectSSESupport()
{
#if MORE_PHI_SIMD_X86
   #ifdef _MSC_VER
    int cpuInfo[4] = {};
    __cpuid(cpuInfo, 1);
    return (cpuInfo[3] & (1 << 25)) != 0;
   #else
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        return (edx & (1 << 25)) != 0;
#endif
#endif
    return false;
}

void SIMDAudio::multiplyScalar(const float* input, float scalar, float* output, size_t numSamples)
{
    if (input == nullptr || output == nullptr || numSamples == 0)
        return;

    if (supportsAVX())
        return multiplyScalarAVX(input, scalar, output, numSamples);

    if (supportsSSE())
        return multiplyScalarSSE(input, scalar, output, numSamples);

    multiplyScalarFallback(input, scalar, output, numSamples);
}

void SIMDAudio::addScalar(const float* input, float scalar, float* output, size_t numSamples)
{
    if (input == nullptr || output == nullptr || numSamples == 0)
        return;

    if (supportsAVX())
        return addScalarAVX(input, scalar, output, numSamples);

    if (supportsSSE())
        return addScalarSSE(input, scalar, output, numSamples);

    addScalarFallback(input, scalar, output, numSamples);
}

void SIMDAudio::multiply(const float* input1, const float* input2, float* output, size_t numSamples)
{
    if (input1 == nullptr || input2 == nullptr || output == nullptr || numSamples == 0)
        return;

    if (supportsAVX())
        return multiplyAVX(input1, input2, output, numSamples);

    if (supportsSSE())
        return multiplySSE(input1, input2, output, numSamples);

    multiplyFallback(input1, input2, output, numSamples);
}

void SIMDAudio::add(const float* input1, const float* input2, float* output, size_t numSamples)
{
    if (input1 == nullptr || input2 == nullptr || output == nullptr || numSamples == 0)
        return;

    if (supportsAVX())
        return addAVX(input1, input2, output, numSamples);

    if (supportsSSE())
        return addSSE(input1, input2, output, numSamples);

    addFallback(input1, input2, output, numSamples);
}

float SIMDAudio::findPeak(const float* input, size_t numSamples)
{
    if (input == nullptr || numSamples == 0)
        return 0.0f;

    if (supportsAVX())
        return findPeakAVX(input, numSamples);

    if (supportsSSE())
        return findPeakSSE(input, numSamples);

    return findPeakFallback(input, numSamples);
}

float SIMDAudio::calculateRMS(const float* input, size_t numSamples)
{
    if (input == nullptr || numSamples == 0)
        return 0.0f;

    if (supportsAVX())
        return calculateRMSAVX(input, numSamples);

    if (supportsSSE())
        return calculateRMSSSE(input, numSamples);

    return calculateRMSFallback(input, numSamples);
}

#if MORE_PHI_SIMD_X86

void SIMDAudio::multiplyScalarAVX(const float* input, float scalar, float* output, size_t numSamples)
{
    const size_t avxSamples = (numSamples / 8) * 8;
    const __m256 scalarVec = _mm256_set1_ps(scalar);

    for (size_t i = 0; i < avxSamples; i += 8)
    {
        const __m256 inputVec = _mm256_loadu_ps(&input[i]);
        const __m256 result = _mm256_mul_ps(inputVec, scalarVec);
        _mm256_storeu_ps(&output[i], result);
    }

    for (size_t i = avxSamples; i < numSamples; ++i)
        output[i] = input[i] * scalar;
}

void SIMDAudio::multiplyScalarSSE(const float* input, float scalar, float* output, size_t numSamples)
{
    const size_t sseSamples = (numSamples / 4) * 4;
    const __m128 scalarVec = _mm_set1_ps(scalar);

    for (size_t i = 0; i < sseSamples; i += 4)
    {
        const __m128 inputVec = _mm_loadu_ps(&input[i]);
        const __m128 result = _mm_mul_ps(inputVec, scalarVec);
        _mm_storeu_ps(&output[i], result);
    }

    for (size_t i = sseSamples; i < numSamples; ++i)
        output[i] = input[i] * scalar;
}

void SIMDAudio::addScalarAVX(const float* input, float scalar, float* output, size_t numSamples)
{
    const size_t avxSamples = (numSamples / 8) * 8;
    const __m256 scalarVec = _mm256_set1_ps(scalar);

    for (size_t i = 0; i < avxSamples; i += 8)
    {
        const __m256 inputVec = _mm256_loadu_ps(&input[i]);
        const __m256 result = _mm256_add_ps(inputVec, scalarVec);
        _mm256_storeu_ps(&output[i], result);
    }

    for (size_t i = avxSamples; i < numSamples; ++i)
        output[i] = input[i] + scalar;
}

void SIMDAudio::addScalarSSE(const float* input, float scalar, float* output, size_t numSamples)
{
    const size_t sseSamples = (numSamples / 4) * 4;
    const __m128 scalarVec = _mm_set1_ps(scalar);

    for (size_t i = 0; i < sseSamples; i += 4)
    {
        const __m128 inputVec = _mm_loadu_ps(&input[i]);
        const __m128 result = _mm_add_ps(inputVec, scalarVec);
        _mm_storeu_ps(&output[i], result);
    }

    for (size_t i = sseSamples; i < numSamples; ++i)
        output[i] = input[i] + scalar;
}

void SIMDAudio::multiplyAVX(const float* input1, const float* input2, float* output, size_t numSamples)
{
    const size_t avxSamples = (numSamples / 8) * 8;

    for (size_t i = 0; i < avxSamples; i += 8)
    {
        const __m256 input1Vec = _mm256_loadu_ps(&input1[i]);
        const __m256 input2Vec = _mm256_loadu_ps(&input2[i]);
        const __m256 result = _mm256_mul_ps(input1Vec, input2Vec);
        _mm256_storeu_ps(&output[i], result);
    }

    for (size_t i = avxSamples; i < numSamples; ++i)
        output[i] = input1[i] * input2[i];
}

void SIMDAudio::multiplySSE(const float* input1, const float* input2, float* output, size_t numSamples)
{
    const size_t sseSamples = (numSamples / 4) * 4;

    for (size_t i = 0; i < sseSamples; i += 4)
    {
        const __m128 input1Vec = _mm_loadu_ps(&input1[i]);
        const __m128 input2Vec = _mm_loadu_ps(&input2[i]);
        const __m128 result = _mm_mul_ps(input1Vec, input2Vec);
        _mm_storeu_ps(&output[i], result);
    }

    for (size_t i = sseSamples; i < numSamples; ++i)
        output[i] = input1[i] * input2[i];
}

void SIMDAudio::addAVX(const float* input1, const float* input2, float* output, size_t numSamples)
{
    const size_t avxSamples = (numSamples / 8) * 8;

    for (size_t i = 0; i < avxSamples; i += 8)
    {
        const __m256 input1Vec = _mm256_loadu_ps(&input1[i]);
        const __m256 input2Vec = _mm256_loadu_ps(&input2[i]);
        const __m256 result = _mm256_add_ps(input1Vec, input2Vec);
        _mm256_storeu_ps(&output[i], result);
    }

    for (size_t i = avxSamples; i < numSamples; ++i)
        output[i] = input1[i] + input2[i];
}

void SIMDAudio::addSSE(const float* input1, const float* input2, float* output, size_t numSamples)
{
    const size_t sseSamples = (numSamples / 4) * 4;

    for (size_t i = 0; i < sseSamples; i += 4)
    {
        const __m128 input1Vec = _mm_loadu_ps(&input1[i]);
        const __m128 input2Vec = _mm_loadu_ps(&input2[i]);
        const __m128 result = _mm_add_ps(input1Vec, input2Vec);
        _mm_storeu_ps(&output[i], result);
    }

    for (size_t i = sseSamples; i < numSamples; ++i)
        output[i] = input1[i] + input2[i];
}

float SIMDAudio::findPeakAVX(const float* input, size_t numSamples)
{
    const size_t avxSamples = (numSamples / 8) * 8;
    __m256 maxVec = _mm256_setzero_ps();

    for (size_t i = 0; i < avxSamples; i += 8)
    {
        const __m256 inputVec = _mm256_loadu_ps(&input[i]);
        const __m256 absVec = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), inputVec);
        maxVec = _mm256_max_ps(maxVec, absVec);
    }

    alignas(32) float maxArray[8] = {};
    _mm256_store_ps(maxArray, maxVec);

    float peak = 0.0f;
    for (float value : maxArray)
        peak = std::max(peak, value);

    for (size_t i = avxSamples; i < numSamples; ++i)
        peak = std::max(peak, std::abs(input[i]));

    return peak;
}

float SIMDAudio::findPeakSSE(const float* input, size_t numSamples)
{
    const size_t sseSamples = (numSamples / 4) * 4;
    __m128 maxVec = _mm_setzero_ps();

    for (size_t i = 0; i < sseSamples; i += 4)
    {
        const __m128 inputVec = _mm_loadu_ps(&input[i]);
        const __m128 absVec = _mm_andnot_ps(_mm_set1_ps(-0.0f), inputVec);
        maxVec = _mm_max_ps(maxVec, absVec);
    }

    alignas(16) float maxArray[4] = {};
    _mm_store_ps(maxArray, maxVec);

    float peak = 0.0f;
    for (float value : maxArray)
        peak = std::max(peak, value);

    for (size_t i = sseSamples; i < numSamples; ++i)
        peak = std::max(peak, std::abs(input[i]));

    return peak;
}

float SIMDAudio::calculateRMSAVX(const float* input, size_t numSamples)
{
    const size_t avxSamples = (numSamples / 8) * 8;
    __m256 sumVec = _mm256_setzero_ps();

    for (size_t i = 0; i < avxSamples; i += 8)
    {
        const __m256 inputVec = _mm256_loadu_ps(&input[i]);
        const __m256 squaredVec = _mm256_mul_ps(inputVec, inputVec);
        sumVec = _mm256_add_ps(sumVec, squaredVec);
    }

    alignas(32) float sumArray[8] = {};
    _mm256_store_ps(sumArray, sumVec);

    float sum = 0.0f;
    for (float value : sumArray)
        sum += value;

    for (size_t i = avxSamples; i < numSamples; ++i)
        sum += input[i] * input[i];

    return std::sqrt(sum / static_cast<float>(numSamples));
}

float SIMDAudio::calculateRMSSSE(const float* input, size_t numSamples)
{
    const size_t sseSamples = (numSamples / 4) * 4;
    __m128 sumVec = _mm_setzero_ps();

    for (size_t i = 0; i < sseSamples; i += 4)
    {
        const __m128 inputVec = _mm_loadu_ps(&input[i]);
        const __m128 squaredVec = _mm_mul_ps(inputVec, inputVec);
        sumVec = _mm_add_ps(sumVec, squaredVec);
    }

    alignas(16) float sumArray[4] = {};
    _mm_store_ps(sumArray, sumVec);

    float sum = 0.0f;
    for (float value : sumArray)
        sum += value;

    for (size_t i = sseSamples; i < numSamples; ++i)
        sum += input[i] * input[i];

    return std::sqrt(sum / static_cast<float>(numSamples));
}

#else

void SIMDAudio::multiplyScalarAVX(const float* input, float scalar, float* output, size_t numSamples)
{
    multiplyScalarFallback(input, scalar, output, numSamples);
}

void SIMDAudio::multiplyScalarSSE(const float* input, float scalar, float* output, size_t numSamples)
{
    multiplyScalarFallback(input, scalar, output, numSamples);
}

void SIMDAudio::addScalarAVX(const float* input, float scalar, float* output, size_t numSamples)
{
    addScalarFallback(input, scalar, output, numSamples);
}

void SIMDAudio::addScalarSSE(const float* input, float scalar, float* output, size_t numSamples)
{
    addScalarFallback(input, scalar, output, numSamples);
}

void SIMDAudio::multiplyAVX(const float* input1, const float* input2, float* output, size_t numSamples)
{
    multiplyFallback(input1, input2, output, numSamples);
}

void SIMDAudio::multiplySSE(const float* input1, const float* input2, float* output, size_t numSamples)
{
    multiplyFallback(input1, input2, output, numSamples);
}

void SIMDAudio::addAVX(const float* input1, const float* input2, float* output, size_t numSamples)
{
    addFallback(input1, input2, output, numSamples);
}

void SIMDAudio::addSSE(const float* input1, const float* input2, float* output, size_t numSamples)
{
    addFallback(input1, input2, output, numSamples);
}

float SIMDAudio::findPeakAVX(const float* input, size_t numSamples)
{
    return findPeakFallback(input, numSamples);
}

float SIMDAudio::findPeakSSE(const float* input, size_t numSamples)
{
    return findPeakFallback(input, numSamples);
}

float SIMDAudio::calculateRMSAVX(const float* input, size_t numSamples)
{
    return calculateRMSFallback(input, numSamples);
}

float SIMDAudio::calculateRMSSSE(const float* input, size_t numSamples)
{
    return calculateRMSFallback(input, numSamples);
}

#endif

void SIMDAudio::multiplyScalarFallback(const float* input, float scalar, float* output, size_t numSamples)
{
    for (size_t i = 0; i < numSamples; ++i)
        output[i] = input[i] * scalar;
}

void SIMDAudio::addScalarFallback(const float* input, float scalar, float* output, size_t numSamples)
{
    for (size_t i = 0; i < numSamples; ++i)
        output[i] = input[i] + scalar;
}

void SIMDAudio::multiplyFallback(const float* input1, const float* input2, float* output, size_t numSamples)
{
    for (size_t i = 0; i < numSamples; ++i)
        output[i] = input1[i] * input2[i];
}

void SIMDAudio::addFallback(const float* input1, const float* input2, float* output, size_t numSamples)
{
    for (size_t i = 0; i < numSamples; ++i)
        output[i] = input1[i] + input2[i];
}

float SIMDAudio::findPeakFallback(const float* input, size_t numSamples)
{
    float peak = 0.0f;
    for (size_t i = 0; i < numSamples; ++i)
        peak = std::max(peak, std::abs(input[i]));

    return peak;
}

float SIMDAudio::calculateRMSFallback(const float* input, size_t numSamples)
{
    float sum = 0.0f;
    for (size_t i = 0; i < numSamples; ++i)
        sum += input[i] * input[i];

    return std::sqrt(sum / static_cast<float>(numSamples));
}

} // namespace more_phi
