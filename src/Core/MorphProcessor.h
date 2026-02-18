/*
 * MorphSnap — Core/MorphProcessor.h
 * Orchestrates morph computation: physics → interpolation → smoothing.
 * All methods are audio-thread safe (no allocations after prepare()).
 */
#pragma once

#include "SnapshotBank.h"
#include "InterpolationEngine.h"
#include "PhysicsEngine.h"
#include <vector>
#include <array>
#include <atomic>

namespace morphsnap {

enum class MorphMode   { Direct = 0, Elastic = 1, Drift = 2 };
enum class MorphSource { XYPad = 0, Fader = 1 };

class MorphProcessor
{
public:
    explicit MorphProcessor(SnapshotBank& bank);

    // Call once from prepareToPlay — pre-allocates all buffers
    void prepare(int maxParamCount);

    // Main compute — call from processBlock
    // rawX, rawY ∈ [0,1], faderPos ∈ [0,1]
    // dt = blockSize / sampleRate
    void process(float rawX, float rawY, float faderPos,
                 MorphSource source, MorphMode mode,
                 float dt, std::vector<float>& output);

    // Physics tuning
    void setElasticPreset(ElasticPreset p) { elasticPreset_ = p; }
    void setDriftSpeed(float s)    { driftSpeed_ = s; }
    void setDriftDistance(float d)  { driftDistance_ = d; }
    void setDriftChaos(float c)    { driftChaos_ = c; }
    void setDriftMode(DriftMode m) { driftMode_ = m; }
    void setSmoothingRate(float r) { smoothRate_ = r; }  // 0=instant, 0.999=very slow

    // Read the physics-processed cursor position (for UI trail)
    float getProcessedX() const { return processedX_; }
    float getProcessedY() const { return processedY_; }

    // Cursor trail ring buffer (written by audio, read by UI)
    static constexpr int TRAIL_SIZE = 64;
    struct TrailPoint { float x, y; };
    const std::array<TrailPoint, TRAIL_SIZE>& getTrail() const { return trail_; }
    int getTrailHead() const { return trailHead_; }

private:
    void updatePhysics(float targetX, float targetY, MorphMode mode, float dt);
    void applySmoothing(std::vector<float>& output);

    SnapshotBank& bank_;

    // Physics state
    ElasticState elasticState_;
    ElasticPreset elasticPreset_ = ElasticPreset::Medium;
    DriftMode driftMode_ = DriftMode::Free;
    float driftSpeed_    = 0.3f;
    float driftDistance_  = 0.4f;
    float driftChaos_    = 0.5f;
    float driftTime_     = 0.0f;

    // Processed position (after physics)
    float processedX_ = 0.5f;
    float processedY_ = 0.5f;

    // Smoothing
    float smoothRate_ = 0.95f;
    std::vector<float> smoothedValues_;

    // Trail
    std::array<TrailPoint, TRAIL_SIZE> trail_{};
    int trailHead_ = 0;
    float trailTimer_ = 0.0f;
    static constexpr float TRAIL_INTERVAL = 0.016f;  // ~60 Hz trail sampling

    bool prepared_ = false;
};

} // namespace morphsnap
