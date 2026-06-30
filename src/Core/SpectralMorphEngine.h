/*
 * More-Phi — Core/SpectralMorphEngine.h
 *
 * STFT-based audio-domain spectral morph engine.
 *
 * Performs phase-vocoder interpolation between two plugin audio outputs
 * using log-magnitude geometric mean for perceptually linear loudness
 * blending and instantaneous-frequency interpolation for phase continuity.
 *
 * Pipeline position (called from processBlock AFTER both hosted plugins render):
 *
 *   Plugin A → bufferA ─┐
 *   Plugin B → bufferB ─┴─► SpectralMorphEngine::processBlock(bufA, bufB, α)
 *                             └─► FormantMorphEngine::processBlock(bufA)
 *                                   └─► output
 *
 * STFT parameters (defaults):
 *   FFT size N  = 2048 samples  (≈46ms @44.1kHz)
 *   Hop size H  = N/4 = 512     (75% overlap, perfect-reconstruction Hann)
 *   Window      = Hann
 *   Latency     = N + H samples (≈58ms @44.1kHz)
 *
 * Spectral interpolation formula:
 *   |M_morph[k]| = |A[k]|^(1-α) * |B[k]|^α   (log-magnitude geometric mean)
 *
 * Phase interpolation (instantaneous frequency vocoder):
 *   IF_morph[k]  = (1-α)*IF_A[k] + α*IF_B[k]
 *   φ_morph[k]  += IF_morph[k] * H
 *
 * Threading:
 *   prepare(), setFFTSize(), setTransientPreserve(), setFormantPreserve(),
 *   setActive() — message thread only.
 *   processBlock(), reset(), getLatencyInSamples(), isActive() — audio thread.
 *
 * Design constraints:
 *   - Zero heap allocations after prepare() — all vectors sized in prepare().
 *   - All audio-path methods noexcept.
 *   - Max 2 channels (stereo).
 */
#pragma once

#include <juce_dsp/juce_dsp.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <memory>
#include <vector>
#include <atomic>

#include "TransientDetector.h"

namespace more_phi {

/**
 * SpectralMorphEngine
 *
 * Performs frequency-domain cross-fade between two plugin output buffers.
 * Alpha=0 → 100% bufferA, Alpha=1 → 100% bufferB.
 */
class SpectralMorphEngine
{
public:
    SpectralMorphEngine();
    ~SpectralMorphEngine();

    SpectralMorphEngine(const SpectralMorphEngine&)            = delete;
    SpectralMorphEngine& operator=(const SpectralMorphEngine&) = delete;

    // ─── Setup (message thread) ───────────────────────────────────────────

    /**
     * Allocate all STFT buffers and initialise the FFT engine.
     *
     * Must be called from AudioProcessor::prepareToPlay(). Safe to re-call
     * with different parameters (will resize all internal buffers).
     *
     * @param sampleRate    Host sample rate in Hz.
     * @param maxBlockSize  Maximum block size the host will deliver.
     */
    void prepare(double sampleRate, int maxBlockSize);

    /** Select FFT size. Valid values: 512, 1024, 2048, 4096.
     *  Takes effect on the next prepare() call. */
    void setFFTSize(int size);

    /** Enable/disable spectral flux transient snap (default: enabled). */
    void setTransientPreserve(bool enabled) noexcept;

    /**
     * Enable formant-preservation hint.
     * When true, SpectralMorphEngine skips internal formant correction so that
     * FormantMorphEngine can apply its own cepstral envelope transplant.
     */
    void setFormantPreserve(bool enabled) noexcept;

    /** Activate or deactivate the engine. When inactive, processBlock() is a no-op. */
    void setActive(bool active) noexcept;

    // ─── Audio thread ─────────────────────────────────────────────────────

    /** Reset all STFT state (circular buffers, phase accumulators). noexcept. */
    void reset() noexcept;

    /**
     * Morphs bufA toward bufB by factor alpha.
     *
     * Performs overlap-save STFT: accumulates input samples into circular
     * buffers, processes complete hops through the phase vocoder, and writes
     * overlap-added output back into bufA.
     *
     * When inactive (isActive() == false), this is a no-op.
     *
     * @param bufA   In/out: plugin A audio. Spectral morph result written here.
     * @param bufB   Read-only: plugin B audio.
     * @param alpha  Morph position: 0 = pure A, 1 = pure B.
     */
    void processBlock(juce::AudioBuffer<float>& bufA,
                      const juce::AudioBuffer<float>& bufB,
                      float alpha) noexcept;

    // ─── Queries ──────────────────────────────────────────────────────────

    /** Latency in samples at the current sample rate (= fftSize + hopSize). */
    [[nodiscard]] int  getLatencyInSamples() const noexcept;

    /**
     * AUDIT-FIX: worst-case tail length in seconds the engine contributes after
     * input stops. The overlap-add output buffer holds up to (fftSize + hopSize)
     * samples of residual energy (windowed tails from the last analysis frames),
     * so the tail equals the latency. Returns 0 when the engine is inactive.
     * Used by MorePhiProcessor::getTailLengthSeconds() so the DAW compensates
     * offline-bounce tails correctly when audio-domain morph is engaged.
     */
    [[nodiscard]] double getTailLengthSeconds() const noexcept;

