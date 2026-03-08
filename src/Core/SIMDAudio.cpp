#include "SIMDAudio.h"
#include <immintrin.h>
#include <algorithm>
#include <cmath>

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif

namespace morphsnap
{
    // CPU capability detection with static caching
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
#ifdef _MSC_VER
        int cpuInfo[4];
        __cpuid(cpuInfo, 1);
        return (cpuInfo[2] & (1 << 28)) != 0; // AVX bit
#else
        unsigned int eax, ebx, ecx, edx;
        if (__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        {
            return (ecx & (1 << 28)) != 0; // AVX bit
        }
        return false;
#endif
    }

    bool SIMDAudio::detectSSESupport()
    {
#ifdef _MSC_VER
        int cpuInfo[4];
        __cpuid(cpuInfo, 1);
        return (cpuInfo[3] & (1 << 25)) != 0; // SSE bit
#else
        unsigned int eax, ebx, ecx, edx;
        if (__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        {
            return (edx & (1 << 25)) != 0; // SSE bit
        }
        return false;
#endif
    }

    // ========== multiplyScalar implementations ==========

    void SIMDAudio::multiplyScalar(const float* input, float scalar, float* output, size_t numSamples)
    {
        if (supportsAVX())
        {
            multiplyScalarAVX(input, scalar, output, numSamples);
        }
        else if (supportsSSE())
        {
            multiplyScalarSSE(input, scalar, output, numSamples);
        }
        else
        {
            multiplyScalarFallback(input, scalar, output, numSamples);
        }
    }

    void SIMDAudio::multiplyScalarAVX(const float* input, float scalar, float* output, size_t numSamples)
    {
        const size_t avxSamples = (numSamples / 8) * 8;
        const __m256 scalarVec = _mm256_set1_ps(scalar);

        for (size_t i = 0; i < avxSamples; i += 8)
        {
            __m256 inputVec = _mm256_loadu_ps(&input[i]);
            __m256 result = _mm256_mul_ps(inputVec, scalarVec);
            _mm256_storeu_ps(&output[i], result);
        }

        // Handle remaining samples
        for (size_t i = avxSamples; i < numSamples; ++i)
        {
            output[i] = input[i] * scalar;
        }
    }

    void SIMDAudio::multiplyScalarSSE(const float* input, float scalar, float* output, size_t numSamples)
    {
        const size_t sseSamples = (numSamples / 4) * 4;
        const __m128 scalarVec = _mm_set1_ps(scalar);

        for (size_t i = 0; i < sseSamples; i += 4)
        {
            __m128 inputVec = _mm_loadu_ps(&input[i]);
            __m128 result = _mm_mul_ps(inputVec, scalarVec);
            _mm_storeu_ps(&output[i], result);
        }

        // Handle remaining samples
        for (size_t i = sseSamples; i < numSamples; ++i)
        {
            output[i] = input[i] * scalar;
        }
    }

    void SIMDAudio::multiplyScalarFallback(const float* input, float scalar, float* output, size_t numSamples)
    {
        for (size_t i = 0; i < numSamples; ++i)
        {
            output[i] = input[i] * scalar;
        }
    }

    // ========== addScalar implementations ==========

    void SIMDAudio::addScalar(const float* input, float scalar, float* output, size_t numSamples)
    {
        if (supportsAVX())
        {
            addScalarAVX(input, scalar, output, numSamples);
        }
        else if (supportsSSE())
        {
            addScalarSSE(input, scalar, output, numSamples);
        }
        else
        {
            addScalarFallback(input, scalar, output, numSamples);
        }
    }

    void SIMDAudio::addScalarAVX(const float* input, float scalar, float* output, size_t numSamples)
    {
        const size_t avxSamples = (numSamples / 8) * 8;
        const __m256 scalarVec = _mm256_set1_ps(scalar);

        for (size_t i = 0; i < avxSamples; i += 8)
        {
            __m256 inputVec = _mm256_loadu_ps(&input[i]);
            __m256 result = _mm256_add_ps(inputVec, scalarVec);
            _mm256_storeu_ps(&output[i], result);
        }

        // Handle remaining samples
        for (size_t i = avxSamples; i < numSamples; ++i)
        {
            output[i] = input[i] + scalar;
        }
    }

    void SIMDAudio::addScalarSSE(const float* input, float scalar, float* output, size_t numSamples)
    {
        const size_t sseSamples = (numSamples / 4) * 4;
        const __m128 scalarVec = _mm_set1_ps(scalar);

        for (size_t i = 0; i < sseSamples; i += 4)
        {
            __m128 inputVec = _mm_loadu_ps(&input[i]);
            __m128 result = _mm_add_ps(inputVec, scalarVec);
            _mm_storeu_ps(&output[i], result);
        }

        // Handle remaining samples
        for (size_t i = sseSamples; i < numSamples; ++i)
        {
            output[i] = input[i] + scalar;
        }
    }

    void SIMDAudio::addScalarFallback(const float* input, float scalar, float* output, size_t numSamples)
    {
        for (size_t i = 0; i < numSamples; ++i)
        {
            output[i] = input[i] + scalar;
        }
    }

    // ========== multiply implementations ==========

    void SIMDAudio::multiply(const float* input1, const float* input2, float* output, size_t numSamples)
    {
        if (supportsAVX())
        {
            multiplyAVX(input1, input2, output, numSamples);
        }
        else if (supportsSSE())
        {
            multiplySSE(input1, input2, output, numSamples);
        }
        else
        {
            multiplyFallback(input1, input2, output, numSamples);
        }
    }

    void SIMDAudio::multiplyAVX(const float* input1, const float* input2, float* output, size_t numSamples)
    {
        const size_t avxSamples = (numSamples / 8) * 8;

        for (size_t i = 0; i < avxSamples; i += 8)
        {
            __m256 input1Vec = _mm256_loadu_ps(&input1[i]);
            __m256 input2Vec = _mm256_loadu_ps(&input2[i]);
            __m256 result = _mm256_mul_ps(input1Vec, input2Vec);
            _mm256_storeu_ps(&output[i], result);
        }

        // Handle remaining samples
        for (size_t i = avxSamples; i < numSamples; ++i)
        {
            output[i] = input1[i] * input2[i];
        }
    }

    void SIMDAudio::multiplySSE(const float* input1, const float* input2, float* output, size_t numSamples)
    {
        const size_t sseSamples = (numSamples / 4) * 4;

        for (size_t i = 0; i < sseSamples; i += 4)
        {
            __m128 input1Vec = _mm_loadu_ps(&input1[i]);
            __m128 input2Vec = _mm_loadu_ps(&input2[i]);
            __m128 result = _mm_mul_ps(input1Vec, input2Vec);
            _mm_storeu_ps(&output[i], result);
        }

        // Handle remaining samples
        for (size_t i = sseSamples; i < numSamples; ++i)
        {
            output[i] = input1[i] * input2[i];
        }
    }

    void SIMDAudio::multiplyFallback(const float* input1, const float* input2, float* output, size_t numSamples)
    {
        for (size_t i = 0; i < numSamples; ++i)
        {
            output[i] = input1[i] * input2[i];
        }
    }

    // ========== add implementations ==========

    void SIMDAudio::add(const float* input1, const float* input2, float* output, size_t numSamples)
    {
        if (supportsAVX())
        {
            addAVX(input1, input2, output, numSamples);
        }
        else if (supportsSSE())
        {
            addSSE(input1, input2, output, numSamples);
        }
        else
        {
            addFallback(input1, input2, output, numSamples);
        }
    }

    void SIMDAudio::addAVX(const float* input1, const float* input2, float* output, size_t numSamples)
    {
        const size_t avxSamples = (numSamples / 8) * 8;

        for (size_t i = 0; i < avxSamples; i += 8)
        {
            __m256 input1Vec = _mm256_loadu_ps(&input1[i]);
            __m256 input2Vec = _mm256_loadu_ps(&input2[i]);
            __m256 result = _mm256_add_ps(input1Vec, input2Vec);
            _mm256_storeu_ps(&output[i], result);
        }

        // Handle remaining samples
        for (size_t i = avxSamples; i < numSamples; ++i)
        {
            output[i] = input1[i] + input2[i];
        }
    }

    void SIMDAudio::addSSE(const float* input1, const float* input2, float* output, size_t numSamples)
    {
        const size_t sseSamples = (numSamples / 4) * 4;

        for (size_t i = 0; i < sseSamples; i += 4)
        {
            __m128 input1Vec = _mm_loadu_ps(&input1[i]);
            __m128 input2Vec = _mm_loadu_ps(&input2[i]);
            __m128 result = _mm_add_ps(input1Vec, input2Vec);
            _mm_storeu_ps(&output[i], result);
        }

        // Handle remaining samples
        for (size_t i = sseSamples; i < numSamples; ++i)
        {
            output[i] = input1[i] + input2[i];
        }
    }

    void SIMDAudio::addFallback(const float* input1, const float* input2, float* output, size_t numSamples)
    {
        for (size_t i = 0; i < numSamples; ++i)
        {
            output[i] = input1[i] + input2[i];
        }
    }

    // ========== findPeak implementations ==========

    float SIMDAudio::findPeak(const float* input, size_t numSamples)
    {
        if (supportsAVX())
        {
            return findPeakAVX(input, numSamples);
        }
        else if (supportsSSE())
        {
            return findPeakSSE(input, numSamples);
        }
        else
        {
            return findPeakFallback(input, numSamples);
        }
    }

    float SIMDAudio::findPeakAVX(const float* input, size_t numSamples)
    {
        const size_t avxSamples = (numSamples / 8) * 8;
        __m256 maxVec = _mm256_setzero_ps();

        for (size_t i = 0; i < avxSamples; i += 8)
        {
            __m256 inputVec = _mm256_loadu_ps(&input[i]);
            __m256 absVec = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), inputVec); // abs(inputVec)
            maxVec = _mm256_max_ps(maxVec, absVec);
        }

