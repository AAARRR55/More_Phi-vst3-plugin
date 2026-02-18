/*
 * MorphSnap — Core/MorphProcessor.cpp
 */
#include "MorphProcessor.h"
#include <cmath>
#include <algorithm>

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
                              float dt, std::vector<float>& output)
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

    // 2) Apply per-parameter smoothing
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
        trailHead_ = (trailHead_ + 1) % TRAIL_SIZE;
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
    // If output is larger, we just smooth what we can (defensive programming)
    const size_t maxSmoothable = std::min(output.size(), smoothedValues_.size());

    const float rate = smoothRate_;
    const float oneMinusRate = 1.0f - rate;

    for (size_t i = 0; i < maxSmoothable; ++i)
    {
        smoothedValues_[i] = smoothedValues_[i] * rate + output[i] * oneMinusRate;
        output[i] = smoothedValues_[i];
    }
}

} // namespace morphsnap
