/*
 * MorphSnap — Core/FormantMorphEngine.h
 *
 * Cepstral-liftering formant preservation engine.
 *
 * Preserves the formant (spectral envelope) character of the source signal
 * during spectral morphing. Called AFTER SpectralMorphEngine::processBlock()
 * to re-apply the original formant structure to the morphed audio.
 *
 * Algorithm (per frame):
 *   1. Compute log magnitude spectrum of the current (morphed) frame via FFT.
 *   2. IFFT of log spectrum → real cepstrum.
 *   3. Apply rectangular lifter: zero cepstral coefficients above lifterOrder_
 *      (retains only the slowly-varying envelope; discards fine pitch structure).
 *   4. FFT of liftered cepstrum → log spectral envelope (smooth).
 *   5. exp() → linear spectral envelope (source formant).
 *   6. Divide input frame spectrum by its own envelope, multiply by source
 *      envelope (formant transplant).
 *   7. IFFT + overlap-add back to time domain.
 *
 * STFT parameters match SpectralMorphEngine defaults:
 *   FFT size = 2048, Hop = 512, Window = Hann.
 *
 * Threading:
 *   prepare(), reset(), setActive(), setPreservationAmount() — message thread.
 *   processBlock() — audio thread only, noexcept, zero allocations.
 *
 * Design constraints:
 *   - Zero heap allocations after prepare().
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

namespace morphsnap {

/**
 * FormantMorphEngine
 *
 * Extracts spectral envelope via cepstral analysis and transplants it to the
 * morphed audio, preserving vocal/instrument character during parameter morphing.
 *
 * setPreservationAmount(1.0) = full formant preservation (source envelope).
 * setPreservationAmount(0.0) = passthrough (no formant correction).
 */
class FormantMorphEngine
{
public:
    FormantMorphEngine();
    ~FormantMorphEngine();

    FormantMorphEngine(const FormantMorphEngine&)            = delete;
    FormantMorphEngine& operator=(const FormantMorphEngine&) = delete;

    // ─── Setup (message thread) ───────────────────────────────────────────

    /**
     * Allocate cepstral analysis buffers and initialise FFT engine.
     *
     * @param sampleRate    Host sample rate in Hz.
     * @param maxBlockSize  Maximum block size the host will deliver.
     */
    void prepare(double sampleRate, int maxBlockSize);

    /** Flush all analysis state. */
    void reset() noexcept;

    // ─── Audio thread ─────────────────────────────────────────────────────

    /**
     * Re-apply formant envelope to the morphed audio.
     *
     * The engine captures the formant envelope from the first complete STFT
     * frame it sees, then applies it to subsequent frames. This means the
     * "source" formant it preserves is from plugin A's output (the dominant
     * signal at low alpha values, which is what was in bufA before morphing).
     *
     * When preservationAmount_ == 0 or active_ == false, this is a no-op.
     *
     * @param buffer  In/out: morphed audio. Formant-corrected output written here.
     */
    void processBlock(juce::AudioBuffer<float>& buffer) noexcept;

    /** Activate or deactivate. When inactive, processBlock() is a no-op. */
    void setActive(bool active) noexcept;

    /** Query active state (any thread). */
    [[nodiscard]] bool isActive() const noexcept;

    /**
     * Set preservation blend: 0 = no correction, 1 = full source envelope.
     * Values are clamped to [0, 1].
     */
    void setPreservationAmount(float amount) noexcept;

    /** Query preservation amount (any thread). */
    [[nodiscard]] float getPreservationAmount() const noexcept;

    /**
     * Manually provide a reference buffer for formant capture.
     * Call this with plugin A's pre-morph output if available, before
     * processBlock(). When not called, formant is estimated from the
     * incoming buffer itself.
     *
     * noexcept — safe to call from audio thread.
     */
    void captureFormants(const juce::AudioBuffer<float>& reference) noexcept;

private:
    // ── Per-channel state ─────────────────────────────────────────────────

    struct ChannelState
    {
        // Circular input accumulation buffer (length = fftSize)
        std::vector<float> inputBuffer;

