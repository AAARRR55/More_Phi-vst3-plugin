/*
 * MorphSnap — Host/PluginHostManager.cpp
 */
#include "PluginHostManager.h"

namespace morphsnap {

PluginHostManager::PluginHostManager()
{
    formatManager.addDefaultFormats();  // VST3 + AU
}

PluginHostManager::~PluginHostManager()
{
    unloadPlugin();
}

void PluginHostManager::prepare(double sampleRate, int blockSize, int numChannels)
{
    currentSampleRate = sampleRate;
    currentBlockSize = blockSize;
    currentNumChannels = numChannels;

    if (hostedPlugin)
        hostedPlugin->prepareToPlay(sampleRate, blockSize);
}

void PluginHostManager::releaseResources()
{
    if (hostedPlugin)
        hostedPlugin->releaseResources();
}

bool PluginHostManager::loadPlugin(const juce::PluginDescription& desc)
{
    unloadPlugin();

    juce::String errorMessage;
    hostedPlugin = formatManager.createPluginInstance(
        desc, currentSampleRate, currentBlockSize, errorMessage);

    if (!hostedPlugin)
    {
        DBG("Failed to load plugin: " + errorMessage);
        return false;
    }

    hostedPlugin->prepareToPlay(currentSampleRate, currentBlockSize);
    hostedPlugin->enableAllBuses();
    return true;
}

void PluginHostManager::unloadPlugin()
{
    if (hostedPlugin)
    {
        hostedPlugin->releaseResources();
        hostedPlugin.reset();
    }
}

void PluginHostManager::processBlock(juce::AudioBuffer<float>& buffer,
                                      juce::MidiBuffer& midi)
{
    if (!hostedPlugin) return;

    // Match channel count
    const int pluginChannels = hostedPlugin->getTotalNumOutputChannels();
    if (buffer.getNumChannels() > pluginChannels)
    {
        // Clear extra channels
        for (int ch = pluginChannels; ch < buffer.getNumChannels(); ++ch)
            buffer.clear(ch, 0, buffer.getNumSamples());
    }

    hostedPlugin->processBlock(buffer, midi);
}

void PluginHostManager::scanPluginFolders()
{
    // Scan default VST3 and AU locations
    for (auto* format : formatManager.getFormats())
    {
        auto defaultLocations = format->getDefaultLocationsToSearch();
        juce::PluginDirectoryScanner scanner(
            knownPlugins, *format, defaultLocations,
            true, juce::File(), false);

        juce::String pluginName;
        while (scanner.scanNextFile(true, pluginName))
        {
            // Progress reported via pluginName
        }
    }
}

} // namespace morphsnap
