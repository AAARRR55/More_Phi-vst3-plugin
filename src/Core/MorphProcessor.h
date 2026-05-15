/*
 * More-Phi — Core/MorphProcessor.h
 * Orchestrates morph computation: physics → interpolation → smoothing.
 * All methods are audio-thread safe (no allocations after prepare()).
 *
 * noexcept guarantee: process() is noexcept because all buffers are
 * pre-allocated in prepare() and all called functions are noexcept.
 */
#pragma once

#include "SnapshotBank.h"
#include "InterpolationEngine.h"
#include "PhysicsEngine.h"
#include <vector>
#include <array>
#include <atomic>
#include <memory>

namespace more_phi {

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
    // noexcept: All buffers pre-allocated in prepare(), no allocations or throwing ops
    void process(float rawX, float rawY, float faderPos,
                 MorphSource source, MorphMode mode,
                 float dt, std::vector<float>& output) noexcept;

    // Physics tuning (ATS-M7+M8: all atomics for cross-thread safety)
    void setElasticPreset(ElasticPreset p) { elasticPreset_.store(static_cast<int>(p), std::memory_order_relaxed); }
    void setDriftSpeed(float s)    { driftSpeed_.store(s, std::memory_order_relaxed); }
    void setDriftDistance(float d)  { driftDistance_.store(d, std::memory_order_relaxed); }
    void setDriftChaos(float c)    { driftChaos_.store(c, std::memory_order_relaxed); }
    void setDriftMode(DriftMode m) { driftMode_.store(static_cast<int>(m), std::memory_order_relaxed); }
    void setSmoothingRate(float r) { smoothRate_.store(r, std::memory_order_relaxed); }

    // Listen Mode: when enabled, discrete parameters are excluded from morph output
    void setListenMode(bool enabled) { listenMode_.store(enabled, std::memory_order_relaxed); }
    bool getListenMode() const { return listenMode_.load(std::memory_order_relaxed); }

    // ATS-H2: Double-buffer pattern — truly lock-free (no shared_ptr ref-count contention)
    void setDiscreteMap(const std::vector<bool>& map)
    {
        int current = discreteActiveIndex_.load(std::memory_order_acquire);
        int next = 1 - current;
        if (!discreteBuffers_[next])
            discreteBuffers_[next] = std::make_unique<DiscreteMask>();
        auto& buf = *discreteBuffers_[next];
        buf.resize(map.size());
        for (size_t i = 0; i < map.size(); ++i)
            buf[i] = map[i] ? 1u : 0u;
        discreteActiveIndex_.store(next, std::memory_order_release);
    }
    void setDiscreteMap(std::vector<bool>&& map)
    {
        setDiscreteMap(map);
    }

    // Sentinel value written to morph output for params that should NOT be applied
    static constexpr float SKIP_SENTINEL = -1.0f;

    // Read the physics-processed cursor position (for UI trail)
    float getProcessedX() const { return processedX_; }
    float getProcessedY() const { return processedY_; }

    // Cursor trail ring buffer (written by audio, read by UI)
    static constexpr int TRAIL_SIZE = 64;
    struct TrailPoint { float x, y; };
    const std::array<TrailPoint, TRAIL_SIZE>& getTrail() const { return trail_; }
    int getTrailHead() const { return trailHead_.load(std::memory_order_relaxed); }

private:
    void updatePhysics(float targetX, float targetY, MorphMode mode, float dt);
    void applySmoothing(std::vector<float>& output);

    SnapshotBank& bank_;

    // Physics state (ATS-M7+M8: enum/float params now atomic for thread safety)
    ElasticState elasticState_;
    std::atomic<int> elasticPreset_{static_cast<int>(ElasticPreset::Medium)};
    std::atomic<int> driftMode_{static_cast<int>(DriftMode::Free)};
    std::atomic<float> driftSpeed_{0.3f};
    std::atomic<float> driftDistance_{0.4f};
    std::atomic<float> driftChaos_{0.5f};
    float driftTime_     = 0.0f;

    // Processed position (after physics)
    float processedX_ = 0.5f;
    float processedY_ = 0.5f;

    // Smoothing
    std::atomic<float> smoothRate_{0.95f};
    std::vector<float> smoothedValues_;

    // Trail
    std::array<TrailPoint, TRAIL_SIZE> trail_{};
    std::atomic<int> trailHead_{0};
    float trailTimer_ = 0.0f;
    static constexpr float TRAIL_INTERVAL = 0.016f;  // ~60 Hz trail sampling

    bool prepared_ = false;

    // Listen Mode
    std::atomic<bool> listenMode_{false};
    // ATS-H2: Double-buffer for truly lock-free discrete map reads on audio thread
    using DiscreteMask = std::vector<uint8_t>;
    std::array<std::unique_ptr<DiscreteMask>, 2> discreteBuffers_{};
    std::atomic<int> discreteActiveIndex_{0};
    void applyListenFilter(std::vector<float>& output) noexcept;
};

} // namespace more_phi
