/*
 * More-Phi — AI/Dataset/FeatureExtractor.cpp
 * Implementation of audio feature extraction for synthetic dataset generation.
 */
#include "FeatureExtractor.h"
#include <algorithm>
#include <numeric>
#include <cmath>

namespace more_phi {

namespace
{
    constexpr float PI = 3.14159265358979323846f;
    constexpr float REFERENCE_LEVEL_LUFS = -23.0f;
    constexpr float ABSOLUTE_THRESHOLD_LUFS = -70.0f;
    constexpr int NUM_MEL_BANDS = 26;
}

FeatureExtractor::FeatureExtractor()
{
}

// ── Main Extraction ─────────────────────────────────────────────────────────────

AudioFeatures FeatureExtractor::extract(const juce::AudioBuffer<float>& buffer,
                                        const ExtractionConfig& config)
{
    AudioFeatures features;
    features.durationSeconds = static_cast<float>(buffer.getNumSamples()) / static_cast<float>(config.sampleRate);

    // Extract frame-level features first
    if (config.computeFrameLevel)
    {
        features.spectralFrames = extractSpectralFrames(buffer, config);

        // Extract RMS per frame
        const int numFrames = static_cast<int>((buffer.getNumSamples() - config.frameSize) / config.hopSize) + 1;
        features.rmsFrames.reserve(numFrames);

        for (int i = 0; i < numFrames; ++i)
        {
            const int startSample = i * config.hopSize;
            const int endSample = std::min(startSample + config.frameSize, buffer.getNumSamples());

            float sumSquares = 0.0f;
            int count = 0;

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const float* channelData = buffer.getReadPointer(ch);
                for (int s = startSample; s < endSample; ++s)
                {
                    sumSquares += channelData[s] * channelData[s];
                    ++count;
                }
            }

            features.rmsFrames.push_back(std::sqrt(sumSquares / std::max(count, 1)));
        }

        features.frameCount = static_cast<int>(features.spectralFrames.size());
    }

    // Aggregate spectral features
    features.spectral = extractSpectral(buffer, config);

    // Extract temporal features
    features.temporal = extractTemporal(buffer, config);

    // Extract perceptual features
    features.perceptual = extractPerceptual(buffer, config);

    return features;
}

// ── Spectral Features ───────────────────────────────────────────────────────────

