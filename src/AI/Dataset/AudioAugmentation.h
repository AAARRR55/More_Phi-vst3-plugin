/*
 * MorphSnap -- AI/Dataset/AudioAugmentation.h
 * Audio and parameter augmentation for synthetic dataset generation.
 * Provides noise injection, dynamic processing, frequency/time masking,
 * gain changes, and parameter-space augmentation utilities.
 * Header-only implementation for easy integration.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "PhaseVocoder.h"
#include <vector>
#include <array>
#include <cmath>
#include <algorithm>

namespace morphsnap {

// ============================================================================
// Enums and configuration structs
// ============================================================================

/** Types of audio augmentation that can be applied to buffers */
enum class AugmentationType
{
    TimeStretch,        ///< Time-domain stretching via phase vocoder (0.8x to 1.2x)
    PitchShift,         ///< Pitch shifting via phase vocoder (-6 to +6 semitones)
    NoiseInjection,     ///< Additive Gaussian noise at configurable SNR
    FrequencyMask,      ///< Zero out a frequency band via FFT
    TimeMask,           ///< Zero out a time segment
    DynamicProcessing,  ///< Soft-knee compressor
    GainChange          ///< Apply gain in dB
};

/** Configuration for a single augmentation in the chain */
struct AugmentationConfig
{
    AugmentationType type = AugmentationType::NoiseInjection;
    float probability = 0.3f;   ///< Probability of applying this augmentation [0,1]
    float intensity = 0.5f;     ///< Intensity control [0,1], mapped to type-specific ranges
    bool enabled = true;        ///< Master enable flag
};

/** Result descriptor for a single augmentation application */
struct AugmentationResult
{
    bool applied = false;               ///< Whether the augmentation was actually applied
    juce::String augmentationType;      ///< Human-readable name of the augmentation
    float intensityUsed = 0.0f;         ///< Actual intensity value used
};

/** Generate a single Gaussian-distributed sample via Box-Muller transform */
inline float gaussianSample(juce::Random& rng)
{
    // Box-Muller transform: two uniform samples -> one Gaussian sample
    float u1, u2;
    do { u1 = rng.nextFloat(); } while (u1 < 1e-10f);
    u2 = rng.nextFloat();
    return std::sqrt(-2.0f * std::log(u1)) *
           std::cos(2.0f * juce::MathConstants<float>::pi * u2);
}

// ============================================================================
// AudioAugmenter -- applies audio-domain augmentations
// ============================================================================

/**
 * Applies a configurable chain of audio augmentations to buffers.
 *
 * Each augmentation is applied probabilistically (Bernoulli trial per config).
 * Augmentations are applied in the order they were added.
 *
 * Thread safety: NOT thread-safe. Create one instance per thread or
 * synchronise externally.
 */
class AudioAugmenter
{
public:
    AudioAugmenter() = default;
    ~AudioAugmenter() = default;

    // Non-copyable due to FFT state; movable
    AudioAugmenter(const AudioAugmenter&) = delete;
    AudioAugmenter& operator=(const AudioAugmenter&) = delete;
    AudioAugmenter(AudioAugmenter&&) = default;
    AudioAugmenter& operator=(AudioAugmenter&&) = default;

    /** Append an augmentation to the processing chain */
    void addAugmentation(AugmentationConfig config)
    {
        chain_.push_back(std::move(config));
    }

    /** Remove all augmentations from the chain */
    void clearAugmentations()
    {
        chain_.clear();
    }

    /**
     * Apply the augmentation chain to an audio buffer.
     *
     * @param buffer     Audio buffer to augment (modified in place)
     * @param sampleRate Sample rate in Hz
     * @param rng        Random number generator (caller-owned for reproducibility)
     * @return Vector of results describing what was applied
     */
    std::vector<AugmentationResult> apply(juce::AudioBuffer<float>& buffer,
                                          float sampleRate,
                                          juce::Random& rng)
    {
        std::vector<AugmentationResult> results;
        results.reserve(chain_.size());

        for (const auto& config : chain_)
        {
            AugmentationResult result;
            result.augmentationType = augmentationName(config.type);
            result.intensityUsed = config.intensity;
            result.applied = false;

            if (config.enabled && rng.nextFloat() < config.probability)
            {
                applyAugmentation(buffer, sampleRate, config, rng);
                result.applied = true;
            }

            results.push_back(result);
        }

        return results;
    }

private:
    // ---- Dispatch ---------------------------------------------------------

