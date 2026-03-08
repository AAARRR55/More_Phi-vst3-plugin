/*
 * MorphSnap — AI/Dataset/OfflineBatchRenderer.h
 * Parallel batch renderer for offline parameter variation rendering.
 * Uses ThreadPool and AudioBufferPool for efficient concurrent processing.
 */
#pragma once

#include "AI/Dataset/EnhancedRenderPipeline.h"
#include "Core/ThreadPool.h"
#include "Core/AudioBufferPool.h"
#include "Core/SIMDAudio.h"
#include "Core/PerformanceProfiler.h"
#include "Host/IPluginHostManager.h"
#include "Host/PluginHostManager.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>
#include <functional>
#include <atomic>
#include <thread>
#include <future>
#include <memory>

namespace morphsnap {

/** Configuration for offline batch rendering operations */
struct OfflineBatchConfig
{
    // Input/Output
    juce::File inputFile;                                           ///< Source audio file to process
    juce::File outputDirectory;                                    ///< Output directory for rendered files
    juce::File pluginFile;                                         ///< VST3 plugin to use for processing

    // Batch settings
    int totalVariations = 100;                                     ///< Total number of parameter variations to render
    int parallelWorkers = std::thread::hardware_concurrency();     ///< Number of parallel worker threads
    bool enableSIMD = true;                                        ///< Enable SIMD optimizations
    bool useMemoryPool = true;                                     ///< Use memory pool for audio buffers

    // Render settings
    RenderConfig renderConfig;                                     ///< Enhanced render pipeline configuration

    // Progress reporting
    std::function<void(int, int)> progressCallback;                ///< Progress callback (completed, total)

    /** Validate configuration parameters */
    bool isValid() const
    {
        return inputFile.existsAsFile() &&
               outputDirectory.isDirectory() &&
               totalVariations > 0 &&
               parallelWorkers > 0;
    }

    /** Check if plugin file is specified and exists */
    bool hasValidPlugin() const
    {
        return pluginFile.existsAsFile() && pluginFile.getFileExtension() == ".vst3";
    }
};

/** Progress information for batch rendering operations */
struct OfflineBatchProgress
{
    int completed = 0;                  ///< Number of completed variations
    int total = 0;                      ///< Total number of variations
    float percentage = 0.0f;           ///< Completion percentage
    juce::String currentStatus;        ///< Current operation status
    double elapsedMs = 0.0;            ///< Elapsed time in milliseconds
    double estimatedRemainingMs = 0.0; ///< Estimated remaining time in milliseconds
    int successfulRenders = 0;         ///< Number of successful renders
    int failedRenders = 0;             ///< Number of failed renders

    /** Update percentage based on completed/total */
    void updatePercentage()
    {
        percentage = total > 0 ? (float(completed) / float(total)) * 100.0f : 0.0f;
    }
};

/**
 * Parallel batch renderer for offline parameter variation rendering.
 *
 * Features:
 * - Concurrent processing using ThreadPool
 * - Memory-efficient buffer pooling with AudioBufferPool
 * - SIMD-optimized audio operations
 * - Progress tracking and error handling
 * - Configurable worker thread count
 *
 * Usage:
 * 1. Create OfflineBatchConfig with input file, output directory, and settings
 * 2. Call setConfig() to initialize thread pool and buffer pool
 * 3. Call startRender() to begin parallel processing
 * 4. Monitor progress via callbacks or getProgress()
 * 5. Call stopRender() to cancel if needed
 */
class OfflineBatchRenderer
{
public:
    OfflineBatchRenderer();
    ~OfflineBatchRenderer();

    // Non-copyable, movable
    OfflineBatchRenderer(const OfflineBatchRenderer&) = delete;
    OfflineBatchRenderer& operator=(const OfflineBatchRenderer&) = delete;
    OfflineBatchRenderer(OfflineBatchRenderer&&) = default;
    OfflineBatchRenderer& operator=(OfflineBatchRenderer&&) = default;

    /**
     * Set configuration and initialize resources.
     *
     * @param config Batch rendering configuration
     * @return true if configuration is valid and resources initialized successfully
     */
    bool setConfig(const OfflineBatchConfig& config);

