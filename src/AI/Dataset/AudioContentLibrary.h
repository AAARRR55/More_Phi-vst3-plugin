/*
 * More-Phi — AI/Dataset/AudioContentLibrary.h
 * Source audio library management for synthetic audio dataset generation.
 * Handles diverse audio content taxonomy, characteristic extraction,
 * and augmentation pipelines.
 */
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <cstdint>

namespace more_phi {

// Forward declaration for use in SourceAudio::toJson()
class AudioContentLibrary;

/** Genre classification for source audio content */
enum class Genre
{
    Electronic,     ///< EDM, Techno, House, DnB, etc. (25%)
    RockPop,        ///< Rock, Pop, Indie, Alternative (20%)
    JazzAcoustic,   ///< Jazz, Acoustic, Folk (15%)
    HipHopRnB,      ///< Hip-Hop, R&B, Soul, Trap (15%)
    Classical,      ///< Orchestral, Chamber, Solo instruments (5%)
    Stems,          ///< Individual track stems (drums, bass, vocals, etc.)
    TestSignals,    ///< Sine sweeps, impulses, noise, calibration signals
    Count           ///< Number of genre categories
};

/** Convert Genre enum to string (inline implementation for use in structs) */
inline juce::String genreToString(Genre g)
{
    switch (g)
    {
        case Genre::Electronic:    return "Electronic";
        case Genre::RockPop:       return "RockPop";
        case Genre::JazzAcoustic:  return "JazzAcoustic";
        case Genre::HipHopRnB:     return "HipHopRnB";
        case Genre::Classical:     return "Classical";
        case Genre::Stems:         return "Stems";
        case Genre::TestSignals:   return "TestSignals";
        default:                   return "Unknown";
    }
}

/** Audio characteristics extracted from source content */
struct AudioCharacteristics
{
    float dynamicRangeDb = 0.0f;      ///< Difference between peak and RMS (dB)
    float spectralCentroidHz = 0.0f;  ///< Brightness measure (Hz)
    float rhythmicDensity = 0.0f;     ///< Events per second (normalized 0-1)
    float harmonicComplexity = 0.0f;  ///< Chord/timbre complexity (normalized 0-1)
    float lufs = 0.0f;                ///< Integrated loudness (LUFS)
    float peakDb = 0.0f;              ///< True peak level (dBFS)
    float zeroCrossingRate = 0.0f;    ///< Noisiness indicator (0-1)
    float spectralFlatness = 0.0f;    ///< Tonal vs noisy (0-1, 1 = white noise)

    /** Default constructor */
    AudioCharacteristics() = default;

    /** Convert to JSON for serialization */
    nlohmann::json toJson() const
    {
        return {
            {"dynamicRangeDb", dynamicRangeDb},
            {"spectralCentroidHz", spectralCentroidHz},
            {"rhythmicDensity", rhythmicDensity},
            {"harmonicComplexity", harmonicComplexity},
            {"lufs", lufs},
            {"peakDb", peakDb},
            {"zeroCrossingRate", zeroCrossingRate},
            {"spectralFlatness", spectralFlatness}
        };
    }

    /** Create from JSON */
    static AudioCharacteristics fromJson(const nlohmann::json& j)
    {
        AudioCharacteristics c;
        if (j.contains("dynamicRangeDb")) c.dynamicRangeDb = j["dynamicRangeDb"].get<float>();
        if (j.contains("spectralCentroidHz")) c.spectralCentroidHz = j["spectralCentroidHz"].get<float>();
        if (j.contains("rhythmicDensity")) c.rhythmicDensity = j["rhythmicDensity"].get<float>();
        if (j.contains("harmonicComplexity")) c.harmonicComplexity = j["harmonicComplexity"].get<float>();
        if (j.contains("lufs")) c.lufs = j["lufs"].get<float>();
        if (j.contains("peakDb")) c.peakDb = j["peakDb"].get<float>();
        if (j.contains("zeroCrossingRate")) c.zeroCrossingRate = j["zeroCrossingRate"].get<float>();
        if (j.contains("spectralFlatness")) c.spectralFlatness = j["spectralFlatness"].get<float>();
        return c;
    }
};

/** Source audio file metadata */
struct SourceAudio
{
    juce::File file;
    Genre genre = Genre::Electronic;
    juce::String subgenre;             ///< e.g., "Techno", "Indie Rock", "Bebop"
    float targetPercentage = 0.0f;     ///< Target distribution percentage
    AudioCharacteristics characteristics;
    double sampleRate = 48000.0;
    int numChannels = 2;
    int64_t numSamples = 0;
    juce::String description;          ///< Human-readable description
    juce::StringArray tags;            ///< Searchable tags

    /** Default constructor */
    SourceAudio() = default;

    /** Constructor with file */
    explicit SourceAudio(const juce::File& f) : file(f) {}