    void applyAugmentation(juce::AudioBuffer<float>& buffer,
                           float sampleRate,
                           const AugmentationConfig& config,
                           juce::Random& rng)
    {
        switch (config.type)
        {
            case AugmentationType::NoiseInjection:
            {
                // intensity 0 -> 40 dB SNR (subtle), 1 -> 5 dB SNR (heavy)
                const float snrDb = juce::jmap(config.intensity, 40.0f, 5.0f);
                applyNoiseInjection(buffer, snrDb, rng);
                break;
            }
            case AugmentationType::GainChange:
            {
                // intensity 0 -> +/-1 dB, 1 -> +/-12 dB
                const float maxGain = juce::jmap(config.intensity, 1.0f, 12.0f);
                const float gainDb = rng.nextFloat() * 2.0f * maxGain - maxGain;
                applyGainChange(buffer, gainDb);
                break;
            }
            case AugmentationType::DynamicProcessing:
            {
                // intensity 0 -> gentle (threshold -10 dB, ratio 2:1)
                // intensity 1 -> aggressive (threshold -30 dB, ratio 8:1)
                const float thresholdDb = juce::jmap(config.intensity, -10.0f, -30.0f);
                const float ratio = juce::jmap(config.intensity, 2.0f, 8.0f);
                applyDynamicProcessing(buffer, thresholdDb, ratio);
                break;
            }
            case AugmentationType::FrequencyMask:
            {
                // Random centre frequency between 200 Hz and sampleRate/3
                const float maxFreq = sampleRate / 3.0f;
                const float centerHz = 200.0f + rng.nextFloat() * (maxFreq - 200.0f);
                // intensity 0 -> narrow (50 Hz), 1 -> wide (2000 Hz)
                const float bandwidthHz = juce::jmap(config.intensity, 50.0f, 2000.0f);
                applyFrequencyMask(buffer, sampleRate, centerHz, bandwidthHz);
                break;
            }
            case AugmentationType::TimeMask:
            {
                const int totalSamples = buffer.getNumSamples();
                if (totalSamples <= 0)
                    break;
                // intensity 0 -> mask 1% of duration, 1 -> mask 20%
                const float fraction = juce::jmap(config.intensity, 0.01f, 0.20f);
                const int durationSamples = std::max(1, static_cast<int>(totalSamples * fraction));
                const int startSample = rng.nextInt(std::max(1, totalSamples - durationSamples));
                applyTimeMask(buffer, startSample, durationSamples);
                break;
            }
            case AugmentationType::TimeStretch:
            {
                // intensity 0 -> 0.8x (slower/longer), 1 -> 1.2x (faster/shorter)
                const float stretchRatio = juce::jmap(config.intensity, 0.8f, 1.2f);
                vocoder_.prepare(sampleRate);
                vocoder_.processTimeStretch(buffer, stretchRatio, rng);
                break;
            }
            case AugmentationType::PitchShift:
            {
                // intensity 0 -> -6 semitones, 1 -> +6 semitones
                const float semitones = juce::jmap(config.intensity, -6.0f, 6.0f);
                vocoder_.prepare(sampleRate);
                vocoder_.processPitchShift(buffer, semitones, rng);
                break;
            }
        }
    }

    // ---- Individual augmentations -----------------------------------------

    /**
     * Add Gaussian noise at a specified signal-to-noise ratio.
     * Uses the Box-Muller transform to generate Gaussian samples.
     */
    void applyNoiseInjection(juce::AudioBuffer<float>& buffer, float snrDb, juce::Random& rng)
    {
        const int numChannels = buffer.getNumChannels();
        const int numSamples = buffer.getNumSamples();
        if (numSamples == 0)
            return;

        // Compute signal RMS across all channels
        float signalPower = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* data = buffer.getReadPointer(ch);
            for (int i = 0; i < numSamples; ++i)
                signalPower += data[i] * data[i];
        }
        signalPower /= static_cast<float>(numChannels * numSamples);

        // Desired noise standard deviation from SNR
        // SNR_dB = 10 * log10(signalPower / noisePower)
        // noisePower = signalPower / 10^(SNR_dB / 10)
        
        float noisePower = 0.0f;
        if (signalPower < 1e-20f)
        {
            // For silent buffers, use a default noise floor (e.g. -60 dB)
            noisePower = std::pow(10.0f, -60.0f / 10.0f);
        }
        else
        {
            noisePower = signalPower / std::pow(10.0f, snrDb / 10.0f);
        }

