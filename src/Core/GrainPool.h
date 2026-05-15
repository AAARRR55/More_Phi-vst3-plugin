/*
 * More-Phi — Core/GrainPool.h
 *
 * Pre-allocated pool of granular synthesis grains with a pre-computed
 * Hann envelope. Designed for zero-allocation use on the audio thread
 * after prepare() is called.
 *
 * Design constraints:
 *   - MAX_GRAINS = 128 grains, all statically allocated via std::array.
 *   - Hann envelope is computed once in prepare() into a std::vector
 *     pre-sized to maxGrainSamples, never reallocated thereafter.
 *   - All audio-path methods are noexcept.
 *   - Not thread-safe — must be called exclusively from the audio thread.
 *
 * Usage:
 *   GrainPool pool;
 *   pool.prepare(maxGrainLengthInSamples);
 *
 *   // Activate a grain:
 *   int idx = pool.activate(0, amplitude, startSample, grainLength, pitchRatio);
 *   if (idx >= 0) { ... }
 *
 *   // Iterate all active grains:
 *   pool.forEachActive([](Grain& g, int idx) { ... });
 */
#pragma once

#include <array>
#include <cmath>
#include <vector>
#include <cstdint>

#ifndef M_PI
  #define M_PI 3.14159265358979323846
#endif

namespace more_phi {

/**
 * A single granular synthesis grain.
 *
 * Fields are plain data — no virtual functions, no allocations.
 * The pool owns the Hann envelope; grains only store playback state.
 */
struct Grain
{
    bool  active       = false;   ///< Whether this slot is in use
    int   sourceSelect = 0;       ///< 0 = source A circular buffer, 1 = source B
    float amplitude    = 1.0f;    ///< Mix amplitude based on morph alpha
    int   startSample  = 0;       ///< Start offset in the source circular buffer
    int   grainLength  = 0;       ///< Total length of this grain, in samples
    int   currentPos   = 0;       ///< Playback cursor within [0, grainLength)
    float pitchRatio   = 1.0f;    ///< Pitch shift factor — 1.0 = no shift
    float pan          = 0.5f;    ///< Stereo panning: 0.0 = full L, 1.0 = full R
};

/**
 * GrainPool
 *
 * Thread safety:
 *   All methods must be called from the audio thread only.
 *   prepare() and reset() must be called from the message thread
 *   (prepareToPlay / releaseResources) and never concurrently with
 *   audio-thread calls.
 */
class GrainPool
{
public:
    static constexpr int MAX_GRAINS = 128;

    GrainPool()  = default;
    ~GrainPool() = default;

    GrainPool(const GrainPool&)            = delete;
    GrainPool& operator=(const GrainPool&) = delete;

    // ─── Configuration (message thread only) ──────────────────────────────────

    /**
     * Pre-allocate the Hann envelope buffer for grains of up to
     * maxGrainSamples length.  Must be called from prepareToPlay().
     *
     * @param maxGrainSamples  Maximum grain length in samples.
     *                         Must be > 0.
     */
    void prepare(int maxGrainSamples) noexcept
    {
        if (maxGrainSamples <= 0)
            return;

        maxGrainSamples_ = maxGrainSamples;

        // Pre-compute a full-resolution Hann window.
        // envelope_[i] = 0.5 * (1 - cos(2π * i / (N-1)))  for N points.
        // index 0 and N-1 both equal 0 (smooth onset and release).
        envelope_.resize(static_cast<size_t>(maxGrainSamples));

        const double invN = 1.0 / static_cast<double>(maxGrainSamples - 1);
        for (int i = 0; i < maxGrainSamples; ++i)
        {
            const double phase = 2.0 * M_PI * static_cast<double>(i) * invN;
            envelope_[static_cast<size_t>(i)] =
                static_cast<float>(0.5 * (1.0 - std::cos(phase)));
        }

        reset();
    }

    /** Deactivate all grains and zero playback state. */
    void reset() noexcept
    {
        for (auto& g : grains_)
        {
            g.active      = false;
            g.currentPos  = 0;
            g.grainLength = 0;
            g.startSample = 0;
            g.amplitude   = 1.0f;
            g.pitchRatio  = 1.0f;
            g.pan         = 0.5f;
            g.sourceSelect = 0;
        }
        activeCount_ = 0;
    }

    // ─── Audio thread interface ───────────────────────────────────────────────