SpectralFeatures FeatureExtractor::extractSpectral(const juce::AudioBuffer<float>& buffer,
                                                   const ExtractionConfig& config)
{
    SpectralFeatures features;

    // Ensure FFT is initialized
    if (!fft_ || currentFftSize_ != config.frameSize)
    {
        const int order = static_cast<int>(std::log2(config.frameSize));
        fft_ = std::make_unique<juce::dsp::FFT>(order);
        currentFftSize_ = config.frameSize;
        fftBuffer_.resize(config.frameSize * 2, 0.0f);
        magnitudeBuffer_.resize(config.frameSize / 2 + 1, 0.0f);
        previousMagnitudeBuffer_.resize(config.frameSize / 2 + 1, 0.0f);
        std::fill(previousMagnitudeBuffer_.begin(), previousMagnitudeBuffer_.end(), 0.0f);
    }

    // Create mel filterbank if needed
    if (config.computeMFCC && (melFilterBank_.empty() || currentMelFilterSize_ != config.frameSize))
    {
        createMelFilterBank(config.frameSize, config.sampleRate, NUM_MEL_BANDS);
        currentMelFilterSize_ = config.frameSize;
    }

    // Accumulate features across frames
    std::vector<float> centroidAccum;
    std::vector<float> rolloffAccum;
    std::vector<float> spreadAccum;
    std::vector<float> flatnessAccum;
    std::vector<float> fluxAccum;
    std::vector<std::array<float, 13>> mfccAccum;
    std::vector<std::array<float, 12>> chromaAccum;

    const int numFrames = static_cast<int>((buffer.getNumSamples() - config.frameSize) / config.hopSize) + 1;

    for (int frame = 0; frame < numFrames; ++frame)
    {
        const int startSample = frame * config.hopSize;

        // Prepare mono frame (average channels)
        std::fill(fftBuffer_.begin(), fftBuffer_.end(), 0.0f);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const float* channelData = buffer.getReadPointer(ch);
            for (int i = 0; i < config.frameSize && (startSample + i) < buffer.getNumSamples(); ++i)
            {
                fftBuffer_[i] += channelData[startSample + i] / buffer.getNumChannels();
            }
        }

        // Apply window
        applyHannWindow(fftBuffer_.data(), config.frameSize);

        // Perform FFT
        performFFT(fftBuffer_.data(), config.frameSize, magnitudeBuffer_.data(), config.frameSize);

        // Compute spectral centroid
        float centroidNum = 0.0f;
        float centroidDenom = 0.0f;
        const float binWidth = static_cast<float>(config.sampleRate) / config.frameSize;

        for (size_t i = 0; i < magnitudeBuffer_.size(); ++i)
        {
            const float freq = static_cast<float>(i) * binWidth;
            centroidNum += freq * magnitudeBuffer_[i];
            centroidDenom += magnitudeBuffer_[i];
        }
        const float centroid = centroidDenom > 0.0f ? centroidNum / centroidDenom : 0.0f;
        centroidAccum.push_back(centroid);

        // Compute spectral spread (standard deviation around centroid)
        float spreadSum = 0.0f;
        for (size_t i = 0; i < magnitudeBuffer_.size(); ++i)
        {
            const float freq = static_cast<float>(i) * binWidth;
            spreadSum += magnitudeBuffer_[i] * std::pow(freq - centroid, 2.0f);
        }
        const float spread = centroidDenom > 0.0f ? std::sqrt(spreadSum / centroidDenom) : 0.0f;
        spreadAccum.push_back(spread);

        // Compute spectral rolloff (85% energy point)
        float totalEnergy = 0.0f;
        for (size_t i = 0; i < magnitudeBuffer_.size(); ++i)
        {
            totalEnergy += magnitudeBuffer_[i] * magnitudeBuffer_[i];
        }
        const float rolloffThreshold = totalEnergy * 0.85f;

        float cumulativeEnergy = 0.0f;
        int rolloffBin = 0;
        for (size_t i = 0; i < magnitudeBuffer_.size(); ++i)
        {
            cumulativeEnergy += magnitudeBuffer_[i] * magnitudeBuffer_[i];
            if (cumulativeEnergy >= rolloffThreshold)
            {
                rolloffBin = static_cast<int>(i);
                break;
            }
        }
        rolloffAccum.push_back(static_cast<float>(rolloffBin) * binWidth);

        // Compute spectral flatness (geometric mean / arithmetic mean)
        float logSum = 0.0f;
        float linSum = 0.0f;
        int validBins = 0;
        for (size_t i = 1; i < magnitudeBuffer_.size(); ++i) // Skip DC
        {
            if (magnitudeBuffer_[i] > 1e-10f)
            {
                logSum += std::log(magnitudeBuffer_[i]);
                linSum += magnitudeBuffer_[i];
                ++validBins;
            }
        }
        const float flatness = validBins > 0
            ? std::exp(logSum / validBins) / (linSum / validBins)
            : 0.0f;
        flatnessAccum.push_back(std::clamp(flatness, 0.0f, 1.0f));

        // Compute spectral flux
        float flux = 0.0f;
        for (size_t i = 0; i < magnitudeBuffer_.size(); ++i)
        {
            const float diff = magnitudeBuffer_[i] - previousMagnitudeBuffer_[i];
            flux += diff * diff;
        }
        fluxAccum.push_back(std::sqrt(flux));

        // Store current magnitudes for next frame
        std::copy(magnitudeBuffer_.begin(), magnitudeBuffer_.end(), previousMagnitudeBuffer_.begin());

        // Compute MFCC
        if (config.computeMFCC)
        {
            mfccAccum.push_back(computeMFCC(magnitudeBuffer_.data(),
                                           static_cast<int>(magnitudeBuffer_.size()),
                                           config.sampleRate));
        }

        // Compute chroma
        if (config.computeChroma)
        {
            chromaAccum.push_back(computeChroma(magnitudeBuffer_.data(),
                                               static_cast<int>(magnitudeBuffer_.size()),
                                               config.sampleRate));
        }
    }

    // Aggregate to global features (mean)
    auto meanOf = [](const std::vector<float>& vals) -> float {
        if (vals.empty()) return 0.0f;
        float sum = std::accumulate(vals.begin(), vals.end(), 0.0f);
        return sum / static_cast<float>(vals.size());
    };

    features.spectralCentroid = meanOf(centroidAccum);
    features.spectralRolloff = meanOf(rolloffAccum);
    features.spectralSpread = meanOf(spreadAccum);
    features.spectralFlatness = meanOf(flatnessAccum);
    features.spectralFlux = meanOf(fluxAccum);

    // Aggregate MFCC
    if (!mfccAccum.empty())
    {
        for (size_t i = 0; i < 13; ++i)
        {
            float sum = 0.0f;
            for (const auto& mfcc : mfccAccum)
            {
                sum += mfcc[i];
            }
            features.mfcc[i] = sum / static_cast<float>(mfccAccum.size());
        }
    }

    // Aggregate chroma
    if (!chromaAccum.empty())
    {
        for (size_t i = 0; i < 12; ++i)
        {
            float sum = 0.0f;
            for (const auto& chroma : chromaAccum)
            {
                sum += chroma[i];
            }
            features.chroma[i] = sum / static_cast<float>(chromaAccum.size());
        }
    }

    return features;
}

