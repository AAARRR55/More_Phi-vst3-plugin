/*
 * MorphSnap -- AI/Dataset/PhaseVocoder.h
 * Phase vocoder implementation for time-stretching and pitch-shifting.
 * Uses JUCE DSP FFT with overlap-add resynthesis.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <cmath>
#include <memory>
#include <complex>

namespace morphsnap {

class PhaseVocoder
{
public:
    PhaseVocoder() = default;
    ~PhaseVocoder() = default;

    // Non-copyable due to FFT state
    PhaseVocoder(const PhaseVocoder&) = delete;
    PhaseVocoder& operator=(const PhaseVocoder&) = delete;
    PhaseVocoder(PhaseVocoder&&) = default;
    PhaseVocoder& operator=(PhaseVocoder&&) = default;

    /**
     * Prepare the vocoder for processing at the given sample rate.
     * @param sampleRate  Sample rate in Hz
     * @param fftSize     FFT size (must be power of 2, default 2048)
     */
    void prepare(double sampleRate, int fftSize = 2048)
    {
        sampleRate_ = sampleRate;
        fftSize_ = fftSize;
        hopSize_ = fftSize_ / 4;  // 75% overlap

        // JUCE FFT order is log2(size)
        const int order = static_cast<int>(std::log2(static_cast<double>(fftSize_)));
        fft_ = std::make_unique<juce::dsp::FFT>(order);

        // Initialize buffers
        initializeBuffers();

        prepared_ = true;
    }

    /**
     * Apply time-stretching to the audio buffer.
     * @param buffer        Audio buffer to modify in place
     * @param stretchRatio  Speed multiplier: <1.0 = slower, >1.0 = faster
     * @param rng           Random number generator for phase randomization
     */
    void processTimeStretch(juce::AudioBuffer<float>& buffer,
                            float stretchRatio,
                            juce::Random& rng)
    {
        if (!prepared_ || buffer.getNumSamples() == 0 || stretchRatio <= 0.0f)
            return;

        const int numChannels = buffer.getNumChannels();
        const int numSamples = buffer.getNumSamples();

        // Clamp stretch ratio to reasonable bounds
        stretchRatio = juce::jlimit(0.25f, 4.0f, stretchRatio);

        // For stretch ratio of 1.0, no processing needed
        if (std::abs(stretchRatio - 1.0f) < 0.001f)
            return;

        // Store original audio for processing
        juce::AudioBuffer<float> inputBuffer(numChannels, numSamples);
        for (int ch = 0; ch < numChannels; ++ch)
            inputBuffer.copyFrom(ch, 0, buffer, ch, 0, numSamples);

        // Calculate output size based on stretch ratio
        // stretchRatio > 1 means faster (shorter output), < 1 means slower (longer output)
        const int outputSize = static_cast<int>(std::ceil(numSamples / stretchRatio));

        // Allocate output buffer
        juce::AudioBuffer<float> outputBuffer(numChannels, outputSize);
        outputBuffer.clear();

        // Process each channel
        for (int ch = 0; ch < numChannels; ++ch)
        {
            processChannelTimeStretch(
                inputBuffer.getReadPointer(ch),
                outputBuffer.getWritePointer(ch),
                numSamples,
                outputSize,
                stretchRatio,
                rng
            );
        }

        // Resample output back to original buffer size if needed
        // For simplicity, we keep the output size and resize the buffer
        buffer.setSize(numChannels, outputSize, true);

        for (int ch = 0; ch < numChannels; ++ch)
            buffer.copyFrom(ch, 0, outputBuffer, ch, 0, outputSize);
    }

    /**
     * Apply pitch-shifting to the audio buffer.
     * @param buffer      Audio buffer to modify in place
     * @param semitones   Pitch shift in semitones (-12 to +12)
     * @param rng         Random number generator
     */
    void processPitchShift(juce::AudioBuffer<float>& buffer,
                           float semitones,
                           juce::Random& rng)
    {
        if (!prepared_ || buffer.getNumSamples() == 0 || std::abs(semitones) < 0.01f)
            return;

        // Ratio for pitch shift
        const float pitchRatio = std::pow(2.0f, semitones / 12.0f);
        
        // 1. Time stretch by 1/pitchRatio to change speed but keep pitch
        // 2. Resample back to original length to change pitch but keep speed
        
        const int originalSamples = buffer.getNumSamples();
        const float stretchRatio = 1.0f / pitchRatio;
        
        processTimeStretch(buffer, stretchRatio, rng);
        
        // Now resample back to originalSamples
        const int stretchedSamples = buffer.getNumSamples();
        if (stretchedSamples == originalSamples)
            return;

        juce::AudioBuffer<float> resampledBuffer(buffer.getNumChannels(), originalSamples);
        
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const float* src = buffer.getReadPointer(ch);
            float* dest = resampledBuffer.getWritePointer(ch);
            
            for (int i = 0; i < originalSamples; ++i)
            {
                float srcPos = (static_cast<float>(i) * stretchedSamples) / originalSamples;
                int idx = static_cast<int>(srcPos);
                float frac = srcPos - idx;
                
                if (idx + 1 < stretchedSamples)
                    dest[i] = src[idx] * (1.0f - frac) + src[idx + 1] * frac;
                else
                    dest[i] = src[idx];
            }
        }
        
        buffer.setSize(buffer.getNumChannels(), originalSamples, false);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.copyFrom(ch, 0, resampledBuffer, ch, 0, originalSamples);
    }

