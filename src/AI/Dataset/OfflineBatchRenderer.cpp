/*
 * More-Phi — AI/Dataset/OfflineBatchRenderer.cpp
 * Sequential hosted-plugin renderer with reusable buffers for offline batches.
 */

#include "AI/Dataset/OfflineBatchRenderer.h"
#include "Core/SIMDAudio.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>

namespace more_phi {

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

struct ParamCache
{
    juce::String lowerName;
    float value = 0.0f;
    int numSteps = 0;
    bool isBoolean = false;
    bool isDiscrete = false;
    bool isNull = true;
};

std::vector<MutationTarget> collectMutationTargets(const juce::AudioPluginInstance* plugin,
                                                    std::vector<float>* outBaseValues = nullptr)
{
    std::vector<MutationTarget> targets;
    if (plugin == nullptr)
        return targets;

    const auto& parameters = plugin->getParameters();
    const int numParams = parameters.size();

    // Pre-cache ALL parameter metadata in one pass to minimize yabridge IPC issues
    std::vector<ParamCache> cache(static_cast<size_t>(numParams));
    for (int index = 0; index < numParams; ++index)
    {
        auto* parameter = parameters[index];
        if (parameter == nullptr)
            continue;

        auto& c = cache[static_cast<size_t>(index)];
        c.isNull = false;
        c.lowerName = parameter->getName(256).toLowerCase().trim();
        c.value = parameter->getValue();
        c.numSteps = parameter->getNumSteps();
        c.isBoolean = parameter->isBoolean();
        c.isDiscrete = !c.isBoolean
            && (parameter->isDiscrete()
                || (c.numSteps > 1 && c.numSteps < std::numeric_limits<int>::max()));
    }

    juce::Array<juce::String> lowerNames;
    lowerNames.ensureStorageAllocated(numParams);
    for (const auto& c : cache)
        lowerNames.add(c.lowerName);

    // Optionally export cached base values
    if (outBaseValues != nullptr)
    {
        outBaseValues->resize(static_cast<size_t>(numParams), 0.0f);
        for (int i = 0; i < numParams; ++i)
            (*outBaseValues)[static_cast<size_t>(i)] = cache[static_cast<size_t>(i)].value;
    }

    const juce::String pluginName = plugin->getName().toLowerCase();
    const bool strictFabFilterMode = pluginName.contains("fabfilter")
        || pluginName.contains("pro-q")
        || pluginName.contains("volcano");

    const auto appendTargets = [&](auto&& predicate)
    {
        for (int index = 0; index < numParams; ++index)
        {
            const auto& c = cache[static_cast<size_t>(index)];
            if (c.isNull)
                continue;

            const auto& lowerName = c.lowerName;

            // Inline exclusion check using cached data (avoids IPC)
            if (c.isBoolean || isTrapBinaryParameter(lowerName) || isStructuralToggle(lowerName))
                continue;
            if (containsAny(lowerName, {
                "dry/wet", "dry", "wet", "mix",
                "stereo placement", "speakers",
                "side chain", "external",
                "output", "input", "master",
                "power", "mono", "latency",
                "oversampling", "quality", "lookahead",
                "analyzer", "meter", "routing"
            }) || isGlobalGainLike(lowerName))
                continue;

            if (!predicate(lowerName))
                continue;

            MutationTarget target;
            target.index = index;
            target.baseValue = c.value;
            target.numSteps = c.numSteps;
            target.isDiscrete = c.isDiscrete;
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

        // Discover and load the plugin inside a single MessageManagerLock scope
        // so the same wineserver/IPC state is used for both operations.
        // NOTE: Do NOT kill wineserver between scan and load — the same yabridge
        // Wine process must stay alive for both operations, otherwise the load
        // segfaults during processBlock due to stale IPC state.
        juce::PluginDescription description;

        auto discoverPrepareAndLoad = [&]()
        {
            juce::String discoveryError;
            if (!PluginHostManager::discoverPlugin(
                    pluginHost_->getFormatManager(), config_.pluginFile,
                    description, discoveryError))
            {
                std::cerr << "[OfflineBatchRenderer] Plugin discovery warning: "
                          << discoveryError << std::endl;
                // Continue with fallback description — loadPlugin may still succeed
            }

            pluginHost_->prepare(config_.renderConfig.sampleRate,
                                 config_.renderConfig.blockSize,
                                 config_.renderConfig.numChannels);

            pluginLoaded_ = pluginHost_->loadPlugin(description);
            if (!pluginLoaded_)
                std::cerr << "[OfflineBatchRenderer] loadPlugin failed for: "
                          << description.fileOrIdentifier << std::endl;
            return pluginLoaded_;
        };

        if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
        {
            juce::MessageManagerLock mmLock(juce::Thread::getCurrentThread());
            if (!mmLock.lockWasGained())
            {
                std::cerr << "[OfflineBatchRenderer] Failed to acquire MessageManagerLock\n";
                return false;
            }

            if (!discoverPrepareAndLoad())
                return false;
        }
        else if (!discoverPrepareAndLoad())
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

    // Run synchronously on the calling thread for yabridge compatibility.
    // yabridge/Wine plugins may require processBlock on the same thread
    // that called prepareToPlay.
    renderLoop(hostManager);
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
        MORE_PHI_PROFILE(profiler_, "load_source_audio");
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
        MORE_PHI_PROFILE(profiler_, "generate_parameter_variations");
        // Use a local copy to request extra variations for spares instead of
        // temporarily mutating config_ (which is fragile if render ever goes async).
        const auto totalWithSpares = config_.totalVariations + spareCount;
        const auto savedTotal = config_.totalVariations;
        config_.totalVariations = totalWithSpares;
        parameterVariations = generateParameterVariations(activeHost != nullptr ? activeHost->getPlugin() : nullptr);
        config_.totalVariations = savedTotal;
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

    if (activeHost != nullptr && activeHost != pluginHost_.get())
    {
        MORE_PHI_PROFILE(profiler_, "prepare_plugin");
        activeHost->prepare(config_.renderConfig.sampleRate,
                            config_.renderConfig.blockSize,
                            sourceAudio.getNumChannels());
    }

    const int processChannels = juce::jmax(sourceAudio.getNumChannels(), config_.renderConfig.numChannels);
    const int blockSize = juce::jmax(1, config_.renderConfig.blockSize);

    const int numWorkers = config_.parallelWorkers;
    if (numWorkers > 1)
        threadPool_ = std::make_unique<ThreadPool>(static_cast<size_t>(numWorkers));

    // Parallel path: Use ThreadPool to render variations concurrently.
    // Each worker thread needs its own PluginHostManager and render buffers
    // to avoid thread-safety issues with plugin state and IPC.
    if (threadPool_ != nullptr)
    {
        updateProgress(0, 0, 0, "Initializing parallel worker pool");

        // Per-worker thread local state for plugin hosting and scratch buffers
        struct WorkerState
        {
            std::unique_ptr<PluginHostManager> host;
            AudioBufferPool::AudioBufferPtr workBuf;
            AudioBufferPool::AudioBufferPtr settleBuf;
            juce::AudioBuffer<float> wetBuf;
            bool failed = false;
        };

        // Initialize worker states before starting the render loop
        std::vector<std::unique_ptr<WorkerState>> workerStates;
        workerStates.reserve(static_cast<size_t>(numWorkers));
        for (int i = 0; i < numWorkers; ++i)
            workerStates.push_back(std::make_unique<WorkerState>());

        std::vector<std::future<void>> initFutures;
        for (int i = 0; i < numWorkers; ++i)
        {
            initFutures.push_back(threadPool_->enqueue([&, i]() {
                auto& state = *workerStates[static_cast<size_t>(i)];
                state.host = std::make_unique<PluginHostManager>();
                
                juce::PluginDescription desc;
                juce::String error;
                if (activeHost != nullptr && activeHost->getLastDescription() != nullptr)
                    desc = *activeHost->getLastDescription();
                else if (config_.hasValidPlugin())
                    PluginHostManager::discoverPlugin(state.host->getFormatManager(), config_.pluginFile, desc, error);

                state.host->prepare(config_.renderConfig.sampleRate, config_.renderConfig.blockSize, sourceAudio.getNumChannels());
                if (!desc.fileOrIdentifier.isEmpty())
                    state.host->loadPlugin(desc);
                
                if (config_.useMemoryPool)
                {
                    auto pool = std::make_unique<AudioBufferPool>(processChannels, blockSize, config_.renderConfig.sampleRate);
                    pool->preallocate(2);
                    state.workBuf = pool->acquireBuffer();
                    state.settleBuf = pool->acquireBuffer();
                }
                else
                {
                    state.workBuf = std::make_unique<juce::AudioBuffer<float>>(processChannels, blockSize);
                    state.settleBuf = std::make_unique<juce::AudioBuffer<float>>(processChannels, blockSize);
                }
                
                state.wetBuf.setSize(sourceAudio.getNumChannels(), sourceAudio.getNumSamples());
                state.failed = !state.workBuf || !state.settleBuf;
            }));
        }

        for (auto& f : initFutures) f.get();

        updateProgress(0, 0, 0, "Rendering variations (Parallel)");

        std::vector<std::future<void>> renderFutures;
        for (int index = 0; index < config_.totalVariations; ++index)
        {
            renderFutures.push_back(threadPool_->enqueue([&, index]() {
                if (shouldStop_.load()) return;

                // Simple round-robin worker assignment
                const size_t workerIdx = static_cast<size_t>(index % numWorkers);
                auto& state = *workerStates[workerIdx];
                if (state.failed) return;

                RenderTask task;
                task.variationIndex = index;
                task.parameters = index < static_cast<int>(parameterVariations.size())
                    ? parameterVariations[static_cast<size_t>(index)]
                    : std::vector<float>{};
                task.outputFile = generateOutputFile(index);

                auto result = renderSingleVariation(task, state.host.get(), sourceAudio, state.wetBuf, *state.workBuf, *state.settleBuf);

                {
                    const juce::ScopedLock lock(progressLock_);
                    ++progress_.completed;
                    if (result.success) ++progress_.successfulRenders;
                    else ++progress_.failedRenders;
                    
                    if (onVariationComplete) onVariationComplete(index, result);
                }

                if (onProgressUpdate)
                {
                    auto p = getProgress();
                    updateProgress(p.completed, p.successfulRenders, p.failedRenders, 
                                   "Rendered variation " + juce::String(p.completed) + " of " + juce::String(p.total));
                    onProgressUpdate(p);
                }
            }));
        }

        for (auto& f : renderFutures) f.get();
        threadPool_->shutdown();
    }
    else
    {
        // Fallback to sequential rendering (preserves existing logic)
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

        // Helper: reload the plugin when the Wine host process crashes (broken pipe).
        auto reloadPlugin = [&]() -> IPluginHostManager*
        {
            pluginHost_.reset();
            pluginLoaded_ = false;

            {
                juce::ChildProcess wineserver;
                wineserver.start("wineserver -k");
                wineserver.waitForProcessToFinish(3000);  // 3 second timeout
            }

            juce::Thread::sleep(2000);

            pluginHost_ = std::make_unique<PluginHostManager>();
            juce::PluginDescription description;
            juce::String discoveryError;
            PluginHostManager::discoverPlugin(pluginHost_->getFormatManager(), config_.pluginFile, description, discoveryError);

            pluginHost_->prepare(config_.renderConfig.sampleRate,
                                 config_.renderConfig.blockSize,
                                 config_.renderConfig.numChannels);

            pluginLoaded_ = pluginHost_->loadPlugin(description);
            if (pluginLoaded_ && pluginHost_->hasPlugin())
                return pluginHost_.get();
            return nullptr;
        };

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

            MORE_PHI_PROFILE(profiler_, "render_variation_total");
            auto result = renderSingleVariation(task, activeHost, sourceAudio, wetBuffer, workBuf, settleBuf);

            // Detect Wine host crash (broken pipe / connection reset) and auto-reload
            if (!result.success
                && (result.errorMessage.contains("Broken pipe")
                    || result.errorMessage.contains("Connection reset")))
            {
                auto* reloaded = reloadPlugin();
                if (reloaded != nullptr)
                {
                    activeHost = reloaded;
                    result = renderSingleVariation(task, activeHost, sourceAudio, wetBuffer, workBuf, settleBuf);
                }
            }

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

            {
                const juce::ScopedLock lock(progressLock_);
                ++progress_.completed;
                if (result.success)
                    ++progress_.successfulRenders;
                else
                    ++progress_.failedRenders;
            }

            const int completed = progress_.completed;
            const int successful = progress_.successfulRenders;
            const int failed = progress_.failedRenders;
            updateProgress(completed, successful, failed,
                           "Rendered variation " + juce::String(completed)
                         + " of " + juce::String(config_.totalVariations));

            if (onVariationComplete)
                onVariationComplete(index, result);

            if (onProgressUpdate)
                onProgressUpdate(getProgress());
        }
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
                MORE_PHI_PROFILE(profiler_, "apply_parameters");
                auto& parameters = plugin->getParameters();
                const auto parameterCount = std::min(task.parameters.size(),
                                                     static_cast<size_t>(parameters.size()));

                for (size_t i = 0; i < parameterCount; ++i)
                {
                    if (auto* parameter = parameters[static_cast<int>(i)])
                        parameter->setValue(task.parameters[i]);
                }
            }

            MORE_PHI_PROFILE(profiler_, "process_audio_blocks");
            juce::MidiBuffer midiBuffer;

            // Settle block: run one silent block through the plugin to flush internal state.
            // Reuse the pooled settleBuf instead of allocating a new buffer per variation (H3).
            {
                settleBuf.clear();
                hostManager->processBlock(settleBuf, midiBuffer);
            }

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

            for (int ch = 0; ch < sourceAudio.getNumChannels(); ++ch)
            {
                const auto* dryPtr = sourceAudio.getReadPointer(ch);
                const auto* wetPtr = wetBuffer.getReadPointer(ch);

                if (config_.enableSIMD)
                {
                    // Use RMS^2 * samples to get energy via SIMD
                    const float dryRMS = SIMDAudio::calculateRMS(dryPtr, static_cast<size_t>(numSamples));
                    dryEnergy += static_cast<double>(dryRMS) * static_cast<double>(dryRMS) * numSamples;

                    // For the difference, we still need a loop unless we use a temp buffer
                    for (int s = 0; s < numSamples; ++s)
                    {
                        const double diff = static_cast<double>(wetPtr[s]) - static_cast<double>(dryPtr[s]);
                        diffEnergy += diff * diff;
                    }
                }
                else
                {
                    for (int s = 0; s < numSamples; ++s)
                    {
                        const double diff = static_cast<double>(wetPtr[s]) - static_cast<double>(dryPtr[s]);
                        diffEnergy += diff * diff;
                        dryEnergy += static_cast<double>(dryPtr[s]) * static_cast<double>(dryPtr[s]);
                    }
                }
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
        float normalizationGainDb = 0.0f;
        if (pluginProcessed && !isPassthrough)
        {
            float peak = 0.0f;
            if (config_.enableSIMD)
            {
                for (int ch = 0; ch < wetBuffer.getNumChannels(); ++ch)
                    peak = std::max(peak, SIMDAudio::findPeak(wetBuffer.getReadPointer(ch), static_cast<size_t>(wetBuffer.getNumSamples())));
            }
            else
            {
                for (int ch = 0; ch < wetBuffer.getNumChannels(); ++ch)
                {
                    const auto* data = wetBuffer.getReadPointer(ch);
                    for (int s = 0; s < wetBuffer.getNumSamples(); ++s)
                        peak = juce::jmax(peak, std::abs(data[s]));
                }
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
            MORE_PHI_PROFILE(profiler_, "write_audio_file");
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
        result.success = false;
    }
    catch (...)
    {
        result.errorMessage = "Unknown exception during render";
        result.success = false;
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


    if (config_.configDirectory.exists() && config_.configDirectory.isDirectory())
    {
        auto jsonFiles = config_.configDirectory.findChildFiles(juce::File::findFiles, false, "*.json");
        jsonFiles.sort(); // Sort alphabetically to maintain deterministic ordering

        configFileBaseNames_.clear();
        
        for (const auto& file : jsonFiles)
        {
            try
            {
                nlohmann::json j = nlohmann::json::parse(file.loadFileAsString().toStdString());
                std::vector<float> params;
                if (j.contains("parameters") && j["parameters"].is_array())
                {
                    // Find max index to size the vector
                    int maxIdx = -1;
                    for (const auto& p : j["parameters"]) {
                        int idx = p["index"].get<int>();
                        if (idx > maxIdx) maxIdx = idx;
                    }
                    // DATASET-4: clamp against the project's parameter ceiling
                    // (MAX_PARAMETERS=2048) so a crafted config can't force a
                    // multi-GB resize from a hostile index value.
                    constexpr int kMaxDatasetParamIndex = 2047;
                    if (maxIdx >= 0 && maxIdx <= kMaxDatasetParamIndex) {
                        params.resize(static_cast<size_t>(maxIdx) + 1, 0.0f);
                        for (const auto& p : j["parameters"]) {
                            int idx = p["index"].get<int>();
                            if (idx < 0 || idx > maxIdx) continue;  // safety
                            float val = p["value"].get<float>();
                            params[static_cast<size_t>(idx)] = val;
                        }
                    }
                }
                variations.push_back(std::move(params));
                configFileBaseNames_.push_back(file.getFileNameWithoutExtension());
            }
            catch (...)
            {
                // ignore invalid files
            }
        }
        
        // If we found valid variations, return them. Otherwise fallback to random generation.
        if (!variations.empty())
        {
            // Truncate to totalVariations if needed
            if (variations.size() > static_cast<size_t>(config_.totalVariations))
                variations.resize(config_.totalVariations);
            return variations;
        }
    }

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

    // Collect mutation targets AND base values in one pass through yabridge IPC
    std::vector<float> baseValues;
    auto mutationTargets = collectMutationTargets(plugin, &baseValues);
    if (baseValues.size() != static_cast<size_t>(numParameters))
        baseValues.resize(static_cast<size_t>(numParameters), 0.0f);

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
    // When using --config-dir, name outputs after the source config files
    if (variationIndex >= 0
        && static_cast<size_t>(variationIndex) < configFileBaseNames_.size())
    {
        return config_.outputDirectory.getChildFile(
            configFileBaseNames_[static_cast<size_t>(variationIndex)]
            + config_.renderConfig.getFileExtension());
    }

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

    // H2: Reject files exceeding maxInputFileSizeMB to prevent OOM.
    // A 2-hour 96kHz stereo file is ~2.6 GB in memory.
    const juce::int64 fileSizeBytes = config_.inputFile.getSize();
    const juce::int64 maxBytes = static_cast<juce::int64>(config_.maxInputFileSizeMB) * 1024 * 1024;
    if (maxBytes > 0 && fileSizeBytes > maxBytes)
    {
        std::cerr << "Error: input file size (" << (fileSizeBytes / (1024 * 1024))
                  << " MB) exceeds limit of " << config_.maxInputFileSizeMB << " MB\n";
        return buffer;
    }

    // H3: Respect renderConfig.fullDuration to limit render length.
    // Calculate max samples to read based on configured duration.
    const juce::int64 maxSamplesToRead = static_cast<juce::int64>(
        config_.renderConfig.fullDuration * reader->sampleRate
    );

    // H4: Skip silent intros - detect and seek past silent regions.
    // Many source files (especially MUSDB18-HQ stems) have silent intros
    // which would result in silent output when rendering from the beginning.
    // Use threshold of 0.005 (-46dB) to properly detect meaningful audio.
    // Lower thresholds catch noise/artifacts which aren't useful for training.
    constexpr float kSilenceThresholdLinear = 0.005f;  // -46dB approximately
    constexpr int kScanBlockSize = 4096;  // Scan in larger blocks for efficiency
    constexpr int kMaxSilentSamplesToScan = 60 * 48000;  // Scan up to 60 seconds for non-silent audio

    juce::int64 startSample = 0;

    // Scan for first non-silent sample
    {
        juce::AudioBuffer<float> scanBuffer(static_cast<int>(reader->numChannels), kScanBlockSize);
        const juce::int64 scanLimit = std::min(reader->lengthInSamples, static_cast<juce::int64>(kMaxSilentSamplesToScan));

        while (startSample < scanLimit)
        {
            const int samplesToScan = static_cast<int>(std::min(
                static_cast<juce::int64>(kScanBlockSize),
                scanLimit - startSample));

            scanBuffer.clear();
            if (!reader->read(&scanBuffer, 0, samplesToScan, startSample, true, reader->numChannels > 1))
                break;

            // Check if this block has non-silent audio
            bool hasAudio = false;
            for (int channel = 0; channel < static_cast<int>(reader->numChannels) && !hasAudio; ++channel)
            {
                const float* channelData = scanBuffer.getReadPointer(channel);
                for (int i = 0; i < samplesToScan; ++i)
                {
                    if (std::abs(channelData[i]) > kSilenceThresholdLinear)
                    {
                        hasAudio = true;
                        break;
                    }
                }
            }

            if (hasAudio)
                break;  // Found non-silent audio, start from here

            startSample += samplesToScan;
        }
    }

    // Calculate available samples from the detected start position
    const juce::int64 availableSamples = reader->lengthInSamples - startSample;
    const juce::int64 samplesToRead = std::min(availableSamples, maxSamplesToRead);

    if (samplesToRead <= 0)
    {
        std::cerr << "Warning: No audio available after silent intro detection\n";
        return buffer;
    }

    buffer.setSize(static_cast<int>(reader->numChannels),
                   static_cast<int>(samplesToRead));

    if (!reader->read(&buffer,
                      0,
                      static_cast<int>(samplesToRead),
                      startSample,
                      true,
                      reader->numChannels > 1))
    {
        buffer.setSize(0, 0);
    }

    return buffer;
}

} // namespace more_phi
