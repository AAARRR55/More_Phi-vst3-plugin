/*
 * MorphSnap — AI/Dataset/OfflineBatchRenderer.cpp
 * Offline faster-than-real-time batch processor for dataset generation.
 *
 * Core architecture:
 *   1. Read dry audio into memory (one-shot)
 *   2. For each parameter variation:
 *      a. Apply parameters to hosted plugin
 *      b. Feed dry buffer through processBlock() in blockSize chunks
 *      c. Write wet output to disk
 *      d. Optionally extract features
 *      e. Record metadata
 *   3. Write manifest.json with all metadata
 */
#include "OfflineBatchRenderer.h"
#include "../../Host/PluginHostManager.h"
#include <fstream>
#include <chrono>
#include <cmath>

namespace morphsnap {

namespace {

bool isVolcano3Plugin(const juce::AudioPluginInstance& plugin)
{
    return plugin.getName().containsIgnoreCase("Volcano 3");
}

bool isVolcano3SafeParameter(const juce::String& name)
{
    static const juce::StringArray safeParams{
        "Mix"
    };

    return safeParams.contains(name);
}

} // namespace

OfflineBatchRenderer::OfflineBatchRenderer() = default;

OfflineBatchRenderer::~OfflineBatchRenderer()
{
    stopRender();
}

void OfflineBatchRenderer::setConfig(const OfflineBatchConfig& config)
{
    if (isRunning_.load(std::memory_order_relaxed))
    {
        DBG("OfflineBatchRenderer::setConfig called while running — ignored.");
        return;
    }
    config_ = config;
}

bool OfflineBatchRenderer::startRender(PluginHostManager& hostManager)
{
    if (isRunning_.load(std::memory_order_relaxed))
        return false;

    shouldStop_.store(false, std::memory_order_relaxed);
    isRunning_.store(true, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(progressMutex_);
        progress_ = {};
        progress_.total = config_.totalVariations;
    }

    workerThread_ = std::make_unique<std::thread>([this, &hostManager]()
    {
        renderLoop(hostManager);
    });

    return true;
}

bool OfflineBatchRenderer::renderBlocking(PluginHostManager& hostManager)
{
    if (isRunning_.load(std::memory_order_relaxed))
        return false;

    shouldStop_.store(false, std::memory_order_relaxed);
    isRunning_.store(true, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(progressMutex_);
        progress_ = {};
        progress_.total = config_.totalVariations;
    }

    renderLoop(hostManager);
    return progress_.success;
}

void OfflineBatchRenderer::stopRender()
{
    shouldStop_.store(true, std::memory_order_relaxed);
    if (workerThread_ && workerThread_->joinable())
        workerThread_->join();
    workerThread_.reset();
}

OfflineBatchProgress OfflineBatchRenderer::getProgress() const
{
    std::lock_guard<std::mutex> lk(progressMutex_);
    return progress_;
}

// ═══════════════════════════════════════════════════════════════════════════════
// renderLoop — The main offline batch render engine
// ═══════════════════════════════════════════════════════════════════════════════
void OfflineBatchRenderer::renderLoop(PluginHostManager& hostManager)
{
    using Clock = std::chrono::steady_clock;
    auto startTime = Clock::now();

    // ── 1. Validate state ───────────────────────────────────────────────────
    if (!config_.inputFile.existsAsFile())
    {
        std::lock_guard<std::mutex> lk(progressMutex_);
        progress_.error = "Input file does not exist: " + config_.inputFile.getFullPathName();
        progress_.finished = true;
        progress_.success = false;
        isRunning_.store(false, std::memory_order_relaxed);
        if (onComplete) onComplete(false, progress_.error);
        return;
    }

    if (!hostManager.hasPlugin())
    {
        std::lock_guard<std::mutex> lk(progressMutex_);
        progress_.error = "No plugin loaded. Load a VST3 plugin first.";
        progress_.finished = true;
        progress_.success = false;
        isRunning_.store(false, std::memory_order_relaxed);
        if (onComplete) onComplete(false, progress_.error);
        return;
    }

    // ── 2. Read the dry audio into memory ───────────────────────────────────
    juce::AudioFormatManager fmtMgr;
    fmtMgr.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(
        fmtMgr.createReaderFor(config_.inputFile));

    if (!reader)
    {
        std::lock_guard<std::mutex> lk(progressMutex_);
        progress_.error = "Cannot read audio file: " + config_.inputFile.getFullPathName();
        progress_.finished = true;
        progress_.success = false;
        isRunning_.store(false, std::memory_order_relaxed);
        if (onComplete) onComplete(false, progress_.error);
        return;
    }

    const int numChannels = static_cast<int>(reader->numChannels);
    const int64_t totalSamples = static_cast<int64_t>(reader->lengthInSamples);
    const double sampleRate = reader->sampleRate;

    juce::AudioBuffer<float> dryBuffer(numChannels, static_cast<int>(totalSamples));
    reader->read(&dryBuffer, 0, static_cast<int>(totalSamples), 0, true, true);
    reader.reset(); // Free file handle immediately

    DBG("OfflineBatchRenderer: Loaded " + config_.inputFile.getFileName()
        + " (" + juce::String(totalSamples) + " samples, "
        + juce::String(sampleRate) + " Hz, "
        + juce::String(numChannels) + " ch)");
    std::cout << "[DEBUG] Audio loaded: " << totalSamples << " samples, " << sampleRate << " Hz\n" << std::flush;

    // ── 3. Create output directory ──────────────────────────────────────────
    auto datasetDir = config_.outputDirectory.getChildFile(config_.datasetName);
    datasetDir.createDirectory();
    auto wetDir = datasetDir.getChildFile("wet");
    wetDir.createDirectory();

    if (config_.saveDryAudio)
    {
        auto dryDir = datasetDir.getChildFile("dry");
        dryDir.createDirectory();
        config_.inputFile.copyFileTo(dryDir.getChildFile(config_.inputFile.getFileName()));
    }

    // ── 4. Generate parameter variations using Latin Hypercube Sampling ──────
    auto* plugin = hostManager.getPlugin();
    if (!plugin)
    {
        std::lock_guard<std::mutex> lk(progressMutex_);
        progress_.error = "Plugin instance lost.";
        progress_.finished = true;
        progress_.success = false;
        isRunning_.store(false, std::memory_order_relaxed);
        if (onComplete) onComplete(false, progress_.error);
        return;
    }

    const int paramCount = plugin->getParameters().size();

    SamplingConfig samplingCfg;
    samplingCfg.sampleCount = config_.totalVariations;
    samplingCfg.seed = config_.randomSeed;
    samplingCfg.strategy = SamplingStrategy::LHS;

    ParameterSampler sampler;
    sampler.setSeed(config_.randomSeed);
    auto paramSets = sampler.generate(samplingCfg, paramCount);

    DBG("OfflineBatchRenderer: Generated " + juce::String(static_cast<int>(paramSets.size()))
        + " parameter variations for " + juce::String(paramCount) + " parameters");
    std::cout << "[DEBUG] Generated " << paramSets.size() << " parameter variations\n" << std::flush;

    // ── 5. Prepare the plugin for offline rendering ─────────────────────────
    const int pluginInputChannels = juce::jmax(plugin->getTotalNumInputChannels(), numChannels);
    const int pluginOutputChannels = juce::jmax(plugin->getTotalNumOutputChannels(), numChannels);
    const int pluginBufferChannels = juce::jmax(pluginInputChannels, pluginOutputChannels);

    std::cout << "Preparing plugin for offline rendering (sampleRate=" << sampleRate
              << ", blockSize=" << config_.blockSize << ", channels=" << pluginBufferChannels << ")...\n" << std::flush;

    // Some plugins (e.g., FabFilter) require message thread interaction during
    // prepareToPlay(). We acquire the MessageManagerLock to ensure the message
    // thread is available for any async operations the plugin may trigger.
    {
        juce::MessageManagerLock mmLock(juce::Thread::getCurrentThread());
        if (mmLock.lockWasGained())
        {
            hostManager.prepare(sampleRate, config_.blockSize, pluginBufferChannels);
        }
        else
        {
            std::lock_guard<std::mutex> lk(progressMutex_);
            progress_.error = "Failed to acquire message manager lock for plugin preparation.";
            progress_.finished = true;
            progress_.success = false;
            isRunning_.store(false, std::memory_order_relaxed);
            if (onComplete) onComplete(false, progress_.error);
            return;
        }
    }

    // ── 6. Manifest for all generated files ─────────────────────────────────
    nlohmann::json manifest;
    manifest["datasetName"] = config_.datasetName.toStdString();
    manifest["inputFile"] = config_.inputFile.getFullPathName().toStdString();
    manifest["sampleRate"] = sampleRate;
    manifest["numChannels"] = numChannels;
    manifest["totalSamples"] = totalSamples;
    manifest["totalVariations"] = config_.totalVariations;
    manifest["blockSize"] = config_.blockSize;
    manifest["randomSeed"] = config_.randomSeed;
    manifest["pluginName"] = plugin->getName().toStdString();
    manifest["parameterCount"] = paramCount;
    manifest["samples"] = nlohmann::json::array();

    // ── 7. Render each variation ────────────────────────────────────────────
    int successCount = 0;
    std::cout << "[DEBUG] Starting render loop for " << paramSets.size() << " variations\n" << std::flush;

    for (int i = 0; i < static_cast<int>(paramSets.size()); ++i)
    {
        if (shouldStop_.load(std::memory_order_relaxed))
            break;

        bool ok = renderVariation(i, paramSets[static_cast<size_t>(i)],
                                   dryBuffer, sampleRate,
                                   hostManager, wetDir, manifest);

        if (ok) ++successCount;

        // Update progress
        auto now = Clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(now - startTime).count();
        double perSampleMs = (i + 1 > 0) ? elapsedMs / (i + 1) : 0.0;
        double remainingMs = perSampleMs * (paramSets.size() - i - 1);

        {
            std::lock_guard<std::mutex> lk(progressMutex_);
            progress_.completed = i + 1;
            progress_.percentage = static_cast<float>(i + 1) / static_cast<float>(paramSets.size()) * 100.0f;
            progress_.currentFile = "variation_" + juce::String(i).paddedLeft('0', 5) + ".wav";
            progress_.elapsedMs = elapsedMs;
            progress_.estimatedRemainingMs = remainingMs;
        }

        if (onProgress)
        {
            std::lock_guard<std::mutex> lk(progressMutex_);
            onProgress(progress_);
        }
    }

    // ── 8. Write manifest.json ──────────────────────────────────────────────
    {
        auto manifestFile = datasetDir.getChildFile("manifest.json");
        std::ofstream ofs(manifestFile.getFullPathName().toStdString());
        if (ofs.is_open())
        {
            ofs << manifest.dump(2);
            ofs.close();
        }
    }

    // ── 9. Finalize ─────────────────────────────────────────────────────────
    auto endTime = Clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

    bool wasCancelled = shouldStop_.load(std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(progressMutex_);
        progress_.finished = true;
        progress_.success = !wasCancelled;
        progress_.elapsedMs = totalMs;
    }

    DBG("OfflineBatchRenderer: " + (wasCancelled ? juce::String("Cancelled") : juce::String("Complete"))
        + " — " + juce::String(successCount) + "/" + juce::String(static_cast<int>(paramSets.size()))
        + " variations in " + juce::String(totalMs / 1000.0, 1) + "s");

    isRunning_.store(false, std::memory_order_relaxed);

    juce::String resultMsg = wasCancelled
        ? "Cancelled after " + juce::String(successCount) + " variations."
        : "Generated " + juce::String(successCount) + " dataset entries in "
          + juce::String(totalMs / 1000.0, 1) + "s → " + datasetDir.getFullPathName();

    if (onComplete)
        onComplete(!wasCancelled, resultMsg);
}

// ═══════════════════════════════════════════════════════════════════════════════
// renderVariation — Process a single parameter variation offline
// ═══════════════════════════════════════════════════════════════════════════════
bool OfflineBatchRenderer::renderVariation(
    int index,
    const std::vector<float>& params,
    const juce::AudioBuffer<float>& dryBuffer,
    double sampleRate,
    PluginHostManager& hostManager,
    const juce::File& outputDir,
    nlohmann::json& manifest)
{
    auto* plugin = hostManager.getPlugin();
    if (!plugin) return false;

    const int totalSamples = dryBuffer.getNumSamples();
    const int numInputChannels = dryBuffer.getNumChannels();
    const int blockSize = config_.blockSize;
    const int pluginInputChannels = juce::jmax(plugin->getTotalNumInputChannels(), numInputChannels);
    const int pluginOutputChannels = juce::jmax(plugin->getTotalNumOutputChannels(), numInputChannels);
    const int pluginBufferChannels = juce::jmax(pluginInputChannels, pluginOutputChannels);

    if (index == 0)
        std::cout << "[DEBUG] Variation 0: start\n" << std::flush;

    // ── 1. Apply parameter values to the hosted plugin ──────────────────────
    // Use setValue() instead of setValueNotifyingHost() for offline rendering.
    // setValueNotifyingHost() triggers async listener notifications that require
    // the message thread, which can cause hangs in headless CLI mode.
    auto& pluginParams = plugin->getParameters();
    const bool volcanoSafeMode = isVolcano3Plugin(*plugin);
    int appliedParamCount = 0;
    for (int p = 0; p < static_cast<int>(pluginParams.size()) && p < static_cast<int>(params.size()); ++p)
    {
        if (auto* param = pluginParams[p])
        {
            const auto paramName = param->getName(128);

            if (volcanoSafeMode && !isVolcano3SafeParameter(paramName))
                continue;

            param->setValue(params[static_cast<size_t>(p)]);
            ++appliedParamCount;
        }
    }

    if (index == 0)
        std::cout << "[DEBUG] Variation 0: applied " << appliedParamCount << " parameters\n" << std::flush;

    // Let the plugin settle (some plugins need a buffer to apply changes)
    {
        juce::AudioBuffer<float> silenceBuf(pluginBufferChannels, blockSize);
        silenceBuf.clear();
        juce::MidiBuffer emptyMidi;
        hostManager.processBlock(silenceBuf, emptyMidi);
    }

    if (index == 0)
        std::cout << "[DEBUG] Variation 0: settle block processed\n" << std::flush;

    // ── 2. Process dry buffer through plugin in blockSize chunks ────────────
    juce::AudioBuffer<float> wetBuffer(pluginOutputChannels, totalSamples);
    wetBuffer.clear();
    juce::AudioBuffer<float> workBuf(pluginBufferChannels, blockSize);

    for (int pos = 0; pos < totalSamples; pos += blockSize)
    {
        const int remaining = totalSamples - pos;
        const int thisBlock = juce::jmin(blockSize, remaining);

        // Use a bus-sized working buffer so plugins with sidechain/aux inputs
        // receive the channel layout they advertised during instantiation.
        workBuf.clear();
        for (int ch = 0; ch < numInputChannels; ++ch)
            workBuf.copyFrom(ch, 0, dryBuffer, ch, pos, thisBlock);

        // Process through the hosted plugin
        juce::MidiBuffer emptyMidi;
        hostManager.processBlock(workBuf, emptyMidi);

        // Copy result into output
        for (int ch = 0; ch < wetBuffer.getNumChannels(); ++ch)
            wetBuffer.copyFrom(ch, pos, workBuf, ch, 0, thisBlock);
    }

    if (index == 0)
        std::cout << "[DEBUG] Variation 0: audio rendered\n" << std::flush;

    // ── 3. Optional peak normalization ──────────────────────────────────────
    if (config_.normalizeOutput)
    {
        float peak = 0.0f;
        for (int ch = 0; ch < wetBuffer.getNumChannels(); ++ch)
            peak = juce::jmax(peak, wetBuffer.getMagnitude(ch, 0, totalSamples));
        if (peak > 0.0f && peak != 1.0f)
        {
            float gain = 0.99f / peak; // normalize to -0.1 dBFS
            for (int ch = 0; ch < wetBuffer.getNumChannels(); ++ch)
                wetBuffer.applyGain(ch, 0, totalSamples, gain);
        }
    }

    // ── 4. Write the wet file to disk ───────────────────────────────────────
    juce::String fileName = "variation_" + juce::String(index).paddedLeft('0', 5) + ".wav";
    auto outFile = outputDir.getChildFile(fileName);

    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wavFormat.createWriterFor(
            new juce::FileOutputStream(outFile),
            sampleRate,
            static_cast<unsigned int>(wetBuffer.getNumChannels()),
            32, // 32-bit float
            {}, 0));

