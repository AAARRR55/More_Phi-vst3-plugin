/*
 * More-Phi — Core/ISpectralMorphEngine.h
 * Abstract interface for STFT-based spectral morphing between audio buffers.
 *
 * Thread safety contract
 * ----------------------
 *   Audio thread  : processBlock(), reset(), setTransientPreserve(),
 *                   setFormantPreserve(), getLatencyInSamples(), getConfig()
 *   Message thread: prepare(), setFFTSize()
 *
 * processBlock() must not allocate heap memory or throw after prepare() has
 * been called. All working buffers (FFT scratch, overlap-add accumulator,
 * magnitude/phase arrays) must be pre-allocated in prepare().
 *
 * Latency
 * -------
 * STFT processing introduces inherent algorithmic latency. Implementations
 * must report their exact latency via getLatencyInSamples() so that the host
 * can compensate via AudioProcessor::getLatencySamples().
 *
 * Typical values:
 *   FFT_512  → ~256 samples at 50% overlap
 *   FFT_2048 → ~1024 samples at 50% overlap
 *
 * FFT size changes
 * ----------------
 * setFFTSize() queues the new size; the change takes effect on the next call
 * to prepare(). Calling setFFTSize() during real-time operation without
 * subsequently calling prepare() is safe but has no immediate effect.
 */
#pragma once

#include "SpectralTypes.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace more_phi {

/**
 * STFT-based spectral morphing engine interface.
 *
 * Transforms two audio streams (A and B) into the frequency domain using
 * short-time Fourier transforms, cross-fades their magnitude and phase
 * spectra according to alpha, then reconstructs the output via overlap-add
 * synthesis in place of bufA.
 *
 * After processBlock() returns, bufA contains the morphed audio signal.
 * bufB is read-only and must not be modified by the implementation.
 *
 * alpha interpretation:
 *   0.0 → 100% bufA (no spectral change)
 *   0.5 → equal blend of both spectra
 *   1.0 → 100% bufB spectrum imposed on bufA timing
 */
class ISpectralMorphEngine
{
public:
    virtual ~ISpectralMorphEngine() = default;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /**
     * Prepares the engine for audio processing.
     * Pre-allocates all FFT working buffers, overlap-add accumulators, and
     * analysis windows for the currently configured FFT size. Call this
     * whenever the host sample rate or block size changes, or after a call
     * to setFFTSize().
     *
     * @param sampleRate  Host sample rate in Hz (> 0).
     * @param blockSize   Maximum number of samples per processBlock() call (> 0).
     *
     * Message-thread only. Must not be called concurrently with processBlock().
     */
    virtual void prepare(double sampleRate, int blockSize) = 0;

    /**
     * Clears all overlap-add accumulator state and resets the write/read
     * pointers to their initial positions.
     * Does not modify FFT size or flag settings.
     *
     * noexcept: No heap allocation or exceptions.
     * Audio-thread safe.
     */
    virtual void reset() noexcept = 0;

    // -----------------------------------------------------------------------
    // Audio-thread processing
    // -----------------------------------------------------------------------

    /**
     * Processes one block of audio: analyses bufA and bufB, cross-fades
     * their frequency-domain representations at ratio alpha, and writes the
     * reconstructed output back into bufA.
     *
     * @param bufA   Input/output buffer (source A and morphed output).
     *               Channel count and sample count must match those passed to
     *               prepare(); behaviour is undefined otherwise.
     * @param bufB   Read-only source B buffer. Must have the same channel
     *               count and sample count as bufA.
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
    // Configuration queries (audio-thread safe unless noted)
    // -----------------------------------------------------------------------

    /**
     * Returns the algorithmic latency introduced by the STFT analysis window.
     * The host should report this value via AudioProcessor::getLatencySamples().
     *
     * noexcept: Returns a pre-computed value, no side effects.
     * Audio-thread safe.
     */
    virtual int getLatencyInSamples() const noexcept = 0;

    /**
     * Queues an FFT size change. The new size takes effect on the next call
     * to prepare(). Has no immediate effect on real-time processing.
     *
     * @param size  One of the FFTSize enumeration values.
     *
     * Message-thread only.
     */
    virtual void setFFTSize(FFTSize size) = 0;

    /**
     * Enables or disables transient preservation.
     * When enabled, the engine detects attack transients in bufA and restores
     * them after spectral crossfade to reduce phasiness on percussive material.
     *
     * Changes take effect at the next processBlock() call.
     *
     * noexcept: Implementations should use an atomic flag.
     * Audio-thread safe.
     */
    virtual void setTransientPreserve(bool enabled) noexcept = 0;

    /**
     * Enables or disables formant preservation.
     * When enabled, the engine normalises the spectral envelope before
     * crossfade and re-applies it after, preserving vocal/instrument formants
     * regardless of alpha.
     *
     * Changes take effect at the next processBlock() call.
     *
     * noexcept: Implementations should use an atomic flag.
     * Audio-thread safe.
     */
    virtual void setFormantPreserve(bool enabled) noexcept = 0;

    /**
     * Returns a snapshot of the current spectral configuration.
     * The returned struct reflects the configuration that was active at the
     * last prepare() call (FFT size, hop size, overlap ratio) plus the
     * current real-time flag states (transient/formant preserve).
     *
     * noexcept: Returns a copy of a pre-computed struct.
     * Audio-thread safe.
     */
    virtual SpectralConfig getConfig() const noexcept = 0;
};

} // namespace more_phi
