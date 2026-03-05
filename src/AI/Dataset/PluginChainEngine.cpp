/*
 * MorphSnap — AI/Dataset/PluginChainEngine.cpp
 * Implementation of sequential plugin chain engine for dataset generation.
 */
#include "PluginChainEngine.h"
#include <juce_dsp/juce_dsp.h>

namespace morphsnap {

// ── PluginSlot JSON Serialization ─────────────────────────────────────────────

PluginSlot PluginSlot::fromJson(const nlohmann::json& j)
{
    PluginSlot slot;

    if (j.contains("description") && j["description"].is_object())
    {
        const auto& desc = j["description"];
        slot.description.name = desc.value("name", "");
        slot.description.descriptiveName = desc.value("descriptiveName", "");
        slot.description.pluginFormatName = desc.value("pluginFormatName", "VST3");
        slot.description.category = desc.value("category", "");
        slot.description.manufacturerName = desc.value("manufacturerName", "");
        slot.description.version = desc.value("version", "");
        slot.description.fileOrIdentifier = desc.value("fileOrIdentifier", "");
        slot.description.lastFileModTime = juce::Time(desc.value("lastFileModTime", 0LL));
        slot.description.uniqueId = desc.value("uniqueId", 0);
        slot.description.isInstrument = desc.value("isInstrument", false);
        slot.description.numInputChannels = desc.value("numInputChannels", 2);
        slot.description.numOutputChannels = desc.value("numOutputChannels", 2);
    }

    if (j.contains("parameters") && j["parameters"].is_array())
    {
        for (const auto& param : j["parameters"])
        {
            slot.parameters.push_back(param.get<float>());
        }
    }

    slot.bypass = j.value("bypass", false);
    slot.settleTimeMs = j.value("settleTimeMs", 50);

    return slot;
}

nlohmann::json PluginSlot::toJson() const
{
    nlohmann::json j;

    nlohmann::json desc;
    desc["name"] = description.name.toStdString();
    desc["descriptiveName"] = description.descriptiveName.toStdString();
    desc["pluginFormatName"] = description.pluginFormatName.toStdString();
    desc["category"] = description.category.toStdString();
    desc["manufacturerName"] = description.manufacturerName.toStdString();
    desc["version"] = description.version.toStdString();
    desc["fileOrIdentifier"] = description.fileOrIdentifier.toStdString();
    desc["lastFileModTime"] = description.lastFileModTime.toMilliseconds();
    desc["uniqueId"] = description.uniqueId;
    desc["isInstrument"] = description.isInstrument;
    desc["numInputChannels"] = description.numInputChannels;
    desc["numOutputChannels"] = description.numOutputChannels;
    j["description"] = desc;

    j["parameters"] = parameters;
    j["bypass"] = bypass;
    j["settleTimeMs"] = settleTimeMs;

    return j;
}

// ── ChainConfig JSON Serialization ────────────────────────────────────────────

ChainConfig ChainConfig::fromJson(const nlohmann::json& j)
{
    ChainConfig config;

    // Parse chain type
    std::string typeStr = j.value("type", "Custom");
    if (typeStr == "EQOnly") config.type = ChainType::EQOnly;
    else if (typeStr == "DynamicsOnly") config.type = ChainType::DynamicsOnly;
    else if (typeStr == "Mastering") config.type = ChainType::Mastering;
    else if (typeStr == "Mixing") config.type = ChainType::Mixing;
    else if (typeStr == "Creative") config.type = ChainType::Creative;
    else config.type = ChainType::Custom;

    config.sampleRate = j.value("sampleRate", 48000.0);
    config.blockSize = j.value("blockSize", 512);
    config.numChannels = j.value("numChannels", 2);

    // Parse plugins array
    if (j.contains("plugins") && j["plugins"].is_array())
    {
        for (const auto& pluginJson : j["plugins"])
        {
            config.plugins.add(PluginSlot::fromJson(pluginJson));
        }
    }

    return config;
}

nlohmann::json ChainConfig::toJson() const
{
    nlohmann::json j;

    // Serialize type
    std::string typeStr;
    switch (type)
    {
        case ChainType::EQOnly: typeStr = "EQOnly"; break;
        case ChainType::DynamicsOnly: typeStr = "DynamicsOnly"; break;
        case ChainType::Mastering: typeStr = "Mastering"; break;
        case ChainType::Mixing: typeStr = "Mixing"; break;
        case ChainType::Creative: typeStr = "Creative"; break;
        case ChainType::Custom: typeStr = "Custom"; break;
    }
    j["type"] = typeStr;
    j["sampleRate"] = sampleRate;
    j["blockSize"] = blockSize;
    j["numChannels"] = numChannels;

    // Serialize plugins
    nlohmann::json pluginsArray = nlohmann::json::array();
    for (const auto& plugin : plugins)
    {
        pluginsArray.push_back(plugin.toJson());
    }
    j["plugins"] = pluginsArray;

    return j;
}

ChainConfig ChainConfig::createMasteringChain()
{
    ChainConfig config;
    config.type = ChainType::Mastering;
    config.sampleRate = 48000.0;
    config.blockSize = 512;
    config.numChannels = 2;

    // Note: These are placeholder descriptions. In practice, users would
    // populate with actual plugin identifiers from their system.
    // This serves as a template structure.

    // EQ slot
    PluginSlot eqSlot;
    eqSlot.description.name = "Mastering EQ";
    eqSlot.description.pluginFormatName = "VST3";
    eqSlot.description.category = "EQ";
    eqSlot.settleTimeMs = 50;
    config.plugins.add(eqSlot);

    // Compressor slot
    PluginSlot compSlot;
    compSlot.description.name = "Mastering Compressor";
    compSlot.description.pluginFormatName = "VST3";
    compSlot.description.category = "Dynamics";
    compSlot.settleTimeMs = 100;  // Dynamics need more settle time
    config.plugins.add(compSlot);

    // Limiter slot
    PluginSlot limiterSlot;
    limiterSlot.description.name = "Mastering Limiter";
    limiterSlot.description.pluginFormatName = "VST3";
    limiterSlot.description.category = "Dynamics";
    limiterSlot.settleTimeMs = 50;
    config.plugins.add(limiterSlot);

    return config;
}

ChainConfig ChainConfig::createEQChain()
{
    ChainConfig config;
    config.type = ChainType::EQOnly;
    config.sampleRate = 48000.0;
    config.blockSize = 512;
    config.numChannels = 2;

    PluginSlot eqSlot;
    eqSlot.description.name = "Parametric EQ";
    eqSlot.description.pluginFormatName = "VST3";
    eqSlot.description.category = "EQ";
    eqSlot.settleTimeMs = 50;
    config.plugins.add(eqSlot);

    return config;
}

ChainConfig ChainConfig::createDynamicsChain()
{
    ChainConfig config;
    config.type = ChainType::DynamicsOnly;
    config.sampleRate = 48000.0;
    config.blockSize = 512;
    config.numChannels = 2;

    PluginSlot compSlot;
    compSlot.description.name = "Compressor";
    compSlot.description.pluginFormatName = "VST3";
    compSlot.description.category = "Dynamics";
    compSlot.settleTimeMs = 100;
    config.plugins.add(compSlot);

    return config;
}

// ── PluginChainEngine Implementation ──────────────────────────────────────────

PluginChainEngine::PluginChainEngine()
    : formatManager_()
{
    formatManager_.addDefaultFormats();
}

PluginChainEngine::~PluginChainEngine()
{
    unloadChain();
}

bool PluginChainEngine::loadChain(const ChainConfig& config)
{
    unloadChain();

    currentConfig_ = config;

    // Prepare before loading if we have valid settings
    if (config.sampleRate > 0 && config.blockSize > 0)
    {
        prepare(config.sampleRate, config.blockSize, config.numChannels);
    }

    // Load each plugin in the chain
    bool allLoaded = true;
    for (const auto& slot : config.plugins)
    {
        if (!loadSinglePlugin(slot))
        {
            allLoaded = false;
            // Continue loading other plugins even if one fails
        }
    }

    // Build parameter index map after all plugins are loaded
    buildParameterIndexMap();

    return allLoaded && !plugins_.isEmpty();
}

bool PluginChainEngine::loadSinglePlugin(const PluginSlot& slot)
{
    if (slot.description.fileOrIdentifier.isEmpty())
    {
        return false;
    }

    // Create plugin instance synchronously
    std::unique_ptr<juce::AudioPluginInstance> plugin;

    // Find the appropriate format by iterating registered formats
    juce::AudioPluginFormat* format = nullptr;
    for (int i = 0; i < formatManager_.getNumFormats(); ++i)
    {
        auto* f = formatManager_.getFormat(i);
        if (f != nullptr && f->getName() == slot.description.pluginFormatName)
        {
            format = f;
            break;
        }
    }
    if (format == nullptr)
    {
        // Try to find by file extension or identifier
        for (int i = 0; i < formatManager_.getNumFormats(); ++i)
        {
            auto* f = formatManager_.getFormat(i);
            if (f != nullptr && f->canScanForPlugins())
            {
                auto types = f->searchPathsForPlugins(juce::FileSearchPath(), false);
                for (const auto& type : types)
                {
                    if (type == slot.description.fileOrIdentifier)
                    {
                        format = f;
                        break;
                    }
                }
            }
            if (format != nullptr) break;
        }
    }

    if (format == nullptr)
    {
        return false;
    }

    // Load the plugin
    juce::String errorMessage;
    plugin = format->createInstanceFromDescription(
        slot.description,
        currentSampleRate_,
        currentBlockSize_,
        errorMessage
    );

    if (plugin == nullptr)
    {
        return false;
    }

    // Prepare the plugin if we're already prepared
    if (isPrepared_)
    {
        plugin->prepareToPlay(currentSampleRate_, currentBlockSize_);
    }

    // Apply initial parameters if provided
    if (!slot.parameters.empty())
    {
        const int numParams = plugin->getNumParameters();
        const int paramsToSet = juce::jmin(static_cast<int>(slot.parameters.size()), numParams);

        for (int i = 0; i < paramsToSet; ++i)
        {
            plugin->setParameter(i, slot.parameters[i]);
        }
    }

    // Store plugin and metadata
    plugins_.add(plugin.release());
    bypassStates_.add(slot.bypass);
    pluginSettleTimes_.add(slot.settleTimeMs);

    return true;
}

void PluginChainEngine::unloadChain()
{
    releaseResources();
    plugins_.clear();
    bypassStates_.clear();
    pluginSettleTimes_.clear();
    parameterIndexMap_.clear();
    currentConfig_ = ChainConfig();
}

void PluginChainEngine::prepare(double sampleRate, int blockSize, int numChannels)
{
    currentSampleRate_ = sampleRate;
    currentBlockSize_ = blockSize;
    currentNumChannels_ = numChannels;

    for (auto* plugin : plugins_)
    {
        if (plugin != nullptr)
        {
            plugin->prepareToPlay(sampleRate, blockSize);
        }
    }

    isPrepared_ = true;
}

void PluginChainEngine::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // Process through each plugin in the chain
    for (int i = 0; i < plugins_.size(); ++i)
    {
        auto* plugin = plugins_[i];

        if (plugin == nullptr || bypassStates_[i])
        {
            continue;
        }

        try
        {
            plugin->processBlock(buffer, midi);
        }
        catch (...)
        {
            // Handle plugin exceptions gracefully
            // Continue processing with remaining plugins
        }
    }
}

