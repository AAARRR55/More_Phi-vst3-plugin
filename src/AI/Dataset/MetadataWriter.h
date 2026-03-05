/*
 * MorphSnap — AI/Dataset/MetadataWriter.h
 * Comprehensive metadata management for synthetic audio dataset generation.
 * Supports JSON schema validation, Parquet export, and ML feature extraction.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <optional>
#include <cstdint>

namespace morphsnap {

/** Source audio provenance information */
struct SourceProvenance
{
    juce::String filePath;
    juce::String genre;
    juce::String contentType;       // e.g., "drums", "vocals", "synth", "mixed"
    float originalLufs = 0.0f;
    float dynamicRangeDb = 0.0f;
    double sampleRate = 48000.0;
    int numChannels = 2;
    int64_t numSamples = 0;
    juce::String fileHash;          // MD5 or SHA256
};

/** Single parameter value with full representation */
struct ParameterValue
{
    juce::String name;
    int index = 0;
    float normalizedValue = 0.0f;
    float rawValue = 0.0f;
    juce::String textValue;
    juce::String category;          // "continuous", "discrete", "binary", "frequency", "decibel"
};

/** Plugin instance details */
struct PluginDetails
{
    juce::String pluginId;
    juce::String pluginName;
    juce::String vendor;
    juce::String version;
    juce::String format;            // "VST3", "AU"
    std::vector<ParameterValue> parameters;
};

/** Processing chain configuration */
struct ProcessingChainDetails
{
    juce::String chainType;         // e.g., "mastering", "mixing", "creative"
    std::vector<PluginDetails> plugins;
    double sampleRate = 48000.0;
    int blockSize = 512;
};

/** Output audio characteristics after processing */
struct OutputCharacteristics
{
    float lufs = 0.0f;
    float truePeakDb = 0.0f;
    float dynamicRangeDb = 0.0f;
    float spectralCentroidHz = 0.0f;
    int64_t numSamples = 0;
    double durationSeconds = 0.0;
};

/** ML training targets */
struct MLTargets
{
    std::vector<float> parameterRegression;  // Normalized parameter values for regression
    juce::String styleClassification;         // Genre/style label for classification
    float processingIntensity = 0.0f;         // 0-1 measure of processing amount
    std::vector<float> featureVector;         // Concatenated features for ML input
};

/** Complete dataset metadata entry */
struct DatasetMetadata
{
    juce::String sampleId;
    int64_t timestamp = 0;

    SourceProvenance source;
    ProcessingChainDetails chain;
    OutputCharacteristics output;

    // Features from FeatureExtractor
    nlohmann::json spectralFeatures;
    nlohmann::json temporalFeatures;
    nlohmann::json perceptualFeatures;

    MLTargets targets;

    // Dataset management
    juce::String split;              // "train", "val", "test"
    juce::StringArray tags;

    /** Default constructor */
    DatasetMetadata() = default;

    /** Generate a unique sample ID based on timestamp and hash */
    void generateSampleId();
};

/**
 * MetadataWriter handles comprehensive JSON metadata for synthetic audio datasets.
 * Supports schema validation, Parquet export, and integration with ML pipelines.
 */
class MetadataWriter
{
public:
    MetadataWriter();
    ~MetadataWriter() = default;

    // ── Write Operations ────────────────────────────────────────────────────────

    /** Write single metadata entry to JSON file */
    bool writeMetadata(const juce::File& outputFile, const DatasetMetadata& metadata);

    /** Write batch metadata as a manifest file (array of metadata entries) */
    bool writeManifest(const juce::File& outputFile,
                      const std::vector<DatasetMetadata>& metadataList);

    // ── Read Operations ─────────────────────────────────────────────────────────

    /** Read single metadata entry from JSON file */
    std::optional<DatasetMetadata> readMetadata(const juce::File& inputFile);

    /** Read manifest file (array of metadata entries) */
    std::vector<DatasetMetadata> readManifest(const juce::File& inputFile);

    // ── Validation ──────────────────────────────────────────────────────────────

    /** Validate metadata struct for required fields and value ranges */
    bool validateMetadata(const DatasetMetadata& metadata, juce::String& outError);

    /** Validate JSON against the schema */
    bool validateAgainstSchema(const nlohmann::json& json, juce::String& outError);

    // ── Schema Management ───────────────────────────────────────────────────────

    /** Get the JSON schema for metadata validation */
    nlohmann::json getSchema() const;

    /** Export schema to a file for external validation tools */
    juce::File exportSchema(const juce::File& outputFile);

    // ── Parquet Export ──────────────────────────────────────────────────────────

    /**
     * Export metadata to CSV-compatible format for Parquet conversion.
     * The output can be converted to Parquet using pandas/pyarrow:
     *   pd.read_csv(file).to_parquet(file.with_suffix('.parquet'))
     */
    bool exportToParquet(const juce::File& outputFile,
                        const std::vector<DatasetMetadata>& metadataList);

    /**
     * Export flattened feature table for ML training.
     * Each row is a sample, columns are features + targets.
     */
    bool exportFeatureTable(const juce::File& outputFile,
                           const std::vector<DatasetMetadata>& metadataList);

    // ── JSON Conversion ─────────────────────────────────────────────────────────

    /** Convert metadata struct to JSON */
    nlohmann::json metadataToJson(const DatasetMetadata& metadata) const;

    /** Convert JSON to metadata struct */
    DatasetMetadata jsonToMetadata(const nlohmann::json& json) const;

    // ── Utility Functions ───────────────────────────────────────────────────────

    /** Compute SHA256 hash of a file for provenance tracking */
    static juce::String computeFileHash(const juce::File& file);

    /** Compute MD5 hash of a file (faster, less secure) */
    static juce::String computeFileHashMD5(const juce::File& file);

    /** Generate a unique ID for a metadata entry */
    static juce::String generateUniqueId();

    /** Calculate processing intensity from parameter changes */
    static float calculateProcessingIntensity(const std::vector<ParameterValue>& params);

private:
    nlohmann::json schema_;

    /** Initialize the JSON schema for validation */
    void initializeSchema();

    /** Convert SourceProvenance to JSON */
    nlohmann::json sourceProvenanceToJson(const SourceProvenance& source) const;

    /** Convert JSON to SourceProvenance */
    SourceProvenance jsonToSourceProvenance(const nlohmann::json& json) const;

    /** Convert PluginDetails to JSON */
    nlohmann::json pluginDetailsToJson(const PluginDetails& plugin) const;

    /** Convert JSON to PluginDetails */
    PluginDetails jsonToPluginDetails(const nlohmann::json& json) const;

    /** Convert ProcessingChainDetails to JSON */
    nlohmann::json processingChainDetailsToJson(const ProcessingChainDetails& chain) const;

    /** Convert JSON to ProcessingChainDetails */
    ProcessingChainDetails jsonToProcessingChainDetails(const nlohmann::json& json) const;

    /** Convert OutputCharacteristics to JSON */
    nlohmann::json outputCharacteristicsToJson(const OutputCharacteristics& output) const;

    /** Convert JSON to OutputCharacteristics */
    OutputCharacteristics jsonToOutputCharacteristics(const nlohmann::json& json) const;

    /** Convert MLTargets to JSON */
    nlohmann::json mlTargetsToJson(const MLTargets& targets) const;

    /** Convert JSON to MLTargets */
    MLTargets jsonToMLTargets(const nlohmann::json& json) const;

    /** Flatten features for Parquet/CSV export */
    nlohmann::json flattenFeatures(const DatasetMetadata& metadata) const;
};

} // namespace morphsnap
