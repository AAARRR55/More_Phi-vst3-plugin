/*
 * MorphSnap — AI/Dataset/AudioContentLibrary.cpp
 * Implementation of source audio library management for dataset generation.
 */
#include "AudioContentLibrary.h"
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <algorithm>

namespace morphsnap {

// ── Constructor ───────────────────────────────────────────────────────────────

AudioContentLibrary::AudioContentLibrary()
{
    // Register standard audio formats
    formatManager_.registerFormat(new juce::WavAudioFormat(), true);
    formatManager_.registerFormat(new juce::FlacAudioFormat(), false);
    formatManager_.registerFormat(new juce::AiffAudioFormat(), false);
}

// ── Library Management ────────────────────────────────────────────────────────

bool AudioContentLibrary::scanDirectory(const juce::File& directory)
{
    if (!directory.isDirectory())
        return false;

    juce::StringArray extensions = {"*.wav", "*.flac", "*.aiff", "*.aif"};
    auto files = directory.findChildFiles(juce::File::findFiles, true, extensions);

    for (const auto& file : files)
    {
        // Try to infer genre from directory structure
        Genre genre = Genre::Electronic; // Default
        juce::String path = file.getFullPathName().toLowerCase();

        if (path.contains("electronic") || path.contains("edm") || path.contains("techno") ||
            path.contains("house") || path.contains("dnb") || path.contains("drum"))
            genre = Genre::Electronic;
        else if (path.contains("rock") || path.contains("pop") || path.contains("indie"))
            genre = Genre::RockPop;
        else if (path.contains("jazz") || path.contains("acoustic") || path.contains("folk"))
            genre = Genre::JazzAcoustic;
        else if (path.contains("hip") || path.contains("hop") || path.contains("r&b") ||
                 path.contains("rap") || path.contains("trap") || path.contains("soul"))
            genre = Genre::HipHopRnB;
        else if (path.contains("classical") || path.contains("orchestra") || path.contains("chamber"))
            genre = Genre::Classical;
        else if (path.contains("stem") || path.contains("track") || path.contains("isolated"))
            genre = Genre::Stems;
        else if (path.contains("test") || path.contains("sweep") || path.contains("impulse") ||
                 path.contains("noise") || path.contains("calibration"))
            genre = Genre::TestSignals;

        addFile(file, genre);
    }

    return true;
}

bool AudioContentLibrary::addFile(const juce::File& file, Genre genre)
{
    if (!file.existsAsFile())
        return false;

    // Check if already in library
    for (const auto& src : sources_)
    {
        if (src.file == file)
            return false; // Already exists
    }

    SourceAudio source(file);
    source.genre = genre;
    source.targetPercentage = getGenreTargetPercentage(genre);

    // Try to read basic audio properties
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(file));
    if (reader != nullptr)
    {
        source.sampleRate = reader->sampleRate;
        source.numChannels = static_cast<int>(reader->numChannels);
        source.numSamples = reader->lengthInSamples;
        source.subgenre = file.getFileNameWithoutExtension();
    }

    sources_.add(source);
    return true;
}

bool AudioContentLibrary::addFile(const SourceAudio& source)
{
    if (!source.file.existsAsFile())
        return false;

    // Check for duplicates
    for (const auto& src : sources_)
    {
        if (src.file == source.file)
            return false;
    }

    sources_.add(source);
    return true;
}

bool AudioContentLibrary::removeFile(const juce::File& file)
{
    for (int i = sources_.size() - 1; i >= 0; --i)
    {
        if (sources_[i].file == file)
        {
            sources_.remove(i);
            return true;
        }
    }
    return false;
}

void AudioContentLibrary::clear()
{
    sources_.clear();
}

// ── Content Retrieval ──────────────────────────────────────────────────────────

juce::Array<SourceAudio> AudioContentLibrary::getByGenre(Genre genre) const
{
    juce::Array<SourceAudio> result;
    for (const auto& src : sources_)
    {
        if (src.genre == genre)
            result.add(src);
    }
    return result;
}

SourceAudio AudioContentLibrary::getRandomSource(Genre genre)
{
    auto genreFiles = getByGenre(genre);
    if (genreFiles.isEmpty())
        return SourceAudio();

    int index = random_.nextInt(genreFiles.size());
    return genreFiles[index];
}

