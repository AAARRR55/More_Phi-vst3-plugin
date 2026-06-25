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
#include "VoronoiMorphEngine.h"
#include "PhysicsEngine.h"
#include <vector>
#include <array>
#include <atomic>
#include <memory>
#include <cmath>
#include <cstdint>
#include <cstring>

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

    // Sets the smoothing "feel". `s` is the legacy [0, 0.999] coefficient value
    // stored in the APVTS `smoothing` parameter and in presets. Internally we
    // derive a time constant τ such that, at the reference block config
    // (512 samples @ 44.1 kHz, dt = kRefDt), the per-block one-pole coefficient
    // reproduces s exactly — so the in-box feel is bit-identical there.
    // Everywhere else (different sample rate / block size) the same τ drives
    // α = exp(-dt/τ), making the smoothing behavior sample-rate independent.
    //   s <= 0   → τ = 0 → no smoothing (instant tracking)
    //   s → 1    → τ → ∞ (heavily smoothed)
    void setSmoothingRate(float r) {
        const float s = std::clamp(r, 0.0f, 0.999f);
        smoothRate_.store(s, std::memory_order_relaxed);
        const float tau = (s <= 0.0f) ? 0.0f : -kRefDt / std::log(s);
        smoothTau_.store(tau, std::memory_order_relaxed);
    }

    // Listen Mode: when enabled, discrete parameters are excluded from morph output
    void setListenMode(bool enabled) { listenMode_.store(enabled, std::memory_order_relaxed); }
    bool getListenMode() const { return listenMode_.load(std::memory_order_relaxed); }

    // ATS-H2: Double-buffer pattern — truly lock-free (no shared_ptr ref-count contention)
    // W5 FIX: take the map by value. The previous && overload forwarded back
    // into the const& overload by name (decaying to an lvalue ref), so no move
    // ever happened — the signature was misleading. By-value is honest: callers
    // can pass an lvalue (copied once) or a moved/temporary (move-constructed
    // into this parameter), and the body always iterates a contiguous vector.
    void setDiscreteMap(std::vector<bool> map)
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

    // Sentinel value written to morph output for params that should NOT be applied
    static constexpr float SKIP_SENTINEL = -1.0f;

    // Read the physics-processed cursor position (for UI trail)
    float getProcessedX() const { return processedX_.load(std::memory_order_relaxed); }
    float getProcessedY() const { return processedY_.load(std::memory_order_relaxed); }

    // Interpolation mode: 0=IDW (legacy), 1=Voronoi/NNI (Phase 2)
    void setInterpolationMode(int mode) { interpolationMode_.store(mode, std::memory_order_relaxed); }
    int getInterpolationMode() const { return interpolationMode_.load(std::memory_order_relaxed); }

    // Rebuild Delaunay triangulation for Voronoi morphing.
    // Must be called on the message thread (allocates).
    void rebuildVoronoi();
    const VoronoiMorphEngine& getVoronoiEngine() const { return voronoiEngine_; }

    // Cursor trail ring buffer (written by audio, read by UI)
    // C3 FIX: Each trail point is published as a single atomic 64-bit value
    // (two floats packed together) so the UI paint thread reads a consistent
    // {x,y} pair with no torn-read data race. The old std::array<TrailPoint>
    // was written element-wise by the audio thread and read element-wise by
    // the UI thread — UB under the C++ memory model.
    static constexpr int TRAIL_SIZE = 64;
    struct TrailPoint { float x, y; };
    TrailPoint getTrailPoint(int index) const
    {
        const uint64_t packed = trail_[static_cast<size_t>(index)].load(std::memory_order_relaxed);
        TrailPoint p;
        std::memcpy(&p, &packed, sizeof(p));   // type-pun via memcpy (no aliasing UB)
        return p;
    }
    int getTrailHead() const { return trailHead_.load(std::memory_order_relaxed); }

