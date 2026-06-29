/*
 * More-Phi — Host/IPluginHostManager.h
 * Interface abstraction for plugin hosting - enables mocking and testing.
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>

namespace more_phi {

/**
 * Interface for plugin host management.
 * Allows dependency injection and mocking in tests.
 */
class IPluginHostManager
{
public:
    virtual ~IPluginHostManager() = default;
    
    // Lifecycle
    virtual void prepare(double sampleRate, int blockSize, int numChannels) = 0;
    virtual void releaseResources() = 0;
    
    // Plugin management
    virtual bool loadPlugin(const juce::PluginDescription& desc) = 0;
    virtual void unloadPlugin() = 0;
    virtual bool hasPlugin() const = 0;
    
    // Audio processing
    virtual void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) noexcept = 0;
    
    // Access
    // P3 NOTE: getPlugin() returns a raw pointer whose lifetime is bound to the
    // PluginHostManager that owns it. The pointer is NOT stable across plugin
    // load/unload cycles. For audio-thread and parameter-bridge access, prefer
    // acquirePluginForUse()/releasePluginFromUse() which provide ref-counted
    // safety. getPlugin() is safe only for single-call access (check-then-use
    // with no intervening yield) and is provided primarily for test stubs.
    virtual juce::AudioPluginInstance* getPlugin() = 0;
    virtual const juce::AudioPluginInstance* getPlugin() const = 0;
    virtual const juce::PluginDescription* getLastDescription() const = 0;
    
    // Plugin discovery
    virtual juce::AudioPluginFormatManager& getFormatManager() = 0;
    virtual juce::KnownPluginList& getKnownPlugins() = 0;
    virtual void scanPluginFolders() = 0;
    
    // Parameter metadata (0 = continuous)
    virtual int getNumSteps(int) const { return 0; }
};

/**
 * Interface for parameter bridge operations.
 * Enables mocking parameter access in tests.
 */
class IParameterBridge
{
public:
    virtual ~IParameterBridge() = default;
    
    virtual int getParameterCount() const = 0;
    virtual float getParameterNormalized(int index) const = 0;
    virtual void setParameterNormalized(int index, float value) = 0;
    virtual juce::String getParameterName(int index) const = 0;
    
    virtual void applyParameterState(const std::vector<float>& values) = 0;
    virtual void applyParameterState(const float* values, int count) = 0;
    virtual std::vector<float> captureParameterState() const = 0;

    // Batch reads — acquire the hosted plugin once for the whole batch instead
    // of once per parameter. Default implementations loop the per-parameter
    // methods so test stubs/mocks compile unchanged; ParameterBridge overrides
    // with a single plugin acquisition (cuts ~2N atomic lock cycles to 2).
    virtual void captureAllNormalized(float* outValues, int count) const
    {
        for (int i = 0; i < count; ++i)
            outValues[i] = getParameterNormalized(i);
    }
    virtual void captureAllNames(juce::StringArray& outNames, int count) const
    {
        outNames.ensureStorageAllocated(count);
        for (int i = 0; i < count; ++i)
            outNames.add(getParameterName(i));
    }

    // Discrete parameter classification (for Listen Mode)
    virtual bool isDiscrete(int index) const = 0;
    virtual std::vector<bool> getDiscreteMap() const = 0;

    // Rich metadata for AI context enrichment (safe from non-audio threads)
    virtual juce::String getParameterLabel(int index) const = 0;
    virtual juce::String getParameterDisplayValue(int index) const = 0;
    virtual float getParameterDefault(int index) const = 0;
    virtual juce::StringArray getParameterValueStrings(int index) const = 0;
    virtual juce::String getParameterStableID(int index) const = 0;
    virtual int getParameterNumSteps(int index) const = 0;
    virtual int getNumSteps(int index) const { return getParameterNumSteps(index); }
};

} // namespace more_phi