SourceAudio AudioContentLibrary::getRandomSourceByDistribution()
{
    if (sources_.isEmpty())
        return SourceAudio();

    // Build weighted distribution based on target percentages
    float totalWeight = 0.0f;
    juce::Array<float> weights;

    for (const auto& src : sources_)
    {
        totalWeight += src.targetPercentage;
        weights.add(src.targetPercentage);
    }

    // Select random weighted
    float selection = random_.nextFloat() * totalWeight;
    float cumulative = 0.0f;

    for (int i = 0; i < sources_.size(); ++i)
    {
        cumulative += weights[i];
        if (selection <= cumulative)
            return sources_[i];
    }

    // Fallback to uniform random
    return sources_[random_.nextInt(sources_.size())];
}

juce::Array<SourceAudio> AudioContentLibrary::findByCharacteristics(
    const AudioCharacteristics& target,
    float tolerance) const
{
    juce::Array<SourceAudio> result;

    for (const auto& src : sources_)
    {
        float diff = 0.0f;
        diff += std::abs(src.characteristics.dynamicRangeDb - target.dynamicRangeDb) / 60.0f;
        diff += std::abs(src.characteristics.spectralCentroidHz - target.spectralCentroidHz) / 10000.0f;
        diff += std::abs(src.characteristics.rhythmicDensity - target.rhythmicDensity);
        diff += std::abs(src.characteristics.harmonicComplexity - target.harmonicComplexity);

        float avgDiff = diff / 4.0f;
        if (avgDiff <= tolerance)
            result.add(src);
    }

    return result;
}

juce::Array<SourceAudio> AudioContentLibrary::findByTag(const juce::String& tag) const
{
    juce::Array<SourceAudio> result;
    juce::String lowerTag = tag.toLowerCase();

    for (const auto& src : sources_)
    {
        for (const auto& t : src.tags)
        {
            if (t.toLowerCase().contains(lowerTag))
            {
                result.add(src);
                break;
            }
        }
    }

    return result;
}

// ── Characteristic Extraction ──────────────────────────────────────────────────

AudioCharacteristics AudioContentLibrary::extractCharacteristics(
    const juce::AudioBuffer<float>& buffer,
    double sampleRate)
{
    AudioCharacteristics chars;

    if (buffer.getNumSamples() == 0 || buffer.getNumChannels() == 0)
        return chars;

    // Compute basic metrics
    chars.peakDb = computePeakDb(buffer);
    chars.zeroCrossingRate = computeZeroCrossingRate(buffer);
    chars.lufs = computeLoudnessLUFS(buffer, sampleRate);
    chars.dynamicRangeDb = chars.peakDb - chars.lufs;

    // Compute spectral features using FFT
    juce::dsp::FFT fft(static_cast<int>(std::log2(fftSize)));
    std::vector<float> window(fftSize);
    std::vector<std::complex<float>> fftData(fftSize);

    // Create Hann window
    for (int i = 0; i < fftSize; ++i)
        window[i] = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * i / (fftSize - 1)));

    // Process multiple frames and average
    int numFrames = 0;
    float spectralCentroidSum = 0.0f;
    float spectralFlatnessSum = 0.0f;

    const float* channelData = buffer.getReadPointer(0);
    int totalSamples = buffer.getNumSamples();

    for (int start = 0; start + fftSize <= totalSamples; start += hopSize)
    {
        // Apply window and prepare FFT input
        for (int i = 0; i < fftSize; ++i)
        {
            fftData[i] = std::complex<float>(channelData[start + i] * window[i], 0.0f);
        }

        fft.perform(fftData.data(), fftData.data(), false);

        // Compute magnitudes
        std::vector<float> magnitudes(fftSize / 2 + 1);
        for (int i = 0; i <= fftSize / 2; ++i)
        {
            magnitudes[i] = std::abs(fftData[i]);
        }

        spectralCentroidSum += computeSpectralCentroid(magnitudes.data(), static_cast<int>(magnitudes.size()), sampleRate);
        spectralFlatnessSum += computeSpectralFlatness(magnitudes.data(), static_cast<int>(magnitudes.size()));
        ++numFrames;
    }

    if (numFrames > 0)
    {
        chars.spectralCentroidHz = spectralCentroidSum / numFrames;
        chars.spectralFlatness = spectralFlatnessSum / numFrames;
    }

    // Compute rhythmic and harmonic features
    chars.rhythmicDensity = computeRhythmicDensity(buffer, sampleRate);
    chars.harmonicComplexity = computeHarmonicComplexity(buffer, sampleRate);

    return chars;
}