std::vector<SpectralFeatures> FeatureExtractor::extractSpectralFrames(const juce::AudioBuffer<float>& buffer,
                                                                       const ExtractionConfig& config)
{
    std::vector<SpectralFeatures> frames;

    // Ensure FFT is initialized
    if (!fft_ || currentFftSize_ != config.frameSize)
    {
        const int order = static_cast<int>(std::log2(config.frameSize));
        fft_ = std::make_unique<juce::dsp::FFT>(order);
        currentFftSize_ = config.frameSize;
        fftBuffer_.resize(config.frameSize * 2, 0.0f);
        magnitudeBuffer_.resize(config.frameSize / 2 + 1, 0.0f);
        previousMagnitudeBuffer_.resize(config.frameSize / 2 + 1, 0.0f);
    }

    // Create mel filterbank if needed
    if (config.computeMFCC && (melFilterBank_.empty() || currentMelFilterSize_ != config.frameSize))
    {
        createMelFilterBank(config.frameSize, config.sampleRate, NUM_MEL_BANDS);
        currentMelFilterSize_ = config.frameSize;
    }

    const int numFrames = static_cast<int>((buffer.getNumSamples() - config.frameSize) / config.hopSize) + 1;
    frames.reserve(numFrames);

    for (int frame = 0; frame < numFrames; ++frame)
    {
        const int startSample = frame * config.hopSize;
        SpectralFeatures sf;

        // Prepare mono frame
        std::fill(fftBuffer_.begin(), fftBuffer_.end(), 0.0f);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const float* channelData = buffer.getReadPointer(ch);
            for (int i = 0; i < config.frameSize && (startSample + i) < buffer.getNumSamples(); ++i)
            {
                fftBuffer_[i] += channelData[startSample + i] / buffer.getNumChannels();
            }
        }

        applyHannWindow(fftBuffer_.data(), config.frameSize);
        performFFT(fftBuffer_.data(), config.frameSize, magnitudeBuffer_.data(), config.frameSize);

        // Compute frame features
        float centroidNum = 0.0f;
        float centroidDenom = 0.0f;
        const float binWidth = static_cast<float>(config.sampleRate) / config.frameSize;

        for (size_t i = 0; i < magnitudeBuffer_.size(); ++i)
        {
            const float freq = static_cast<float>(i) * binWidth;
            centroidNum += freq * magnitudeBuffer_[i];
            centroidDenom += magnitudeBuffer_[i];
        }
        sf.spectralCentroid = centroidDenom > 0.0f ? centroidNum / centroidDenom : 0.0f;

        // Spectral flux
        float flux = 0.0f;
        for (size_t i = 0; i < magnitudeBuffer_.size(); ++i)
        {
            const float diff = magnitudeBuffer_[i] - previousMagnitudeBuffer_[i];
            flux += diff * diff;
        }
        sf.spectralFlux = std::sqrt(flux);
        std::copy(magnitudeBuffer_.begin(), magnitudeBuffer_.end(), previousMagnitudeBuffer_.begin());

        if (config.computeMFCC)
        {
            sf.mfcc = computeMFCC(magnitudeBuffer_.data(),
                                 static_cast<int>(magnitudeBuffer_.size()),
                                 config.sampleRate);
        }

        if (config.computeChroma)
        {
            sf.chroma = computeChroma(magnitudeBuffer_.data(),
                                     static_cast<int>(magnitudeBuffer_.size()),
                                     config.sampleRate);
        }

        frames.push_back(sf);
    }

    return frames;
}

// ── Temporal Features ───────────────────────────────────────────────────────────

