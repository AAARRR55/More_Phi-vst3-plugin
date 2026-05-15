/*
 * More-Phi — Core/IGranularMorphEngine.h
 * Abstract interface for granular synthesis morphing between audio streams.
 *
 * Thread safety contract
 * ----------------------
 *   Audio thread  : processBlock(), reset(), setGrainSize(),
 *                   setGrainDensity(), setPitchRandomization(), getConfig()
 *   Message thread: prepare()
 *
 * processBlock() must not allocate heap memory or throw after prepare() has
 * been called. The grain pool and all scheduling structures must be
 * pre-allocated in prepare() up to GranularConfig::maxGrains.
 *
 * Grain scheduling model
 * ----------------------
 * Each call to processBlock() may spawn new grains (up to maxGrains active
 * at any time) and advance all currently active grains by blockSize samples.
 * Grains are crossfaded using a Hann window to avoid clicks at grain
 * boundaries. The output is a superposition of:
 *
 *   grains drawn from bufA * (1 - alpha) +
 *   grains drawn from bufB * alpha
 *
 * written in-place into bufA.
 *
 * Position randomisation scatters the per-grain read pointer within a window
 * centred on the current playback position, measured as a fraction of bufA/
 * bufB size. A value of 0 produces synchronous, deterministic grains; 1 fully
 * randomises read position.
 */
#pragma once

#include "SpectralTypes.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace more_phi {

/**
 * Granular synthesis morphing engine interface.
 *
 * Slices bufA and bufB into short overlapping grains, applies per-grain
 * amplitude envelopes (Hann window), and superimposes them to produce the
 * output signal in bufA. The ratio of A-grains to B-grains is controlled by
 * alpha: at 0.0 all grains are drawn from bufA, at 1.0 all grains are drawn
 * from bufB.
 *
 * Grain parameters (size, density, pitch randomisation) may be updated on
 * the audio thread via their respective setters; implementations must use
 * atomic reads of these parameters inside processBlock().
 */
class IGranularMorphEngine
{
public:
    virtual ~IGranularMorphEngine() = default;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /**
     * Prepares the engine for audio processing.
     * Allocates the grain pool (up to GranularConfig::maxGrains grains),
     * internal read/write buffers, and any lookup tables required by the
     * envelope and pitch-shift stages. Call this whenever the host sample
     * rate or block size changes.
     *
     * @param sampleRate  Host sample rate in Hz (> 0).
     * @param blockSize   Maximum number of samples per processBlock() call (> 0).
     *
     * Message-thread only. Must not be called concurrently with processBlock().
     */
    virtual void prepare(double sampleRate, int blockSize) = 0;

    /**
     * Immediately kills all active grains and resets the grain scheduler.
     * Does not modify configuration parameters (grain size, density, etc.).
     *
     * noexcept: No heap allocation or exceptions.
     * Audio-thread safe.
     */
    virtual void reset() noexcept = 0;

    // -----------------------------------------------------------------------
    // Audio-thread processing
    // -----------------------------------------------------------------------

    /**
     * Renders one block of granular-morphed audio into bufA.
     *
     * The engine schedules new grains according to the current grain density,
     * advances all active grains by blockSize samples, and sums their windowed
     * output. Source grains are read from bufA (source A) or bufB (source B)
     * according to alpha.
     *
     * @param bufA   Input/output buffer (source A and morphed output).
     *               Must have the same channel count and sample count as
     *               supplied to prepare(); behaviour is undefined otherwise.
     * @param bufB   Read-only source B buffer. Must match bufA dimensions.
     * @param alpha  Morph position [0.0, 1.0]; 0 = pure A, 1 = pure B.
     *               Values outside [0, 1] are clamped by implementations.
     *
     * noexcept: No heap allocation or exceptions after prepare().
     * Audio-thread only.
     */
    virtual void processBlock(juce::AudioBuffer<float>& bufA,
                              const juce::AudioBuffer<float>& bufB,
                              float alpha) noexcept = 0;

    // -----------------------------------------------------------------------
    // Real-time parameter control (audio-thread safe)
    // -----------------------------------------------------------------------

    /**
     * Sets the grain duration.
     * Affects grains spawned after this call; already-active grains continue
     * at their original size to avoid clicks.
     *
     * @param ms  Grain size in milliseconds [20.0, 200.0].
     *            Values outside this range are clamped by implementations.
     *
     * noexcept: Implementations should store via an atomic.
     * Audio-thread safe.
     */
    virtual void setGrainSize(float ms) noexcept = 0;

    /**
     * Sets the grain emission rate.
     * Lower values produce sparse, separated grains; higher values produce
     * dense, overlapping textures.
     *
     * @param grainsPerSecond  Target grain rate (> 0). Actual rate is
     *                         quantised to block boundaries.
     *
     * noexcept: Implementations should store via an atomic.
     * Audio-thread safe.
     */
    virtual void setGrainDensity(float grainsPerSecond) noexcept = 0;

    /**
     * Sets per-grain pitch randomisation.
     * Each new grain's playback rate is multiplied by a random factor
     * derived from this range (centred on 1.0, i.e., unshifted).
     *
     * @param semitones  Maximum random pitch deviation in semitones [0.0, 12.0].
     *                   0.0 disables pitch randomisation.
     *
     * noexcept: Implementations should store via an atomic.
     * Audio-thread safe.
     */
    virtual void setPitchRandomization(float semitones) noexcept = 0;

    // -----------------------------------------------------------------------
    // Configuration query (audio-thread safe)
    // -----------------------------------------------------------------------

    /**
     * Returns a snapshot of the current granular configuration reflecting the
     * most recently applied parameter values.
     *
     * noexcept: Returns a copy of atomically-read configuration fields.
     * Audio-thread safe.
     */
    virtual GranularConfig getConfig() const noexcept = 0;
};

} // namespace more_phi