void PluginChainEngine::releaseResources()
{
    for (auto* plugin : plugins_)
    {
        if (plugin != nullptr)
        {
            plugin->releaseResources();
        }
    }

    isPrepared_ = false;
}

int PluginChainEngine::getTotalParameterCount() const
{
    return static_cast<int>(parameterIndexMap_.size());
}

std::vector<ParameterMapping> PluginChainEngine::getAllParameters() const
{
    std::vector<ParameterMapping> mappings;
    mappings.reserve(parameterIndexMap_.size());

    for (size_t globalIdx = 0; globalIdx < parameterIndexMap_.size(); ++globalIdx)
    {
        const auto& [pluginIdx, paramIdx] = parameterIndexMap_[globalIdx];

        ParameterMapping mapping;
        mapping.pluginIndex = pluginIdx;
        mapping.parameterIndex = paramIdx;

        if (pluginIdx >= 0 && pluginIdx < plugins_.size())
        {
            auto* plugin = plugins_[pluginIdx];
            if (plugin != nullptr && paramIdx >= 0 && paramIdx < plugin->getNumParameters())
            {
                mapping.parameterName = plugin->getParameterName(paramIdx);
                mapping.normalizedValue = plugin->getParameter(paramIdx);
                mapping.rawValue = mapping.normalizedValue;  // JUCE uses normalized internally
                mapping.defaultValue = plugin->getParameterDefaultValue(paramIdx);

                // Check for discrete/boolean parameters
                auto* p = plugin->getParameters()[paramIdx];
                if (p != nullptr && p->getNumSteps() > 0 && p->getNumSteps() != 0x7fffffff)
                {
                    mapping.isDiscrete = true;
                }
            }
        }

        mappings.push_back(mapping);
    }

    return mappings;
}

