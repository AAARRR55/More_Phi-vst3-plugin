/*
 * MorphSnap — AI/Dataset/DatasetGeneratorV2.cpp
 * Implementation of the comprehensive synthetic audio dataset generator.
 */
#include "DatasetGeneratorV2.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <chrono>
#include <thread>
#include <mutex>

namespace morphsnap {

//==============================================================================
// DatasetGeneratorConfig Implementation
//==============================================================================

DatasetGeneratorConfig DatasetGeneratorConfig::fromJson(const nlohmann::json& j)
{
    DatasetGeneratorConfig config;

    if (j.contains("outputDirectory"))
        config.outputDirectory = juce::File(j["outputDirectory"].get<std::string>());

    if (j.contains("datasetName"))
        config.datasetName = juce::String(j["datasetName"].get<std::string>());

    if (j.contains("totalSamples"))
        config.totalSamples = j["totalSamples"].get<int>();

    if (j.contains("randomSeed"))
        config.randomSeed = j["randomSeed"].get<unsigned int>();

    if (j.contains("sampleRate"))
        config.sampleRate = j["sampleRate"].get<double>();

    if (j.contains("blockSize"))
        config.blockSize = j["blockSize"].get<int>();

    if (j.contains("numChannels"))
        config.numChannels = j["numChannels"].get<int>();

    if (j.contains("fullDuration"))
        config.fullDuration = j["fullDuration"].get<float>();

    if (j.contains("transientDuration"))
        config.transientDuration = j["transientDuration"].get<float>();

    if (j.contains("steadyStateDuration"))
        config.steadyStateDuration = j["steadyStateDuration"].get<float>();

    if (j.contains("outputFormat"))
    {
        auto fmt = j["outputFormat"].get<std::string>();
        if (fmt == "WAV32Float") config.outputFormat = OutputFormat::WAV32Float;
        else if (fmt == "WAV24") config.outputFormat = OutputFormat::WAV24;
        else if (fmt == "FLAC24") config.outputFormat = OutputFormat::FLAC24;
    }

    if (j.contains("chainType"))
    {
        auto type = j["chainType"].get<std::string>();
        if (type == "EQOnly") config.chainType = ChainType::EQOnly;
        else if (type == "DynamicsOnly") config.chainType = ChainType::DynamicsOnly;
        else if (type == "Mastering") config.chainType = ChainType::Mastering;
        else if (type == "Mixing") config.chainType = ChainType::Mixing;
        else if (type == "Creative") config.chainType = ChainType::Creative;
        else if (type == "Custom") config.chainType = ChainType::Custom;
    }

    if (j.contains("chainConfigFile"))
        config.chainConfigFile = juce::File(j["chainConfigFile"].get<std::string>());

    if (j.contains("sourceAudioDirectory"))
        config.sourceAudioDirectory = juce::File(j["sourceAudioDirectory"].get<std::string>());

    if (j.contains("useAugmentation"))
        config.useAugmentation = j["useAugmentation"].get<bool>();

    if (j.contains("numParallelThreads"))
        config.numParallelThreads = j["numParallelThreads"].get<int>();

    if (j.contains("pluginSettleTimeMs"))
        config.pluginSettleTimeMs = j["pluginSettleTimeMs"].get<int>();

    if (j.contains("dryRun"))
        config.dryRun = j["dryRun"].get<bool>();

    if (j.contains("enableValidation"))
        config.enableValidation = j["enableValidation"].get<bool>();

    if (j.contains("targetVolumeCoverage"))
        config.targetVolumeCoverage = j["targetVolumeCoverage"].get<float>();

    if (j.contains("targetGridCoverage"))
        config.targetGridCoverage = j["targetGridCoverage"].get<float>();

    if (j.contains("maxPerformanceGap"))
        config.maxPerformanceGap = j["maxPerformanceGap"].get<float>();

    // Sampling config
    if (j.contains("samplingConfig"))
    {
        auto& sc = j["samplingConfig"];
        if (sc.contains("sampleCount")) config.samplingConfig.sampleCount = sc["sampleCount"].get<int>();
        if (sc.contains("seed")) config.samplingConfig.seed = sc["seed"].get<unsigned int>();
    }

    // Split config
    if (j.contains("splitConfig"))
    {
        auto& sp = j["splitConfig"];
        if (sp.contains("trainRatio")) config.splitConfig.trainRatio = sp["trainRatio"].get<float>();
        if (sp.contains("valRatio")) config.splitConfig.valRatio = sp["valRatio"].get<float>();
        if (sp.contains("testRatio")) config.splitConfig.testRatio = sp["testRatio"].get<float>();
        if (sp.contains("stratifyByGenre")) config.splitConfig.stratifyByGenre = sp["stratifyByGenre"].get<bool>();
        if (sp.contains("stratifyByIntensity")) config.splitConfig.stratifyByIntensity = sp["stratifyByIntensity"].get<bool>();
        if (sp.contains("randomSeed")) config.splitConfig.randomSeed = sp["randomSeed"].get<unsigned int>();
    }

    return config;
}

nlohmann::json DatasetGeneratorConfig::toJson() const
{
    nlohmann::json j;

    j["outputDirectory"] = outputDirectory.getFullPathName().toStdString();
    j["datasetName"] = datasetName.toStdString();
    j["totalSamples"] = totalSamples;
    j["randomSeed"] = randomSeed;
    j["sampleRate"] = sampleRate;
    j["blockSize"] = blockSize;
    j["numChannels"] = numChannels;
    j["fullDuration"] = fullDuration;
    j["transientDuration"] = transientDuration;
    j["steadyStateDuration"] = steadyStateDuration;

    switch (outputFormat)
    {
        case OutputFormat::WAV32Float: j["outputFormat"] = "WAV32Float"; break;
        case OutputFormat::WAV24: j["outputFormat"] = "WAV24"; break;
        case OutputFormat::FLAC24: j["outputFormat"] = "FLAC24"; break;
    }

    switch (chainType)
    {
        case ChainType::EQOnly: j["chainType"] = "EQOnly"; break;
        case ChainType::DynamicsOnly: j["chainType"] = "DynamicsOnly"; break;
        case ChainType::Mastering: j["chainType"] = "Mastering"; break;
        case ChainType::Mixing: j["chainType"] = "Mixing"; break;
        case ChainType::Creative: j["chainType"] = "Creative"; break;
        case ChainType::Custom: j["chainType"] = "Custom"; break;
    }

    if (chainConfigFile.exists())
        j["chainConfigFile"] = chainConfigFile.getFullPathName().toStdString();

    if (sourceAudioDirectory.exists())
        j["sourceAudioDirectory"] = sourceAudioDirectory.getFullPathName().toStdString();

    j["useAugmentation"] = useAugmentation;
    j["numParallelThreads"] = numParallelThreads;
    j["pluginSettleTimeMs"] = pluginSettleTimeMs;
    j["dryRun"] = dryRun;
    j["enableValidation"] = enableValidation;
    j["targetVolumeCoverage"] = targetVolumeCoverage;
    j["targetGridCoverage"] = targetGridCoverage;
    j["maxPerformanceGap"] = maxPerformanceGap;

    // Sampling config
    j["samplingConfig"] = {
        {"sampleCount", samplingConfig.sampleCount},
        {"seed", samplingConfig.seed}
    };

    // Split config
    j["splitConfig"] = {
        {"trainRatio", splitConfig.trainRatio},
        {"valRatio", splitConfig.valRatio},
        {"testRatio", splitConfig.testRatio},
        {"stratifyByGenre", splitConfig.stratifyByGenre},
        {"stratifyByIntensity", splitConfig.stratifyByIntensity},
        {"randomSeed", splitConfig.randomSeed}
    };

    return j;
}

DatasetGeneratorConfig DatasetGeneratorConfig::fromFile(const juce::File& file)
{
    if (!file.existsAsFile())
        return {};

    std::ifstream ifs(file.getFullPathName().toStdString());
    nlohmann::json j;
    ifs >> j;

    return fromJson(j);
}

bool DatasetGeneratorConfig::toFile(const juce::File& file) const
{
    auto json = toJson();

    std::ofstream ofs(file.getFullPathName().toStdString());
    ofs << json.dump(4);

    return ofs.good();
}

//==============================================================================
// DatasetGeneratorV2 Implementation
//==============================================================================

DatasetGeneratorV2::DatasetGeneratorV2()
    : organizer_(nullptr)
{
}

DatasetGeneratorV2::~DatasetGeneratorV2()
{
    stopGeneration();
}

void DatasetGeneratorV2::setConfig(const DatasetGeneratorConfig& config)
{
    config_ = config;
    sampler_.setSeed(config.randomSeed);
}

bool DatasetGeneratorV2::validateConfig(const DatasetGeneratorConfig& config, juce::String& outError)
{
    if (config.outputDirectory == juce::File())
    {
        outError = "Output directory not specified";
        return false;
    }

    if (config.totalSamples <= 0)
    {
        outError = "Total samples must be positive";
        return false;
    }

    if (config.sampleRate <= 0)
    {
        outError = "Sample rate must be positive";
        return false;
    }

    if (config.blockSize <= 0)
    {
        outError = "Block size must be positive";
        return false;
    }

    if (config.numChannels <= 0 || config.numChannels > 32)
    {
        outError = "Number of channels must be between 1 and 32";
        return false;
    }

    if (config.splitConfig.trainRatio + config.splitConfig.valRatio + config.splitConfig.testRatio != 1.0f)
    {
        outError = "Split ratios must sum to 1.0";
        return false;
    }

    return true;
}

bool DatasetGeneratorV2::startGeneration()
{
    if (isGenerating_.load())
        return false;

    juce::String error;
    if (!validateConfig(config_, error))
    {
        if (onComplete)
        {
            GenerationResult result;
            result.success = false;
            result.errors.add(error);
            onComplete(result);
        }
        return false;
    }

    isGenerating_ = true;
    shouldStop_ = false;
    samplesCompleted_ = 0;
    startTimeMs_ = juce::Time::getMillisecondCounter();

    // Create organizer
    organizer_ = std::make_unique<DatasetOrganizer>(config_.outputDirectory.getChildFile(config_.datasetName));

    // Initialize directory structure
    if (!organizer_->initializeStructure())
    {
        isGenerating_ = false;
        if (onComplete)
        {
            GenerationResult result;
            result.success = false;
            result.errors.add("Failed to initialize directory structure");
            onComplete(result);
        }
        return false;
    }

    // Start generation thread
    generationThread_ = new std::thread([this]() {
        if (!initializeModules())
        {
            finalizeGeneration();
            return;
        }

        if (!loadSourceAudio())
        {
            finalizeGeneration();
            return;
        }

        if (!loadPluginChain())
        {
            finalizeGeneration();
            return;
        }

        // Generate parameter samples
        auto parameterSets = sampler_.generateLHS(config_.samplingConfig,
                                                   chainEngine_.getTotalParameterCount());

        // Main generation loop
        for (int i = 0; i < config_.totalSamples && !shouldStop_.load(); ++i)
        {
            if (config_.dryRun)
            {
                // Dry run mode - just validate without rendering
                samplesCompleted_++;
                continue;
            }

            // Get random source audio
            auto source = audioLibrary_.getRandomSourceByDistribution();

            // Generate sample
            processSample(i, parameterSets[i % parameterSets.size()]);

            samplesCompleted_++;

            // Report progress
            if (onProgress)
            {
                GenerationProgress progress;
                progress.samplesCompleted = samplesCompleted_.load();
                progress.totalSamples = config_.totalSamples;
                progress.percentage = static_cast<float>(progress.samplesCompleted) / progress.totalSamples * 100.0f;
                progress.currentPhase = "Generating samples";
                progress.currentSample = juce::String(i);

                auto elapsed = juce::Time::getMillisecondCounter() - startTimeMs_;
                progress.elapsedMs = elapsed;
                if (progress.samplesCompleted > 0)
                {
                    progress.estimatedRemainingMs = (elapsed / progress.samplesCompleted) * (progress.totalSamples - progress.samplesCompleted);
                    progress.samplesPerHour = static_cast<int>(progress.samplesCompleted * 3600000.0 / elapsed);
                }

                onProgress(progress);
            }
        }

        finalizeGeneration();
    });

    if (generationThread_)
        generationThread_->detach();

    return true;
}

void DatasetGeneratorV2::stopGeneration()
{
    shouldStop_ = true;

    if (generationThread_)
    {
        if (generationThread_->joinable())
            generationThread_->join();
        delete generationThread_;
        generationThread_ = nullptr;
    }

    isGenerating_ = false;
}

GenerationProgress DatasetGeneratorV2::getProgress() const
{
    GenerationProgress progress;
    progress.samplesCompleted = samplesCompleted_.load();
    progress.totalSamples = config_.totalSamples;
    progress.percentage = static_cast<float>(progress.samplesCompleted) / progress.totalSamples * 100.0f;

    auto elapsed = juce::Time::getMillisecondCounter() - startTimeMs_;
    progress.elapsedMs = elapsed;

    if (progress.samplesCompleted > 0)
    {
        progress.estimatedRemainingMs = (elapsed / progress.samplesCompleted) * (progress.totalSamples - progress.samplesCompleted);
        progress.samplesPerHour = static_cast<int>(progress.samplesCompleted * 3600000.0 / elapsed);
    }

    return progress;
}

bool DatasetGeneratorV2::initializeModules()
{
    // Prepare feature extractor
    featureExtractor_ = FeatureExtractor();

    return true;
}

bool DatasetGeneratorV2::loadSourceAudio()
{
    if (!config_.sourceAudioDirectory.exists())
    {
        // Create test signals if no source directory
        return true;
    }

    return audioLibrary_.scanDirectory(config_.sourceAudioDirectory);
}

bool DatasetGeneratorV2::loadPluginChain()
{
    ChainConfig chainConfig;

    if (config_.chainType == ChainType::Custom && config_.chainConfigFile.exists())
    {
        // Load custom chain config
        std::ifstream ifs(config_.chainConfigFile.getFullPathName().toStdString());
        nlohmann::json j;
        ifs >> j;
        chainConfig = ChainConfig::fromJson(j);
    }
    else
    {
        // Use preset chain
        switch (config_.chainType)
        {
            case ChainType::Mastering:
                chainConfig = PluginChainEngine::createMasteringChain();
                break;
            case ChainType::EQOnly:
                chainConfig = PluginChainEngine::createEQChain();
                break;
            case ChainType::DynamicsOnly:
                chainConfig = PluginChainEngine::createDynamicsChain();
                break;
            default:
                chainConfig = PluginChainEngine::createMasteringChain();
                break;
        }
    }

    chainConfig.sampleRate = config_.sampleRate;
    chainConfig.blockSize = config_.blockSize;

    return chainEngine_.loadChain(chainConfig);
}

void DatasetGeneratorV2::processSample(int sampleIndex, const std::vector<float>& parameters)
{
    // Get source audio
    auto source = audioLibrary_.getRandomSourceByDistribution();

    // Generate sample
    auto metadata = generateSample(sampleIndex, source, parameters);

    // Determine split
    organizer_->performSplit(config_.splitConfig);
    auto split = organizer_->getSplitForSample(metadata.sampleId);
    metadata.split = split;

    // Save to organizer
    juce::File audioFile = organizer_->getAudioDirectory(split, AudioContentLibrary::genreToString(source.genre))
                           .getChildFile(metadata.sampleId + ".wav");

    organizer_->addSample(metadata.sampleId, audioFile, metadataWriter_.metadataToJson(metadata), split);
}

DatasetMetadata DatasetGeneratorV2::generateSample(
    int sampleIndex,
    const SourceAudio& source,
    const std::vector<float>& parameters)
{
    DatasetMetadata metadata;
    metadata.sampleId = organizer_->generateSampleId();
    metadata.timestamp = juce::Time::currentTimeMillis();

    // Source provenance
    metadata.source.filePath = source.file.getFullPathName();
    metadata.source.genre = AudioContentLibrary::genreToString(source.genre);
    metadata.source.originalLufs = source.characteristics.lufs;
    metadata.source.dynamicRangeDb = source.characteristics.dynamicRangeDb;
    metadata.source.sampleRate = source.sampleRate;
    metadata.source.numChannels = source.numChannels;

    // Apply parameters to plugin chain
    chainEngine_.applyParameterSet(parameters);
    chainEngine_.waitForSettle();

    // Render audio (simplified - actual implementation would read source file)
    int numSamples = static_cast<int>(config_.fullDuration * config_.sampleRate);
    juce::AudioBuffer<float> buffer(config_.numChannels, numSamples);

    // Fill with source audio (placeholder)
    // In real implementation, would load and process source audio file

    // Process through chain
    juce::MidiBuffer midi;
    int samplesProcessed = 0;
    while (samplesProcessed < numSamples)
    {
        int toProcess = std::min(config_.blockSize, numSamples - samplesProcessed);
        juce::AudioBuffer<float> subBuffer(buffer.getArrayOfWritePointers(),
                                           config_.numChannels,
                                           samplesProcessed,
                                           toProcess);
        chainEngine_.processBlock(subBuffer, midi);
        samplesProcessed += toProcess;
    }

    // Extract features
    auto features = featureExtractor_.extract(buffer, config_.extractionConfig);
    metadata.spectralFeatures = featureExtractor_.toJson(features).value("spectral", nlohmann::json::object());
    metadata.temporalFeatures = featureExtractor_.toJson(features).value("temporal", nlohmann::json::object());
    metadata.perceptualFeatures = featureExtractor_.toJson(features).value("perceptual", nlohmann::json::object());

    // Output characteristics
    metadata.output.lufs = features.perceptual.lufs;
    metadata.output.truePeakDb = features.perceptual.truePeakDb;
    metadata.output.dynamicRangeDb = features.perceptual.dynamicRange;
    metadata.output.spectralCentroidHz = features.spectral.spectralCentroid;
    metadata.output.numSamples = numSamples;
    metadata.output.durationSeconds = config_.fullDuration;

    // ML targets
    metadata.targets.parameterRegression = parameters;
    metadata.targets.styleClassification = AudioContentLibrary::genreToString(source.genre);
    metadata.targets.processingIntensity = 0.5f; // Placeholder
    metadata.targets.featureVector = featureExtractor_.toVector(features);

    return metadata;
}

void DatasetGeneratorV2::finalizeGeneration()
{
    if (!shouldStop_.load())
    {
        // Update manifests
        organizer_->updateManifests();

        // Run validation if enabled
        if (config_.enableValidation)
        {
            // Generate validation report
            // (simplified - would load samples and run full validation)
        }
    }

    isGenerating_ = false;

    // Report completion
    if (onComplete)
    {
        GenerationResult result;
        result.success = !shouldStop_.load();
        result.samplesGenerated = samplesCompleted_.load();
        result.stats = organizer_->computeStats();
        result.totalTimeMs = juce::Time::getMillisecondCounter() - startTimeMs_;

        if (result.totalTimeMs > 0)
        {
            result.samplesPerHour = static_cast<int>(result.samplesGenerated * 3600000.0 / result.totalTimeMs);
            result.averageSampleTimeMs = static_cast<float>(result.totalTimeMs / result.samplesGenerated);
        }

        result.datasetDirectory = organizer_->getRootDirectory();
        result.manifestFile = organizer_->getMetadataDirectory().getChildFile("manifest.json");

        onComplete(result);
    }

    // Notify change broadcasters
    sendChangeMessage();
}

bool DatasetGeneratorV2::saveCheckpoint(const juce::File& checkpointFile)
{
    checkpointData_ = config_.toJson();
    checkpointData_["samplesCompleted"] = samplesCompleted_.load();

    std::ofstream ofs(checkpointFile.getFullPathName().toStdString());
    ofs << checkpointData_.dump(4);

    hasCheckpoint_ = ofs.good();
    return hasCheckpoint_;
}

bool DatasetGeneratorV2::loadCheckpoint(const juce::File& checkpointFile)
{
    if (!checkpointFile.existsAsFile())
        return false;

    std::ifstream ifs(checkpointFile.getFullPathName().toStdString());
    ifs >> checkpointData_;

    config_ = DatasetGeneratorConfig::fromJson(checkpointData_);
    samplesCompleted_ = checkpointData_.value("samplesCompleted", 0);

    hasCheckpoint_ = true;
    return true;
}

} // namespace morphsnap
