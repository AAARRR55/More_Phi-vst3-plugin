/*
 * MorphSnap — AI/Dataset/OfflineBatchRenderer.cpp
 * Sequential hosted-plugin renderer with reusable buffers for offline batches.
 */

#include "AI/Dataset/OfflineBatchRenderer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>

namespace morphsnap {

namespace {

struct MutationTarget
{
    int index = 0;
    float baseValue = 0.0f;
    int numSteps = 0;
    bool isDiscrete = false;
    juce::Array<int> activationIndices;
    juce::String lowerName;
};

bool populatePluginDescription(juce::AudioPluginFormatManager& formatManager,
                               const juce::File& pluginFile,
                               juce::PluginDescription& outDescription)
{
    const auto pluginPath = pluginFile.getFullPathName();

    for (auto* format : formatManager.getFormats())
    {
        if (!format->fileMightContainThisPluginType(pluginPath))
            continue;

        juce::OwnedArray<juce::PluginDescription> descriptions;
        format->findAllTypesForFile(descriptions, pluginPath);

        if (!descriptions.isEmpty())
        {
            outDescription = *descriptions.getFirst();
            return true;
        }
    }

    outDescription.fileOrIdentifier = pluginPath;
    outDescription.pluginFormatName = pluginFile.hasFileExtension(".vst3") ? "VST3" : juce::String();
    outDescription.name = pluginFile.getFileNameWithoutExtension();
    return false;
}

void logProfilingSummary(const PerformanceProfiler& profiler)
{
    const auto stats = profiler.getAllStats();
    for (const auto& [name, stat] : stats)
    {
        DBG("OfflineBatchRenderer profile [" + juce::String(name) + "]: "
            + juce::String(stat.callCount) + " calls, avg "
            + juce::String(stat.averageTimeMs, 2) + " ms, total "
            + juce::String(stat.totalTimeMs, 2) + " ms");
    }
}

bool containsAny(const juce::String& text, std::initializer_list<const char*> needles)
{
    for (const auto* needle : needles)
    {
        if (text.contains(needle))
            return true;
    }

    return false;
}

bool isScopedFilterControl(const juce::String& lowerName)
{
    return lowerName.startsWith("band ") || lowerName.startsWith("filter ");
}

bool isTrapBinaryParameter(const juce::String& lowerName)
{
    return containsAny(lowerName, { "bypass", "mute", "solo", "audition", "listen", "phase", "invert", "polarity" });
}

bool isStructuralToggle(const juce::String& lowerName)
{
    return lowerName.endsWith(" used")
        || lowerName.endsWith(" enabled")
        || lowerName.endsWith(" active");
}

bool isGlobalGainLike(const juce::String& lowerName)
{
    return containsAny(lowerName, { "output gain", "input gain", "master gain", "makeup", "trim", "volume", " level" });
}

bool shouldExcludeFromMutation(const juce::String& lowerName, const juce::AudioProcessorParameter* parameter)
{
    if (parameter == nullptr)
        return true;

    if (parameter->isBoolean() || isTrapBinaryParameter(lowerName) || isStructuralToggle(lowerName))
        return true;

    return containsAny(lowerName, {
        "dry/wet", "dry", "wet", "mix",
        "stereo placement", "speakers",
        "side chain", "external",
        "output", "input", "master",
        "power", "mono", "latency",
        "oversampling", "quality", "lookahead",
        "analyzer", "meter", "routing"
    }) || isGlobalGainLike(lowerName);
}

bool isFabFilterSafeParameter(const juce::String& lowerName)
{
    const bool scoped = isScopedFilterControl(lowerName);

    if ((lowerName.contains("frequency") || lowerName.contains("freq") || lowerName.contains("cutoff"))
        && (scoped || lowerName.contains("cutoff")))
    {
        return true;
    }

    if ((lowerName.endsWith(" q") || lowerName.contains(" q ")
         || lowerName.contains("resonance") || lowerName.contains("reso"))
        && scoped)
    {
        return true;
    }

    if (lowerName.contains("gain") && scoped && !isGlobalGainLike(lowerName))
        return true;

    if ((lowerName.contains("shape")
         || lowerName.contains("filter type")
         || lowerName.endsWith(" type")
         || lowerName.contains("slope"))
        && scoped)
    {
        return true;
    }

    return false;
}

bool isGenericSafeParameter(const juce::String& lowerName)
{
    if (lowerName.contains("frequency") || lowerName.contains("freq") || lowerName.contains("cutoff"))
        return true;

    if (lowerName.endsWith(" q") || lowerName.contains(" q ")
        || lowerName.contains("resonance") || lowerName.contains("reso"))
    {
        return true;
    }

    if (lowerName.contains("gain") && isScopedFilterControl(lowerName) && !isGlobalGainLike(lowerName))
        return true;

    return (lowerName.contains("shape")
            || lowerName.contains("filter type")
            || lowerName.endsWith(" type")
            || lowerName.contains("slope"))
        && isScopedFilterControl(lowerName);
}

bool isFallbackSafeParameter(const juce::String& lowerName)
{
    return !containsAny(lowerName, {
        "threshold", "attack", "release", "ratio", "knee",
        "gate", "compress", "limit", "expand"
    });
}

juce::String extractGroupPrefix(const juce::String& lowerName)
{
    const int lastSpace = lowerName.lastIndexOfChar(' ');
    if (lastSpace <= 0)
        return {};

    return lowerName.substring(0, lastSpace).trim();
}

juce::Array<int> findActivationIndices(const juce::Array<juce::String>& lowerNames, const juce::String& groupPrefix)
{
    juce::Array<int> activationIndices;
    if (groupPrefix.isEmpty())
        return activationIndices;

    const auto usedName = groupPrefix + " used";
    const auto enabledName = groupPrefix + " enabled";
    const auto activeName = groupPrefix + " active";

    for (int index = 0; index < lowerNames.size(); ++index)
    {
        const auto& name = lowerNames.getReference(index);
        if (name == usedName || name == enabledName || name == activeName)
            activationIndices.add(index);
    }

    return activationIndices;
}

float generateContinuousValue(std::mt19937& generator, float baseValue)
{
    std::uniform_real_distribution<float> distribution(0.0f, 1.0f);

    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const float candidate = distribution(generator);
        if (std::abs(candidate - baseValue) >= 0.08f)
            return candidate;
    }