void PluginChainEngine::setParameterNormalized(int globalIndex, float value)
{
    if (globalIndex < 0 || globalIndex >= static_cast<int>(parameterIndexMap_.size()))
    {
        return;
    }

    const auto& [pluginIdx, paramIdx] = parameterIndexMap_[globalIndex];

    if (pluginIdx >= 0 && pluginIdx < plugins_.size())
    {
        auto* plugin = plugins_[pluginIdx];
        if (plugin != nullptr && paramIdx >= 0 && paramIdx < plugin->getNumParameters())
        {
            plugin->setParameter(paramIdx, juce::jlimit(0.0f, 1.0f, value));
        }
    }
}

float PluginChainEngine::getParameterNormalized(int globalIndex) const
{
    if (globalIndex < 0 || globalIndex >= static_cast<int>(parameterIndexMap_.size()))
    {
        return 0.0f;
    }

    const auto& [pluginIdx, paramIdx] = parameterIndexMap_[globalIndex];

    if (pluginIdx >= 0 && pluginIdx < plugins_.size())
    {
        auto* plugin = plugins_[pluginIdx];
        if (plugin != nullptr && paramIdx >= 0 && paramIdx < plugin->getNumParameters())
        {
            return plugin->getParameter(paramIdx);
        }
    }

    return 0.0f;
}