        const float noiseSigma = std::sqrt(noisePower);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* data = buffer.getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i)
                data[i] += gaussianSample(rng) * noiseSigma;
        }
    }

    /** Apply gain in decibels to the entire buffer */
    void applyGainChange(juce::AudioBuffer<float>& buffer, float gainDb)
    {
        const float gainLinear = std::pow(10.0f, gainDb / 20.0f);
        buffer.applyGain(gainLinear);
    }

    /**
     * Soft-knee compressor applied sample-by-sample.
     * Uses a simple feed-forward design with ballistics-free envelope
     * (instantaneous level detection) for augmentation purposes.
     *
     * @param thresholdDb Compression threshold in dB
     * @param ratio       Compression ratio (e.g. 4.0 means 4:1)
     */
    void applyDynamicProcessing(juce::AudioBuffer<float>& buffer,
                                float thresholdDb,
                                float ratio)
    {
        const int numChannels = buffer.getNumChannels();
        const int numSamples = buffer.getNumSamples();
        if (numSamples == 0 || ratio <= 1.0f)
            return;

        const float kneeWidthDb = 6.0f; // Soft knee width in dB
        const float halfKnee = kneeWidthDb * 0.5f;
        const float slopeReduction = 1.0f - (1.0f / ratio);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* data = buffer.getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i)
            {
                const float sample = data[i];
                const float absSample = std::fabs(sample);
                if (absSample < 1e-20f)
                    continue;

                const float inputDb = 20.0f * std::log10(absSample);
                float gainReductionDb = 0.0f;

                if (inputDb > thresholdDb + halfKnee)
                {
                    // Above knee -- full compression
                    gainReductionDb = slopeReduction * (inputDb - thresholdDb);
                }
                else if (inputDb > thresholdDb - halfKnee)
                {
                    // Within knee -- quadratic interpolation
                    const float delta = inputDb - thresholdDb + halfKnee;
                    gainReductionDb = slopeReduction * (delta * delta) / (2.0f * kneeWidthDb);
                }
                // else: below knee -- no compression

                const float gainLinear = std::pow(10.0f, -gainReductionDb / 20.0f);
                data[i] = sample * gainLinear;
            }
        }
    }

    /**
     * Zero out a frequency band by performing FFT, zeroing bins in the
     * specified range, and performing inverse FFT.
     *
     * Processes each channel independently. Uses a Hann window with
     * overlap-add (50% overlap) for artifact-free reconstruction.
     */
    void applyFrequencyMask(juce::AudioBuffer<float>& buffer,
                            float sampleRate,
                            float centerFreqHz,
                            float bandwidthHz)
    {
        const int numChannels = buffer.getNumChannels();
        const int numSamples = buffer.getNumSamples();
        if (numSamples == 0 || sampleRate <= 0.0f)
            return;

        // Choose FFT order (at least 1024 bins)
        const int fftOrder = std::max(10, static_cast<int>(std::ceil(std::log2(
            std::max(1024, numSamples)))));
        const int fftSize = 1 << fftOrder;
        const int hopSize = fftSize / 2;
        const int numBins = fftSize / 2 + 1;

        juce::dsp::FFT fft(fftOrder);

        // Compute bin indices for the masked band
        const float lowFreq = std::max(0.0f, centerFreqHz - bandwidthHz * 0.5f);
        const float highFreq = std::min(sampleRate * 0.5f, centerFreqHz + bandwidthHz * 0.5f);
        const int lowBin = static_cast<int>(std::floor(lowFreq * fftSize / sampleRate));
        const int highBin = std::min(numBins - 1,
                                     static_cast<int>(std::ceil(highFreq * fftSize / sampleRate)));

        if (lowBin >= highBin)
            return;

        // Prepare Hann window
        std::vector<float> window(static_cast<size_t>(fftSize));
        for (int i = 0; i < fftSize; ++i)
            window[static_cast<size_t>(i)] = 0.5f * (1.0f - std::cos(
                2.0f * juce::MathConstants<float>::pi * static_cast<float>(i)
                / static_cast<float>(fftSize)));

        // Working buffers: FFT requires 2*fftSize floats for interleaved complex
        std::vector<float> fftData(static_cast<size_t>(fftSize) * 2, 0.0f);
        std::vector<float> outputAccumulator;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* inputData = buffer.getReadPointer(ch);

            // Accumulator for overlap-add output
            outputAccumulator.assign(static_cast<size_t>(numSamples), 0.0f);
            std::vector<float> windowAccumulator(static_cast<size_t>(numSamples), 0.0f);

            for (int frameStart = 0; frameStart < numSamples; frameStart += hopSize)
            {
                // Fill FFT input with windowed samples
                std::fill(fftData.begin(), fftData.end(), 0.0f);

                const int frameSamples = std::min(fftSize, numSamples - frameStart);
                for (int i = 0; i < frameSamples; ++i)
                    fftData[static_cast<size_t>(i)] = inputData[frameStart + i] * window[static_cast<size_t>(i)];

                // Forward FFT (real input -> interleaved complex output)
                fft.performRealOnlyForwardTransform(fftData.data(), true);

                // Zero out masked bins (interleaved: real at 2*bin, imag at 2*bin+1)
                for (int bin = lowBin; bin <= highBin; ++bin)
                {
                    fftData[static_cast<size_t>(bin) * 2]     = 0.0f;
                    fftData[static_cast<size_t>(bin) * 2 + 1] = 0.0f;
                }

                // Inverse FFT
                fft.performRealOnlyInverseTransform(fftData.data());

                // Overlap-add with window
                for (int i = 0; i < frameSamples; ++i)
                {
                    const auto idx = static_cast<size_t>(frameStart + i);
                    outputAccumulator[idx] += fftData[static_cast<size_t>(i)] * window[static_cast<size_t>(i)];
                    windowAccumulator[idx] += window[static_cast<size_t>(i)] * window[static_cast<size_t>(i)];
                }
            }

            // Normalise by accumulated window energy and write back
            float* outputData = buffer.getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i)
            {
                const auto idx = static_cast<size_t>(i);
                if (windowAccumulator[idx] > 1e-8f)
                    outputData[i] = outputAccumulator[idx] / windowAccumulator[idx];
                else
                    outputData[i] = 0.0f;
            }
        }
    }

    /** Zero out a contiguous segment of samples across all channels */
    void applyTimeMask(juce::AudioBuffer<float>& buffer, int startSample, int durationSamples)
    {
        const int numChannels = buffer.getNumChannels();
        const int numSamples = buffer.getNumSamples();
        const int clampedStart = std::max(0, std::min(startSample, numSamples));
        const int clampedEnd = std::max(clampedStart, std::min(startSample + durationSamples, numSamples));
        const int count = clampedEnd - clampedStart;

        if (count <= 0)
            return;

        for (int ch = 0; ch < numChannels; ++ch)
            buffer.clear(ch, clampedStart, count);
    }

    // ---- Helpers -----------------------------------------------------------

    /** Convert augmentation type to human-readable name */
    static juce::String augmentationName(AugmentationType type)
    {
        switch (type)
        {
            case AugmentationType::TimeStretch:       return "TimeStretch";
            case AugmentationType::PitchShift:        return "PitchShift";
            case AugmentationType::NoiseInjection:    return "NoiseInjection";
            case AugmentationType::FrequencyMask:     return "FrequencyMask";
            case AugmentationType::TimeMask:          return "TimeMask";
            case AugmentationType::DynamicProcessing: return "DynamicProcessing";
            case AugmentationType::GainChange:        return "GainChange";
            default:                                  return "Unknown";
        }
    }

    std::vector<AugmentationConfig> chain_;
    PhaseVocoder vocoder_;
};

