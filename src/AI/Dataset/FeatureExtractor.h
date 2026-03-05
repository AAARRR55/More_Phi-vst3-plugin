/*
 * MorphSnap — AI/Dataset/FeatureExtractor.h
 * Audio feature extraction for synthetic dataset generation.
 * Provides spectral, temporal, and perceptual feature extraction
 * using JUCE DSP primitives for FFT and filtering.
 */
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <nlohmann/json.hpp>
#include <array>
#include <vector>
#include <cmath>

namespace morphsnap {

/** Spectral features extracted from frequency domain analysis */
struct SpectralFeatures
{
    std::array<float, 13> mfcc{};       ///< Mel-frequency cepstral coefficients
    float spectralCentroid = 0.0f;       ///< Hz - weighted mean of frequencies
    float spectralRolloff = 0.0f;        ///< Hz - frequency below which 85% of energy lies
    float spectralFlux = 0.0f;           ///< Rate of spectral change between frames
    float spectralSpread = 0.0f;         ///< Hz - standard deviation around centroid
    float spectralFlatness = 0.0f;       ///< Noise-like vs tone-like measure (0-1)
    std::array<float, 12> chroma{};      ///< Chroma vector (12 semitones, C=0 to B=11)
};

/** Temporal features extracted from time domain analysis */
struct TemporalFeatures
{
    float rmsEnergy = 0.0f;              ///< dB - root mean square energy
    float peakAmplitude = 0.0f;          ///< dB - maximum absolute sample value
    float crestFactor = 0.0f;            ///< Peak/RMS ratio (linear)
    float attackTime = 0.0f;             ///< ms - time from 10% to 90% of peak
    float transientDensity = 0.0f;       ///< Transients per second
    float zeroCrossingRate = 0.0f;       ///< Zero crossings per second
};

/** Perceptual features based on psychoacoustic models */
struct PerceptualFeatures
{
    float lufs = 0.0f;                   ///< Loudness units relative to full scale (ITU-R BS.1770-4)
    float truePeakDb = 0.0f;             ///< dB - true peak level after oversampling
    float roughness = 0.0f;              ///< Daniel-Weber roughness (acoustic units)
    float sharpness = 0.0f;              ///< Aures sharpness (acum)
    float brightness = 0.0f;             ///< High frequency content ratio (0-1)
    float dynamicRange = 0.0f;           ///< LUFS range (difference between loud/quiet)
};

/** Complete feature set for an audio buffer */
struct AudioFeatures
{
    SpectralFeatures spectral;
    TemporalFeatures temporal;
    PerceptualFeatures perceptual;

    // Frame-level features for time-series analysis
    std::vector<SpectralFeatures> spectralFrames;
    std::vector<float> rmsFrames;

    // Global metadata
    float durationSeconds = 0.0f;
    int frameCount = 0;
};

/** Configuration for feature extraction */
struct ExtractionConfig
{
    double sampleRate = 48000.0;
    int frameSize = 2048;               ///< FFT size (power of 2)
    int hopSize = 512;                  ///< Samples between frames
    bool computeFrameLevel = true;      ///< Compute per-frame features
    bool computeMFCC = true;
    bool computeChroma = true;
    int mfccCoefficients = 13;
    float lufsIntegrationTime = 0.4f;   ///< Seconds for LUFS gating
};

/**
 * Audio feature extractor supporting spectral, temporal, and perceptual analysis.
 * Thread-safe for read operations. FFT-based features use JUCE's DSP module.
 */
class FeatureExtractor
{
public:
    FeatureExtractor();
    ~FeatureExtractor() = default;

    // Allow move operations for reset patterns
    FeatureExtractor(FeatureExtractor&&) = default;
    FeatureExtractor& operator=(FeatureExtractor&&) = default;

    // Delete copy operations due to unique_ptr member
    FeatureExtractor(const FeatureExtractor&) = delete;
    FeatureExtractor& operator=(const FeatureExtractor&) = delete;