void PluginChainEngine::applyParameterSet(const std::vector<float>& normalizedValues)
{
    const int count = juce::jmin(static_cast<int>(normalizedValues.size()),
                                  static_cast<int>(parameterIndexMap_.size()));

    for (int i = 0; i < count; ++i)
    {
        setParameterNormalized(i, normalizedValues[i]);
    }
}

std::vector<float> PluginChainEngine::captureParameterSet() const
{
    std::vector<float> values;
    values.reserve(parameterIndexMap_.size());

    for (size_t i = 0; i < parameterIndexMap_.size(); ++i)
    {
        values.push_back(getParameterNormalized(static_cast<int>(i)));
    }

    return values;
}

void PluginChainEngine::setPluginParameters(int pluginIndex, const std::vector<float>& normalizedValues)
{
    if (pluginIndex < 0 || pluginIndex >= plugins_.size())
    {
        return;
    }

    auto* plugin = plugins_[pluginIndex];
    if (plugin == nullptr)
    {
        return;
    }

    const int numParams = plugin->getNumParameters();
    const int count = juce::jmin(static_cast<int>(normalizedValues.size()), numParams);

    for (int i = 0; i < count; ++i)
    {
        plugin->setParameter(i, juce::jlimit(0.0f, 1.0f, normalizedValues[i]));
    }
}