TemporalFeatures FeatureExtractor::extractTemporal(const juce::AudioBuffer<float>& buffer,
                                                   const ExtractionConfig& config)
{
    TemporalFeatures features;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (numSamples == 0 || numChannels == 0)
        return features;

    // RMS and Peak
    float sumSquares = 0.0f;
    float peak = 0.0f;
    int zeroCrossings = 0;
    float prevSample = 0.0f;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* data = buffer.getReadPointer(ch);
        for (int i = 0; i < numSamples; ++i)
        {
            const float sample = data[i];
            sumSquares += sample * sample;
            peak = std::max(peak, std::abs(sample));

            // Zero crossing (only for first channel to avoid double counting)
            if (ch == 0)
            {
                if ((prevSample >= 0.0f && sample < 0.0f) || (prevSample < 0.0f && sample >= 0.0f))
                {
                    ++zeroCrossings;
                }
                prevSample = sample;
            }
        }
    }

    const float rms = std::sqrt(sumSquares / (numSamples * numChannels));
    features.rmsEnergy = linearToDb(rms);
    features.peakAmplitude = linearToDb(peak);
    features.crestFactor = rms > 0.0f ? peak / rms : 1.0f;
    features.zeroCrossingRate = static_cast<float>(zeroCrossings) / (numSamples / static_cast<float>(config.sampleRate));

    // Attack time estimation
    // Find the onset by looking for maximum energy increase
    const int analysisWindowSize = static_cast<int>(0.05f * config.sampleRate); // 50ms window
    float maxEnergy = 0.0f;
    int peakPosition = 0;

    for (int i = 0; i < numSamples - analysisWindowSize; i += analysisWindowSize / 4)
    {
        float windowEnergy = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* data = buffer.getReadPointer(ch);
            for (int j = 0; j < analysisWindowSize && (i + j) < numSamples; ++j)
            {
                windowEnergy += data[i + j] * data[i + j];
            }
        }

        if (windowEnergy > maxEnergy)
        {
            maxEnergy = windowEnergy;
            peakPosition = i;
        }
    }

    // Find 10% and 90% points for attack time
    const float peakEnergy = maxEnergy / analysisWindowSize;
    float threshold10 = peakEnergy * 0.1f;
    float threshold90 = peakEnergy * 0.9f;
    int pos10 = 0;
    int pos90 = 0;
    bool found10 = false;

    for (int i = 0; i < peakPosition; ++i)
    {
        float instantEnergy = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            instantEnergy += buffer.getReadPointer(ch)[i] * buffer.getReadPointer(ch)[i];
        }

        if (!found10 && instantEnergy >= threshold10)
        {
            pos10 = i;
            found10 = true;
        }
        if (instantEnergy >= threshold90)
        {
            pos90 = i;
            break;
        }
    }

    features.attackTime = static_cast<float>(pos90 - pos10) / static_cast<float>(config.sampleRate) * 1000.0f;

    // Transient density (count significant energy increases)
    const float transientThreshold = peakEnergy * 0.3f;
    int transientCount = 0;
    float prevWindowEnergy = 0.0f;

    for (int i = 0; i < numSamples - analysisWindowSize; i += analysisWindowSize / 2)
    {
        float windowEnergy = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* data = buffer.getReadPointer(ch);
            for (int j = 0; j < analysisWindowSize && (i + j) < numSamples; ++j)
            {
                windowEnergy += data[i + j] * data[i + j];
            }
        }
        windowEnergy /= analysisWindowSize;

        if (windowEnergy > prevWindowEnergy + transientThreshold)
        {
            ++transientCount;
        }
        prevWindowEnergy = windowEnergy;
    }

    const float durationSeconds = numSamples / static_cast<float>(config.sampleRate);
    features.transientDensity = transientCount / durationSeconds;

    return features;
}

// ── Perceptual Features ─────────────────────────────────────────────────────────

