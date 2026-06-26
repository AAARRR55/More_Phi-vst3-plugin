/*
 * More-Phi — Core/GranularMorphEngine.h
 *
 * Granular synthesis crossfade between two hosted-plugin audio streams.
 * Grains are extracted from both source A and source B circular buffers,
 * with per-grain amplitude, pitch, and position randomisation applied.
 * Morph alpha controls the blend: 0.0 = all-A grains, 1.0 = all-B grains.
 *
 * Design constraints:
 *   - Zero heap allocations after prepare().
 *   - All audio-path methods are noexcept.
 *   - PRNG is a simple xorshift32 — no <random>, no heap.
 *   - Source audio is captured into pre-allocated circular buffers
 *     (mono mixdown, kCircularBufferSize = 65536 samples ≈ 1.4 s @ 48 kHz).
 *   - Grain envelope is owned by GrainPool and pre-computed in prepare().
 *
 * Threading:
 *   prepare() and reset() — message thread (prepareToPlay / releaseResources).
 *   processBlock()        — audio thread only.
 *   set*() configuration  — plain value stores; read at top of processBlock().
 *
 * Usage:
 *   GranularMorphEngine engine;
 *   engine.prepare(sampleRate, maxBlockSize);
 *   engine.setGrainSize(50.0f);
 *   engine.setGrainDensity(20.0f);
 *   engine.setActive(true);
 *
 *   // Audio thread:
 *   engine.processBlock(bufA, bufB, alpha);
 *   // bufA now contains granular morph output.
 */
#pragma once

#include "GrainPool.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <vector>
#include <cstdint>
#include <cmath>
#include <atomic>

namespace more_phi {

/**
 * GranularMorphEngine
 *
 * Reads from two pre-buffered audio sources and synthesises a grain cloud
 * that crossfades smoothly between them according to morph alpha.
 */
class GranularMorphEngine
{
public:
    // ─── Constants ────────────────────────────────────────────────────────────

    /** Circular buffer length per source (samples).
     *  65536 ≈ 1.365 s at 48 kHz — comfortably larger than the longest grain. */
    static constexpr int kCircularBufferSize = 65536;

    /** Minimum / maximum grain size in milliseconds. */
    static constexpr float kGrainSizeMinMs = 20.0f;
    static constexpr float kGrainSizeMaxMs = 200.0f;

    /** Minimum / maximum grain density in grains per second. */
    static constexpr float kGrainDensityMin =  5.0f;
    static constexpr float kGrainDensityMax = 100.0f;

    /** Maximum pitch randomisation in semitones. */
    static constexpr float kPitchRandomMaxSemitones = 2.0f;

    // ─── Lifecycle ────────────────────────────────────────────────────────────

    GranularMorphEngine();
    ~GranularMorphEngine();

    GranularMorphEngine(const GranularMorphEngine&)            = delete;
    GranularMorphEngine& operator=(const GranularMorphEngine&) = delete;

    /**
     * Allocate internal buffers and prepare the grain pool.
     * Must be called from prepareToPlay() on the message thread.
     *
     * @param sampleRate    Host sample rate in Hz.
     * @param maxBlockSize  Maximum samples per processBlock() call.
     */
    void prepare(double sampleRate, int maxBlockSize);

    /** Zero all circular buffers and deactivate all grains. */
    void reset() noexcept;

    // ─── Audio thread ─────────────────────────────────────────────────────────

    /**
     * Perform granular crossfade between bufA and bufB.
     *
     * Captures mono mixdowns of both inputs into circular buffers, schedules
     * new grains, renders the active grain cloud, and writes the result back
     * into bufA (all channels).
     *
     * If the engine is inactive (setActive(false)), bufA is left unchanged.
     *
     * @param bufA   Source A — modified in place to hold the output.
     * @param bufB   Source B — read-only.
     * @param alpha  Morph position: 0.0 = all A, 1.0 = all B.
     */
    void processBlock(juce::AudioBuffer<float>& bufA,
                      const juce::AudioBuffer<float>& bufB,
                      float alpha) noexcept;

