#pragma once

#include <cstddef>

namespace more_phi
{
    /**
     * SIMD-accelerated audio operations with runtime CPU capability detection
     * Falls back to scalar operations when SIMD instructions not available
     */
    class SIMDAudio
    {
    public:
        // Audio operations
        static void multiplyScalar(const float* input, float scalar, float* output, size_t numSamples);
        static void addScalar(const float* input, float scalar, float* output, size_t numSamples);
        static void multiply(const float* input1, const float* input2, float* output, size_t numSamples);
        static void add(const float* input1, const float* input2, float* output, size_t numSamples);

        // Analysis operations
        static float findPeak(const float* input, size_t numSamples);
        static float calculateRMS(const float* input, size_t numSamples);

        // CPU capability detection
        static bool supportsAVX();
        static bool supportsSSE();

    private:
        // Internal CPU feature detection
        static bool detectAVXSupport();
        static bool detectSSESupport();

        // SIMD implementations
        static void multiplyScalarAVX(const float* input, float scalar, float* output, size_t numSamples);
        static void multiplyScalarSSE(const float* input, float scalar, float* output, size_t numSamples);
        static void multiplyScalarFallback(const float* input, float scalar, float* output, size_t numSamples);

        static void addScalarAVX(const float* input, float scalar, float* output, size_t numSamples);
        static void addScalarSSE(const float* input, float scalar, float* output, size_t numSamples);
        static void addScalarFallback(const float* input, float scalar, float* output, size_t numSamples);

        static void multiplyAVX(const float* input1, const float* input2, float* output, size_t numSamples);
        static void multiplySSE(const float* input1, const float* input2, float* output, size_t numSamples);
        static void multiplyFallback(const float* input1, const float* input2, float* output, size_t numSamples);

        static void addAVX(const float* input1, const float* input2, float* output, size_t numSamples);
        static void addSSE(const float* input1, const float* input2, float* output, size_t numSamples);
        static void addFallback(const float* input1, const float* input2, float* output, size_t numSamples);

        static float findPeakAVX(const float* input, size_t numSamples);
        static float findPeakSSE(const float* input, size_t numSamples);
        static float findPeakFallback(const float* input, size_t numSamples);

        static float calculateRMSAVX(const float* input, size_t numSamples);
        static float calculateRMSSSE(const float* input, size_t numSamples);
        static float calculateRMSFallback(const float* input, size_t numSamples);
    };
}