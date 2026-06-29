/*
 * More-Phi — Core/MorphProcessor.cpp
 * SIMD-optimized smoothing for real-time performance.
 */
#include "MorphProcessor.h"
#include <cmath>
#include <algorithm>

// SIMD headers
#if defined(__AVX2__)
    #include <immintrin.h>
    #define MORE_PHI_USE_AVX 1
#elif defined(__SSE2__) || (defined(_MSC_VER) && defined(_M_X64))
    #include <emmintrin.h>
    #define MORE_PHI_USE_SSE 1
#endif

namespace more_phi {

MorphProcessor::MorphProcessor(SnapshotBank& bank)
    : bank_(bank)
{
}

void MorphProcessor::prepare(int maxParamCount)
{
    smoothedValues_.resize(static_cast<size_t>(maxParamCount), 0.0f);
    // C3 FIX: initialize atomic-packed trail points to centre {0.5, 0.5}.
    TrailPoint centre{ 0.5f, 0.5f };
    uint64_t packedCentre = 0;
    std::memcpy(&packedCentre, &centre, sizeof(packedCentre));
    for (auto& slot : trail_)
        slot.store(packedCentre, std::memory_order_relaxed);
    trailHead_.store(0, std::memory_order_relaxed);

    // C-3 FIX: Reset all physics state so stale values from a previous
    // sample rate / block size don't cause large initial displacement.
    // NOTE: processedX_/Y_ are kept at 0.5 (their default member initializer)
    // for state/back-compat. W-2 (mode-switch stale state) is handled separately
    // in updatePhysics, which re-seeds elasticState_ from the CURRENT cursor on
    // any transition into Elastic — so the init value here does not cause a
    // first-block jump (the seed runs on the first Elastic block regardless).
    elasticState_ = ElasticState{};
    processedX_.store(0.5f, std::memory_order_relaxed);
    processedY_.store(0.5f, std::memory_order_relaxed);
    driftTime_ = 0.0f;

    // L3: Pre-initialize InterpolationEngine's function-local static
    // (kUnitPositions) on the message thread so the first audio-thread call
    // avoids any potential static-init mutex.
    InterpolationEngine::getClockPositions();

    prepared_.store(true, std::memory_order_release);
}

void MorphProcessor::process(float rawX, float rawY, float faderPos,
                              MorphSource source, MorphMode mode,
                              float dt, std::span<float> output) noexcept
{
    if (!prepared_.load(std::memory_order_acquire) || !bank_.hasAnyOccupied()) return;

    // 1) Apply physics to cursor position
    if (source == MorphSource::XYPad)
    {
        float targetX = rawX * 2.0f - 1.0f;
        float targetY = rawY * 2.0f - 1.0f;
        updatePhysics(targetX, targetY, mode, dt);

        // 1b) Compute interpolated parameter values using physics-processed position
        const int interpMode = interpolationMode_.load(std::memory_order_relaxed);
        if (interpMode == 1 && voronoiEngine_.isValid())
            InterpolationEngine::compute2D_Voronoi(processedX_.load(std::memory_order_relaxed), processedY_.load(std::memory_order_relaxed), bank_, voronoiEngine_, output);
        else
            InterpolationEngine::compute2D(processedX_.load(std::memory_order_relaxed), processedY_.load(std::memory_order_relaxed), bank_, output);
    }
    else // Fader
    {
        InterpolationEngine::compute1D(faderPos, bank_, output);
        processedX_.store(faderPos, std::memory_order_relaxed);
        processedY_.store(0.0f, std::memory_order_relaxed);
    }

    // 2) Apply per-parameter smoothing (SIMD optimized)
    // C-4 FIX (audit): Direct mode used to skip smoothing entirely, so a
    // discontinuous cursor move produced a full-vector jump → click on
    // unsmoothed hosted params. Direct mode now gets a guaranteed minimum
    // de-zipper (computeSmoothingRateFor forces kDirectMinSmoothingTau);
    // Elastic/Drift honor the user's smoothTau_ as before.
    {
        const float rate = computeSmoothingRateFor(mode, dt);
        if (rate < 0.999f)   // 0.999 == effectively no smoothing needed
            applySmoothing(output, rate);
    }

    // 3) Apply Listen Mode filter — mark discrete params as "skip"
    applyListenFilter(output);

    // 4) Update cursor trail for visualization
    trailTimer_ += dt;
    if (trailTimer_ >= TRAIL_INTERVAL)
    {
        trailTimer_ -= TRAIL_INTERVAL;
        // C3 FIX: publish {x,y} as one atomic 64-bit store so the UI reads a
        // consistent pair (no torn x/y across blocks).
        TrailPoint p{
            (processedX_.load(std::memory_order_relaxed) + 1.0f) * 0.5f,
            (processedY_.load(std::memory_order_relaxed) + 1.0f) * 0.5f
        };
        uint64_t packed = 0;
        std::memcpy(&packed, &p, sizeof(packed));
        const int head = trailHead_.load(std::memory_order_relaxed);
        trail_[static_cast<size_t>(head)].store(packed, std::memory_order_relaxed);
        trailHead_.store((head + 1) % TRAIL_SIZE, std::memory_order_relaxed);
    }
}

void MorphProcessor::updatePhysics(float targetX, float targetY,
                                     MorphMode mode, float dt) noexcept
{
    switch (mode)
    {
        case MorphMode::Direct:
            processedX_.store(targetX, std::memory_order_relaxed);
            processedY_.store(targetY, std::memory_order_relaxed);
            break;

        case MorphMode::Elastic:
            // W-2 FIX (audit): when entering Elastic from a different mode (or
            // on the first block), seed the spring state from the current
            // cursor so there is no one-block jump toward target from stale
            // {0,0} state. processedX_/Y_ are in [-1,1] (same space as
            // elasticState_.x/y). Zero the velocity for a clean start.
            if (lastPhysicsMode_ != MorphMode::Elastic)
            {
                elasticState_.x  = processedX_.load(std::memory_order_relaxed);
                elasticState_.y  = processedY_.load(std::memory_order_relaxed);
                elasticState_.vx = 0.0f;
                elasticState_.vy = 0.0f;
            }
            PhysicsEngine::updateElastic(elasticState_, targetX, targetY,
                                          static_cast<ElasticPreset>(elasticPreset_.load(std::memory_order_relaxed)), dt);
            // AUDIT-FIX (C6): reflect the published-cursor clamp back into the
            // spring state. Without this, an underdamped preset (Slow/Medium,
            // ζ<1) overshoots ±1 internally while the readout shows a flat top —
            // the pad "sticks then jumps" as the hidden state keeps ringing.
            // Zeroing velocity on clamp kills the underlying oscillation cleanly.
            {
                const float clampedX = std::clamp(elasticState_.x, -1.0f, 1.0f);
                const float clampedY = std::clamp(elasticState_.y, -1.0f, 1.0f);
                if (clampedX != elasticState_.x) { elasticState_.x = clampedX; elasticState_.vx = 0.0f; }
                if (clampedY != elasticState_.y) { elasticState_.y = clampedY; elasticState_.vy = 0.0f; }
                processedX_.store(clampedX, std::memory_order_relaxed);
                processedY_.store(clampedY, std::memory_order_relaxed);
            }
            break;

        case MorphMode::Drift:
        {
            // AUDIT-FIX (C5): let driftTime_ grow and wrap on the perlin-space
            // coordinate instead (see PhysicsEngine::updateDrift). The previous
            // `if (driftTime_ > 256.0f) driftTime_ -= 256.0f` wrap was continuous
            // in perlin() only when `speed` was an integer; for non-integer speed
            // it produced a step discontinuity in perlin(time*speed) every
            // 256/speed seconds. Coarse reset at 1e9 avoids double-precision drift
            // over multi-hour sessions while preserving perlin's own periodicity.
            driftTime_ += dt;
            if (driftTime_ > 1e9f) driftTime_ -= 1e6f;
            float px = processedX_.load(std::memory_order_relaxed);
            float py = processedY_.load(std::memory_order_relaxed);
            PhysicsEngine::updateDrift(px, py,
                                        driftTime_,
                                        driftSpeed_.load(std::memory_order_relaxed),
                                        driftDistance_.load(std::memory_order_relaxed),
                                        driftChaos_.load(std::memory_order_relaxed),
                                        static_cast<DriftMode>(driftMode_.load(std::memory_order_relaxed)),
                                        targetX, targetY, 0.5f);
            processedX_.store(std::clamp(px, -1.0f, 1.0f), std::memory_order_relaxed);
            processedY_.store(std::clamp(py, -1.0f, 1.0f), std::memory_order_relaxed);
            break;
        }
    }

    // W-2 FIX (audit): record the mode so the next call can detect a transition
    // into Elastic and re-seed the spring state (see the Elastic case above).
    lastPhysicsMode_ = mode;
}

float MorphProcessor::computeSmoothingRateFor(MorphMode mode, float dt) noexcept
{
    // Fix 2.1 (preserved): derive the one-pole coefficient from the smoothing
    // time constant τ and the actual block duration. This makes the smoothing
    // response independent of host sample rate / block size: a given user
    // "smoothing" setting yields the same effective time constant everywhere.
    const float safeDt = (dt > 0.0f) ? dt : kRefDt;

    float tau;
    if (mode == MorphMode::Direct)
    {
        // C-4 FIX (audit): Direct mode always de-zippers with a fast minimum
        // time constant, ignoring the user's smoothTau_ (which is intended for
        // Elastic/Drift). This suppresses clicks from discontinuous moves
        // while keeping Direct feeling instant.
        tau = kDirectMinSmoothingTau;
    }
    else
    {
        tau = smoothTau_.load(std::memory_order_relaxed);
        // W-3 FIX (audit): Drift mode writes a fresh Perlin cursor sample every
        // block. If the user disabled smoothing (tau <= 0), the cursor — and
        // therefore every interpolated hosted parameter — would jump block-to-
        // block, producing zipper noise / clicks. Clamp to a safety floor so a
        // continuous trajectory is always guaranteed. Elastic doesn't need this:
        // its spring-damper already produces a continuous trajectory even with
        // smoothing off, so we honor the user's explicit "no smoothing" there.
        if (tau <= 0.0f)
        {
            if (mode == MorphMode::Drift)
                tau = kDriftMinSmoothingTau;
            else
                return 0.0f;               // Elastic + user disabled → instant track
        }
    }

    float rate = std::exp(-safeDt / tau);
    if (rate < 0.0f) rate = 0.0f;
    if (rate > 0.999f) rate = 0.999f;
    return rate;
}

void MorphProcessor::applySmoothing(std::span<float> output, float rate) noexcept
{
    // CRITICAL: Never resize in audio thread - output should fit pre-allocated buffer
    const size_t maxSmoothable = std::min(output.size(), smoothedValues_.size());

    // C-4 FIX (audit): rate is now computed by computeSmoothingRateFor and
    // passed in. rate == 0 means instant tracking (no smoothing).
    const float oneMinusRate = 1.0f - rate;

#if defined(MORE_PHI_USE_AVX)
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

#elif defined(MORE_PHI_USE_SSE)
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

void MorphProcessor::applyListenFilter(std::span<float> output) noexcept
{
    if (!listenMode_.load(std::memory_order_relaxed)) return;

    // ATS-H2: Double-buffer read — truly lock-free, no ref-count contention
    auto idx = discreteActiveIndex_.load(std::memory_order_acquire);
    auto* discreteMap = discreteBuffers_[idx].get();
    if (!discreteMap || discreteMap->empty()) return;

    const size_t count = std::min(output.size(), discreteMap->size());
    for (size_t i = 0; i < count; ++i)
    {
        if ((*discreteMap)[i] != 0)
            output[i] = SKIP_SENTINEL;
    }
}

void MorphProcessor::rebuildVoronoi()
{
    const auto positions = InterpolationEngine::getClockPositions();
    voronoiEngine_.rebuild(positions, bank_);
}

} // namespace more_phi