AudioCharacteristics AudioContentLibrary::extractCharacteristicsFromFile(const juce::File& file)
{
    auto buffer = loadFile(file);
    if (buffer == nullptr)
        return AudioCharacteristics();

    // Get sample rate from reader
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(file));
    double sampleRate = reader ? reader->sampleRate : 48000.0;

    return extractCharacteristics(*buffer, sampleRate);
}

void AudioContentLibrary::analyzeCharacteristics(SourceAudio& source)
{
    source.characteristics = extractCharacteristicsFromFile(source.file);

    // Update sample info if not set
    if (source.sampleRate == 0 || source.numSamples == 0)
    {
        std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(source.file));
        if (reader)
        {
            source.sampleRate = reader->sampleRate;
            source.numChannels = static_cast<int>(reader->numChannels);
            source.numSamples = reader->lengthInSamples;
        }
    }
}

void AudioContentLibrary::analyzeAllCharacteristics()
{
    for (auto& source : sources_)
    {
        analyzeCharacteristics(source);
    }
}

// ── Augmentation ───────────────────────────────────────────────────────────────

juce::AudioBuffer<float> AudioContentLibrary::applyAugmentation(
    const juce::AudioBuffer<float>& source,
    const AugmentationSettings& settings,
    double sampleRate)
{
    if (!settings.enabled)
        return source;

    juce::AudioBuffer<float> result = source;

    // Apply time stretch (most computationally expensive, do first)
    if (std::abs(settings.timeStretchRatio - 1.0f) > 0.01f)
    {
        result = applyTimeStretch(result, settings.timeStretchRatio, sampleRate);
    }

    // Apply pitch shift
    if (settings.pitchShiftSemitones != 0)
    {
        result = applyPitchShift(result, settings.pitchShiftSemitones, sampleRate);
    }

    // Apply dynamic range manipulation
    if (std::abs(settings.dynamicRangeScale - 1.0f) > 0.01f)
    {
        result = applyDynamicRangeManipulation(result, settings.dynamicRangeScale);
    }

    // Apply gain and normalization
    applyGainAndNormalize(result, settings.gainDb, settings.normalizeOutput);

    return result;
}

std::unique_ptr<juce::AudioBuffer<float>> AudioContentLibrary::loadAndAugment(
    const juce::File& file,
    const AugmentationSettings& settings)
{
    auto buffer = loadFile(file);
    if (buffer == nullptr)
        return nullptr;

    // Get sample rate
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(file));
    double sampleRate = reader ? reader->sampleRate : 48000.0;

    auto augmented = std::make_unique<juce::AudioBuffer<float>>(
        applyAugmentation(*buffer, settings, sampleRate));

    return augmented;
}

// ── Genre Utilities ────────────────────────────────────────────────────────────

juce::String AudioContentLibrary::genreToString(Genre g)
{
    switch (g)
    {
        case Genre::Electronic:   return "Electronic";
        case Genre::RockPop:      return "Rock/Pop";
        case Genre::JazzAcoustic: return "Jazz/Acoustic";
        case Genre::HipHopRnB:    return "Hip-Hop/R&B";
        case Genre::Classical:    return "Classical";
        case Genre::Stems:        return "Stems";
        case Genre::TestSignals:  return "Test Signals";
        default:                  return "Unknown";
    }
}

Genre AudioContentLibrary::stringToGenre(const juce::String& s)
{
    auto lower = s.toLowerCase().removeCharacters(" /-");

    if (lower == "electronic" || lower == "edm") return Genre::Electronic;
    if (lower == "rockpop" || lower == "rock" || lower == "pop") return Genre::RockPop;
    if (lower == "jazzacoustic" || lower == "jazz" || lower == "acoustic") return Genre::JazzAcoustic;
    if (lower == "hiphopr&b" || lower == "hiphop" || lower == "r&b") return Genre::HipHopRnB;
    if (lower == "classical") return Genre::Classical;
    if (lower == "stems") return Genre::Stems;
    if (lower == "testsignals" || lower == "test") return Genre::TestSignals;

    return Genre::Electronic; // Default
}