    /**
     * Start rendering process.
     *
     * @param hostManager Plugin host manager for parameter processing
     * @return true if rendering started successfully
     */
    bool startRender(IPluginHostManager* hostManager = nullptr);

    /**
     * Stop rendering process.
     * Waits for current tasks to complete before returning.
     */
    void stopRender();

    /**
     * Check if rendering is currently active.
     *
     * @return true if rendering is in progress
     */
    bool isRendering() const { return isRendering_.load(); }

    /**
     * Get current progress information.
     *
     * @return Current progress structure
     */
    OfflineBatchProgress getProgress() const;

    /**
     * Get number of configured parallel workers.
     *
     * @return Number of parallel workers
     */
    int getParallelWorkerCount() const { return config_.parallelWorkers; }

    /**
     * Get estimated completion time in milliseconds.
     *
     * @return Estimated time to completion
     */
    double getEstimatedTimeRemaining() const;

    /**
     * Get access to the performance profiler.
     *
     * @return Reference to the performance profiler
     */
    PerformanceProfiler& getProfiler() { return profiler_; }

    /**
     * Get read-only access to the performance profiler.
     *
     * @return Const reference to the performance profiler
     */
    const PerformanceProfiler& getProfiler() const { return profiler_; }

    // Event callbacks
    std::function<void(const OfflineBatchProgress&)> onProgressUpdate;
    std::function<void(int, const RenderResult&)> onVariationComplete;
    std::function<void(bool, const juce::String&)> onRenderComplete;

private:
    /** Individual render task for a parameter variation */
    struct RenderTask
    {
        int variationIndex;                     ///< Index of this variation
        std::vector<float> parameters;          ///< Parameter values for this variation
        juce::File outputFile;                 ///< Output file for this variation
        nlohmann::json metadata;               ///< Metadata associated with this variation

        RenderTask(int index, std::vector<float> params, juce::File file, nlohmann::json meta)
            : variationIndex(index), parameters(std::move(params)), outputFile(std::move(file)), metadata(std::move(meta)) {}
    };

    /**
     * Render multiple variations in parallel using thread pool.
     *
     * @param tasks Vector of render tasks to process
     */
    void renderVariationsParallel(const std::vector<RenderTask>& tasks);

    /**
     * Render a single variation.
     *
     * @param task Render task to process
     * @param hostManager Plugin host manager for parameter processing
     * @return Render result with success status and metadata
     */
    RenderResult renderSingleVariation(const RenderTask& task, IPluginHostManager* hostManager);

    /**
     * Generate parameter variations for batch processing.
     *
     * @return Vector of parameter variation vectors
     */
    std::vector<std::vector<float>> generateParameterVariations();

    /**
     * Generate output filename for a variation index.
     *
     * @param variationIndex Index of the variation
     * @return Output file path
     */
    juce::File generateOutputFile(int variationIndex);

    /**
     * Update progress information thread-safely.
     *
     * @param completed Number of completed variations
     * @param successful Number of successful renders
     * @param failed Number of failed renders
     */
    void updateProgress(int completed, int successful, int failed);

    /**
     * Load source audio file.
     *
     * @return Audio buffer containing source audio, or empty buffer on failure
     */
    juce::AudioBuffer<float> loadSourceAudio();

    // Configuration and state
    OfflineBatchConfig config_;
    OfflineBatchProgress progress_;
    std::atomic<bool> isRendering_{false};
    std::atomic<bool> shouldStop_{false};

    // Thread pool and memory management
    std::unique_ptr<ThreadPool> threadPool_;
    std::unique_ptr<AudioBufferPool> bufferPool_;

    // Plugin hosting
    std::unique_ptr<PluginHostManager> pluginHost_;
    bool pluginLoaded_ = false;

    // Rendering pipeline
    EnhancedRenderPipeline renderPipeline_;

    // Performance profiling
    PerformanceProfiler profiler_;

    // Timing
    juce::int64 startTime_ = 0;

    // Thread safety
    mutable juce::CriticalSection progressLock_;
    std::vector<std::future<RenderResult>> activeTasks_;

    // Audio format support
    juce::AudioFormatManager formatManager_;
};

} // namespace morphsnap