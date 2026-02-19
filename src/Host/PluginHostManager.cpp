/*
 * MorphSnap — Host/PluginHostManager.cpp
 * Robust plugin hosting with exception handling for stability.
 *
 * Stability: exceptionCount_ tracks consecutive processBlock failures.
 * When it reaches MAX_PLUGIN_EXCEPTIONS the plugin is auto-unloaded to
 * prevent a misbehaving guest from continuously disrupting real-time audio.
 */
#include "PluginHostManager.h"
#include <exception>

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
    {
        try
        {
            hostedPlugin->prepareToPlay(sampleRate, blockSize);
        }
        catch (const std::exception& e)
        {
            juce::ignoreUnused(e);
            DBG("Exception in hosted plugin prepareToPlay: " + juce::String(e.what()));
            // Keep plugin loaded, will attempt recovery on next process
        }
        catch (...)
        {
            DBG("Unknown exception in hosted plugin prepareToPlay");
        }
    }
}

void PluginHostManager::releaseResources()
{
    if (hostedPlugin)
    {
        try
        {
            hostedPlugin->releaseResources();
        }
        catch (...)
        {
            // Silently handle - plugin may be in bad state
            DBG("Exception during hosted plugin releaseResources");
        }
    }
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

    {
        const juce::SpinLock::ScopedLockType guard(descLock_);
        lastDescription = desc;  // protected write
    }
    
    try
    {
        hostedPlugin->prepareToPlay(currentSampleRate, currentBlockSize);
        hostedPlugin->enableAllBuses();
    }
    catch (const std::exception& e)
    {
        juce::ignoreUnused(e);
        DBG("Exception during plugin initialization: " + juce::String(e.what()));
        unloadPlugin();
        return false;
    }
    catch (...)
    {
        DBG("Unknown exception during plugin initialization");
        unloadPlugin();
        return false;
    }
    
    exceptionCount_.store(0, std::memory_order_relaxed);  // reset on successful load
    return true;
}

void PluginHostManager::unloadPlugin()
{
    if (hostedPlugin)
    {
        try
        {
            hostedPlugin->releaseResources();
        }
        catch (...)
        {
            // Silently handle during unload
        }
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

    try
    {
        hostedPlugin->processBlock(buffer, midi);
        exceptionCount_.store(0, std::memory_order_relaxed);  // reset on success
    }
    catch (const std::exception& e)
    {
        juce::ignoreUnused(e);
        DBG("Exception in hosted plugin processBlock: " + juce::String(e.what()));
        buffer.clear();  // zero output to prevent garbage audio
        const int count = exceptionCount_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count >= MAX_PLUGIN_EXCEPTIONS)
        {
            DBG("PluginHostManager: auto-unloading misbehaving plugin after "
                + juce::String(count) + " consecutive exceptions");
            unloadPlugin();
        }
    }
    catch (...)
    {
        DBG("Unknown exception in hosted plugin processBlock");
        buffer.clear();
        const int count = exceptionCount_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count >= MAX_PLUGIN_EXCEPTIONS)
        {
            DBG("PluginHostManager: auto-unloading misbehaving plugin after "
                + juce::String(count) + " consecutive unknown exceptions");
            unloadPlugin();
        }
    }
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

const juce::PluginDescription* PluginHostManager::getLastDescription() const
{
    const juce::SpinLock::ScopedLockType guard(descLock_);
    return hasPlugin() ? &lastDescription : nullptr;
}

} // namespace morphsnap