private:
    std::unique_ptr<juce::dsp::FFT> fft_;
    double sampleRate_ = 48000.0;
    int fftSize_ = 2048;
    int hopSize_ = 512;
    bool prepared_ = false;

    // Working buffers
    std::vector<float> window_;
    std::vector<float> inputBuffer_;
    std::vector<float> outputBuffer_;
    std::vector<std::complex<float>> fftBuffer_;
    std::vector<float> magnitudes_;
    std::vector<float> phases_;
    std::vector<float> previousPhases_;
    std::vector<float> phaseAccumulators_;

    void initializeBuffers()
    {
        // Create Hann window
        window_.resize(static_cast<size_t>(fftSize_));
        for (int i = 0; i < fftSize_; ++i)
        {
            window_[i] = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i / (fftSize_ - 1)));
        }

        // Allocate working buffers
        inputBuffer_.resize(static_cast<size_t>(fftSize_), 0.0f);
        outputBuffer_.resize(static_cast<size_t>(fftSize_), 0.0f);
        fftBuffer_.resize(static_cast<size_t>(fftSize_));
        magnitudes_.resize(static_cast<size_t>(fftSize_ / 2 + 1));
        phases_.resize(static_cast<size_t>(fftSize_ / 2 + 1));
        previousPhases_.resize(static_cast<size_t>(fftSize_ / 2 + 1), 0.0f);
        phaseAccumulators_.resize(static_cast<size_t>(fftSize_ / 2 + 1), 0.0f);
    }

    void processChannelTimeStretch(const float* input,
                                   float* output,
                                   int inputSize,
                                   int outputSize,
                                   float stretchRatio,
                                   [[maybe_unused]] juce::Random& rng)
    {
        // Reset phase accumulators for each channel
        std::fill(phaseAccumulators_.begin(), phaseAccumulators_.end(), 0.0f);
        std::fill(previousPhases_.begin(), previousPhases_.end(), 0.0f);

        // Analysis hop size (input)
        const int analysisHop = hopSize_;
        // Synthesis hop size (output) - modified by stretch ratio
        const int synthesisHop = static_cast<int>(std::round(hopSize_ * stretchRatio));

        if (synthesisHop <= 0 || analysisHop <= 0)
            return;

        // Overlap-add accumulator
        std::vector<float> accumulator(outputSize + fftSize_, 0.0f);
        std::vector<float> windowSum(outputSize + fftSize_, 0.0f);

        // Phase vocoder processing
        int inputPos = 0;
        int outputPos = 0;

        while (inputPos + fftSize_ <= inputSize && outputPos + fftSize_ <= outputSize)
        {
            // 1. Window and copy input frame
            for (int i = 0; i < fftSize_; ++i)
            {
                inputBuffer_[i] = input[inputPos + i] * window_[i];
            }

            // 2. Perform FFT
            // JUCE FFT works in-place with real/imaginary interleaved
            std::fill(fftBuffer_.begin(), fftBuffer_.end(), std::complex<float>(0.0f, 0.0f));
            for (int i = 0; i < fftSize_; ++i)
                fftBuffer_[i] = std::complex<float>(inputBuffer_[i], 0.0f);

            fft_->perform(fftBuffer_.data(), fftBuffer_.data(), false);

            // 3. Extract magnitude and phase
            const int numBins = fftSize_ / 2 + 1;
            for (int bin = 0; bin < numBins; ++bin)
            {
                magnitudes_[bin] = std::abs(fftBuffer_[bin]);
                phases_[bin] = std::arg(fftBuffer_[bin]);
            }

            // 4. Phase propagation (phase vocoder identity)
            const float binFreq = static_cast<float>(sampleRate_) / fftSize_;

            for (int bin = 0; bin < numBins; ++bin)
            {
                // Calculate phase difference
                float phaseDiff = phases_[bin] - previousPhases_[bin];

                // Unwrap phase to [-pi, pi]
                while (phaseDiff > M_PI) phaseDiff -= 2.0f * static_cast<float>(M_PI);
                while (phaseDiff < -M_PI) phaseDiff += 2.0f * static_cast<float>(M_PI);

                // Calculate true frequency
                float trueFreq = bin * binFreq + phaseDiff * static_cast<float>(sampleRate_) / (2.0f * static_cast<float>(M_PI) * analysisHop);

                // Accumulate phase for synthesis
                phaseAccumulators_[bin] += trueFreq * synthesisHop * 2.0f * static_cast<float>(M_PI) / static_cast<float>(sampleRate_);

                // Store current phase for next frame
                previousPhases_[bin] = phases_[bin];
            }

            // 5. Reconstruct FFT with accumulated phases
            for (int bin = 0; bin < numBins; ++bin)
            {
                fftBuffer_[bin] = std::polar(magnitudes_[bin], phaseAccumulators_[bin]);
            }

            // Mirror for negative frequencies
            for (int bin = 1; bin < fftSize_ / 2; ++bin)
            {
                fftBuffer_[fftSize_ - bin] = std::conj(fftBuffer_[bin]);
            }

            // 6. Inverse FFT
            fft_->perform(fftBuffer_.data(), fftBuffer_.data(), true);

            // 7. Window and overlap-add to output
            for (int i = 0; i < fftSize_; ++i)
            {
                if (outputPos + i < static_cast<int>(accumulator.size()))
                {
                    accumulator[outputPos + i] += std::real(fftBuffer_[i]) * window_[i];
                    windowSum[outputPos + i] += window_[i] * window_[i];
                }
            }

            // Advance positions
            inputPos += analysisHop;
            outputPos += synthesisHop;
        }

        // 8. Normalize by window sum and copy to output
        for (int i = 0; i < outputSize; ++i)
        {
            if (windowSum[i] > 1e-8f)
                output[i] = accumulator[i] / windowSum[i];
            else
                output[i] = 0.0f;
        }
    }
};

} // namespace morphsnap