    /**
     * Activate an unused grain slot with the supplied parameters.
     *
     * @param sourceSelect  0 = circular buffer A, 1 = circular buffer B
     * @param amplitude     Output amplitude [0, 1]
     * @param startSample   Read offset in the source circular buffer
     * @param grainLength   Length of this grain in samples (clamped to maxGrainSamples)
     * @param pitchRatio    Playback speed ratio (1.0 = no shift)
     * @returns             Grain index on success, -1 when pool is exhausted
     */
    [[nodiscard]] int activate(int   sourceSelect,
                               float amplitude,
                               int   startSample,
                               int   grainLength,
                               float pitchRatio) noexcept
    {
        // Find a free slot via linear scan — MAX_GRAINS = 128, acceptable cost.
        for (int i = 0; i < MAX_GRAINS; ++i)
        {
            if (!grains_[i].active)
            {
                Grain& g      = grains_[i];
                g.active       = true;
                g.sourceSelect = sourceSelect;
                g.amplitude    = amplitude;
                g.startSample  = startSample;
                g.grainLength  = (grainLength > maxGrainSamples_)
                                     ? maxGrainSamples_
                                     : grainLength;
                g.currentPos   = 0;
                g.pitchRatio   = pitchRatio;
                g.pan          = 0.5f;
                ++activeCount_;
                return i;
            }
        }
        return -1; // Pool exhausted
    }

    /**
     * Deactivate a grain slot by index.
     * Safe to call with an already-inactive slot (idempotent).
     */
    void deactivate(int index) noexcept
    {
        if (index < 0 || index >= MAX_GRAINS)
            return;

        if (grains_[index].active)
        {
            grains_[index].active = false;
            if (activeCount_ > 0)
                --activeCount_;
        }
    }

    // ─── Access ───────────────────────────────────────────────────────────────

    Grain& operator[](int index) noexcept
    {
        return grains_[static_cast<size_t>(index)];
    }

    const Grain& operator[](int index) const noexcept
    {
        return grains_[static_cast<size_t>(index)];
    }

    /** Number of currently active grains. */
    [[nodiscard]] int getActiveCount() const noexcept
    {
        return activeCount_;
    }

    /**
     * Iterate every active grain.
     *
     * Fn signature: void(Grain& grain, int grainIndex)
     *
     * The callback may call deactivate(grainIndex) to retire a finished grain.
     * Safe because we iterate by index and check active state on each call.
     */
    template<typename Fn>
    void forEachActive(Fn&& fn) noexcept
    {
        static_assert(noexcept(fn(std::declval<Grain&>(), 0)),
            "forEachActive callback must be noexcept for audio thread safety");
        for (int i = 0; i < MAX_GRAINS; ++i)
        {
            if (grains_[i].active)
                fn(grains_[i], i);
        }
    }

    /**
     * Sample the pre-computed Hann envelope at a normalised position.
     *
     * @param normalizedPos  Position within the grain: 0.0 = onset, 1.0 = release.
     *                       Values outside [0, 1] are clamped.
     * @returns              Envelope amplitude in [0, 1].
     */
    [[nodiscard]] float getEnvelope(float normalizedPos) const noexcept
    {
        if (envelope_.empty())
            return 1.0f;

        // Clamp to valid range
        if (normalizedPos <= 0.0f) return envelope_.front();
        if (normalizedPos >= 1.0f) return envelope_.back();

        // Map normalised position to a sample index with linear interpolation
        // so that grains of any length shorter than maxGrainSamples_ still get
        // a smooth, full-window envelope shape.
        const float  fIdx = normalizedPos * static_cast<float>(maxGrainSamples_ - 1);
        const int    lo   = static_cast<int>(fIdx);
        const int    hi   = lo + 1;
        const float  frac = fIdx - static_cast<float>(lo);

        const float envLo = envelope_[static_cast<size_t>(lo)];
        const float envHi = (hi < maxGrainSamples_)
                                ? envelope_[static_cast<size_t>(hi)]
                                : envelope_.back();

        return envLo + frac * (envHi - envLo);
    }

private:
    std::array<Grain, MAX_GRAINS> grains_{};
    int activeCount_    = 0;

    // Pre-computed Hann envelope — sized to maxGrainSamples_ in prepare().
    std::vector<float> envelope_;
    int maxGrainSamples_ = 0;
};

} // namespace more_phi