    return juce::jlimit(0.0f, 1.0f, baseValue < 0.5f ? baseValue + 0.25f : baseValue - 0.25f);
}

// Gain-aware generator: constrains to approx +/-15 dB range (0.25 to 0.75 normalized)
// Uses triangular distribution centered on 0.5 (0 dB) for realistic EQ curves.
float generateGainValue(std::mt19937& generator, float baseValue)
{
    std::uniform_real_distribution<float> u(0.0f, 1.0f);

    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const float r = u(generator);
        // Triangular distribution: peak at 0.5, range [0.25, 0.75]
        float candidate;
        if (r < 0.5f)
            candidate = 0.25f + std::sqrt(r * 0.5f) * 0.5f;
        else
            candidate = 0.75f - std::sqrt((1.0f - r) * 0.5f) * 0.5f;

        if (std::abs(candidate - baseValue) >= 0.05f)
            return candidate;
    }

    return juce::jlimit(0.25f, 0.75f, baseValue < 0.5f ? baseValue + 0.1f : baseValue - 0.1f);
}

float generateDiscreteValue(std::mt19937& generator, float baseValue, int numSteps)
{
    const int steps = juce::jmax(2, numSteps);
    const int maxStep = steps - 1;
    const int baseStep = juce::jlimit(0, maxStep, juce::roundToInt(baseValue * static_cast<float>(maxStep)));

    if (steps == 2)
        return baseStep == 0 ? 1.0f : 0.0f;

    std::uniform_int_distribution<int> distribution(0, maxStep - 1);
    int nextStep = distribution(generator);
    if (nextStep >= baseStep)
        ++nextStep;

    return static_cast<float>(nextStep) / static_cast<float>(maxStep);
}

std::vector<MutationTarget> collectMutationTargets(const juce::AudioPluginInstance* plugin)
{
    std::vector<MutationTarget> targets;
    if (plugin == nullptr)
        return targets;

    const auto& parameters = plugin->getParameters();
    juce::Array<juce::String> lowerNames;
    lowerNames.ensureStorageAllocated(parameters.size());

    for (int index = 0; index < parameters.size(); ++index)
    {
        auto* parameter = parameters[index];
        lowerNames.add(parameter != nullptr ? parameter->getName(256).toLowerCase().trim()
                                            : juce::String());
    }

    const juce::String pluginName = plugin->getName().toLowerCase();
    const bool strictFabFilterMode = pluginName.contains("fabfilter")
        || pluginName.contains("pro-q")
        || pluginName.contains("volcano");

    const auto appendTargets = [&](auto&& predicate)
    {
        for (int index = 0; index < parameters.size(); ++index)
        {
            auto* parameter = parameters[index];
            const auto lowerName = lowerNames[index];

            if (shouldExcludeFromMutation(lowerName, parameter) || !predicate(lowerName))
                continue;

            const int numSteps = parameter->getNumSteps();
            const bool isDiscrete = !parameter->isBoolean()
                && (parameter->isDiscrete()
                    || (numSteps > 1 && numSteps < std::numeric_limits<int>::max()));

            MutationTarget target;
            target.index = index;
            target.baseValue = parameter->getValue();
            target.numSteps = numSteps;
            target.isDiscrete = isDiscrete;
            target.activationIndices = findActivationIndices(lowerNames, extractGroupPrefix(lowerName));
            target.lowerName = lowerName;
            targets.push_back(std::move(target));
        }
    };

    if (strictFabFilterMode)
        appendTargets([](const juce::String& lowerName) { return isFabFilterSafeParameter(lowerName); });

    if (targets.empty())
        appendTargets([](const juce::String& lowerName) { return isGenericSafeParameter(lowerName); });

    if (targets.empty())
        appendTargets([](const juce::String& lowerName) { return isFallbackSafeParameter(lowerName); });

    return targets;
}

} // namespace