    // ─── Configuration ────────────────────────────────────────────────────────

    /** Grain size in milliseconds [20, 200]. */
    void setGrainSize(float ms) noexcept;

    /** Grain emission rate in grains per second [5, 100]. */
    void setGrainDensity(float grainsPerSec) noexcept;

    /** Maximum random pitch deviation in semitones [0, 2]. */
    void setPitchRandomization(float semitones) noexcept;

    /** Position randomisation amount [0, 1] — fraction of circular buffer length. */
    void setPositionRandomization(float amount) noexcept;

    /** Enable / disable granular processing. */
    void setActive(bool active) noexcept;

    /** Returns true when the engine is active. */
    [[nodiscard]] bool isActive() const noexcept;

    /**
     * AUDIT-FIX: worst-case tail length in seconds. Granular playback can emit
     * grains already in flight (up to grainSizeMs_) after input stops, plus the
     * circular-buffer read tail. Reported as grainSizeMs_ / 1000 so the DAW
     * compensates offline-bounce tails. Returns 0 when inactive.
     */
    [[nodiscard]] double getTailLengthSeconds() const noexcept;

private:
    // ─── Internal types ───────────────────────────────────────────────────────

    /** Mono circular write buffer for one audio source. */
    struct SourceBuffer
    {
        std::array<float, kCircularBufferSize> data{};
        int writePos = 0;
    };

    // ─── PRNG ─────────────────────────────────────────────────────────────────

    /**
     * xorshift32 — fast, deterministic, no heap, no <random>.
     * Returns a value in [0, 1).
     */
    float nextRandom() noexcept;

    // ─── Internal audio-thread helpers ────────────────────────────────────────

    /**
     * Feed new samples from a host audio block into the mono circular buffer
     * for the given source index (0 = A, 1 = B).
     * Mixes down all channels to mono before writing.
     */
    void feedSourceBuffer(int sourceIndex,
                          const float* const* channelData,
                          int numChannels,
                          int numSamples) noexcept;

    /**
     * Schedule new grains for the current block based on grain density and
     * morph alpha.  May activate grains from source A, source B, or both
     * depending on the blend ratio.
     *
     * @param alpha      Morph position [0, 1].
     * @param blockSize  Number of samples in the current block.
     */
    void scheduleGrains(float alpha, int blockSize) noexcept;

    /**
     * Render all active grains into the pre-allocated mixBuffer_.
     * Uses the Hann envelope from GrainPool for smooth onset/release.
     *
     * @param output      Pointer to a zeroed float buffer of numSamples.
     * @param numSamples  Number of output samples to produce.
     */
    void renderGrains(float* output, int numSamples) noexcept;

    // ─── Members ──────────────────────────────────────────────────────────────

    GrainPool pool_;

    std::array<SourceBuffer, 2> sourceBuffers_;  // [0] = A, [1] = B

    // Grain parameters — written by set*() methods, read in processBlock().
    float grainSizeMs_    = 50.0f;
    float grainDensity_   = 20.0f;
    float pitchRandom_    = 0.0f;
    float positionRandom_ = 0.0f;

    // Grain scheduler accumulator — tracks fractional inter-grain interval.
    float schedulerAccum_ = 0.0f;

    double sampleRate_         = 48000.0;
    std::atomic<bool> active_  {false};

    // Pre-allocated mono mix buffer for grain output (maxBlockSize samples).
    std::vector<float> mixBuffer_;

    // H-2 FIX: Pre-computed pitch LUT for 2^(n/12), n in [-12..+12].
    // Avoids std::pow() on the audio thread.
    static constexpr int kPitchLUTSize = 25;  // -12 to +12 inclusive
    std::array<float, kPitchLUTSize> pitchLUT_{};

    // xorshift32 state — initialised to a non-zero seed.
    uint32_t rngState_ = 12345u;
};

} // namespace more_phi