PerceptualFeatures FeatureExtractor::extractPerceptual(const juce::AudioBuffer<float>& buffer,
                                                       const ExtractionConfig& config)
{
    PerceptualFeatures features;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (numSamples == 0 || numChannels == 0)
        return features;

    // LUFS computation
    features.lufs = computeLUFS(buffer, config.sampleRate);

    // True peak (simplified: 4x oversampling peak)
    float truePeak = 0.0f;
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* data = buffer.getReadPointer(ch);
        for (int i = 0; i < numSamples - 3; ++i)
        {
            // Simple interpolation for true peak estimation
            for (float t = 0.0f; t < 1.0f; t += 0.25f)
            {
                float interp = data[i] * (1.0f - t) + data[i + 1] * t;
                truePeak = std::max(truePeak, std::abs(interp));
            }
        }
    }
    features.truePeakDb = linearToDb(truePeak);

    // Brightness (ratio of high frequency energy > 2kHz)
    if (!fft_ || currentFftSize_ != config.frameSize)
    {
        const int order = static_cast<int>(std::log2(config.frameSize));
        fft_ = std::make_unique<juce::dsp::FFT>(order);
        currentFftSize_ = config.frameSize;
        fftBuffer_.resize(config.frameSize * 2, 0.0f);
        magnitudeBuffer_.resize(config.frameSize / 2 + 1, 0.0f);
    }

    float totalEnergy = 0.0f;
    float highFreqEnergy = 0.0f;
    const float binWidth = static_cast<float>(config.sampleRate) / config.frameSize;
    const int highFreqBin = static_cast<int>(2000.0f / binWidth);

    const int numFrames = static_cast<int>((numSamples - config.frameSize) / config.hopSize) + 1;
    for (int frame = 0; frame < numFrames; ++frame)
    {
        const int startSample = frame * config.hopSize;

        std::fill(fftBuffer_.begin(), fftBuffer_.end(), 0.0f);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* channelData = buffer.getReadPointer(ch);
            for (int i = 0; i < config.frameSize && (startSample + i) < numSamples; ++i)
            {
                fftBuffer_[i] += channelData[startSample + i] / numChannels;
            }
        }

        applyHannWindow(fftBuffer_.data(), config.frameSize);
        performFFT(fftBuffer_.data(), config.frameSize, magnitudeBuffer_.data(), config.frameSize);

        for (size_t i = 1; i < magnitudeBuffer_.size(); ++i)
        {
            const float energy = magnitudeBuffer_[i] * magnitudeBuffer_[i];
            totalEnergy += energy;
            if (static_cast<int>(i) >= highFreqBin)
            {
                highFreqEnergy += energy;
            }
        }
    }

    features.brightness = totalEnergy > 0.0f ? highFreqEnergy / totalEnergy : 0.0f;

    // Roughness (Daniel-Weber method - based on critical bandwidth and amplitude modulation)
    // Simplified: measure dissonance based on peak distances
    float roughnessSum = 0.0f;
    int roughnessCount = 0;

    for (int frame = 0; frame < numFrames; ++frame)
    {
        const int startSample = frame * config.hopSize;

        std::fill(fftBuffer_.begin(), fftBuffer_.end(), 0.0f);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* channelData = buffer.getReadPointer(ch);
            for (int i = 0; i < config.frameSize && (startSample + i) < numSamples; ++i)
            {
                fftBuffer_[i] += channelData[startSample + i] / numChannels;
            }
        }

        applyHannWindow(fftBuffer_.data(), config.frameSize);
        performFFT(fftBuffer_.data(), config.frameSize, magnitudeBuffer_.data(), config.frameSize);

        // Find spectral peaks
        std::vector<std::pair<float, float>> peaks; // (frequency, magnitude)
        for (size_t i = 2; i < magnitudeBuffer_.size() - 2; ++i)
        {
            if (magnitudeBuffer_[i] > magnitudeBuffer_[i - 1] &&
                magnitudeBuffer_[i] > magnitudeBuffer_[i + 1] &&
                magnitudeBuffer_[i] > magnitudeBuffer_[i - 2] &&
                magnitudeBuffer_[i] > magnitudeBuffer_[i + 2])
            {
                const float freq = static_cast<float>(i) * binWidth;
                if (freq > 50.0f && freq < 10000.0f) // Focus on audible range
                {
                    peaks.emplace_back(freq, magnitudeBuffer_[i]);
                }
            }
        }

        // Calculate roughness between peak pairs
        for (size_t i = 0; i < peaks.size(); ++i)
        {
            for (size_t j = i + 1; j < peaks.size(); ++j)
            {
                const float f1 = peaks[i].first;
                const float f2 = peaks[j].first;
                const float a1 = peaks[i].second;
                const float a2 = peaks[j].second;

                // Critical bandwidth (Bark scale approximation)
                const float fMin = std::min(f1, f2);
                const float criticalBandwidth = 25.0f + 75.0f * std::pow(1.0f + 1.4f * std::pow(fMin / 1000.0f, 2.0f), 0.69f);

                const float freqDiff = std::abs(f2 - f1);
                const float s = freqDiff / criticalBandwidth;

                // Dissonance curve (Plomp-Levelt model)
                float dissonance = 0.0f;
                if (s < 1.2f)
                {
                    dissonance = std::exp(-0.84f * s) * std::sin(PI * s / 1.2f);
                }

                // Amplitude weighting
                const float ampProduct = a1 * a2;
                roughnessSum += dissonance * ampProduct;
                ++roughnessCount;
            }
        }
    }

    features.roughness = roughnessCount > 0 ? roughnessSum / roughnessCount : 0.0f;

    // Sharpness (Aures model - high frequency weighted loudness)
    // Simplified: weighted sum with sharpness weighting curve
    float sharpnessNumerator = 0.0f;
    float sharpnessDenominator = 0.0f;

    for (int frame = 0; frame < numFrames; ++frame)
    {
        const int startSample = frame * config.hopSize;

        std::fill(fftBuffer_.begin(), fftBuffer_.end(), 0.0f);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* channelData = buffer.getReadPointer(ch);
            for (int i = 0; i < config.frameSize && (startSample + i) < numSamples; ++i)
            {
                fftBuffer_[i] += channelData[startSample + i] / numChannels;
            }
        }

        applyHannWindow(fftBuffer_.data(), config.frameSize);
        performFFT(fftBuffer_.data(), config.frameSize, magnitudeBuffer_.data(), config.frameSize);

        for (size_t i = 1; i < magnitudeBuffer_.size(); ++i)
        {
            const float freq = static_cast<float>(i) * binWidth;
            const float mag = magnitudeBuffer_[i];

            // Sharpness weighting (increases with frequency)
            float weight = 1.0f;
            if (freq > 2000.0f)
            {
                weight = 1.0f + 0.5f * std::log10(freq / 2000.0f);
            }

            sharpnessNumerator += mag * weight * weight;
            sharpnessDenominator += mag;
        }
    }

    features.sharpness = sharpnessDenominator > 0.0f ? sharpnessNumerator / sharpnessDenominator : 0.0f;

    // Dynamic range (simplified: difference between 95th and 10th percentile RMS)
    std::vector<float> rmsValues;
    const int rmsWindowSize = static_cast<int>(0.05f * config.sampleRate); // 50ms

    for (int i = 0; i < numSamples - rmsWindowSize; i += rmsWindowSize / 2)
    {
        float rms = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* data = buffer.getReadPointer(ch);
            for (int j = 0; j < rmsWindowSize && (i + j) < numSamples; ++j)
            {
                rms += data[i + j] * data[i + j];
            }
        }
        rmsValues.push_back(std::sqrt(rms / (rmsWindowSize * numChannels)));
    }

    if (rmsValues.size() >= 10)
    {
        std::sort(rmsValues.begin(), rmsValues.end());
        const size_t idx95 = static_cast<size_t>(rmsValues.size() * 0.95);
        const size_t idx10 = static_cast<size_t>(rmsValues.size() * 0.10);
        const float rms95 = rmsValues[std::min(idx95, rmsValues.size() - 1)];
        const float rms10 = rmsValues[idx10];

        features.dynamicRange = linearToDb(rms95 / std::max(rms10, 1e-10f));
    }

    return features;
}