OfflineBatchRenderer::OfflineBatchRenderer()
{
    formatManager_.registerBasicFormats();
}

OfflineBatchRenderer::~OfflineBatchRenderer()
{
    stopRender();
}

bool OfflineBatchRenderer::setConfig(const OfflineBatchConfig& config)
{
    stopRender();

    if (!config.isValid())
        return false;

    if (config.pluginFile != juce::File() && !config.hasValidPlugin())
        return false;

    config_ = config;
    if (config_.renderConfig.outputDirectory == juce::File())
        config_.renderConfig.outputDirectory = config_.outputDirectory;

    if (!config_.outputDirectory.exists() && !config_.outputDirectory.createDirectory())
        return false;

    if (!config_.renderConfig.outputDirectory.exists()
        && !config_.renderConfig.outputDirectory.createDirectory())
    {
        return false;
    }

    pluginLoaded_ = false;
    pluginHost_.reset();

    if (config_.hasValidPlugin())
    {
        pluginHost_ = std::make_unique<PluginHostManager>();

        juce::PluginDescription description;
        populatePluginDescription(pluginHost_->getFormatManager(), config_.pluginFile, description);

        auto prepareAndLoad = [&]()
        {
            pluginHost_->prepare(config_.renderConfig.sampleRate,
                                 config_.renderConfig.blockSize,
                                 config_.renderConfig.numChannels);

            pluginLoaded_ = pluginHost_->loadPlugin(description);
            return pluginLoaded_;
        };

        if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
        {
            juce::MessageManagerLock mmLock(juce::Thread::getCurrentThread());
            if (!mmLock.lockWasGained())
                return false;

            if (!prepareAndLoad())
                return false;
        }
        else if (!prepareAndLoad())
        {
            return false;
        }
    }

    profiler_.reset();
    progress_ = OfflineBatchProgress{};
    progress_.total = config_.totalVariations;
    return true;
}

bool OfflineBatchRenderer::startRender(IPluginHostManager* hostManager)
{
    if (isRendering_.load())
        return false;

    if (!config_.isValid())
    {
        if (onRenderComplete)
            onRenderComplete(false, "Invalid configuration");
        return false;
    }

    if (renderThread_.joinable())
        renderThread_.join();

    profiler_.reset();
    shouldStop_.store(false);
    isRendering_.store(true);
    startTime_ = juce::Time::getCurrentTime().toMilliseconds();

    renderThread_ = std::thread([this, hostManager]() { renderLoop(hostManager); });
    return true;
}

void OfflineBatchRenderer::stopRender()
{
    shouldStop_.store(true);

    if (renderThread_.joinable() && renderThread_.get_id() != std::this_thread::get_id())
        renderThread_.join();

    isRendering_.store(false);
}

OfflineBatchProgress OfflineBatchRenderer::getProgress() const
{
    const juce::ScopedLock lock(progressLock_);
    auto progress = progress_;

    if (isRendering_.load() && startTime_ > 0)
    {
        progress.elapsedMs = static_cast<double>(juce::Time::getCurrentTime().toMilliseconds() - startTime_);
        if (progress.completed > 0 && progress.total > progress.completed)
        {
            const auto averageTimePerItem = progress.elapsedMs / static_cast<double>(progress.completed);
            progress.estimatedRemainingMs = averageTimePerItem * static_cast<double>(progress.total - progress.completed);
        }
    }

    return progress;
}

double OfflineBatchRenderer::getEstimatedTimeRemaining() const
{
    return getProgress().estimatedRemainingMs;
}