private:
    void updatePhysics(float targetX, float targetY, MorphMode mode, float dt) noexcept;

    VoronoiMorphEngine voronoiEngine_;
    std::atomic<int> interpolationMode_{0};
    // Fix 2.1: dt is the block duration in seconds (blockSize / sampleRate).
    // The one-pole coefficient is derived from the smoothing time constant τ
    // via α = exp(-dt/τ), making the smoothing behavior independent of host
    // sample rate and block size.
    // C-4 FIX (audit): applySmoothing now takes an explicit one-pole rate
    // (computed by computeSmoothingRateFor below) so Direct mode can opt into
    // a guaranteed fast de-zipper without affecting Elastic/Drift behavior.
    void applySmoothing(std::vector<float>& output, float rate) noexcept;

    // C-4 FIX (audit): Derives the per-block one-pole rate from the user's
    // smoothTau_ and the active morph mode. Non-Direct modes honor smoothTau_
    // (0 → instant, >0 → τ-second one-pole). Direct mode ALWAYS applies a
    // minimum de-zipper (kDirectMinSmoothingTau) so a discontinuous pad move
    // / automation step can't produce a full-vector jump → click, while still
    // feeling effectively instant (~2 ms settle). Returns the clamped rate.
    float computeSmoothingRateFor(MorphMode mode, float dt) noexcept;

    // C-4 FIX (audit): minimum de-zipper time constant for Direct mode.
    // ~2 ms: fast enough to feel instant, slow enough to suppress clicks from
    // discontinuous cursor moves on unsmoothed hosted params (gain, output).
    static constexpr float kDirectMinSmoothingTau = 0.002f;

    // W-3 FIX (audit): minimum safety de-zipper for Drift mode when the user
    // has disabled smoothing (smoothTau_ <= 0). Without it, updateDrift writes
    // a fresh Perlin sample into the cursor every block, so with smoothing off
    // the cursor (and thus every interpolated param) jumps block-to-block →
    // zipper noise / clicks. This slower-than-Direct floor (~20 ms) preserves
    // Drift's organic feel while guaranteeing a continuous cursor trajectory.
    static constexpr float kDriftMinSmoothingTau = 0.020f;

    // Reference block config — kRefDt is defined in ParameterState.h as a
    // namespace-level constant shared by all consumers.

    SnapshotBank& bank_;

    // Physics state (ATS-M7+M8: enum/float params now atomic for thread safety)
    ElasticState elasticState_;
    // W-2 FIX (audit): tracks the mode used on the previous updatePhysics call
    // so a transition INTO Elastic re-seeds elasticState_ from the current
    // cursor (no one-block jump from stale spring state). Audio-thread only.
    MorphMode lastPhysicsMode_{ MorphMode::Direct };
    std::atomic<int> elasticPreset_{static_cast<int>(ElasticPreset::Medium)};
    std::atomic<int> driftMode_{static_cast<int>(DriftMode::Free)};
    std::atomic<float> driftSpeed_{0.3f};
    std::atomic<float> driftDistance_{0.4f};
    std::atomic<float> driftChaos_{0.5f};
    float driftTime_     = 0.0f;

    // Processed position (after physics) — audio thread writes, UI thread reads.
    std::atomic<float> processedX_{ 0.5f };
    std::atomic<float> processedY_{ 0.5f };

    // Smoothing. smoothRate_ is the legacy [0,0.999] coefficient (kept for
    // API/state compatibility). smoothTau_ is the derived time constant in
    // seconds used by the actual one-pole filter (Fix 2.1).
    std::atomic<float> smoothRate_{0.95f};
    std::atomic<float> smoothTau_{0.226f};   // −kRefDt/ln(0.95) ≈ 0.226 s
    std::vector<float> smoothedValues_;

    // Trail
    // C3 FIX: atomic-packed trail points (two floats per uint64_t) — see getTrailPoint.
    std::array<std::atomic<uint64_t>, TRAIL_SIZE> trail_{};
    std::atomic<int> trailHead_{0};
    float trailTimer_ = 0.0f;
    static constexpr float TRAIL_INTERVAL = 0.016f;  // ~60 Hz trail sampling

    std::atomic<bool> prepared_{false};

    // Listen Mode
    std::atomic<bool> listenMode_{false};
    // ATS-H2: Double-buffer for truly lock-free discrete map reads on audio thread
    using DiscreteMask = std::vector<uint8_t>;
    std::array<std::unique_ptr<DiscreteMask>, 2> discreteBuffers_{};
    std::atomic<int> discreteActiveIndex_{0};
    void applyListenFilter(std::vector<float>& output) noexcept;
};

} // namespace more_phi