// ── MFCC Computation ───────────────────────────────────────────────────────────

std::array<float, 13> FeatureExtractor::computeMFCC(const float* magnitudes, int size, double /*sampleRate*/)
{
    std::array<float, 13> mfcc{};
    mfcc.fill(0.0f);

    if (melFilterBank_.empty() || static_cast<int>(melFilterBank_.size()) != NUM_MEL_BANDS)
    {
        return mfcc;
    }

    // Apply mel filterbank
    std::vector<float> melEnergies(NUM_MEL_BANDS, 0.0f);
    for (int band = 0; band < NUM_MEL_BANDS; ++band)
    {
        float sum = 0.0f;
        for (int i = 0; i < size && i < static_cast<int>(melFilterBank_[band].size()); ++i)
        {
            sum += magnitudes[i] * magnitudes[i] * melFilterBank_[band][i];
        }
        melEnergies[band] = std::log(std::max(sum, 1e-10f));
    }

    // Apply DCT-II
    computeDCT(melEnergies.data(), mfcc.data(), 13, NUM_MEL_BANDS);

    return mfcc;
}

// ── LUFS Computation ────────────────────────────────────────────────────────────

float FeatureExtractor::computeLUFS(const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (numSamples == 0 || numChannels == 0)
        return -std::numeric_limits<float>::infinity();

    // Prepare K-weighting filters (ITU-R BS.1770-4)
    prepareLUFSFilters(sampleRate);

    // Create a copy for filtering
    juce::AudioBuffer<float> filteredBuffer(numChannels, numSamples);
    for (int ch = 0; ch < numChannels; ++ch)
    {
        filteredBuffer.copyFrom(ch, 0, buffer, ch, 0, numSamples);
    }

    // Apply K-weighting: high shelf + high pass
    juce::dsp::AudioBlock<float> block(filteredBuffer);
    juce::dsp::ProcessContextReplacing<float> context(block);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        // Reset filter state for each channel
        prepareLUFSFilters(sampleRate);
        auto channelBlock = block.getSingleChannelBlock(ch);
        juce::dsp::ProcessContextReplacing<float> channelContext(channelBlock);
        lufsHighShelf_.process(channelContext);
        lufsHighPass_.process(channelContext);
    }

    // Calculate mean square for each channel
    // ITU-R BS.1770-4 uses specific channel weights
    const float channelWeights[] = {1.0f, 1.0f, 1.0f, 1.41f, 1.41f}; // L, R, C, Ls, Rs
    float weightedSum = 0.0f;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float weight = ch < 5 ? channelWeights[ch] : 1.0f;
        const float* data = filteredBuffer.getReadPointer(ch);

        float sumSquares = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            sumSquares += data[i] * data[i];
        }
        weightedSum += weight * sumSquares / numSamples;
    }

    // Apply gating (ITU-R BS.1770-4)
    // Absolute gate at -70 LUFS
    const float ungatedLoudness = -0.691f + 10.0f * std::log10(std::max(weightedSum, 1e-20f));

    if (ungatedLoudness < ABSOLUTE_THRESHOLD_LUFS)
    {
        return ungatedLoudness; // Below gate
    }

    // Relative gate at -10 LU relative to ungated loudness
    const float relativeThreshold = ungatedLoudness - 10.0f;

    // Recalculate with gating
    const float gateThreshold = std::pow(10.0f, (relativeThreshold + 0.691f) / 10.0f);
    float gatedSum = 0.0f;
    int gatedCount = 0;

    const int blockSize = static_cast<int>(0.4f * sampleRate); // 400ms blocks
    for (int blockStart = 0; blockStart < numSamples; blockStart += blockSize / 2)
    {
        const int blockEnd = std::min(blockStart + blockSize, numSamples);
        float blockSumSquares = 0.0f;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float weight = ch < 5 ? channelWeights[ch] : 1.0f;
            const float* data = filteredBuffer.getReadPointer(ch);

            for (int i = blockStart; i < blockEnd; ++i)
            {
                blockSumSquares += weight * data[i] * data[i];
            }
        }

        const float blockMeanSquare = blockSumSquares / ((blockEnd - blockStart) * numChannels);
        if (blockMeanSquare >= gateThreshold)
        {
            gatedSum += blockMeanSquare;
            ++gatedCount;
        }
    }

    if (gatedCount == 0)
    {
        return ungatedLoudness;
    }

    const float gatedLoudness = -0.691f + 10.0f * std::log10(gatedSum / gatedCount);
    return gatedLoudness;
}