    /** True when the engine is active (spectral processing occurs). */
    [[nodiscard]] bool isActive()            const noexcept;

private:
    // ── Per-channel STFT state ────────────────────────────────────────────

    struct ChannelState
    {
        // Circular input accumulation buffers (length = fftSize)
        std::vector<float> inputBufferA;
        std::vector<float> inputBufferB;

        // Overlap-add output buffer (length = fftSize + hopSize)
        std::vector<float> outputBuffer;

        // FFT working buffers (length = fftSize each, real part used for analysis)
        std::vector<float> fftRealA, fftImagA;
        std::vector<float> fftRealB, fftImagB;
        std::vector<float> fftRealOut, fftImagOut;

        // Phase vocoder state (length = numBins each)
        std::vector<float> prevPhaseA;   // previous-frame analysis phase for A
        std::vector<float> prevPhaseB;   // previous-frame analysis phase for B
        std::vector<float> synthPhase;   // running synthesis phase accumulator

        // Magnitude buffers (length = numBins)
        std::vector<float> magA, magB, magOut;

        // Scratch buffer for JUCE FFT (interleaved real/imag, length = fftSize * 2)
        std::vector<float> fftScratch;

        int writePos = 0;  // next write position in circular input buffer
        int hopCount = 0;  // samples accumulated since last hop

        // FIX 2.3: Per-frame overlap-add write head. Advances by hopSize_ for
        // every processed frame so consecutive frames overlap correctly even
        // when more than one hop fires within a single processBlock() call.
        // Decremented by the per-block drain amount so the next block's first
        // frame resumes at the right offset.
        int outWritePos = 0;

        // PERF-OLA (2026-06-30): circular read head for the overlap-add output
        // ring. Replaces the per-block memmove drain (O(fftSize)/block) with
        // O(1) indexing. outReadPos chases outWritePos: processFrame advances
        // outWritePos by hopSize_ per frame; processBlock drains numSamples
        // from outReadPos and advances it by the same amount. The buffer is
        // sized (in prepare) with enough margin that outWritePos never wraps
        // around and overtakes outReadPos before the drain catches up.
        int outReadPos = 0;
    };

    static constexpr int kMaxChannels = 2;
    std::array<ChannelState, kMaxChannels> channels_;

    // ── Shared resources ──────────────────────────────────────────────────

    /** Pre-computed Hann window (length = fftSize). Computed once in prepare(). */
    std::vector<float> window_;

    /** JUCE FFT engine. Replaced on each prepare() if fftSize changes. */
    std::unique_ptr<juce::dsp::FFT> fft_;

    /** Per-channel transient detectors (preserves stereo coherence). */
    std::array<TransientDetector, kMaxChannels> transientDetectors_;

    // ── Parameters ────────────────────────────────────────────────────────

    int    fftSize_        = 2048;
    std::atomic<int> pendingFFTSize_ { 2048 };
    int    hopSize_        = 512;
    int    numBins_        = 0;       // fftSize / 2 + 1, set in prepare()
    double sampleRate_     = 48000.0;
    int    latencySamples_ = 0;       // fftSize + hopSize

    std::atomic<bool> active_            { false };
    std::atomic<bool> transientPreserve_ { true  };
    std::atomic<bool> formantPreserve_   { false };

    // Holds transient-modified alphas computed on channel 0 for each hop in a block.
    // Reused by channel 1 to guarantee stereo phase and timing coherence.
    std::vector<float> blockAlphas_;

    // ── Internal DSP helpers (audio thread, all noexcept) ─────────────────

    void processFrame(ChannelState& ch, float alpha) noexcept;
    void computeHannWindow() noexcept;

    void forwardFFT(const float* input,
                    float* real, float* imag,
                    float* scratch) noexcept;

    void inverseFFT(const float* real, const float* imag,
                    float* output,
                    float* scratch) noexcept;

    void computeMagnitudePhase(const float* real, const float* imag,
                               float* mag, float* phase,
                               int numBins) noexcept;

    void interpolateMagnitude(const float* magA, const float* magB,
                              float* magOut,
                              int numBins, float alpha) noexcept;

    // AUDIT-FIX (phase locking): magnitude inputs drive identity phase locking —
    // each bin's phase advances by the instantaneous frequency of whichever source
    // dominates it (magnitude × (1−α) vs α), instead of a physically-meaningless
    // blended IF. Reduces the classic phase-vocoder "phasiness" on polyphonic
    // material (Laroche & Dolson 1999).
    void interpolatePhase(const float* phaseA, const float* phaseB,
                          const float* magA,    const float* magB,
                          float* prevPhaseA,   float* prevPhaseB,
                          float* synthPhase,
                          int numBins, float alpha) noexcept;
};

} // namespace more_phi
