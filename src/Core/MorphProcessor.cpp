/*
 * MorphSnap — Core/MorphProcessor.cpp
 * SIMD-optimized smoothing for real-time performance.
 */
#include "MorphProcessor.h"
#include <cmath>
#include <algorithm>

// SIMD headers
#if defined(__AVX2__)
    #include <immintrin.h>
    #define MORPHSNAP_USE_AVX 1
#elif defined(__SSE2__) || (defined(_MSC_VER) && defined(_M_X64))
    #include <emmintrin.h>
    #define MORPHSNAP_USE_SSE 1
#endif

namespace morphsnap {

MorphProcessor::MorphProcessor(SnapshotBank& bank)
    : bank_(bank)
{
}

void MorphProcessor::prepare(int maxParamCount)
{
    smoothedValues_.resize(static_cast<size_t>(maxParamCount), 0.0f);
    trail_.fill({0.5f, 0.5f});
    trailHead_ = 0;
    driftTime_ = 0.0f;
    prepared_ = true;
}

void MorphProcessor::process(float rawX, float rawY, float faderPos,
                              MorphSource source, MorphMode mode,
                              float dt, std::vector<float>& output) noexcept
{
    if (!prepared_ || !bank_.hasAnyOccupied()) return;

    // 1) Apply physics to cursor position
    if (source == MorphSource::XYPad)
    {
        // Map [0,1] to [-1,1] for interpolation engine
        float targetX = rawX * 2.0f - 1.0f;
        float targetY = rawY * 2.0f - 1.0f;
        updatePhysics(targetX, targetY, mode, dt);

        // Compute interpolated parameter values using physics-processed position
        InterpolationEngine::compute2D(processedX_, processedY_, bank_, output);
    }
    else // Fader
    {
        InterpolationEngine::compute1D(faderPos, bank_, output);
        // Store fader position as "processed" for trail
        processedX_ = faderPos;
        processedY_ = 0.0f;
    }

    // 2) Apply per-parameter smoothing (SIMD optimized)
    applySmoothing(output);

    // 3) Update cursor trail for visualization
    trailTimer_ += dt;
    if (trailTimer_ >= TRAIL_INTERVAL)
    {
        trailTimer_ -= TRAIL_INTERVAL;
        // Map back to [0,1] for UI
        trail_[trailHead_] = {
            (processedX_ + 1.0f) * 0.5f,
            (processedY_ + 1.0f) * 0.5f
        };
        trailHead_.store((trailHead_.load(std::memory_order_relaxed) + 1) % TRAIL_SIZE,
                         std::memory_order_relaxed);
    }
}

void MorphProcessor::updatePhysics(float targetX, float targetY,
                                     MorphMode mode, float dt)
{
    switch (mode)
    {
        case MorphMode::Direct:
            processedX_ = targetX;
            processedY_ = targetY;
            break;

        case MorphMode::Elastic:
            PhysicsEngine::updateElastic(elasticState_, targetX, targetY,
                                          elasticPreset_, dt);
            processedX_ = std::clamp(elasticState_.x, -1.0f, 1.0f);
            processedY_ = std::clamp(elasticState_.y, -1.0f, 1.0f);
            break;

        case MorphMode::Drift:
            driftTime_ += dt;
            PhysicsEngine::updateDrift(processedX_, processedY_,
                                        driftTime_, driftSpeed_, driftDistance_,
                                        driftChaos_, driftMode_,
                                        targetX, targetY, 0.5f);
            processedX_ = std::clamp(processedX_, -1.0f, 1.0f);
            processedY_ = std::clamp(processedY_, -1.0f, 1.0f);
            break;
    }
}

void MorphProcessor::applySmoothing(std::vector<float>& output)
{
    // CRITICAL: Never resize in audio thread - output should fit pre-allocated buffer
    const size_t maxSmoothable = std::min(output.size(), smoothedValues_.size());

    const float rate = smoothRate_;
    const float oneMinusRate = 1.0f - rate;

#if defined(MORPHSNAP_USE_AVX)
    // AVX2 SIMD path - 8 floats at once
    __m256 rateVec = _mm256_set1_ps(rate);
    __m256 oneMinusRateVec = _mm256_set1_ps(oneMinusRate);
    
    const size_t simdCount = maxSmoothable - (maxSmoothable % 8);
    
    for (size_t i = 0; i < simdCount; i += 8)
    {
        __m256 smoothed = _mm256_loadu_ps(smoothedValues_.data() + i);
        __m256 out = _mm256_loadu_ps(output.data() + i);
        
        // smoothed = smoothed * rate + output * (1 - rate)
        __m256 newSmoothed = _mm256_add_ps(
            _mm256_mul_ps(smoothed, rateVec),
            _mm256_mul_ps(out, oneMinusRateVec));
        
        _mm256_storeu_ps(smoothedValues_.data() + i, newSmoothed);
        _mm256_storeu_ps(output.data() + i, newSmoothed);
    }
    
    // Handle remaining elements
    for (size_t i = simdCount; i < maxSmoothable; ++i)
    {
        smoothedValues_[i] = smoothedValues_[i] * rate + output[i] * oneMinusRate;
        output[i] = smoothedValues_[i];
    }

#elif defined(MORPHSNAP_USE_SSE)
    // SSE2 SIMD path - 4 floats at once
    __m128 rateVec = _mm_set1_ps(rate);
    __m128 oneMinusRateVec = _mm_set1_ps(oneMinusRate);
    
    const size_t simdCount = maxSmoothable - (maxSmoothable % 4);
    
    for (size_t i = 0; i < simdCount; i += 4)
    {
        __m128 smoothed = _mm_loadu_ps(smoothedValues_.data() + i);
        __m128 out = _mm_loadu_ps(output.data() + i);
        
        __m128 newSmoothed = _mm_add_ps(
            _mm_mul_ps(smoothed, rateVec),
            _mm_mul_ps(out, oneMinusRateVec));
        
        _mm_storeu_ps(smoothedValues_.data() + i, newSmoothed);
        _mm_storeu_ps(output.data() + i, newSmoothed);
    }
    
    // Handle remaining elements
    for (size_t i = simdCount; i < maxSmoothable; ++i)
    {
        smoothedValues_[i] = smoothedValues_[i] * rate + output[i] * oneMinusRate;
        output[i] = smoothedValues_[i];
    }

#else
    // Scalar fallback
    for (size_t i = 0; i < maxSmoothable; ++i)
    {
        smoothedValues_[i] = smoothedValues_[i] * rate + output[i] * oneMinusRate;
        output[i] = smoothedValues_[i];
    }
#endif
}

} // namespace morphsnap