// ── Chroma Computation ──────────────────────────────────────────────────────────

std::array<float, 12> FeatureExtractor::computeChroma(const float* magnitudes, int size, double sampleRate)
{
    std::array<float, 12> chroma{};
    chroma.fill(0.0f);

    const float binWidth = static_cast<float>(sampleRate) / (size * 2);
    const float refFreq = 130.81278f; // C3 in Hz
    const int minBin = static_cast<int>(refFreq / binWidth) - 1;

    for (int i = std::max(1, minBin); i < size; ++i)
    {
        const float freq = static_cast<float>(i) * binWidth;

        if (freq < 20.0f || freq > 20000.0f)
            continue;

        // Convert frequency to pitch class
        const float semitones = 12.0f * std::log2(freq / refFreq);
        const int pitchClass = static_cast<int>(std::round(semitones)) % 12;
        const int clampedPitchClass = ((pitchClass % 12) + 12) % 12;

        chroma[clampedPitchClass] += magnitudes[i] * magnitudes[i];
    }

    // Normalize
    float maxChroma = 0.0f;
    for (int i = 0; i < 12; ++i)
    {
        maxChroma = std::max(maxChroma, chroma[i]);
    }
    if (maxChroma > 0.0f)
    {
        for (int i = 0; i < 12; ++i)
        {
            chroma[i] /= maxChroma;
        }
    }

    return chroma;
}

// ── JSON Export ─────────────────────────────────────────────────────────────────

nlohmann::json FeatureExtractor::toJson(const AudioFeatures& features) const
{
    nlohmann::json j;

    // Spectral features
    j["spectral"]["mfcc"] = std::vector<float>(features.spectral.mfcc.begin(), features.spectral.mfcc.end());
    j["spectral"]["centroid_hz"] = features.spectral.spectralCentroid;
    j["spectral"]["rolloff_hz"] = features.spectral.spectralRolloff;
    j["spectral"]["flux"] = features.spectral.spectralFlux;
    j["spectral"]["spread_hz"] = features.spectral.spectralSpread;
    j["spectral"]["flatness"] = features.spectral.spectralFlatness;
    j["spectral"]["chroma"] = std::vector<float>(features.spectral.chroma.begin(), features.spectral.chroma.end());

    // Temporal features
    j["temporal"]["rms_db"] = features.temporal.rmsEnergy;
    j["temporal"]["peak_db"] = features.temporal.peakAmplitude;
    j["temporal"]["crest_factor"] = features.temporal.crestFactor;
    j["temporal"]["attack_ms"] = features.temporal.attackTime;
    j["temporal"]["transient_density"] = features.temporal.transientDensity;
    j["temporal"]["zcr"] = features.temporal.zeroCrossingRate;

    // Perceptual features
    j["perceptual"]["lufs"] = features.perceptual.lufs;
    j["perceptual"]["true_peak_db"] = features.perceptual.truePeakDb;
    j["perceptual"]["roughness"] = features.perceptual.roughness;
    j["perceptual"]["sharpness"] = features.perceptual.sharpness;
    j["perceptual"]["brightness"] = features.perceptual.brightness;
    j["perceptual"]["dynamic_range"] = features.perceptual.dynamicRange;

    // Metadata
    j["duration_seconds"] = features.durationSeconds;
    j["frame_count"] = features.frameCount;

    return j;
}

// ── Vector Export ───────────────────────────────────────────────────────────────