// ============================================================================
// ParameterAugmenter -- parameter-space augmentation utilities
// ============================================================================

/**
 * Static utilities for augmenting parameter vectors.
 * All methods operate on normalised [0,1] parameter spaces and guarantee
 * output values remain clamped to [0,1].
 */
class ParameterAugmenter
{
public:
    ParameterAugmenter() = delete; // Static-only class

    /**
     * Add Gaussian noise to a parameter vector.
     * Each parameter is perturbed independently with the given standard deviation.
     * Results are clamped to [0,1].
     *
     * @param params Input parameter vector (normalised [0,1])
     * @param sigma  Standard deviation of Gaussian noise
     * @param rng    Random number generator
     * @return Noisy parameter vector clamped to [0,1]
     */
    static std::vector<float> addNoise(const std::vector<float>& params,
                                       float sigma,
                                       juce::Random& rng)
    {
        std::vector<float> result(params.size());
        for (size_t i = 0; i < params.size(); ++i)
        {
            const float noise = gaussianSample(rng) * sigma;
            result[i] = std::clamp(params[i] + noise, 0.0f, 1.0f);
        }
        return result;
    }

    /**
     * Linearly interpolate between two parameter vectors.
     * Result is clamped to [0,1] (redundant if inputs are valid, but safe).
     *
     * @param a     First parameter vector
     * @param b     Second parameter vector
     * @param alpha Blend factor: 0.0 returns a, 1.0 returns b
     * @return Interpolated parameter vector
     */
    static std::vector<float> interpolate(const std::vector<float>& a,
                                          const std::vector<float>& b,
                                          float alpha)
    {
        jassert(a.size() == b.size());
        const size_t n = std::min(a.size(), b.size());
        std::vector<float> result(n);
        const float oneMinusAlpha = 1.0f - alpha;

        for (size_t i = 0; i < n; ++i)
            result[i] = std::clamp(oneMinusAlpha * a[i] + alpha * b[i], 0.0f, 1.0f);

        return result;
    }

