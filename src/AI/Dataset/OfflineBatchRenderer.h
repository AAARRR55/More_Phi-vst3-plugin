/*
 * More-Phi — AI/Dataset/OfflineBatchRenderer.h
 * Sequential offline renderer for hosted-plugin parameter variation batches.
 *
 * IMPORTANT: Uses ParameterSafetyConfig to avoid the "Parameter Trap":
 *   - Dangerous binary parameters (Mute, Solo, Bypass) are locked to safe values
 *   - Only safe DSP parameters (Frequency, Gain, Q, etc.) are randomized
 *   - Prevents silent outputs and duplicate files
 */
#pragma once

#include "AI/Dataset/EnhancedRenderPipeline.h"
#include "AI/Dataset/ParameterSafetyConfig.h"
#include "Core/AudioBufferPool.h"
#include "Core/PerformanceProfiler.h"
#include "Core/ThreadPool.h"
#include "Host/IPluginHostManager.h"
#include "Host/PluginHostManager.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace more_phi {

struct OfflineBatchConfig
{
    juce::File inputFile;
    juce::File outputDirectory;
    juce::File pluginFile;
    juce::File configDirectory;

    int totalVariations = 100;
    int parallelWorkers = 1;
    bool enableSIMD = true;
    bool useMemoryPool = true;
    int maxInputFileSizeMB = 500;  // Prevent OOM on very large audio files

    RenderConfig renderConfig;

    std::function<void(int, int)> progressCallback;

    bool isValid() const
    {
        if (!inputFile.existsAsFile())
            return false;

        if (outputDirectory == juce::File())
            return false;

        if (outputDirectory.exists() && !outputDirectory.isDirectory())
            return false;

        return totalVariations > 0 && parallelWorkers > 0;
    }

    bool hasValidPlugin() const
    {
        return (pluginFile.existsAsFile() || pluginFile.isDirectory())
            && pluginFile.hasFileExtension(".vst3");
    }
};

struct OfflineBatchProgress
{
    int completed = 0;
    int total = 0;
    float percentage = 0.0f;
    juce::String currentStatus;
    double elapsedMs = 0.0;
    double estimatedRemainingMs = 0.0;
    int successfulRenders = 0;
    int failedRenders = 0;

    void updatePercentage()
    {
        percentage = total > 0 ? (static_cast<float>(completed) / static_cast<float>(total)) * 100.0f : 0.0f;
    }
};

class OfflineBatchRenderer
{
public:
    OfflineBatchRenderer();
    ~OfflineBatchRenderer();

    OfflineBatchRenderer(const OfflineBatchRenderer&) = delete;
    OfflineBatchRenderer& operator=(const OfflineBatchRenderer&) = delete;
    OfflineBatchRenderer(OfflineBatchRenderer&&) = delete;
    OfflineBatchRenderer& operator=(OfflineBatchRenderer&&) = delete;

    bool setConfig(const OfflineBatchConfig& config);
    bool startRender(IPluginHostManager* hostManager = nullptr);
    void stopRender();

    bool isRendering() const { return isRendering_.load(); }
    OfflineBatchProgress getProgress() const;
    int getParallelWorkerCount() const { return config_.parallelWorkers; }
    double getEstimatedTimeRemaining() const;

    PerformanceProfiler& getProfiler() { return profiler_; }
    const PerformanceProfiler& getProfiler() const { return profiler_; }

    /** Get the loaded plugin instance (valid only after setConfig succeeds with a plugin). */
    const juce::AudioPluginInstance* getLoadedPlugin() const
    {
        return pluginLoaded_ && pluginHost_ ? pluginHost_->getPlugin() : nullptr;
    }

    /** Set a custom parameter safety configuration */
    void setSafetyConfig(const ParameterSafetyConfig& config) { safetyConfig_ = config; }

    /** Get the current safety configuration */
    ParameterSafetyConfig& getSafetyConfig() { return safetyConfig_; }
    const ParameterSafetyConfig& getSafetyConfig() const { return safetyConfig_; }

    /** Enable/disable safe parameter randomization (default: enabled) */
    void setSafeRandomizationEnabled(bool enabled) { safeRandomizationEnabled_ = enabled; }
    bool isSafeRandomizationEnabled() const { return safeRandomizationEnabled_; }

    std::function<void(const OfflineBatchProgress&)> onProgressUpdate;
    std::function<void(int, const RenderResult&)> onVariationComplete;
    std::function<void(bool, const juce::String&)> onRenderComplete;

private:
    struct RenderTask
    {
        int variationIndex = 0;
        std::vector<float> parameters;
        juce::File outputFile;
    };

    void renderLoop(IPluginHostManager* hostManager);
    RenderResult renderSingleVariation(const RenderTask& task,
                                       IPluginHostManager* hostManager,
                                       const juce::AudioBuffer<float>& sourceAudio,
                                       juce::AudioBuffer<float>& wetBuffer,
                                       juce::AudioBuffer<float>& workBuf,
                                       juce::AudioBuffer<float>& settleBuf);

    std::vector<std::vector<float>> generateParameterVariations(const juce::AudioPluginInstance* plugin) const;
    juce::File generateOutputFile(int variationIndex) const;
    void updateProgress(int completed, int successful, int failed, const juce::String& status);
    juce::AudioBuffer<float> loadSourceAudio();

    OfflineBatchConfig config_;
    OfflineBatchProgress progress_;
    std::atomic<bool> isRendering_{false};
    std::atomic<bool> shouldStop_{false};

    std::unique_ptr<PluginHostManager> pluginHost_;
    bool pluginLoaded_ = false;

    // When --config-dir is used, stores the base filenames (no extension) of each
    // loaded JSON config so that output WAVs are named to match their source configs.
    mutable std::vector<juce::String> configFileBaseNames_;

    EnhancedRenderPipeline renderPipeline_;
    PerformanceProfiler profiler_;
    ParameterSafetyConfig safetyConfig_;
    bool safeRandomizationEnabled_ = true;

    juce::int64 startTime_ = 0;
    mutable juce::CriticalSection progressLock_;
    std::thread renderThread_;
    std::unique_ptr<ThreadPool> threadPool_;
    juce::AudioFormatManager formatManager_;
};

} // namespace more_phi