    if (!writer)
    {
        DBG("OfflineBatchRenderer: Failed to create writer for " + outFile.getFullPathName());
        return false;
    }

    writer->writeFromAudioSampleBuffer(wetBuffer, 0, totalSamples);
    writer.reset(); // Flush and close

    if (index == 0)
        std::cout << "[DEBUG] Variation 0: file written\n" << std::flush;

    // ── 5. Record metadata ──────────────────────────────────────────────────
    nlohmann::json entry;
    entry["index"] = index;
    entry["outputFile"] = fileName.toStdString();

    // Store all parameter values
    nlohmann::json paramJson = nlohmann::json::object();
    for (int p = 0; p < static_cast<int>(pluginParams.size()) && p < static_cast<int>(params.size()); ++p)
    {
        if (auto* param = pluginParams[p])
        {
            paramJson[param->getName(128).toStdString()] = param->getValue();
        }
    }
    entry["parameters"] = paramJson;
    entry["samplingProfile"] = volcanoSafeMode ? "volcano3_safe_subset" : "full_parameter_space";

    // Optional feature extraction
    if (config_.extractFeatures)
    {
        float peakDb = juce::Decibels::gainToDecibels(
            wetBuffer.getMagnitude(0, 0, totalSamples));
        float rms = 0.0f;
        for (int ch = 0; ch < wetBuffer.getNumChannels(); ++ch)
            rms += wetBuffer.getRMSLevel(ch, 0, totalSamples);
        rms /= static_cast<float>(wetBuffer.getNumChannels());

        entry["features"] = {
            {"peakDb", peakDb},
            {"rmsDb", juce::Decibels::gainToDecibels(rms)},
            {"durationSec", static_cast<double>(totalSamples) / sampleRate}
        };
    }

    manifest["samples"].push_back(entry);
    return true;
}

} // namespace morphsnap
