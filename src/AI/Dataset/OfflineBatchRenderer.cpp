/*
 * MorphSnap — AI/Dataset/OfflineBatchRenderer.cpp
 * Implementation of parallel batch renderer for offline parameter variation rendering.
 */

#include "AI/Dataset/OfflineBatchRenderer.h"
#include <random>
#include <algorithm>

namespace morphsnap {

OfflineBatchRenderer::OfflineBatchRenderer()
{
    // Register basic audio formats
    formatManager_.registerBasicFormats();
}

OfflineBatchRenderer::~OfflineBatchRenderer()
{
    stopRender();
}

bool OfflineBatchRenderer::setConfig(const OfflineBatchConfig& config)
{
    // Validate configuration
    if (!config.isValid())
    {
        return false;
    }

    config_ = config;

    // Create output directory if it doesn't exist
    if (!config_.outputDirectory.exists())
    {
        config_.outputDirectory.createDirectory();
    }

    // Load plugin if specified
    if (config_.hasValidPlugin())
    {
        pluginHost_ = std::make_unique<PluginHostManager>();

        // First, prepare the host manager with sample rate/block size
        pluginHost_->prepare(config_.renderConfig.sampleRate, config_.renderConfig.blockSize, config_.renderConfig.numChannels);

        // Create PluginDescription from the plugin file
        // For VST3 plugins, we can create a minimal description
        juce::PluginDescription pluginDesc;
        pluginDesc.fileOrIdentifier = config_.pluginFile.getFullPathName();
        pluginDesc.pluginFormatName = "VST3";
        pluginDesc.name = config_.pluginFile.getFileNameWithoutExtension();

        pluginLoaded_ = pluginHost_->loadPlugin(pluginDesc);

        if (!pluginLoaded_)
        {
            // Log warning but continue - we can still render with gain-only processing
            juce::Logger::writeToLog("Warning: Failed to load plugin, using fallback gain processing");
        }
    }

    // Initialize thread pool if using parallel workers
    if (config_.parallelWorkers > 1)
    {
        threadPool_ = std::make_unique<ThreadPool>(config_.parallelWorkers);
    }

    // Initialize audio buffer pool if enabled
    if (config_.useMemoryPool)
    {
        // Calculate buffer size based on longest segment duration
        int maxSamples = static_cast<int>(config_.renderConfig.sampleRate * config_.renderConfig.fullDuration);

        bufferPool_ = std::make_unique<AudioBufferPool>(
            config_.renderConfig.numChannels,
            maxSamples,
            config_.renderConfig.sampleRate
        );

        // Pre-allocate buffers for parallel workers
        bufferPool_->preallocate(config_.parallelWorkers * 2);
    }

    // Reset progress
    progress_ = OfflineBatchProgress{};
    progress_.total = config_.totalVariations;

    return true;
}

bool OfflineBatchRenderer::startRender(IPluginHostManager* hostManager)
{
    if (isRendering_.load())
    {
        return false; // Already rendering
    }

    // Validate that we have a valid configuration
    if (!config_.isValid())
    {
        if (onRenderComplete)
            onRenderComplete(false, "Invalid configuration");
        return false;
    }

    isRendering_.store(true);
    shouldStop_.store(false);
    startTime_ = juce::Time::getCurrentTime().toMilliseconds();

    // Generate parameter variations
    auto parameterVariations = generateParameterVariations();

    // Create render tasks
    std::vector<RenderTask> tasks;
    tasks.reserve(config_.totalVariations);

    for (int i = 0; i < config_.totalVariations; ++i)
    {
        auto outputFile = generateOutputFile(i);
        nlohmann::json metadata;
        metadata["variation_index"] = i;
        metadata["total_variations"] = config_.totalVariations;
        metadata["timestamp"] = juce::Time::getCurrentTime().toISO8601(true).toStdString();

        tasks.emplace_back(i, parameterVariations[i], outputFile, metadata);
    }

    // Start rendering
    if (config_.parallelWorkers > 1 && threadPool_)
    {
        // Use parallel rendering
        std::thread([this, tasks = std::move(tasks)]() {
            renderVariationsParallel(tasks);
        }).detach();
    }
    else
    {
        // Use sequential rendering
        std::thread([this, tasks = std::move(tasks), hostManager]() {
            for (const auto& task : tasks)
            {
                if (shouldStop_.load())
                    break;

                auto result = renderSingleVariation(task, hostManager);

                {
                    juce::ScopedLock lock(progressLock_);
                    progress_.completed++;
                    if (result.success)
                        progress_.successfulRenders++;
                    else
                        progress_.failedRenders++;

                    progress_.updatePercentage();
                }

                if (onVariationComplete)
                    onVariationComplete(task.variationIndex, result);

                if (onProgressUpdate)
                    onProgressUpdate(getProgress());
            }

            isRendering_.store(false);
            if (onRenderComplete)
                onRenderComplete(!shouldStop_.load(), shouldStop_.load() ? "Cancelled" : "Completed");
        }).detach();
    }

    return true;
}

void OfflineBatchRenderer::stopRender()
{
    shouldStop_.store(true);

    // Wait for all active tasks to complete
    for (auto& future : activeTasks_)
    {
        if (future.valid())
        {
            try
            {
                future.wait();
            }
            catch (...)
            {
                // Ignore task exceptions during shutdown
            }
        }
    }
    activeTasks_.clear();

    isRendering_.store(false);
}

OfflineBatchProgress OfflineBatchRenderer::getProgress() const
{
    juce::ScopedLock lock(progressLock_);
    auto progress = progress_;

    // Update timing information
    if (isRendering_.load() && startTime_ > 0)
    {
        progress.elapsedMs = juce::Time::getCurrentTime().toMilliseconds() - startTime_;

        if (progress.completed > 0 && progress.total > progress.completed)
        {
            double avgTimePerItem = progress.elapsedMs / progress.completed;
            progress.estimatedRemainingMs = avgTimePerItem * (progress.total - progress.completed);
        }
    }

    return progress;
}

double OfflineBatchRenderer::getEstimatedTimeRemaining() const
{
    auto progress = getProgress();
    return progress.estimatedRemainingMs;
}

void OfflineBatchRenderer::renderVariationsParallel(const std::vector<RenderTask>& tasks)
{
    if (!threadPool_)
    {
        return;
    }

    // Submit all tasks to thread pool
    for (const auto& task : tasks)
    {
        if (shouldStop_.load())
            break;

        auto future = threadPool_->enqueue([this, task]() -> RenderResult {
            return renderSingleVariation(task, nullptr); // TODO: Handle hostManager in parallel context
        });

        activeTasks_.push_back(std::move(future));
    }

    // Wait for all tasks to complete and collect results
    for (size_t i = 0; i < activeTasks_.size(); ++i)
    {
        if (shouldStop_.load())
            break;

        try
        {
            auto result = activeTasks_[i].get();

            {
                juce::ScopedLock lock(progressLock_);
                progress_.completed++;
                if (result.success)
                    progress_.successfulRenders++;
                else
                    progress_.failedRenders++;

                progress_.updatePercentage();
            }

            if (onVariationComplete && i < tasks.size())
                onVariationComplete(tasks[i].variationIndex, result);

            if (onProgressUpdate)
                onProgressUpdate(getProgress());
        }
        catch (const std::exception& e)
        {
            // Handle task exception
            {
                juce::ScopedLock lock(progressLock_);
                progress_.completed++;
                progress_.failedRenders++;
                progress_.updatePercentage();
            }

            if (onProgressUpdate)
                onProgressUpdate(getProgress());
        }
    }

    activeTasks_.clear();
    isRendering_.store(false);

    if (onRenderComplete)
        onRenderComplete(!shouldStop_.load(), shouldStop_.load() ? "Cancelled" : "Completed");
}

RenderResult OfflineBatchRenderer::renderSingleVariation(const RenderTask& task, IPluginHostManager* hostManager)
{
    MORPHSNAP_PROFILE(profiler_, "render_variation");

    RenderResult result;
    result.sampleIndex = task.variationIndex;

    // Declare buffer outside try block for cleanup in catch
    AudioBufferPool::AudioBufferPtr pooledBuffer;

    try
    {
        // Load source audio
        juce::AudioBuffer<float> sourceAudio;
        {
            MORPHSNAP_PROFILE(profiler_, "plugin_load");
            sourceAudio = loadSourceAudio();
            if (sourceAudio.getNumSamples() == 0)
            {
                result.errorMessage = "Failed to load source audio";
                return result;
            }
        }

        // Get buffer from pool if available, otherwise create new one
        juce::AudioBuffer<float> workingBuffer;
        {
            MORPHSNAP_PROFILE(profiler_, "parameter_apply");

            if (bufferPool_ && config_.useMemoryPool)
            {
                pooledBuffer = bufferPool_->acquireBuffer();
                if (pooledBuffer && pooledBuffer->getNumChannels() >= sourceAudio.getNumChannels() &&
                    pooledBuffer->getNumSamples() >= sourceAudio.getNumSamples())
                {
                    workingBuffer.setDataToReferTo(pooledBuffer->getArrayOfWritePointers(),
                                                   sourceAudio.getNumChannels(),
                                                   sourceAudio.getNumSamples());
                }
                else
                {
                    workingBuffer.setSize(sourceAudio.getNumChannels(), sourceAudio.getNumSamples());
                }
            }
            else
            {
                workingBuffer.setSize(sourceAudio.getNumChannels(), sourceAudio.getNumSamples());
            }

            // Copy source audio to working buffer
            for (int channel = 0; channel < sourceAudio.getNumChannels(); ++channel)
            {
                if (config_.enableSIMD)
                {
                    // Use SIMD multiplication with 1.0f for copying
                    SIMDAudio::multiplyScalar(
                        sourceAudio.getReadPointer(channel),
                        1.0f,
                        workingBuffer.getWritePointer(channel),
                        sourceAudio.getNumSamples()
                    );
                }
                else
                {
                    workingBuffer.copyFrom(channel, 0, sourceAudio, channel, 0, sourceAudio.getNumSamples());
                }
            }

            // Process through plugin if loaded
            if (pluginLoaded_ && pluginHost_ && pluginHost_->hasPlugin())
            {
                // Apply parameter variation to plugin
                auto* plugin = pluginHost_->getPlugin();
                if (!task.parameters.empty() && plugin)
                {
                    // Set plugin parameters based on variation
                    auto& params = plugin->getParameters();
                    for (size_t i = 0; i < task.parameters.size() && i < static_cast<size_t>(params.size()); ++i)
                    {
                        if (auto* param = params[static_cast<int>(i)])
                        {
                            param->setValue(task.parameters[i]); // Normalized value 0.0-1.0
                        }
                    }
                }

                // Process audio through plugin in blocks
                juce::MidiBuffer midiBuffer; // Empty MIDI buffer for offline processing
                int blockSize = config_.renderConfig.blockSize;
                int totalSamples = workingBuffer.getNumSamples();

                for (int sampleOffset = 0; sampleOffset < totalSamples; sampleOffset += blockSize)
                {
                    int samplesThisBlock = juce::jmin(blockSize, totalSamples - sampleOffset);

                    // Create sub-buffer view for this block
                    juce::AudioBuffer<float> blockBuffer(
                        workingBuffer.getArrayOfWritePointers(),
                        workingBuffer.getNumChannels(),
                        sampleOffset,
                        samplesThisBlock
                    );

                    // Process through plugin (in-place processing)
                    pluginHost_->processBlock(blockBuffer, midiBuffer);
                }
            }
            else
            {
                // Fallback: apply simple gain based on first parameter
                if (!task.parameters.empty())
                {
                    float gain = task.parameters[0];
                    workingBuffer.applyGain(gain);
                }
            }
        }

        // Render using enhanced render pipeline
        {
            MORPHSNAP_PROFILE(profiler_, "audio_process");
            auto renderResult = renderPipeline_.render(task.variationIndex, workingBuffer, config_.renderConfig);

            // Copy render result
            result = renderResult;
            result.outputFile = task.outputFile;
        }

        {
            MORPHSNAP_PROFILE(profiler_, "file_write");
            // Return buffer to pool if we acquired it
            if (pooledBuffer)
            {
                bufferPool_->releaseBuffer(std::move(pooledBuffer));
            }
        }

        return result;
    }
    catch (const std::exception& e)
    {
        // Return buffer to pool if we acquired it
        if (pooledBuffer)
        {
            bufferPool_->releaseBuffer(std::move(pooledBuffer));
        }

        result.errorMessage = juce::String("Exception during render: ") + e.what();
        return result;
    }
}

std::vector<std::vector<float>> OfflineBatchRenderer::generateParameterVariations()
{
    std::vector<std::vector<float>> variations;
    variations.reserve(config_.totalVariations);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    // For demonstration, generate random parameter variations
    // In a real implementation, this would use more sophisticated sampling
    for (int i = 0; i < config_.totalVariations; ++i)
    {
        std::vector<float> params;
        params.reserve(8); // Assume 8 parameters for demo

        for (int p = 0; p < 8; ++p)
        {
            params.push_back(dist(gen));
        }

        variations.push_back(std::move(params));
    }

    return variations;
}

juce::File OfflineBatchRenderer::generateOutputFile(int variationIndex)
{
    auto extension = config_.renderConfig.getFileExtension();
    auto filename = juce::String::formatted("variation_%04d%s", variationIndex, extension);
    return config_.outputDirectory.getChildFile(filename);
}

void OfflineBatchRenderer::updateProgress(int completed, int successful, int failed)
{
    juce::ScopedLock lock(progressLock_);

    progress_.completed = completed;
    progress_.successfulRenders = successful;
    progress_.failedRenders = failed;
    progress_.updatePercentage();

    if (startTime_ > 0)
    {
        progress_.elapsedMs = juce::Time::getCurrentTime().toMilliseconds() - startTime_;

        if (completed > 0 && progress_.total > completed)
        {
            double avgTimePerItem = progress_.elapsedMs / completed;
            progress_.estimatedRemainingMs = avgTimePerItem * (progress_.total - completed);
        }
    }
}

juce::AudioBuffer<float> OfflineBatchRenderer::loadSourceAudio()
{
    juce::AudioBuffer<float> buffer;

    if (!config_.inputFile.existsAsFile())
    {
        return buffer; // Return empty buffer
    }

    auto reader = std::unique_ptr<juce::AudioFormatReader>(formatManager_.createReaderFor(config_.inputFile));
    if (!reader)
    {
        return buffer; // Return empty buffer
    }

    buffer.setSize(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
    reader->read(&buffer, 0, static_cast<int>(reader->lengthInSamples), 0, true, true);

    return buffer;
}

} // namespace morphsnap