float AudioContentLibrary::getGenreTargetPercentage(Genre g)
{
    switch (g)
    {
        case Genre::Electronic:   return 0.25f;   // 25%
        case Genre::RockPop:      return 0.20f;   // 20%
        case Genre::JazzAcoustic: return 0.15f;   // 15%
        case Genre::HipHopRnB:    return 0.15f;   // 15%
        case Genre::Classical:    return 0.05f;   // 5%
        case Genre::Stems:        return 0.15f;   // 15%
        case Genre::TestSignals:  return 0.05f;   // 5%
        default:                  return 0.10f;
    }
}

juce::StringArray AudioContentLibrary::getAllGenreNames()
{
    return {
        genreToString(Genre::Electronic),
        genreToString(Genre::RockPop),
        genreToString(Genre::JazzAcoustic),
        genreToString(Genre::HipHopRnB),
        genreToString(Genre::Classical),
        genreToString(Genre::Stems),
        genreToString(Genre::TestSignals)
    };
}

// ── File I/O ───────────────────────────────────────────────────────────────────

std::unique_ptr<juce::AudioBuffer<float>> AudioContentLibrary::loadFile(const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(file));
    if (reader == nullptr)
        return nullptr;

    auto buffer = std::make_unique<juce::AudioBuffer<float>>(
        static_cast<int>(reader->numChannels),
        static_cast<int>(reader->lengthInSamples));

    reader->read(buffer.get(), 0, static_cast<int>(reader->lengthInSamples), 0, true, true);

    return buffer;
}