    /**
     * Extract all features from an audio buffer.
     * @param buffer Audio samples (can be mono or multi-channel)
     * @param config Extraction configuration
     * @return Complete feature set
     */
    AudioFeatures extract(const juce::AudioBuffer<float>& buffer,
                         const ExtractionConfig& config);

    /**
     * Extract spectral features only.
     */
    SpectralFeatures extractSpectral(const juce::AudioBuffer<float>& buffer,
                                    const ExtractionConfig& config);

    /**
     * Extract temporal features only.
     */
    TemporalFeatures extractTemporal(const juce::AudioBuffer<float>& buffer,
                                    const ExtractionConfig& config);

    /**
     * Extract perceptual features only.
     */
    PerceptualFeatures extractPerceptual(const juce::AudioBuffer<float>& buffer,
                                        const ExtractionConfig& config);

    /**
     * Extract frame-level spectral features.
     * @return Vector of spectral features per frame
     */
    std::vector<SpectralFeatures> extractSpectralFrames(const juce::AudioBuffer<float>& buffer,
                                                        const ExtractionConfig& config);

    /**
     * Compute MFCC coefficients from magnitude spectrum.
     * @param magnitudes FFT magnitude values
     * @param size Number of magnitude bins
     * @param sampleRate Sample rate in Hz
     * @return 13 MFCC coefficients
     */
    std::array<float, 13> computeMFCC(const float* magnitudes, int size, double sampleRate);

    /**
     * Compute LUFS loudness (ITU-R BS.1770-4).
     * Uses K-weighting filter and gating as per specification.
     * @param buffer Audio samples
     * @param sampleRate Sample rate in Hz
     * @return Integrated loudness in LUFS
     */
    float computeLUFS(const juce::AudioBuffer<float>& buffer, double sampleRate);

    /**
     * Compute chroma features from magnitude spectrum.
     * Maps frequency bins to 12 semitone classes.
     * @param magnitudes FFT magnitude values
     * @param size Number of magnitude bins
     * @param sampleRate Sample rate in Hz
     * @return 12-element chroma vector (C=0, C#=1, ..., B=11)
     */
    std::array<float, 12> computeChroma(const float* magnitudes, int size, double sampleRate);

    /**
     * Export features to JSON format.
     */
    nlohmann::json toJson(const AudioFeatures& features) const;

    /**
     * Export features as flat vector (NumPy-compatible).
     * Order: spectral (19), temporal (6), perceptual (6) = 31 floats
     */
    std::vector<float> toVector(const AudioFeatures& features) const;

    /**
     * Get total number of scalar features (for vector dimension).
     */
    static constexpr int getFeatureDimension() { return 31; }

private:
    // FFT processing
    void performFFT(const float* samples, int numSamples, float* magnitudes, int fftSize);

    // Initialize mel filterbank
    void createMelFilterBank(int fftSize, double sampleRate, int numBands);

    // Initialize LUFS filters
    void prepareLUFSFilters(double sampleRate);

    // DCT-II for MFCC computation
    void computeDCT(const float* input, float* output, int numCoeffs, int numFilters);

    // Window function
    void applyHannWindow(float* samples, int size);

    // Mel scale conversion
    static float hzToMel(float hz) { return 2595.0f * std::log10(1.0f + hz / 700.0f); }
    static float melToHz(float mel) { return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f); }

    // Decibel conversion
    static float linearToDb(float linear) { return 20.0f * std::log10(std::max(linear, 1e-10f)); }

    // FFT instance (reusable)
    std::unique_ptr<juce::dsp::FFT> fft_;
    int currentFftSize_ = 0;

    // Working buffers
    std::vector<float> fftBuffer_;
    std::vector<float> magnitudeBuffer_;
    std::vector<float> previousMagnitudeBuffer_;

    // Mel filterbank
    std::vector<std::vector<float>> melFilterBank_;
    int currentMelFilterSize_ = 0;

    // LUFS filters
    juce::dsp::IIR::Filter<float> lufsHighShelf_;
    juce::dsp::IIR::Filter<float> lufsHighPass_;
    double currentLufsSampleRate_ = 0.0;
};

} // namespace morphsnap