std::vector<float> FeatureExtractor::toVector(const AudioFeatures& features) const
{
    std::vector<float> vec;
    vec.reserve(getFeatureDimension());

    // Spectral (30 values)
    for (const auto& mfcc : features.spectral.mfcc)
        vec.push_back(mfcc);
    vec.push_back(features.spectral.spectralCentroid);
    vec.push_back(features.spectral.spectralRolloff);
    vec.push_back(features.spectral.spectralFlux);
    vec.push_back(features.spectral.spectralSpread);
    vec.push_back(features.spectral.spectralFlatness);
    for (const auto& chroma : features.spectral.chroma)
        vec.push_back(chroma);

    // Temporal (6 values)
    vec.push_back(features.temporal.rmsEnergy);
    vec.push_back(features.temporal.peakAmplitude);
    vec.push_back(features.temporal.crestFactor);
    vec.push_back(features.temporal.attackTime);
    vec.push_back(features.temporal.transientDensity);
    vec.push_back(features.temporal.zeroCrossingRate);

    // Perceptual (6 values)
    vec.push_back(features.perceptual.lufs);
    vec.push_back(features.perceptual.truePeakDb);
    vec.push_back(features.perceptual.roughness);
    vec.push_back(features.perceptual.sharpness);
    vec.push_back(features.perceptual.brightness);
    vec.push_back(features.perceptual.dynamicRange);

    return vec;
}

// ── Private Methods ─────────────────────────────────────────────────────────────

void FeatureExtractor::performFFT(const float* samples, int numSamples, float* magnitudes, int fftSize)
{
    // Copy samples to FFT buffer (real part)
    for (int i = 0; i < fftSize; ++i)
    {
        fftBuffer_[i * 2] = i < numSamples ? samples[i] : 0.0f;
        fftBuffer_[i * 2 + 1] = 0.0f; // Imaginary part
    }

    // Perform FFT
    fft_->perform(reinterpret_cast<const juce::dsp::Complex<float>*>(fftBuffer_.data()),
                 reinterpret_cast<juce::dsp::Complex<float>*>(fftBuffer_.data()), false);

    // Calculate magnitudes
    const int numBins = fftSize / 2 + 1;
    for (int i = 0; i < numBins; ++i)
    {
        const float real = fftBuffer_[i * 2];
        const float imag = fftBuffer_[i * 2 + 1];
        magnitudes[i] = std::sqrt(real * real + imag * imag) / fftSize;
    }
}

void FeatureExtractor::createMelFilterBank(int fftSize, double sampleRate, int numBands)
{
    const int numBins = fftSize / 2 + 1;
    const float binWidth = static_cast<float>(sampleRate) / fftSize;

    const float lowMel = hzToMel(0.0f);
    const float highMel = hzToMel(static_cast<float>(sampleRate) / 2.0f);
    const float melStep = (highMel - lowMel) / (numBands + 1);

    std::vector<float> melPoints(numBands + 2);
    for (int i = 0; i < numBands + 2; ++i)
    {
        melPoints[i] = lowMel + melStep * i;
    }

    std::vector<int> binPoints(numBands + 2);
    for (int i = 0; i < numBands + 2; ++i)
    {
        const float hz = melToHz(melPoints[i]);
        binPoints[i] = static_cast<int>(std::round(hz / binWidth));
    }

    melFilterBank_.resize(numBands);
    for (int band = 0; band < numBands; ++band)
    {
        melFilterBank_[band].resize(numBins, 0.0f);

        const int left = binPoints[band];
        const int center = binPoints[band + 1];
        const int right = binPoints[band + 2];

        for (int i = left; i < center && i < numBins; ++i)
        {
            melFilterBank_[band][i] = static_cast<float>(i - left) / (center - left);
        }
        for (int i = center; i < right && i < numBins; ++i)
        {
            melFilterBank_[band][i] = static_cast<float>(right - i) / (right - center);
        }
    }
}

void FeatureExtractor::prepareLUFSFilters(double sampleRate)
{
    if (std::abs(currentLufsSampleRate_ - sampleRate) < 0.1)
        return;

    currentLufsSampleRate_ = sampleRate;

    // High shelf filter: +4dB at 1681Hz (Q = 0.707)
    juce::dsp::IIR::Coefficients<float>::Ptr highShelfCoeffs =
        juce::dsp::IIR::Coefficients<float>::makeHighShelf(sampleRate, 1681.0f, 0.707f, 4.0f);
    *lufsHighShelf_.coefficients = *highShelfCoeffs;

    // High pass filter: -3dB at 38Hz (Q = 0.5)
    juce::dsp::IIR::Coefficients<float>::Ptr highPassCoeffs =
        juce::dsp::IIR::Coefficients<float>::makeHighPass(sampleRate, 38.0f, 0.5f);
    *lufsHighPass_.coefficients = *highPassCoeffs;
}

void FeatureExtractor::computeDCT(const float* input, float* output, int numCoeffs, int numFilters)
{
    for (int k = 0; k < numCoeffs; ++k)
    {
        float sum = 0.0f;
        for (int n = 0; n < numFilters; ++n)
        {
            sum += input[n] * std::cos(PI * k * (n + 0.5f) / numFilters);
        }
        output[k] = sum;
    }
}

void FeatureExtractor::applyHannWindow(float* samples, int size)
{
    for (int i = 0; i < size; ++i)
    {
        const float window = 0.5f * (1.0f - std::cos(2.0f * PI * i / (size - 1)));
        samples[i] *= window;
    }
}

} // namespace more_phi
