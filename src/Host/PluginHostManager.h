/*
 * MorphSnap — Host/PluginHostManager.h
 * Manages loading and running a hosted VST3/AU plugin instance.
 * Implements IPluginHostManager for testability.
 *
 * Stability: An exception counter tracks repeated failures from a hosted
 * plugin. When it exceeds MAX_PLUGIN_EXCEPTIONS the plugin is auto-unloaded
 * to prevent a misbehaving guest from continuously disrupting real-time audio.
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "IPluginHostManager.h"
#include <atomic>

namespace morphsnap {

class PluginHostManager : public IPluginHostManager
{
public:
    PluginHostManager();
    ~PluginHostManager() override;

    // IPluginHostManager implementation
    void prepare(double sampleRate, int blockSize, int numChannels) override;
    void releaseResources() override;
    bool loadPlugin(const juce::PluginDescription& desc) override;
    void unloadPlugin() override;
    bool hasPlugin() const override { return hostedPlugin != nullptr; }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    
    juce::AudioPluginInstance* getPlugin() override { return hostedPlugin.get(); }
    const juce::AudioPluginInstance* getPlugin() const override { return hostedPlugin.get(); }
    
    const juce::PluginDescription* getLastDescription() const override;

    /** Number of processing exceptions since last successful load. */
    int getExceptionCount() const { return exceptionCount_.load(std::memory_order_relaxed); }

    juce::AudioPluginFormatManager& getFormatManager() override { return formatManager; }
    juce::KnownPluginList& getKnownPlugins() override { return knownPlugins; }
    void scanPluginFolders() override;

    /** Get the last loaded plugin description — available even after unload for recovery. */
    const juce::PluginDescription& getLastDescriptionRef() const { return lastDescription; }

private:
    // Suspend (bypass audio) a misbehaving plugin after this many consecutive
    // exceptions. Raised from 5 to tolerate short DAW reconfiguration bursts.
    static constexpr int MAX_PLUGIN_EXCEPTIONS = 20;

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    std::unique_ptr<juce::AudioPluginInstance> hostedPlugin;
    juce::PluginDescription lastDescription;
    mutable juce::SpinLock  descLock_;    // guards lastDescription

    // Counts consecutive processBlock exceptions; reset on successful load.
    // When it reaches MAX_PLUGIN_EXCEPTIONS the plugin is suspended (NOT unloaded).
    std::atomic<int> exceptionCount_{0};

    // When true, plugin is suspended (audio bypassed) but NOT destroyed.
    // Recovery is attempted automatically when processBlock succeeds.
    std::atomic<bool> suspended_{false};

    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;
    int currentNumChannels = 2;
};

} // namespace morphsnap