    /** Get duration in seconds */
    double getDurationSeconds() const
    {
        return sampleRate > 0 ? static_cast<double>(numSamples) / sampleRate : 0.0;
    }

    /** Convert to JSON */
    nlohmann::json toJson() const
    {
        nlohmann::json j = {
            {"filePath", file.getFullPathName().toStdString()},
            {"genre", static_cast<int>(genre)},
            {"genreName", genreToString(genre).toStdString()},
            {"subgenre", subgenre.toStdString()},
            {"targetPercentage", targetPercentage},
            {"characteristics", characteristics.toJson()},
            {"sampleRate", sampleRate},
            {"numChannels", numChannels},
            {"numSamples", numSamples},
            {"durationSeconds", getDurationSeconds()},
            {"description", description.toStdString()}
        };
        if (!tags.isEmpty())
        {
            j["tags"] = nlohmann::json::array();
            for (const auto& tag : tags)
                j["tags"].push_back(tag.toStdString());
        }
        return j;
    }
};

/** Augmentation settings for audio transformation */
struct AugmentationSettings
{
    float timeStretchRatio = 1.0f;     ///< Playback speed ratio (0.8 to 1.2)
    int pitchShiftSemitones = 0;       ///< Pitch shift in semitones (-6 to +6)
    float dynamicRangeScale = 1.0f;    ///< Compress/expand dynamics (0.5 to 2.0)
    float gainDb = 0.0f;               ///< Output gain adjustment (dB)
    bool normalizeOutput = true;       ///< Normalize to -1 dBFS peak
    bool enabled = false;

    /** Default constructor */
    AugmentationSettings() = default;

    /** Create random augmentation within safe bounds */
    static AugmentationSettings createRandom(juce::Random& rng)
    {
        AugmentationSettings settings;
        settings.enabled = true;
        settings.timeStretchRatio = 0.8f + rng.nextFloat() * 0.4f; // 0.8 - 1.2
        settings.pitchShiftSemitones = rng.nextInt(juce::Range<int>(-6, 7)); // -6 to +6
        settings.dynamicRangeScale = 0.5f + rng.nextFloat() * 1.5f; // 0.5 - 2.0
        settings.gainDb = -3.0f + rng.nextFloat() * 6.0f; // -3 to +3 dB
        return settings;
    }

    /** Convert to JSON */
    nlohmann::json toJson() const
    {
        return {
            {"timeStretchRatio", timeStretchRatio},
            {"pitchShiftSemitones", pitchShiftSemitones},
            {"dynamicRangeScale", dynamicRangeScale},
            {"gainDb", gainDb},
            {"normalizeOutput", normalizeOutput},
            {"enabled", enabled}
        };
    }

    /** Create from JSON */
    static AugmentationSettings fromJson(const nlohmann::json& j)
    {
        AugmentationSettings s;
        if (j.contains("timeStretchRatio")) s.timeStretchRatio = j["timeStretchRatio"].get<float>();
        if (j.contains("pitchShiftSemitones")) s.pitchShiftSemitones = j["pitchShiftSemitones"].get<int>();
        if (j.contains("dynamicRangeScale")) s.dynamicRangeScale = j["dynamicRangeScale"].get<float>();
        if (j.contains("gainDb")) s.gainDb = j["gainDb"].get<float>();
        if (j.contains("normalizeOutput")) s.normalizeOutput = j["normalizeOutput"].get<bool>();
        if (j.contains("enabled")) s.enabled = j["enabled"].get<bool>();
        return s;
    }
};

/**
 * Manages a library of source audio content for dataset generation.
 * Supports genre classification, characteristic extraction, and augmentation.
 */
class AudioContentLibrary
{
public:
    AudioContentLibrary();
    ~AudioContentLibrary() = default;

    // ── Library Management ─────────────────────────────────────────────────────

    /** Scan a directory for audio files and add them to the library */
    bool scanDirectory(const juce::File& directory);

    /** Add a single file to the library with specified genre */
    bool addFile(const juce::File& file, Genre genre);

    /** Add a file with full metadata */
    bool addFile(const SourceAudio& source);

    /** Remove a file from the library */
    bool removeFile(const juce::File& file);

    /** Clear all files from the library */
    void clear();

    /** Get total number of files in library */
    int getFileCount() const { return sources_.size(); }

    /** Check if library is empty */
    bool isEmpty() const { return sources_.isEmpty(); }

    // ── Content Retrieval ───────────────────────────────────────────────────────

    /** Get all files of a specific genre */
    juce::Array<SourceAudio> getByGenre(Genre genre) const;

    /** Get all files in the library */
    juce::Array<SourceAudio> getAll() const { return sources_; }

    /** Get a random source file of a specific genre */
    SourceAudio getRandomSource(Genre genre);

