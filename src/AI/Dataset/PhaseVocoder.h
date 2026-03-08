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
    void prepare(double sampleRate, int fftSize = 2048);

    /**
     * Apply time-stretching to the audio buffer.
     * @param buffer        Audio buffer to modify in place
     * @param stretchRatio  Speed multiplier: <1.0 = slower, >1.0 = faster
     * @param rng           Random number generator for phase randomization
     */
    void processTimeStretch(juce::AudioBuffer<float>& buffer,
                            float stretchRatio,
                            juce::Random& rng);

    /**
     * Apply pitch-shifting to the audio buffer.
     * @param buffer      Audio buffer to modify in place
     * @param semitones   Pitch shift in semitones (-12 to +12)
     * @param rng         Random number generator
     */
    void processPitchShift(juce::AudioBuffer<float>& buffer,
                           float semitones,
                           juce::Random& rng);

private:
    std::unique_ptr<juce::dsp::FFT> fft_;
    double sampleRate_ = 48000.0;
    int fftSize_ = 2048;
    int hopSize_ = 512;
    bool prepared_ = false;
};

} // namespace morphsnap
