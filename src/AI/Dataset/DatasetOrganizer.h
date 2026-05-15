/*
 * More-Phi — AI/Dataset/DatasetOrganizer.h
 * Manages dataset directory structure, stratified splitting, integrity verification,
 * and deduplication for synthetic audio datasets used in ML training.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>
#include <map>
#include <vector>
#include <cstdint>

namespace more_phi {

/** Configuration for train/validation/test splitting. */
struct SplitConfig
{
    float trainRatio = 0.70f;
    float valRatio = 0.15f;
    float testRatio = 0.15f;
    bool stratifyByGenre = true;
    bool stratifyByIntensity = true;
    unsigned int randomSeed = 42;
};

/** Statistics about the organized dataset. */
struct DatasetStats
{
    int totalSamples = 0;
    int trainSamples = 0;
    int valSamples = 0;
    int testSamples = 0;
    std::map<juce::String, int> samplesPerGenre;
    std::map<juce::String, int> samplesPerSplit;
    int64_t totalSizeBytes = 0;
    int corruptedFiles = 0;
    int duplicateFiles = 0;
};

/** Report from dataset integrity verification. */
struct IntegrityReport
{
    bool passed = false;
    juce::StringArray missingFiles;
    juce::StringArray corruptedFiles;
    juce::StringArray invalidMetadata;
    juce::StringArray orphanedFiles;
    juce::String summary;
};

/**
 * Organizes synthetic audio datasets for ML training.
 *
 * Directory structure:
 *   root/
 *     audio/
 *       train/{genre}/
 *       val/{genre}/
 *       test/{genre}/
 *     metadata/
 *       manifests.json
 *     features/
 *       spectral/
 *       temporal/
 *       perceptual/
 *     targets/
 *       regression/
 *       classification/
 */
class DatasetOrganizer
{
public:
    explicit DatasetOrganizer(const juce::File& rootDirectory);
    ~DatasetOrganizer() = default;

    // ── Directory Structure Management ────────────────────────────────────────

    /** Creates the full dataset directory structure. Returns false on failure. */
    bool initializeStructure();

    /** Checks if the directory structure already exists. */
    bool structureExists() const;

    /** Gets the audio directory for a specific split and optional genre. */
    juce::File getAudioDirectory(const juce::String& split, const juce::String& genre = "");

    /** Gets the metadata directory. */
    juce::File getMetadataDirectory();

    /** Gets the features directory, optionally for a specific feature type. */
    juce::File getFeaturesDirectory(const juce::String& featureType = "");

    /** Gets the targets directory, optionally for a specific target type. */
    juce::File getTargetsDirectory(const juce::String& targetType = "");

    // ── Sample Management ─────────────────────────────────────────────────────

    /**
     * Adds a sample to the dataset.
     * @param sampleId Unique identifier for the sample
     * @param audioFile Source audio file to copy
     * @param metadata JSON metadata for the sample
     * @param split Target split ("train", "val", "test"), or empty for auto-assignment
     * @return true on success
     */
    bool addSample(const juce::String& sampleId,
                   const juce::File& audioFile,
                   const nlohmann::json& metadata,
                   const juce::String& split = "");

    /** Moves a sample to a different split. */
    bool moveSample(const juce::String& sampleId,
                    const juce::String& targetSplit);

    /** Removes a sample from the dataset. */
    bool removeSample(const juce::String& sampleId);

    // ── Splitting ──────────────────────────────────────────────────────────────

    /** Performs stratified train/val/test split on all unassigned samples. */
    void performSplit(const SplitConfig& config);

    /** Manually assigns a sample to a specific split. */
    void assignSplit(const juce::String& sampleId, const juce::String& split);

    /** Gets the current split assignment for a sample. Returns empty if unassigned. */
    juce::String getSplitForSample(const juce::String& sampleId);

    // ── Manifest Management ────────────────────────────────────────────────────

    /** Updates all manifest files from the current directory state. */
    bool updateManifests();

    /** Loads the manifest for a specific split. */
    nlohmann::json loadManifest(const juce::String& split);

    /** Saves a manifest for a specific split. */
    bool saveManifest(const juce::String& split, const nlohmann::json& manifest);

    // ── Integrity Verification ─────────────────────────────────────────────────

    /** Verifies dataset integrity and returns a detailed report. */
    IntegrityReport verifyIntegrity();

    /** Attempts to repair issues found in the integrity report. */
    bool repairIntegrity(const IntegrityReport& report);

    // ── Deduplication ──────────────────────────────────────────────────────────

    /** Finds duplicate samples based on file hash. Returns sample IDs. */
    std::vector<juce::String> findDuplicates();

    /** Removes duplicate samples, keeping the first occurrence. */
    bool removeDuplicates();

    // ── Statistics ─────────────────────────────────────────────────────────────

    /** Computes and returns statistics about the dataset. */
    DatasetStats computeStats();

    // ── Incremental Extension ──────────────────────────────────────────────────

    /**
     * Extends the dataset from a source directory.
     * @param sourceDir Directory containing new samples
     * @param config Split configuration for new samples
     * @param checkDuplicates Whether to check for duplicates before adding
     * @return true on success
     */
    bool extendFromDirectory(const juce::File& sourceDir,
                             const SplitConfig& config,
                             bool checkDuplicates = true);

    // ── Export for ML Frameworks ───────────────────────────────────────────────

    /** Exports a unified dataset index file for ML frameworks. */
    bool exportDatasetIndex(const juce::File& outputFile);

    /** Exports separate train/val/test split files. */
    bool exportTrainValTestSplit(const juce::File& outputDir);

    // ── Utility ────────────────────────────────────────────────────────────────

    /** Gets the root directory of the dataset. */
    juce::File getRootDirectory() const { return rootDirectory_; }

    /** Generates a unique sample ID. */
    juce::String generateSampleId();

private:
    juce::File rootDirectory_;
    std::map<juce::String, nlohmann::json> sampleCache_;
    juce::Random random_;

    // ── Helper Methods ─────────────────────────────────────────────────────────

    bool createDirectoryIfNeeded(const juce::File& dir);
    juce::String computeFileHash(const juce::File& file);
    bool copyAudioFile(const juce::File& source, const juce::File& dest);
    std::vector<juce::String> listSamplesInSplit(const juce::String& split);
    std::vector<juce::String> getAllGenres();
    nlohmann::json loadGlobalManifest();
    bool saveGlobalManifest(const nlohmann::json& manifest);
    juce::File getSampleFile(const juce::String& sampleId);
    juce::File getSampleMetadataFile(const juce::String& sampleId);
    bool validateMetadata(const nlohmann::json& metadata);
    juce::String getGenreFromMetadata(const nlohmann::json& metadata);
    float getIntensityFromMetadata(const nlohmann::json& metadata);
    void logError(const juce::String& message);
};

} // namespace more_phi