    /** Get a random source file weighted by genre distribution percentages */
    SourceAudio getRandomSourceByDistribution();

    /** Get files matching characteristics within tolerance */
    juce::Array<SourceAudio> findByCharacteristics(
        const AudioCharacteristics& target,
        float tolerance = 0.2f) const;

    /** Get files by tag */
    juce::Array<SourceAudio> findByTag(const juce::String& tag) const;

    // ── Characteristic Extraction ───────────────────────────────────────────────

    /** Extract characteristics from an audio buffer */
    AudioCharacteristics extractCharacteristics(
        const juce::AudioBuffer<float>& buffer,
        double sampleRate);

    /** Extract characteristics from a file (loads file into memory) */
    AudioCharacteristics extractCharacteristicsFromFile(const juce::File& file);

    /** Analyze and update characteristics for a source entry */
    void analyzeCharacteristics(SourceAudio& source);

    /** Analyze all files in the library */
    void analyzeAllCharacteristics();

    // ── Augmentation ────────────────────────────────────────────────────────────

    /** Apply augmentation to an audio buffer */
    juce::AudioBuffer<float> applyAugmentation(
        const juce::AudioBuffer<float>& source,
        const AugmentationSettings& settings,
        double sampleRate);

    /** Load a file and apply augmentation in one step */
    std::unique_ptr<juce::AudioBuffer<float>> loadAndAugment(
        const juce::File& file,
        const AugmentationSettings& settings);

    // ── Genre Utilities ─────────────────────────────────────────────────────────

    /** Convert genre enum to string */
    static juce::String genreToString(Genre g);

    /** Convert string to genre enum */
    static Genre stringToGenre(const juce::String& s);

    /** Get target distribution percentage for a genre */
    static float getGenreTargetPercentage(Genre g);

    /** Get all genre names as a string array */
    static juce::StringArray getAllGenreNames();

    // ── File I/O ────────────────────────────────────────────────────────────────

    /** Load a source file into a buffer */
    std::unique_ptr<juce::AudioBuffer<float>> loadFile(const juce::File& file);

    /** Save a buffer to a file */
    bool saveFile(const juce::AudioBuffer<float>& buffer,
                  const juce::File& file,
                  double sampleRate,
                  int bitDepth = 32);

    // ── Serialization ───────────────────────────────────────────────────────────

    /** Export library metadata to JSON */
    nlohmann::json toJson() const;

    /** Import library metadata from JSON (files must exist on disk) */
    bool fromJson(const nlohmann::json& j);

    /** Save library metadata to file */
    bool saveMetadata(const juce::File& file) const;

    /** Load library metadata from file */
    bool loadMetadata(const juce::File& file);

private:
    // ── Internal Analysis Helpers ───────────────────────────────────────────────

    /** Compute spectral centroid from FFT magnitudes */
    float computeSpectralCentroid(const float* magnitudes, int size, double sampleRate);

    /** Compute rhythmic density using onset detection */
    float computeRhythmicDensity(const juce::AudioBuffer<float>& buffer, double sampleRate);

    /** Compute harmonic complexity using chroma analysis */
    float computeHarmonicComplexity(const juce::AudioBuffer<float>& buffer, double sampleRate);

    /** Compute spectral flatness (Wiener entropy) */
    float computeSpectralFlatness(const float* magnitudes, int size);

    /** Compute LUFS loudness */
    float computeLoudnessLUFS(const juce::AudioBuffer<float>& buffer, double sampleRate);

    /** Compute true peak level */
    float computePeakDb(const juce::AudioBuffer<float>& buffer);

    /** Compute zero crossing rate */
    float computeZeroCrossingRate(const juce::AudioBuffer<float>& buffer);

    // ── Augmentation Helpers ─────────────────────────────────────────────────────

    /** Apply time stretch using phase vocoder */
    juce::AudioBuffer<float> applyTimeStretch(
        const juce::AudioBuffer<float>& source,
        float ratio,
        double sampleRate);

    /** Apply pitch shift using resampling */
    juce::AudioBuffer<float> applyPitchShift(
        const juce::AudioBuffer<float>& source,
        int semitones,
        double sampleRate);

    /** Apply dynamic range compression/expansion */
    juce::AudioBuffer<float> applyDynamicRangeManipulation(
        const juce::AudioBuffer<float>& source,
        float scale);

    /** Apply gain and optional normalization */
    void applyGainAndNormalize(
        juce::AudioBuffer<float>& buffer,
        float gainDb,
        bool normalize);

    // ── Members ─────────────────────────────────────────────────────────────────

    juce::AudioFormatManager formatManager_;
    juce::Array<SourceAudio> sources_;
    juce::Random random_;

    // FFT configuration for analysis
    static constexpr int fftSize = 2048;
    static constexpr int hopSize = 512;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioContentLibrary)
};

} // namespace more_phi