        // Extract maximum from AVX register
        alignas(32) float maxArray[8];
        _mm256_store_ps(maxArray, maxVec);
        float peak = 0.0f;
        for (int i = 0; i < 8; ++i)
        {
            peak = std::max(peak, maxArray[i]);
        }

        // Handle remaining samples
        for (size_t i = avxSamples; i < numSamples; ++i)
        {
            peak = std::max(peak, std::abs(input[i]));
        }

        return peak;
    }

    float SIMDAudio::findPeakSSE(const float* input, size_t numSamples)
    {
        const size_t sseSamples = (numSamples / 4) * 4;
        __m128 maxVec = _mm_setzero_ps();

        for (size_t i = 0; i < sseSamples; i += 4)
        {
            __m128 inputVec = _mm_loadu_ps(&input[i]);
            __m128 absVec = _mm_andnot_ps(_mm_set1_ps(-0.0f), inputVec); // abs(inputVec)
            maxVec = _mm_max_ps(maxVec, absVec);
        }

        // Extract maximum from SSE register
        alignas(16) float maxArray[4];
        _mm_store_ps(maxArray, maxVec);
        float peak = 0.0f;
        for (int i = 0; i < 4; ++i)
        {
            peak = std::max(peak, maxArray[i]);
        }

        // Handle remaining samples
        for (size_t i = sseSamples; i < numSamples; ++i)
        {
            peak = std::max(peak, std::abs(input[i]));
        }

        return peak;
    }

    float SIMDAudio::findPeakFallback(const float* input, size_t numSamples)
    {
        float peak = 0.0f;
        for (size_t i = 0; i < numSamples; ++i)
        {
            peak = std::max(peak, std::abs(input[i]));
        }
        return peak;
    }

    // ========== calculateRMS implementations ==========

    float SIMDAudio::calculateRMS(const float* input, size_t numSamples)
    {
        if (supportsAVX())
        {
            return calculateRMSAVX(input, numSamples);
        }
        else if (supportsSSE())
        {
            return calculateRMSSSE(input, numSamples);
        }
        else
        {
            return calculateRMSFallback(input, numSamples);
        }
    }

    float SIMDAudio::calculateRMSAVX(const float* input, size_t numSamples)
    {
        if (numSamples == 0) return 0.0f;

        const size_t avxSamples = (numSamples / 8) * 8;
        __m256 sumVec = _mm256_setzero_ps();

        for (size_t i = 0; i < avxSamples; i += 8)
        {
            __m256 inputVec = _mm256_loadu_ps(&input[i]);
            __m256 squaredVec = _mm256_mul_ps(inputVec, inputVec);
            sumVec = _mm256_add_ps(sumVec, squaredVec);
        }

        // Extract sum from AVX register
        alignas(32) float sumArray[8];
        _mm256_store_ps(sumArray, sumVec);
        float sum = 0.0f;
        for (int i = 0; i < 8; ++i)
        {
            sum += sumArray[i];
        }

        // Handle remaining samples
        for (size_t i = avxSamples; i < numSamples; ++i)
        {
            sum += input[i] * input[i];
        }

        return std::sqrt(sum / static_cast<float>(numSamples));
    }

    float SIMDAudio::calculateRMSSSE(const float* input, size_t numSamples)
    {
        if (numSamples == 0) return 0.0f;

        const size_t sseSamples = (numSamples / 4) * 4;
        __m128 sumVec = _mm_setzero_ps();

        for (size_t i = 0; i < sseSamples; i += 4)
        {
            __m128 inputVec = _mm_loadu_ps(&input[i]);
            __m128 squaredVec = _mm_mul_ps(inputVec, inputVec);
            sumVec = _mm_add_ps(sumVec, squaredVec);
        }

        // Extract sum from SSE register
        alignas(16) float sumArray[4];
        _mm_store_ps(sumArray, sumVec);
        float sum = 0.0f;
        for (int i = 0; i < 4; ++i)
        {
            sum += sumArray[i];
        }

        // Handle remaining samples
        for (size_t i = sseSamples; i < numSamples; ++i)
        {
            sum += input[i] * input[i];
        }

        return std::sqrt(sum / static_cast<float>(numSamples));
    }

    float SIMDAudio::calculateRMSFallback(const float* input, size_t numSamples)
    {
        if (numSamples == 0) return 0.0f;

        float sum = 0.0f;
        for (size_t i = 0; i < numSamples; ++i)
        {
            sum += input[i] * input[i];
        }

        return std::sqrt(sum / static_cast<float>(numSamples));
    }
}