    /**
     * Extrapolate a parameter vector along a direction.
     * Computes: params + direction * magnitude, clamped to [0,1].
     *
     * Useful for generating "overshoot" variations beyond the convex hull
     * of existing snapshots.
     *
     * @param params    Base parameter vector
     * @param direction Direction vector (typically another param vector minus params)
     * @param magnitude Scale factor for the direction
     * @return Extrapolated parameter vector clamped to [0,1]
     */
    static std::vector<float> extrapolate(const std::vector<float>& params,
                                          const std::vector<float>& direction,
                                          float magnitude)
    {
        jassert(params.size() == direction.size());
        const size_t n = std::min(params.size(), direction.size());
        std::vector<float> result(n);

        for (size_t i = 0; i < n; ++i)
            result[i] = std::clamp(params[i] + direction[i] * magnitude, 0.0f, 1.0f);

        return result;
    }
};

// ============================================================================
// AugmentationChainPreset -- factory presets for common augmentation chains
// ============================================================================

/**
 * Provides static factory methods for common augmentation chain configurations.
 * Each factory returns a vector of AugmentationConfig that can be passed to
 * AudioAugmenter::addAugmentation() in sequence.
 */
struct AugmentationChainPreset
{
    /**
     * Balanced default chain.
     * Moderate probability of noise, gain, and time masking.
     */
    static std::vector<AugmentationConfig> createDefault()
    {
        return {
            { AugmentationType::NoiseInjection,    0.3f, 0.3f, true },
            { AugmentationType::GainChange,        0.4f, 0.4f, true },
            { AugmentationType::TimeMask,          0.2f, 0.3f, true },
            { AugmentationType::FrequencyMask,     0.15f, 0.3f, true },
            { AugmentationType::DynamicProcessing, 0.15f, 0.3f, true }
        };
    }

    /**
     * Noise-focused chain.
     * Higher probability and intensity of noise injection with light gain variation.
     */
    static std::vector<AugmentationConfig> createNoisy()
    {
        return {
            { AugmentationType::NoiseInjection,    0.7f, 0.6f, true },
            { AugmentationType::GainChange,        0.3f, 0.2f, true },
            { AugmentationType::FrequencyMask,     0.2f, 0.4f, true }
        };
    }

    /**
     * Dynamics-focused chain.
     * Emphasises compression and gain changes for dynamic-range augmentation.
     */
    static std::vector<AugmentationConfig> createDynamic()
    {
        return {
            { AugmentationType::DynamicProcessing, 0.6f, 0.6f, true },
            { AugmentationType::GainChange,        0.5f, 0.5f, true },
            { AugmentationType::NoiseInjection,    0.15f, 0.2f, true }
        };
    }

    /**
     * Creative chain -- all augmentation types with varied probabilities.
     * Suitable for maximum dataset diversity.
     */
    static std::vector<AugmentationConfig> createCreative()
    {
        return {
            { AugmentationType::NoiseInjection,    0.4f, 0.5f, true },
            { AugmentationType::GainChange,        0.5f, 0.6f, true },
            { AugmentationType::DynamicProcessing, 0.4f, 0.5f, true },
            { AugmentationType::FrequencyMask,     0.3f, 0.5f, true },
            { AugmentationType::TimeMask,          0.3f, 0.4f, true },
            { AugmentationType::TimeStretch,       0.2f, 0.4f, true },
            { AugmentationType::PitchShift,        0.2f, 0.4f, true },
        };
    }
};

} // namespace morphsnap