bool AudioContentLibrary::saveFile(const juce::AudioBuffer<float>& buffer,
                                    const juce::File& file,
                                    double sampleRate,
                                    int bitDepth)
{
    juce::WavAudioFormat wavFormat;
    juce::FileOutputStream* fos = new juce::FileOutputStream(file);

    if (!fos->openedOk())
    {
        delete fos;
        return false;
    }

    std::unique_ptr<juce::AudioFormatWriter> writer(
        wavFormat.createWriterFor(fos, sampleRate, buffer.getNumChannels(), bitDepth, {}, 0));

    if (writer == nullptr)
        return false;

    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

// ── Serialization ──────────────────────────────────────────────────────────────

nlohmann::json AudioContentLibrary::toJson() const
{
    nlohmann::json j;
    j["version"] = 1;
    j["sources"] = nlohmann::json::array();

    for (const auto& src : sources_)
    {
        j["sources"].push_back(src.toJson());
    }

    // Add statistics
    j["statistics"] = {
        {"totalFiles", sources_.size()},
        {"byGenre", nlohmann::json::object()}
    };

    for (int g = 0; g < static_cast<int>(Genre::Count); ++g)
    {
        auto genre = static_cast<Genre>(g);
        int count = getByGenre(genre).size();
        j["statistics"]["byGenre"][genreToString(genre).toStdString()] = count;
    }

    return j;
}

bool AudioContentLibrary::fromJson(const nlohmann::json& j)
{
    if (!j.contains("sources") || !j["sources"].is_array())
        return false;

    sources_.clear();

    for (const auto& srcJson : j["sources"])
    {
        SourceAudio src;
        src.file = juce::File(srcJson.value("filePath", ""));

        if (!src.file.existsAsFile())
            continue; // Skip missing files

        if (srcJson.contains("genre"))
            src.genre = static_cast<Genre>(srcJson["genre"].get<int>());

        if (srcJson.contains("subgenre"))
            src.subgenre = juce::String(srcJson["subgenre"].get<std::string>());

        if (srcJson.contains("targetPercentage"))
            src.targetPercentage = srcJson["targetPercentage"].get<float>();

        if (srcJson.contains("sampleRate"))
            src.sampleRate = srcJson["sampleRate"].get<double>();

        if (srcJson.contains("numChannels"))
            src.numChannels = srcJson["numChannels"].get<int>();

        if (srcJson.contains("numSamples"))
            src.numSamples = srcJson["numSamples"].get<int64>();

        if (srcJson.contains("description"))
            src.description = juce::String(srcJson["description"].get<std::string>());

        if (srcJson.contains("characteristics"))
            src.characteristics = AudioCharacteristics::fromJson(srcJson["characteristics"]);

        if (srcJson.contains("tags") && srcJson["tags"].is_array())
        {
            for (const auto& tag : srcJson["tags"])
                src.tags.add(juce::String(tag.get<std::string>()));
        }

        sources_.add(src);
    }

    return true;
}

bool AudioContentLibrary::saveMetadata(const juce::File& file) const
{
    try
    {
        auto j = toJson();
        juce::String jsonStr = juce::String(j.dump(2));
        return file.replaceWithText(jsonStr);
    }
    catch (...)
    {
        return false;
    }
}

bool AudioContentLibrary::loadMetadata(const juce::File& file)
{
    if (!file.existsAsFile())
        return false;

    try
    {
        juce::String jsonStr = file.loadFileAsString();
        auto j = nlohmann::json::parse(jsonStr.toStdString());
        return fromJson(j);
    }
    catch (...)
    {
        return false;
    }
}

// ── Internal Analysis Helpers ──────────────────────────────────────────────────

float AudioContentLibrary::computeSpectralCentroid(const float* magnitudes, int size, double sampleRate)
{
    float sumWeighted = 0.0f;
    float sumMagnitudes = 0.0f;

    for (int i = 0; i < size; ++i)
    {
        float freq = static_cast<float>(i * sampleRate / (2.0 * (size - 1)));
        sumWeighted += magnitudes[i] * freq;
        sumMagnitudes += magnitudes[i];
    }

    return sumMagnitudes > 0.0f ? sumWeighted / sumMagnitudes : 0.0f;
}

float AudioContentLibrary::computeRhythmicDensity(const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    // Use onset detection via spectral flux
    const int frameSize = 1024;
    const int hop = 512;
    const float* data = buffer.getReadPointer(0);
    int numSamples = buffer.getNumSamples();

    if (numSamples < frameSize * 2)
        return 0.0f;

    juce::dsp::FFT fft(static_cast<int>(std::log2(frameSize)));
    std::vector<float> prevMagnitudes(frameSize / 2 + 1, 0.0f);
    std::vector<std::complex<float>> fftData(frameSize);

    float totalFlux = 0.0f;
    int frameCount = 0;

    for (int start = 0; start + frameSize <= numSamples; start += hop)
    {
        // Apply window and FFT
        for (int i = 0; i < frameSize; ++i)
        {
            float window = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * i / (frameSize - 1)));
            fftData[i] = std::complex<float>(data[start + i] * window, 0.0f);
        }

        fft.perform(fftData.data(), fftData.data(), false);

        // Compute spectral flux
        float flux = 0.0f;
        for (int i = 0; i <= frameSize / 2; ++i)
        {
            float mag = std::abs(fftData[i]);
            float diff = mag - prevMagnitudes[i];
            if (diff > 0.0f)
                flux += diff * diff;
            prevMagnitudes[i] = mag;
        }

        totalFlux += std::sqrt(flux);
        ++frameCount;
    }

    // Normalize to 0-1 range based on typical values
    if (frameCount == 0)
        return 0.0f;

    float avgFlux = totalFlux / frameCount;
    float duration = static_cast<float>(numSamples) / static_cast<float>(sampleRate);

    // Events per second, normalized to 0-1 (assuming max ~20 events/sec)
    float eventsPerSecond = avgFlux * 10.0f; // Scaling factor
    return std::clamp(eventsPerSecond / 20.0f, 0.0f, 1.0f);
}

