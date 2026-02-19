/*
 * MorphSnap — Host/IPluginHostManager.h
 * Interface abstraction for plugin hosting - enables mocking and testing.
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>

namespace morphsnap {

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
    virtual void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) = 0;
    
    // Access
    virtual juce::AudioPluginInstance* getPlugin() = 0;
    virtual const juce::AudioPluginInstance* getPlugin() const = 0;
    virtual const juce::PluginDescription* getLastDescription() const = 0;
    
    // Plugin discovery
    virtual juce::AudioPluginFormatManager& getFormatManager() = 0;
    virtual juce::KnownPluginList& getKnownPlugins() = 0;
    virtual void scanPluginFolders() = 0;
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
};

/**
 * Interface for MCP server operations.
 * Enables mocking AI integration in tests.
 */
class IMCPServer
{
public:
    virtual ~IMCPServer() = default;
    
    virtual void startServer(int port = 30001) = 0;
    virtual void stopServer() = 0;
    
    virtual bool isRunning() const = 0;
    virtual int getPort() const = 0;
    virtual int getConnectedClients() const = 0;
    virtual bool isHealthy() const = 0;
    
    virtual void setIdentity(const struct InstanceIdentity& id) = 0;
    virtual const struct InstanceIdentity& getIdentity() const = 0;
    virtual const juce::String& getAuthToken() const = 0;
};

} // namespace morphsnap
