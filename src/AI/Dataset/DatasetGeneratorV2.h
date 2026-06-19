/*
 * More-Phi — AI/Dataset/DatasetGeneratorV2.h
 * Comprehensive synthetic audio dataset generator with ground-truth DSP parameters.
 * Integrates all dataset modules for end-to-end generation pipeline.
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include "ParameterSampler.h"
#include "AudioContentLibrary.h"
#include "PluginChainEngine.h"
#include "EnhancedRenderPipeline.h"
#include "FeatureExtractor.h"
#include "MetadataWriter.h"
#include "ValidationEngine.h"
#include "DatasetOrganizer.h"
#include "../../Host/IPluginHostManager.h"
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>

namespace more_phi {

/**
 * Configuration for the complete dataset generation pipeline.
 */
struct DatasetGeneratorConfig
{
    // Output settings
    juce::File outputDirectory;
    juce::String datasetName = "morephi_dataset";

    // Generation settings
    int totalSamples = 1000;
    unsigned int randomSeed = 42;

    // Audio settings
    double sampleRate = 48000.0;
    int blockSize = 512;
    int numChannels = 2;

    // Render settings
    float fullDuration = 30.0f;
    float transientDuration = 2.0f;
    float steadyStateDuration = 5.0f;
    OutputFormat outputFormat = OutputFormat::WAV32Float;

    // Chain settings
    ChainType chainType = ChainType::Mastering;
    juce::File chainConfigFile;  // Optional custom chain config

    // Source audio settings
    juce::File sourceAudioDirectory;

    // Sampling settings
    SamplingConfig samplingConfig;
    SplitConfig splitConfig;

    // Feature extraction settings
    ExtractionConfig extractionConfig;

    // Validation settings
    bool enableValidation = true;
    float targetVolumeCoverage = 0.75f;
    float targetGridCoverage = 0.80f;
    float maxPerformanceGap = 0.15f;

    // Processing settings
    int numParallelThreads = 4;
    int pluginSettleTimeMs = 50;
    bool dryRun = false;

    // Serialization
    static DatasetGeneratorConfig fromJson(const nlohmann::json& j);
    nlohmann::json toJson() const;
    static DatasetGeneratorConfig fromFile(const juce::File& file);
    bool toFile(const juce::File& file) const;
};

/**
 * Progress information for generation callbacks.
 */
struct GenerationProgress
{
    int samplesCompleted = 0;
    int totalSamples = 0;
    float percentage = 0.0f;
    juce::String currentPhase;
    juce::String currentSample;
    double elapsedMs = 0.0;
    double estimatedRemainingMs = 0.0;
    int samplesPerHour = 0;

    // Error tracking
    int errors = 0;
    juce::String lastError;
};

/**
 * Result of a complete generation run.
 */
struct GenerationResult
{
    bool success = false;
    int samplesGenerated = 0;
    int trainSamples = 0;
    int valSamples = 0;
    int testSamples = 0;

    // Statistics
    DatasetStats stats;
    ValidationReport validation;

    // Performance metrics
    double totalTimeMs = 0.0;
    int samplesPerHour = 0;
    float averageSampleTimeMs = 0.0f;

    // Errors
    juce::StringArray errors;
    juce::StringArray warnings;

    // Output locations
    juce::File datasetDirectory;
    juce::File manifestFile;
    juce::File validationReportFile;
};

/**
 * Main class for synthetic audio dataset generation.
 * Integrates all modules into a complete pipeline.
 */
class DatasetGeneratorV2 : public juce::ChangeBroadcaster
{
public:
    DatasetGeneratorV2();
    ~DatasetGeneratorV2();

    // Configuration
    void setConfig(const DatasetGeneratorConfig& config);
    const DatasetGeneratorConfig& getConfig() const { return config_; }

    /**
     * Optionally attach the processor's hosted plugin.
     * When set, the chain engine uses this plugin as a single-plugin chain
     * (unless a custom chain config is provided).
     */
    void setHostManager(IPluginHostManager* hostManager) { hostManager_ = hostManager; }

    /**
     * Initialize all modules (source audio, plugin chain, etc.).
     * Must be called before processSingleSample(). startGeneration() calls
     * this internally; call explicitly only when driving V2 from V3.
     * @return true if all modules initialized successfully.
     */
    bool initialize();

    /**
     * Process a single sample with the given parameters.
     * Thread-safe: serialises chain-engine access with an internal mutex.
     * Requires initialize() to have been called first.
     * @return Populated DatasetMetadata for this sample.
     */
    DatasetMetadata processSingleSample(int sampleIndex,
                                        const std::vector<float>& parameters);

    // Generation control
    bool startGeneration();
    void stopGeneration();
    bool isGenerating() const { return isGenerating_.load(); }

    // Progress
    GenerationProgress getProgress() const;
    std::function<void(const GenerationProgress&)> onProgress;
    std::function<void(const GenerationResult&)> onComplete;

    // Module access (for advanced usage)
    ParameterSampler& getSampler() { return sampler_; }
    AudioContentLibrary& getAudioLibrary() { return audioLibrary_; }
    PluginChainEngine& getChainEngine() { return chainEngine_; }
    EnhancedRenderPipeline& getRenderPipeline() { return renderPipeline_; }
    FeatureExtractor& getFeatureExtractor() { return featureExtractor_; }
    MetadataWriter& getMetadataWriter() { return metadataWriter_; }
    ValidationEngine& getValidationEngine() { return validationEngine_; }
    DatasetOrganizer& getOrganizer() { return *organizer_; }

    // Checkpoint/Resume support
    bool saveCheckpoint(const juce::File& checkpointFile);
    bool loadCheckpoint(const juce::File& checkpointFile);
    bool hasCheckpoint() const { return hasCheckpoint_; }

    // Utility
    static bool validateConfig(const DatasetGeneratorConfig& config, juce::String& outError);

private:
    // Configuration
    DatasetGeneratorConfig config_;

    // Optional host manager for single-plugin mode
    IPluginHostManager* hostManager_ = nullptr;

    // Modules
    ParameterSampler sampler_;
    AudioContentLibrary audioLibrary_;
    PluginChainEngine chainEngine_;
    EnhancedRenderPipeline renderPipeline_;
    FeatureExtractor featureExtractor_;
    MetadataWriter metadataWriter_;
    ValidationEngine validationEngine_;
    std::unique_ptr<DatasetOrganizer> organizer_;

    // Chain engine mutex — serialises processBlock to support V3 worker threads
    mutable std::mutex chainMutex_;

    // State
    std::atomic<bool> isGenerating_{false};
    std::atomic<bool> shouldStop_{false};
    std::atomic<int> samplesCompleted_{0};
    juce::int64 startTimeMs_ = 0;

    // Checkpoint state
    bool hasCheckpoint_ = false;
    nlohmann::json checkpointData_;

    // Threading
    std::thread* generationThread_ = nullptr;

    // Internal methods
    bool initializeModules();
    bool loadSourceAudio();
    bool loadPluginChain();
    void processSample(int sampleIndex, const std::vector<float>& parameters);
    void finalizeGeneration();

    // Single sample generation
    DatasetMetadata generateSample(
        int sampleIndex,
        const SourceAudio& source,
        const std::vector<float>& parameters
    );

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DatasetGeneratorV2)
};

} // namespace more_phi
