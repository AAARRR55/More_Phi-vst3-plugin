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
    const bool configurationChanged = !hasPreparedConfiguration_
        || currentSampleRate != sampleRate
        || currentBlockSize != blockSize
        || currentNumChannels != numChannels;

    currentSampleRate = sampleRate;
    currentBlockSize = blockSize;
    currentNumChannels = numChannels;
    hasPreparedConfiguration_ = true;

    if (hostedPlugin && configurationChanged)
    {
        try
        {
            hostedPlugin->disableNonMainBuses();
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
    // Create the new instance BEFORE destroying the old one.
    // If creation fails, the current plugin remains untouched.
    juce::String errorMessage;
    auto newPlugin = formatManager.createPluginInstance(
        desc, currentSampleRate, currentBlockSize, errorMessage);

    if (!newPlugin)
    {
        DBG("Failed to load plugin: " + errorMessage);
        // IMPORTANT: do NOT unload the current plugin — keep it running
        return false;
    }

    try
    {
        newPlugin->disableNonMainBuses();
        newPlugin->prepareToPlay(currentSampleRate, currentBlockSize);
    }
    catch (const std::exception& e)
    {
        juce::ignoreUnused(e);
        DBG("Exception during plugin initialization: " + juce::String(e.what()));
        // newPlugin goes out of scope and is destroyed; old plugin stays.
        return false;
    }
    catch (...)
    {
        DBG("Unknown exception during plugin initialization");
        return false;
    }

    // New plugin is valid — now swap it in.
    unloadPlugin();
    hostedPlugin = std::move(newPlugin);

    {
        const juce::SpinLock::ScopedLockType guard(descLock_);
        lastDescription = desc;  // protected write
    }

    hasPreparedConfiguration_ = true;
    exceptionCount_.store(0, std::memory_order_relaxed);
    suspended_.store(false, std::memory_order_relaxed);
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

    hasPreparedConfiguration_ = false;
}

void PluginHostManager::processBlock(juce::AudioBuffer<float>& buffer,
                                      juce::MidiBuffer& midi)
{
    if (!hostedPlugin) return;

    // If suspended, pass-through silence — do NOT unload.
    if (suspended_.load(std::memory_order_relaxed))
    {
        // Periodically attempt recovery (every ~100 blocks ≈ once per second)
        const int count = exceptionCount_.load(std::memory_order_relaxed);
        if (count > 0 && (count % 100) == 0)
        {
            try
            {
                hostedPlugin->processBlock(buffer, midi);
                // If we get here, the plugin recovered!
                suspended_.store(false, std::memory_order_relaxed);
                exceptionCount_.store(0, std::memory_order_relaxed);
                DBG("PluginHostManager: plugin recovered from suspended state");
                return;
            }
            catch (...)
            {
                // Still failing — stay suspended
                exceptionCount_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        else
        {
            exceptionCount_.fetch_add(1, std::memory_order_relaxed);
        }
        return;  // Output silence (buffer unchanged = pass-through)
    }

    // Match channel count
    const int pluginChannels = hostedPlugin->getTotalNumOutputChannels();
    if (buffer.getNumChannels() > pluginChannels)
    {
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
        buffer.clear();
        const int count = exceptionCount_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count >= MAX_PLUGIN_EXCEPTIONS)
        {
            // SUSPEND instead of UNLOAD — plugin stays in memory for recovery
            DBG("PluginHostManager: suspending misbehaving plugin after "
                + juce::String(count) + " consecutive exceptions (NOT unloading)");
            suspended_.store(true, std::memory_order_relaxed);
        }
    }
    catch (...)
    {
        DBG("Unknown exception in hosted plugin processBlock");
        buffer.clear();
        const int count = exceptionCount_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count >= MAX_PLUGIN_EXCEPTIONS)
        {
            DBG("PluginHostManager: suspending misbehaving plugin after "
                + juce::String(count) + " consecutive unknown exceptions (NOT unloading)");
            suspended_.store(true, std::memory_order_relaxed);
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
    // Always return description if we ever had a plugin — enables recovery
    // and proper state persistence even after auto-suspend/unload.
    return lastDescription.name.isNotEmpty() ? &lastDescription : nullptr;
}

} // namespace morphsnap