void OfflineBatchRenderer::renderLoop(IPluginHostManager* hostManager)
{
    auto finish = [this](bool success, const juce::String& message)
    {
        logProfilingSummary(profiler_);
        isRendering_.store(false);
        if (onRenderComplete)
            onRenderComplete(success, message);
    };

    juce::AudioBuffer<float> sourceAudio;
    {
        MORPHSNAP_PROFILE(profiler_, "load_source_audio");
        sourceAudio = loadSourceAudio();
    }

    if (sourceAudio.getNumSamples() == 0 || sourceAudio.getNumChannels() == 0)
    {
        finish(false, "Failed to load source audio");
        return;
    }

    IPluginHostManager* activeHost = nullptr;
    if (pluginLoaded_ && pluginHost_ && pluginHost_->hasPlugin())
        activeHost = pluginHost_.get();
    else if (hostManager != nullptr && hostManager->hasPlugin())
        activeHost = hostManager;

    if (activeHost == nullptr)
    {
        std::cerr << "[WARNING] No plugin host available — all renders will be dry passthrough!\n"
                  << "  Check that the --plugin path points to a valid .vst3 file.\n";
    }

    // Generate parameter variations + spare sets for near-silent retry
    const int spareCount = juce::jmax(10, config_.totalVariations / 5);
    std::vector<std::vector<float>> parameterVariations;
    {
        MORPHSNAP_PROFILE(profiler_, "generate_parameter_variations");
        // Temporarily request extra variations, then split into main + spares
        const auto origTotal = config_.totalVariations;
        config_.totalVariations = origTotal + spareCount;
        parameterVariations = generateParameterVariations(activeHost != nullptr ? activeHost->getPlugin() : nullptr);
        config_.totalVariations = origTotal;
    }

    // Split: first N are primary, rest are spares for near-silent retries
    std::vector<std::vector<float>> spareVariations;
    if (static_cast<int>(parameterVariations.size()) > config_.totalVariations)
    {
        spareVariations.assign(
            parameterVariations.begin() + config_.totalVariations,
            parameterVariations.end());
        parameterVariations.resize(static_cast<size_t>(config_.totalVariations));
    }
    int nextSpare = 0;

    if (activeHost != nullptr)
    {
        MORPHSNAP_PROFILE(profiler_, "prepare_plugin");
        activeHost->prepare(config_.renderConfig.sampleRate,
                            config_.renderConfig.blockSize,
                            sourceAudio.getNumChannels());
    }

    const int processChannels = juce::jmax(sourceAudio.getNumChannels(), config_.renderConfig.numChannels);
    const int blockSize = juce::jmax(1, config_.renderConfig.blockSize);

    AudioBufferPool::AudioBufferPtr workBufferOwner;
    AudioBufferPool::AudioBufferPtr settleBufferOwner;
    std::unique_ptr<AudioBufferPool> scratchPool;

    if (config_.useMemoryPool)
    {
        scratchPool = std::make_unique<AudioBufferPool>(processChannels, blockSize, config_.renderConfig.sampleRate);
        scratchPool->preallocate(2);
        workBufferOwner = scratchPool->acquireBuffer();
        settleBufferOwner = scratchPool->acquireBuffer();
    }
    else
    {
        workBufferOwner = std::make_unique<juce::AudioBuffer<float>>(processChannels, blockSize);
        settleBufferOwner = std::make_unique<juce::AudioBuffer<float>>(processChannels, blockSize);
    }

    if (!workBufferOwner || !settleBufferOwner)
    {
        finish(false, "Failed to allocate render buffers");
        return;
    }

    auto& workBuf = *workBufferOwner;
    auto& settleBuf = *settleBufferOwner;
    juce::AudioBuffer<float> wetBuffer(sourceAudio.getNumChannels(), sourceAudio.getNumSamples());

    // Copy dry source audio to output directory for ML pipeline pairing
    {
        auto dryOutputFile = config_.outputDirectory.getChildFile(
            "dry_source" + config_.renderConfig.getFileExtension());
        if (!dryOutputFile.existsAsFile())
            renderPipeline_.writeAudioFile(dryOutputFile, sourceAudio,
                                           config_.renderConfig.sampleRate,
                                           config_.renderConfig.format);
    }

    updateProgress(0, 0, 0, "Rendering variations");

    for (int index = 0; index < config_.totalVariations; ++index)
    {
        if (shouldStop_.load())
        {
            finish(false, "Cancelled");
            return;
        }

        RenderTask task;
        task.variationIndex = index;
        task.parameters = index < static_cast<int>(parameterVariations.size())
            ? parameterVariations[static_cast<size_t>(index)]
            : std::vector<float>{};
        task.outputFile = generateOutputFile(index);

        MORPHSNAP_PROFILE(profiler_, "render_variation_total");
        auto result = renderSingleVariation(task, activeHost, sourceAudio, wetBuffer, workBuf, settleBuf);

        // Retry near-silent renders with spare parameter sets
        constexpr float nearSilentThresholdDb = -40.0f;
        constexpr int maxRetries = 3;
        int retryCount = 0;
        while (result.success && result.rmsDb < nearSilentThresholdDb
               && retryCount < maxRetries && nextSpare < static_cast<int>(spareVariations.size()))
        {
            task.parameters = spareVariations[static_cast<size_t>(nextSpare++)];
            result = renderSingleVariation(task, activeHost, sourceAudio, wetBuffer, workBuf, settleBuf);
            ++retryCount;
        }

        int completed = 0;
        int successful = 0;
        int failed = 0;
        {
            const juce::ScopedLock lock(progressLock_);
            ++progress_.completed;
            if (result.success)
                ++progress_.successfulRenders;
            else
                ++progress_.failedRenders;

            progress_.currentStatus = "Rendered variation " + juce::String(progress_.completed)
                                    + " of " + juce::String(progress_.total);
            progress_.updatePercentage();
            completed = progress_.completed;
            successful = progress_.successfulRenders;
            failed = progress_.failedRenders;
        }

        updateProgress(completed, successful, failed,
                       "Rendered variation " + juce::String(completed)
                     + " of " + juce::String(config_.totalVariations));

        if (onVariationComplete)
            onVariationComplete(index, result);

        if (onProgressUpdate)
            onProgressUpdate(getProgress());
    }

    const auto finalProgress = getProgress();
    const bool success = finalProgress.failedRenders == 0;
    finish(success, success ? "Completed" : "Completed with errors");
}

