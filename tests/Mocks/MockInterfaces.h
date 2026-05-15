/*
 * More-Phi — Tests/Mocks/MockInterfaces.h
 * Mock implementations for testing - enables unit testing without real plugins.
 */
#pragma once

#include "Host/IPluginHostManager.h"
#include "AI/InstanceIdentity.h"
#include <gmock/gmock.h>

namespace more_phi {
namespace mocks {

/**
 * Mock implementation of IPluginHostManager for testing.
 */
class MockPluginHostManager : public IPluginHostManager
{
public:
    MOCK_METHOD(void, prepare, (double sampleRate, int blockSize, int numChannels), (override));
    MOCK_METHOD(void, releaseResources, (), (override));
    MOCK_METHOD(bool, loadPlugin, (const juce::PluginDescription& desc), (override));
    MOCK_METHOD(void, unloadPlugin, (), (override));
    MOCK_METHOD(bool, hasPlugin, (), (const, override));
    MOCK_METHOD(void, processBlock, (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi), (override));
    MOCK_METHOD(juce::AudioPluginInstance*, getPlugin, (), (override));
    MOCK_METHOD(const juce::AudioPluginInstance*, getPlugin, (const), (override));
    MOCK_METHOD(const juce::PluginDescription*, getLastDescription, (), (const, override));
    MOCK_METHOD(juce::AudioPluginFormatManager&, getFormatManager, (), (override));
    MOCK_METHOD(juce::KnownPluginList&, getKnownPlugins, (), (override));
    MOCK_METHOD(void, scanPluginFolders, (), (override));
};

/**
 * Mock implementation of IParameterBridge for testing.
 */
class MockParameterBridge : public IParameterBridge
{
public:
    MOCK_METHOD(int, getParameterCount, (), (const, override));
    MOCK_METHOD(float, getParameterNormalized, (int index), (const, override));
    MOCK_METHOD(void, setParameterNormalized, (int index, float value), (override));
    MOCK_METHOD(juce::String, getParameterName, (int index), (const, override));
    MOCK_METHOD(void, applyParameterState, (const std::vector<float>& values), (override));
    MOCK_METHOD(void, applyParameterState, (const float* values, int count), (override));
    MOCK_METHOD(std::vector<float>, captureParameterState, (), (const, override));
    MOCK_METHOD(bool, isDiscrete, (int index), (const, override));
    MOCK_METHOD(std::vector<bool>, getDiscreteMap, (), (const, override));
};

/**
 * Mock implementation of IMCPServer for testing.
 */
class MockMCPServer : public IMCPServer
{
public:
    MOCK_METHOD(void, startServer, (int port), (override));
    MOCK_METHOD(void, stopServer, (), (override));
    MOCK_METHOD(bool, isRunning, (), (const, override));
    MOCK_METHOD(int, getPort, (), (const, override));
    MOCK_METHOD(int, getConnectedClients, (), (const, override));
    MOCK_METHOD(bool, isHealthy, (), (const, override));
    MOCK_METHOD(void, setIdentity, (const InstanceIdentity& id), (override));
    MOCK_METHOD(const InstanceIdentity&, getIdentity, (), (const, override));
    MOCK_METHOD(const juce::String&, getAuthToken, (), (const, override));
};

/**
 * Simple stub implementation for basic tests that don't need full mocking.
 */
class StubParameterBridge : public IParameterBridge
{
public:
    std::vector<float> parameters;
    
    int getParameterCount() const override 
    { 
        return static_cast<int>(parameters.size()); 
    }
    
    float getParameterNormalized(int index) const override 
    { 
        if (index < 0 || index >= static_cast<int>(parameters.size())) 
            return 0.0f;
        return parameters[index]; 
    }
    
    void setParameterNormalized(int index, float value) override 
    { 
        if (index >= 0 && index < static_cast<int>(parameters.size()))
            parameters[index] = value;
    }
    
    juce::String getParameterName(int index) const override 
    { 
        return "Param" + juce::String(index); 
    }
    
    void applyParameterState(const std::vector<float>& values) override 
    { 
        parameters = values; 
    }
    
    void applyParameterState(const float* values, int count) override 
    { 
        parameters.assign(values, values + count); 
    }
    
    std::vector<float> captureParameterState() const override 
    { 
        return parameters; 
    }
    
    void resize(int count, float value = 0.5f)
    {
        parameters.resize(count, value);
    }

    bool isDiscrete(int /*index*/) const override { return false; }
    std::vector<bool> getDiscreteMap() const override
    {
        return std::vector<bool>(parameters.size(), false);
    }
};

} // namespace mocks
} // namespace more_phi