float AudioContentLibrary::computeHarmonicComplexity(const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    // Simplified chroma-based harmonic complexity
    const int frameSize = 4096;
    const int hop = 2048;
    const float* data = buffer.getReadPointer(0);
    int numSamples = buffer.getNumSamples();

    if (numSamples < frameSize)
        return 0.0f;

    juce::dsp::FFT fft(static_cast<int>(std::log2(frameSize)));
    std::vector<std::complex<float>> fftData(frameSize);

    // Accumulate chroma across frames
    std::vector<float> chroma(12, 0.0f);
    int frameCount = 0;

    for (int start = 0; start + frameSize <= numSamples; start += hop)
    {
        for (int i = 0; i < frameSize; ++i)
        {
            float window = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * i / (frameSize - 1)));
            fftData[i] = std::complex<float>(data[start + i] * window, 0.0f);
        }

        fft.perform(fftData.data(), fftData.data(), false);

        // Map frequency bins to pitch classes
        for (int i = 1; i <= frameSize / 2; ++i)
        {
            float freq = static_cast<float>(i * sampleRate / frameSize);
            float mag = std::abs(fftData[i]);

            if (freq < 20.0f || freq > 5000.0f)
                continue;

            // Convert frequency to MIDI note, then to pitch class
            float midiNote = 69.0f + 12.0f * std::log2(freq / 440.0f);
            int pitchClass = static_cast<int>(std::round(midiNote)) % 12;
            if (pitchClass < 0) pitchClass += 12;

            chroma[pitchClass] += mag;
        }

        ++frameCount;
    }

    // Compute complexity as entropy of chroma distribution
    float sum = 0.0f;
    for (float c : chroma)
        sum += c;

    if (sum == 0.0f)
        return 0.0f;

    float entropy = 0.0f;
    for (float c : chroma)
    {
        if (c > 0.0f)
        {
            float p = c / sum;
            entropy -= p * std::log2(p);
        }
    }

    // Normalize: max entropy for 12 classes is log2(12) ~ 3.58
    return entropy / std::log2(12.0f);
}

float AudioContentLibrary::computeSpectralFlatness(const float* magnitudes, int size)
{
    float sumLog = 0.0f;
    float sum = 0.0f;
    int count = 0;

    for (int i = 0; i < size; ++i)
    {
        if (magnitudes[i] > 1e-10f)
        {
            sumLog += std::log(magnitudes[i]);
            sum += magnitudes[i];
            ++count;
        }
    }

    if (count == 0 || sum == 0.0f)
        return 0.0f;

    float geometricMean = std::exp(sumLog / count);
    float arithmeticMean = sum / count;

    return arithmeticMean > 0.0f ? geometricMean / arithmeticMean : 0.0f;
}

float AudioContentLibrary::computeLoudnessLUFS(const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    // Simplified LUFS calculation using K-weighting
    const float* left = buffer.getReadPointer(0);
    const float* right = buffer.getNumChannels() > 1 ? buffer.getReadPointer(1) : left;
    int numSamples = buffer.getNumSamples();

    // Apply K-weighting (high-pass + high-shelf)
    // Simplified: just compute RMS with a frequency-weighting approximation
    float sumSq = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        float sample = (left[i] + right[i]) * 0.5f;
        sumSq += sample * sample;
    }

    float rms = numSamples > 0 ? std::sqrt(sumSq / numSamples) : 0.0f;

    // Convert to LUFS (simplified, actual LUFS is more complex)
    if (rms < 1e-10f)
        return -100.0f;

    return 20.0f * std::log10(rms) - 0.7f; // Approximate offset
}

float AudioContentLibrary::computePeakDb(const juce::AudioBuffer<float>& buffer)
{
    float maxSample = 0.0f;

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        const float* data = buffer.getReadPointer(ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            maxSample = std::max(maxSample, std::abs(data[i]));
        }
    }

    if (maxSample < 1e-10f)
        return -100.0f;

    return 20.0f * std::log10(maxSample);
}

float AudioContentLibrary::computeZeroCrossingRate(const juce::AudioBuffer<float>& buffer)
{
    if (buffer.getNumSamples() < 2)
        return 0.0f;

    const float* data = buffer.getReadPointer(0);
    int crossings = 0;

    for (int i = 1; i < buffer.getNumSamples(); ++i)
    {
        if ((data[i] >= 0.0f && data[i-1] < 0.0f) || (data[i] < 0.0f && data[i-1] >= 0.0f))
            ++crossings;
    }

    // Normalize by sample count
    return static_cast<float>(crossings) / static_cast<float>(buffer.getNumSamples() - 1);
}

// ── Augmentation Helpers ───────────────────────────────────────────────────────