RenderResult OfflineBatchRenderer::renderSingleVariation(const RenderTask& task,
                                                         IPluginHostManager* hostManager,
                                                         const juce::AudioBuffer<float>& sourceAudio,
                                                         juce::AudioBuffer<float>& wetBuffer,
                                                         juce::AudioBuffer<float>& workBuf,
                                                         juce::AudioBuffer<float>& settleBuf)
{
    RenderResult result;
    result.sampleIndex = task.variationIndex;
    result.outputFile = task.outputFile;

    const auto startMs = juce::Time::getMillisecondCounterHiRes();

    try
    {
        wetBuffer.clear();
        for (int channel = 0; channel < sourceAudio.getNumChannels(); ++channel)
        {
            wetBuffer.copyFrom(channel, 0, sourceAudio, channel, 0, sourceAudio.getNumSamples());
        }

        bool pluginProcessed = false;

        if (hostManager != nullptr && hostManager->hasPlugin())
        {
            if (auto* plugin = hostManager->getPlugin())
            {
                MORPHSNAP_PROFILE(profiler_, "apply_parameters");
                auto& parameters = plugin->getParameters();
                const auto parameterCount = std::min(task.parameters.size(),
                                                     static_cast<size_t>(parameters.size()));

                for (size_t i = 0; i < parameterCount; ++i)
                {
                    if (auto* parameter = parameters[static_cast<int>(i)])
                        parameter->setValue(task.parameters[i]);
                }
            }

            MORPHSNAP_PROFILE(profiler_, "process_audio_blocks");
            juce::MidiBuffer midiBuffer;

            settleBuf.clear();
            juce::AudioBuffer<float> settleView(settleBuf.getArrayOfWritePointers(),
                                                settleBuf.getNumChannels(),
                                                config_.renderConfig.blockSize);
            hostManager->processBlock(settleView, midiBuffer);

            const int totalSamples = sourceAudio.getNumSamples();
            const int blockSize = juce::jmax(1, config_.renderConfig.blockSize);

            for (int sampleOffset = 0; sampleOffset < totalSamples; sampleOffset += blockSize)
            {
                const int samplesThisBlock = juce::jmin(blockSize, totalSamples - sampleOffset);
                workBuf.clear();

                for (int channel = 0; channel < sourceAudio.getNumChannels(); ++channel)
                {
                    workBuf.copyFrom(channel, 0, sourceAudio, channel, sampleOffset, samplesThisBlock);
                }

                juce::AudioBuffer<float> blockView(workBuf.getArrayOfWritePointers(),
                                                   workBuf.getNumChannels(),
                                                   samplesThisBlock);
                hostManager->processBlock(blockView, midiBuffer);

                for (int channel = 0; channel < wetBuffer.getNumChannels(); ++channel)
                {
                    wetBuffer.copyFrom(channel, sampleOffset, blockView, channel, 0, samplesThisBlock);
                }
            }

            pluginProcessed = true;
        }
        else
        {
            // BYPASS WARNING: Plugin host is null or has no plugin.
            // Output will be identical to dry input — no DSP processing occurred.
            result.errorMessage = "WARNING: Plugin not loaded — output is a dry passthrough, no DSP applied";
            result.success = false;
        }

        // Passthrough detection: compare wet vs dry to catch silent bypasses
        if (pluginProcessed)
        {
            double diffEnergy = 0.0;
            double dryEnergy = 0.0;
            const int numSamples = sourceAudio.getNumSamples();
            const auto* dryPtr = sourceAudio.getReadPointer(0);
            const auto* wetPtr = wetBuffer.getReadPointer(0);
            for (int s = 0; s < numSamples; ++s)
            {
                const double diff = static_cast<double>(wetPtr[s]) - static_cast<double>(dryPtr[s]);
                diffEnergy += diff * diff;
                dryEnergy += static_cast<double>(dryPtr[s]) * static_cast<double>(dryPtr[s]);
            }
            const double diffRatio = dryEnergy > 0.0 ? diffEnergy / dryEnergy : 0.0;

            // If the wet/dry difference is negligible, the plugin is likely bypassed
            if (diffRatio < 1e-10 && dryEnergy > 0.0)
            {
                result.errorMessage = "PASSTHROUGH DETECTED: wet output is identical to dry input — "
                                      "plugin may be bypassed or not processing audio";
            }
        }

        const bool isPassthrough = result.errorMessage.contains("PASSTHROUGH");

        // Peak-normalize if clipping: scale wet buffer so peak = -0.3 dBFS.
        // Applied after passthrough detection (which needs raw output) but before
        // writing, so files are clipping-free for ML training.
        float normalizationGainDb = 0.0f;
        if (pluginProcessed && !isPassthrough)
        {
            float peak = 0.0f;
            for (int ch = 0; ch < wetBuffer.getNumChannels(); ++ch)
            {
                const auto* data = wetBuffer.getReadPointer(ch);
                for (int s = 0; s < wetBuffer.getNumSamples(); ++s)
                    peak = juce::jmax(peak, std::abs(data[s]));
            }

            constexpr float targetPeak = 0.966f; // -0.3 dBFS
            if (peak > targetPeak)
            {
                const float gain = targetPeak / peak;
                normalizationGainDb = 20.0f * std::log10(gain);
                wetBuffer.applyGain(gain);
            }
        }

        {
            MORPHSNAP_PROFILE(profiler_, "write_audio_file");
            const auto valid = renderPipeline_.validateAudio(wetBuffer,
                                                             config_.renderConfig,
                                                             result.peakDb,
                                                             result.rmsDb);
            result.hasSilence = !valid && result.rmsDb < config_.renderConfig.silenceThresholdDb;
            result.hasClipping = !valid && result.peakDb > config_.renderConfig.clippingThresholdDb;

            // Always write the audio file (even passthroughs) so verification tools can inspect it
            const bool writeOk = renderPipeline_.writeAudioFile(result.outputFile,
                                                                wetBuffer,
                                                                config_.renderConfig.sampleRate,
                                                                config_.renderConfig.format);
            if (!writeOk)
            {
                result.errorMessage = "Failed to write audio file: " + result.outputFile.getFullPathName();
                result.success = false;
            }
            else
            {
                // File written, but mark as failed if passthrough was detected
                result.success = !isPassthrough;
            }

            // Always write parameter metadata JSON sidecar
            if (writeOk && !task.parameters.empty())
            {
                auto jsonPath = result.outputFile.withFileExtension(".json");
                nlohmann::json meta;
                meta["variation_index"] = task.variationIndex;
                meta["peak_db"] = result.peakDb;
                meta["rms_db"] = result.rmsDb;
                meta["has_silence"] = result.hasSilence;
                meta["has_clipping"] = result.hasClipping;
                meta["plugin_processed"] = pluginProcessed;
                meta["passthrough_detected"] = isPassthrough;
                meta["normalization_gain_db"] = normalizationGainDb;

                nlohmann::json paramArray = nlohmann::json::array();
                for (size_t pi = 0; pi < task.parameters.size(); ++pi)
                    paramArray.push_back({{"index", static_cast<int>(pi)}, {"value", task.parameters[pi]}});
                meta["parameters"] = std::move(paramArray);

                std::ofstream ofs(jsonPath.getFullPathName().toStdString());
                if (ofs.is_open())
                    ofs << meta.dump(2);
            }
        }
    }
    catch (const std::exception& e)
    {
        result.errorMessage = juce::String("Exception during render: ") + e.what();
    }
    catch (...)
    {
        result.errorMessage = "Unknown exception during render";
    }

    result.renderTimeMs = juce::Time::getMillisecondCounterHiRes() - startMs;
    return result;
}

