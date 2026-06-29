/*
 * More-Phi — AI/MelSpectrogram.h
 *
 * 128-bin log-mel spectrogram frontend for the genre classifier's ONNX model.
 *
 * Pipeline: mono PCM -> Hann-windowed STFT (FFT 2048, hop 512) -> power
 * spectrum -> 128 triangular Slaney-scale mel filters -> log (dB) ->
 * per-frame mean/variance normalization. Output is a flat [melBins * frameCount]
 * buffer (row-major [melBins][frameCount]) ready to be reshaped into a model's
 * [1, 128, T] input tensor.
 *
 * The frame count is determined by the input buffer length and the hop size;
 * the caller passes the desired analysis duration in samples via prepare(). The
 * ONNX session reads the resulting frame count from the model's declared input
 * shape and must match — the classifier's loadModel validates this.
 *
 * Real-time-safe: all buffers and the mel filter matrix are pre-allocated in
 * prepare(). process() does pointer arithmetic, one FFT per frame, and the mel
 * matrix-vector product — no allocations, no throws. Message-thread only by
 * contract (matches GenreClassifier's 1 Hz / 30 s cadence), but allocation-free
 * so it could run on a worker too.
 *
 * ponytail: written from scratch (~130 lines) rather than linking the dataset
 * FeatureExtractor, which is 80-band, MFCC-emitting, and dataset-scoped (not in
 * the plugin target). The mel math here is the standard Slaney triangle filter.
 */
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <cstddef>
#include <vector>

namespace more_phi {

class MelSpectrogram
{
public:
    static constexpr int    kMelBins   = 128;
    static constexpr int    kFftSize   = 2048;
    static constexpr int    kHopSize   = 512;
    static constexpr double kMinFreqHz = 0.0;     // mel filterbank low edge
    static constexpr double kRefDbFloor = 1e-10f; // log floor (power)

    MelSpectrogram();

    // Message thread. Sizes all buffers and precomputes the mel filterbank for
    // the given sample rate. `inputSamples` is the length of the mono buffer
    // process() will receive (e.g. sampleRate * 10 for a 10 s window). The
    // frame count = 1 + max(0, (inputSamples - kFftSize) / kHopSize).
    void prepare(double sampleRate, std::size_t inputSamples) noexcept;

    void reset() noexcept;

    // Returns the number of STFT frames the current config produces. 0 before
    // prepare(). Used by the classifier to size its model input buffer.
    [[nodiscard]] std::size_t getFrameCount() const noexcept { return frameCount_; }

    // Process `numSamples` of mono audio into the log-mel output buffer. The
    // output is written to the internal buffer returned by getOutput() (flat
    // [kMelBins * frameCount], row-major [mel][frame]). noexcept, no alloc.
    // numSamples must match the value passed to prepare(); extra samples are
    // ignored, fewer samples zero-pads the last frame (deterministic).
    void process(const float* mono, std::size_t numSamples) noexcept;

    // Direct access to the last process() output. Valid until the next process().
    [[nodiscard]] const float* getOutput() const noexcept { return melOutput_.data(); }
    [[nodiscard]] std::size_t getOutputCount() const noexcept { return melOutput_.size(); }

private:
    void computeHannWindow() noexcept;
    void computeMelFilterbank(double sampleRate) noexcept;

    double sampleRate_ = 48000.0;
    std::size_t inputSamples_ = 0;
    std::size_t frameCount_ = 0;
    int numBins_ = 0;          // kFftSize/2 + 1

    std::unique_ptr<juce::dsp::FFT> fft_;
    std::vector<float> window_;       // Hann, kFftSize
    std::vector<float> frame_;        // kFftSize working frame (time domain)
    std::vector<float> fftScratch_;   // kFftSize*2 for the in-place complex FFT
    std::vector<float> powerSpectrum_;// numBins_ power per frame

    // Mel filterbank as a flat [kMelBins * numBins_] matrix. Row r multiplies
    // the power spectrum to yield mel bin r. Sparse in principle; stored dense
    // for cache-friendly matrix-vector product (numBins_ ~ 1025, so ~131 KB).
    std::vector<float> melFilter_;
    std::vector<float> melOutput_;    // [kMelBins * frameCount_], row-major [mel][frame]
};

} // namespace more_phi