        // Overlap-add output buffer (length = fftSize + hopSize)
        std::vector<float> outputBuffer;

        // Captured source formant envelope (length = numBins)
        std::vector<float> sourceEnvelope;

        // Cepstrum scratch buffers (length = fftSize)
        std::vector<float> cepstrumBuffer;

        // Spectral envelope of the current frame (length = numBins)
        std::vector<float> currentEnvelope;

        // FFT scratch (length = fftSize * 2, interleaved complex for JUCE FFT)
        std::vector<float> fftScratch;

        // Time-domain output of IFFT (length = fftSize)
        std::vector<float> ifftOut;

        // Complex spectrum of current frame (length = numBins each)
        std::vector<float> specReal, specImag;

        int writePos = 0;
        int hopCount = 0;

        bool hasSourceEnvelope = false;
    };

    static constexpr int kMaxChannels = 2;
    std::array<ChannelState, kMaxChannels> channels_;

    // ── Shared resources ──────────────────────────────────────────────────

    std::vector<float> window_;
    std::unique_ptr<juce::dsp::FFT> fft_;

    // ── Parameters ────────────────────────────────────────────────────────

    int    fftSize_             = 2048;
    int    hopSize_             = 512;
    int    numBins_             = 0;
    int    lifterOrder_         = 30;   // cepstral lifter cutoff
    double sampleRate_          = 48000.0;

    std::atomic<bool>  active_              { false };
    std::atomic<float> preservationAmount_  { 1.0f  };

    // ── Internal DSP helpers (audio thread, all noexcept) ─────────────────

    /**
     * Process one STFT frame with formant transplant.
     *
     * @param ch     Channel state.
     * @param amount Preservation blend [0, 1].
     */
    void processFrame(ChannelState& ch, float amount) noexcept;

    /** Compute Hann window into window_. */
    void computeHannWindow() noexcept;

    /**
     * Extract log spectral envelope via cepstral liftering.
     *
     * Steps:
     *   1. log(|spectrum[k]| + eps) → log magnitude.
     *   2. IFFT of log magnitude → real cepstrum (scratch must be fftSize*2).
     *   3. Zero cepstral coefficients above lifterOrder_.
     *   4. FFT of liftered cepstrum → smooth log envelope.
     *   5. exp() → linear envelope stored in envelope[].
     *
     * @param real       Real part of spectrum (length = numBins_).
     * @param imag       Imag part of spectrum (length = numBins_).
     * @param envelope   Output linear spectral envelope (length = numBins_).
     * @param cepBuf     Scratch cepstrum buffer (length = fftSize_).
     * @param scratch    JUCE FFT scratch (length = fftSize_ * 2).
     */
    void extractFormantEnvelope(const float* real, const float* imag,
                                float* envelope,
                                float* cepBuf,
                                float* scratch) noexcept;

    /**
     * Apply formant envelope transplant to a complex spectrum.
     *
     * For each bin k:
     *   ratio[k]     = sourceEnv[k] / (currentEnv[k] + eps)
     *   blended[k]   = lerp(1.0, ratio[k], amount)
     *   real[k]     *= blended[k]
     *   imag[k]     *= blended[k]
     *
     * @param real        In/out real part (length = numBins_).
     * @param imag        In/out imag part (length = numBins_).
     * @param sourceEnv   Source (target) spectral envelope.
     * @param currentEnv  Current frame spectral envelope.
     * @param amount      Blend factor [0, 1].
     */
    void applyFormantEnvelope(float* real, float* imag,
                              const float* sourceEnv,
                              const float* currentEnv,
                              float amount) noexcept;

    /**
     * Forward FFT into scratch. Input is windowed time domain (length = fftSize_).
     * Output: deinterleaved real[], imag[] (length = numBins_).
     */
    void forwardFFT(const float* input,
                    float* real, float* imag,
                    float* scratch) noexcept;

    /**
     * Inverse FFT. Input: real[], imag[] (length = numBins_).
     * Output: windowed time-domain samples (length = fftSize_).
     */
    void inverseFFT(const float* real, const float* imag,
                    float* output,
                    float* scratch) noexcept;
};

} // namespace morphsnap