std::vector<std::vector<float>> OfflineBatchRenderer::generateParameterVariations(const juce::AudioPluginInstance* plugin) const
{
    std::vector<std::vector<float>> variations;
    variations.reserve(static_cast<size_t>(config_.totalVariations));

    std::random_device rd;
    std::mt19937 generator(rd());
    juce::Random juceRng;
    juceRng.setSeedRandomly();

    // If safe randomization is enabled and we have a safety config, use it
    const bool useSafetyConfig = safeRandomizationEnabled_ && safetyConfig_.getRuleCount() > 0;

    if (plugin == nullptr)
    {
        // No plugin - generate 8 default parameters with safety if available
        for (int i = 0; i < config_.totalVariations; ++i)
        {
            std::vector<float> parameters;
            parameters.reserve(8);

            for (int param = 0; param < 8; ++param)
            {
                if (useSafetyConfig && safetyConfig_.isSafeToRandomize(param))
                    parameters.push_back(safetyConfig_.generateSafeRandomValue(param, juceRng));
                else if (useSafetyConfig)
                    parameters.push_back(safetyConfig_.sanitizeValue(param, juceRng.nextFloat()));
                else
                    parameters.push_back(std::uniform_real_distribution<float>(0.0f, 1.0f)(generator));
            }

            variations.push_back(std::move(parameters));
        }

        return variations;
    }

    const auto& pluginParameters = plugin->getParameters();
    const auto numParameters = pluginParameters.size();

    // Only use safety config if one was explicitly set
    // Otherwise, trust collectMutationTargets which already has safety logic
    ParameterSafetyConfig effectiveConfig = safetyConfig_;

    // Get base parameter values from plugin
    std::vector<float> baseValues(static_cast<size_t>(numParameters), 0.0f);
    for (int index = 0; index < numParameters; ++index)
    {
        if (auto* parameter = pluginParameters[index])
            baseValues[static_cast<size_t>(index)] = parameter->getValue();
    }

    const auto mutationTargets = collectMutationTargets(plugin);
    if (mutationTargets.empty())
    {
        // No mutation targets - use base values with safety sanitization
        for (int i = 0; i < config_.totalVariations; ++i)
        {
            auto parameters = baseValues;
            if (useSafetyConfig)
                effectiveConfig.sanitizeParameterVector(parameters);
            variations.push_back(std::move(parameters));
        }

        return variations;
    }

    // Filter mutation targets to only include safe parameters
    // If no safety rule exists for a parameter, trust collectMutationTargets (it already excludes dangerous params)
    std::vector<int> safeMutationTargetIndices;
    for (size_t t = 0; t < mutationTargets.size(); ++t)
    {
        const int paramIndex = mutationTargets[t].index;
        const auto* rule = effectiveConfig.getRule(paramIndex);

        // If no rule exists, trust collectMutationTargets - it already has safety logic
        if (!useSafetyConfig || rule == nullptr || rule->randomizeEnabled)
            safeMutationTargetIndices.push_back(static_cast<int>(t));
    }

    if (safeMutationTargetIndices.empty())
    {
        // All parameters are dangerous - just return sanitized base values
        for (int i = 0; i < config_.totalVariations; ++i)
        {
            auto parameters = baseValues;
            if (useSafetyConfig)
                effectiveConfig.sanitizeParameterVector(parameters);
            variations.push_back(std::move(parameters));
        }
        return variations;
    }

    // Group targets by band: targets with the same activationIndices belong to the same band.
    // When a band is selected for mutation, ALL its safe parameters get randomized together.
    // This prevents the "0 dB gain" problem where frequency changes but gain stays flat.
    struct BandGroup
    {
        juce::Array<int> activationIndices;
        std::vector<size_t> targetIndicesIntoSafe; // indices into safeMutationTargetIndices
    };

    std::vector<BandGroup> bandGroups;
    {
        std::map<juce::String, size_t> activationKeyToBandIndex;

        for (size_t si = 0; si < safeMutationTargetIndices.size(); ++si)
        {
            const auto& target = mutationTargets[static_cast<size_t>(safeMutationTargetIndices[si])];

            // Build a string key from activation indices for grouping
            juce::String key;
            for (int ai : target.activationIndices)
                key += juce::String(ai) + ",";
            if (key.isEmpty())
                key = "solo_" + juce::String(target.index);

            auto it = activationKeyToBandIndex.find(key);
            if (it != activationKeyToBandIndex.end())
            {
                bandGroups[it->second].targetIndicesIntoSafe.push_back(si);
            }
            else
            {
                activationKeyToBandIndex[key] = bandGroups.size();
                BandGroup group;
                group.activationIndices = target.activationIndices;
                group.targetIndicesIntoSafe.push_back(si);
                bandGroups.push_back(std::move(group));
            }
        }
    }

    const int maxBands = juce::jmin(4, static_cast<int>(bandGroups.size()));
    std::uniform_int_distribution<int> bandCountDistribution(1, juce::jmax(1, maxBands));

    std::vector<int> bandOrder(bandGroups.size());
    std::iota(bandOrder.begin(), bandOrder.end(), 0);

    for (int i = 0; i < config_.totalVariations; ++i)
    {
        auto parameters = baseValues;
        std::shuffle(bandOrder.begin(), bandOrder.end(), generator);

        const int bandCount = bandCountDistribution(generator);
        for (int bandIdx = 0; bandIdx < bandCount; ++bandIdx)
        {
            const auto& band = bandGroups[static_cast<size_t>(bandOrder[static_cast<size_t>(bandIdx)])];

            // Activate the band
            for (int activationIndex : band.activationIndices)
                parameters[static_cast<size_t>(activationIndex)] = 1.0f;

            // Mutate ALL safe parameters in this band
            for (size_t si : band.targetIndicesIntoSafe)
            {
                const auto& target = mutationTargets[static_cast<size_t>(safeMutationTargetIndices[si])];

                const auto* targetRule = effectiveConfig.getRule(target.index);
                if (useSafetyConfig && targetRule != nullptr && !targetRule->randomizeEnabled)
                    continue;

                if (useSafetyConfig && targetRule != nullptr)
                {
                    parameters[static_cast<size_t>(target.index)] = effectiveConfig.generateSafeRandomValue(
                        target.index, juceRng);
                }
                else
                {
                    const bool isGainParam = target.lowerName.contains("gain");
                    parameters[static_cast<size_t>(target.index)] = target.isDiscrete
                        ? generateDiscreteValue(generator, target.baseValue, target.numSteps)
                        : (isGainParam ? generateGainValue(generator, target.baseValue)
                                       : generateContinuousValue(generator, target.baseValue));
                }
            }
        }

        // Final sanitization ONLY for parameters with explicit dangerous rules
        if (useSafetyConfig)
        {
            for (size_t p = 0; p < parameters.size(); ++p)
            {
                const auto* rule = effectiveConfig.getRule(static_cast<int>(p));
                if (rule != nullptr &&
                    (rule->category == ParameterCategory::DangerousBinary ||
                     rule->category == ParameterCategory::DangerousSystem))
                {
                    parameters[p] = rule->category == ParameterCategory::DangerousBinary
                        ? rule->lockedValue
                        : rule->defaultValue;
                }
            }
        }

        variations.push_back(std::move(parameters));
    }

    return variations;
}