std::vector<float> PluginChainEngine::getPluginParameters(int pluginIndex) const
{
    std::vector<float> values;

    if (pluginIndex < 0 || pluginIndex >= plugins_.size())
    {
        return values;
    }

    auto* plugin = plugins_[pluginIndex];
    if (plugin == nullptr)
    {
        return values;
    }

    const int numParams = plugin->getNumParameters();
    values.reserve(numParams);

    for (int i = 0; i < numParams; ++i)
    {
        values.push_back(plugin->getParameter(i));
    }

    return values;
}

void PluginChainEngine::waitForSettle()
{
    waitForSettle(defaultSettleTimeMs_);
}

void PluginChainEngine::waitForSettle(int settleTimeMs)
{
    if (settleTimeMs <= 0 || !isPrepared_)
    {
        return;
    }

    // Calculate number of samples to process for settle time
    const int samplesToProcess = static_cast<int>((settleTimeMs / 1000.0) * currentSampleRate_);

    // Process silence through the chain
    processSilence(samplesToProcess);
}

void PluginChainEngine::processSilence(int numSamples)
{
    if (numSamples <= 0 || !isPrepared_)
    {
        return;
    }

    // Create a silence buffer
    juce::AudioBuffer<float> silenceBuffer(currentNumChannels_, numSamples);
    silenceBuffer.clear();

    juce::MidiBuffer emptyMidi;

    // Process in blocks
    int samplesProcessed = 0;
    while (samplesProcessed < numSamples)
    {
        const int blockSamples = juce::jmin(currentBlockSize_, numSamples - samplesProcessed);

        juce::AudioBuffer<float> blockBuffer(
            silenceBuffer.getArrayOfWritePointers(),
            currentNumChannels_,
            samplesProcessed,
            blockSamples
        );

        processBlock(blockBuffer, emptyMidi);
        samplesProcessed += blockSamples;
    }
}

juce::AudioPluginInstance* PluginChainEngine::getPlugin(int index)
{
    if (index < 0 || index >= plugins_.size())
    {
        return nullptr;
    }
    return plugins_[index];
}

const juce::AudioPluginInstance* PluginChainEngine::getPlugin(int index) const
{
    if (index < 0 || index >= plugins_.size())
    {
        return nullptr;
    }
    return plugins_[index];
}

void PluginChainEngine::setPluginBypass(int pluginIndex, bool bypass)
{
    if (pluginIndex >= 0 && pluginIndex < bypassStates_.size())
    {
        bypassStates_.set(pluginIndex, bypass);
    }
}

bool PluginChainEngine::getPluginBypass(int pluginIndex) const
{
    if (pluginIndex >= 0 && pluginIndex < bypassStates_.size())
    {
        return bypassStates_[pluginIndex];
    }
    return false;
}

void PluginChainEngine::setAllBypass(bool bypass)
{
    for (int i = 0; i < bypassStates_.size(); ++i)
    {
        bypassStates_.set(i, bypass);
    }
}

void PluginChainEngine::buildParameterIndexMap()
{
    parameterIndexMap_.clear();

    for (int pluginIdx = 0; pluginIdx < plugins_.size(); ++pluginIdx)
    {
        auto* plugin = plugins_[pluginIdx];
        if (plugin == nullptr)
        {
            continue;
        }

        const int numParams = plugin->getNumParameters();
        for (int paramIdx = 0; paramIdx < numParams; ++paramIdx)
        {
            parameterIndexMap_.emplace_back(pluginIdx, paramIdx);
        }
    }
}

} // namespace morphsnap