juce::AudioBuffer<float> AudioContentLibrary::applyTimeStretch(
    const juce::AudioBuffer<float>& source,
    float ratio,
    double sampleRate)
{
    if (ratio <= 0.0f || std::abs(ratio - 1.0f) < 0.001f)
        return source;

    // Simple resampling-based time stretch (phase vocoder would be better but more complex)
    int sourceSamples = source.getNumSamples();
    int numChannels = source.getNumChannels();
    int targetSamples = static_cast<int>(sourceSamples / ratio);

    juce::AudioBuffer<float> result(numChannels, targetSamples);

    // Use linear interpolation for simplicity
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* src = source.getReadPointer(ch);
        float* dst = result.getWritePointer(ch);

        for (int i = 0; i < targetSamples; ++i)
        {
            float srcPos = static_cast<float>(i) * ratio;
            int srcIndex = static_cast<int>(srcPos);
            float frac = srcPos - srcIndex;

            if (srcIndex + 1 < sourceSamples)
            {
                dst[i] = src[srcIndex] * (1.0f - frac) + src[srcIndex + 1] * frac;
            }
            else if (srcIndex < sourceSamples)
            {
                dst[i] = src[srcIndex];
            }
            else
            {
                dst[i] = 0.0f;
            }
        }
    }

    return result;
}

juce::AudioBuffer<float> AudioContentLibrary::applyPitchShift(
    const juce::AudioBuffer<float>& source,
    int semitones,
    double sampleRate)
{
    if (semitones == 0)
        return source;

    // Simple pitch shift via resampling
    float pitchRatio = std::pow(2.0f, static_cast<float>(semitones) / 12.0f);

    int sourceSamples = source.getNumSamples();
    int numChannels = source.getNumChannels();
    int targetSamples = static_cast<int>(sourceSamples * pitchRatio);

    juce::AudioBuffer<float> result(numChannels, targetSamples);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* src = source.getReadPointer(ch);
        float* dst = result.getWritePointer(ch);

        for (int i = 0; i < targetSamples; ++i)
        {
            float srcPos = static_cast<float>(i) / pitchRatio;
            int srcIndex = static_cast<int>(srcPos);
            float frac = srcPos - srcIndex;

            if (srcIndex + 1 < sourceSamples)
            {
                dst[i] = src[srcIndex] * (1.0f - frac) + src[srcIndex + 1] * frac;
            }
            else if (srcIndex < sourceSamples)
            {
                dst[i] = src[srcIndex];
            }
            else
            {
                dst[i] = 0.0f;
            }
        }
    }

    return result;
}

juce::AudioBuffer<float> AudioContentLibrary::applyDynamicRangeManipulation(
    const juce::AudioBuffer<float>& source,
    float scale)
{
    if (std::abs(scale - 1.0f) < 0.001f)
        return source;

    juce::AudioBuffer<float> result(source.getNumChannels(), source.getNumSamples());
    result = source;

    // Simple dynamic range manipulation via compression/expansion
    // scale < 1: compression (reduce dynamic range)
    // scale > 1: expansion (increase dynamic range)

    for (int ch = 0; ch < result.getNumChannels(); ++ch)
    {
        float* data = result.getWritePointer(ch);

        for (int i = 0; i < result.getNumSamples(); ++i)
        {
            float sample = data[i];
            float sign = sample >= 0.0f ? 1.0f : -1.0f;
            float absSample = std::abs(sample);

            // Apply power law transformation
            float transformed = std::pow(absSample, 1.0f / scale) * sign;
            data[i] = transformed;
        }
    }

    return result;
}

void AudioContentLibrary::applyGainAndNormalize(
    juce::AudioBuffer<float>& buffer,
    float gainDb,
    bool normalize)
{
    float gainLinear = std::pow(10.0f, gainDb / 20.0f);

    // Apply gain
    buffer.applyGain(gainLinear);

    if (normalize)
    {
        // Find peak
        float peak = 0.0f;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const float* data = buffer.getReadPointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                peak = std::max(peak, std::abs(data[i]));
            }
        }

        // Normalize to -1 dBFS
        if (peak > 1e-10f)
        {
            float targetPeak = std::pow(10.0f, -1.0f / 20.0f); // -1 dBFS
            float normalizeGain = targetPeak / peak;
            buffer.applyGain(normalizeGain);
        }
    }
}

} // namespace morphsnap