juce::File OfflineBatchRenderer::generateOutputFile(int variationIndex) const
{
    const auto filename = juce::String::formatted("variation_%04d", variationIndex)
                        + config_.renderConfig.getFileExtension();
    return config_.outputDirectory.getChildFile(filename);
}

void OfflineBatchRenderer::updateProgress(int completed, int successful, int failed, const juce::String& status)
{
    const juce::ScopedLock lock(progressLock_);
    progress_.completed = completed;
    progress_.successfulRenders = successful;
    progress_.failedRenders = failed;
    progress_.currentStatus = status;
    progress_.updatePercentage();

    if (startTime_ > 0)
    {
        progress_.elapsedMs = static_cast<double>(juce::Time::getCurrentTime().toMilliseconds() - startTime_);
        if (completed > 0 && progress_.total > completed)
        {
            const auto averageTimePerItem = progress_.elapsedMs / static_cast<double>(completed);
            progress_.estimatedRemainingMs = averageTimePerItem * static_cast<double>(progress_.total - completed);
        }
    }
}

juce::AudioBuffer<float> OfflineBatchRenderer::loadSourceAudio()
{
    juce::AudioBuffer<float> buffer;

    if (!config_.inputFile.existsAsFile())
        return buffer;

    auto reader = std::unique_ptr<juce::AudioFormatReader>(formatManager_.createReaderFor(config_.inputFile));
    if (!reader)
        return buffer;

    buffer.setSize(static_cast<int>(reader->numChannels),
                   static_cast<int>(reader->lengthInSamples));

    if (!reader->read(&buffer,
                      0,
                      static_cast<int>(reader->lengthInSamples),
                      0,
                      true,
                      reader->numChannels > 1))
    {
        buffer.setSize(0, 0);
    }

    return buffer;
}

} // namespace morphsnap
