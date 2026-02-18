/*
 * MorphSnap — Host/PluginHostManager.h
 * Manages loading and running a hosted VST3/AU plugin instance.
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace morphsnap {

class PluginHostManager
{
public:
    PluginHostManager();
    ~PluginHostManager();

    void prepare(double sampleRate, int blockSize, int numChannels);
    void releaseResources();

    // Plugin lifecycle
    bool loadPlugin(const juce::PluginDescription& desc);
    void unloadPlugin();
    bool hasPlugin() const { return hostedPlugin != nullptr; }

    // Audio processing — call from audio thread
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);

    // Access the hosted plugin instance (for GUI, parameter enumeration)
    juce::AudioPluginInstance* getPlugin() { return hostedPlugin.get(); }
    const juce::AudioPluginInstance* getPlugin() const { return hostedPlugin.get(); }

    // Plugin scanning
    juce::AudioPluginFormatManager& getFormatManager() { return formatManager; }
    juce::KnownPluginList& getKnownPlugins() { return knownPlugins; }

    // Scan standard plugin folders (call from background thread)
    void scanPluginFolders();

private:
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    std::unique_ptr<juce::AudioPluginInstance> hostedPlugin;

    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;
    int currentNumChannels = 2;
};

} // namespace morphsnap
