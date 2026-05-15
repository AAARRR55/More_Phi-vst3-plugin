#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>

namespace more_phi {

class IPluginHostManager;
class ParameterBridge;
class PluginHostManager;

namespace standalone_mcp {

struct ToolCallOutcome
{
    nlohmann::json body;
    bool isError = false;
};

struct ParameterListArgs
{
    std::optional<std::string> query;
    bool includeValues = true;
};

struct RunAssistantArgs
{
    std::optional<int> assistantParameterIndex;
    std::optional<std::string> inputAudioPath;
    double analysisSeconds = 30.0;
};

class OzonePluginBackend
{
public:
    virtual ~OzonePluginBackend() = default;

    virtual ToolCallOutcome getParameters(const ParameterListArgs& args) = 0;
    virtual ToolCallOutcome setParameter(int index, float value) = 0;
    virtual ToolCallOutcome runMasterAssistant(const RunAssistantArgs& args) = 0;
    virtual ToolCallOutcome getState() = 0;
    virtual ToolCallOutcome setState(const std::string& stateBase64) = 0;
};

class HostedOzonePluginBackend final : public OzonePluginBackend
{
public:
    HostedOzonePluginBackend();
    explicit HostedOzonePluginBackend(IPluginHostManager& externalHost);
    ~HostedOzonePluginBackend() override;

    ToolCallOutcome getParameters(const ParameterListArgs& args) override;
    ToolCallOutcome setParameter(int index, float value) override;
    ToolCallOutcome runMasterAssistant(const RunAssistantArgs& args) override;
    ToolCallOutcome getState() override;
    ToolCallOutcome setState(const std::string& stateBase64) override;

private:
    std::unique_ptr<PluginHostManager> ownedHost;
    IPluginHostManager* host = nullptr;
    std::unique_ptr<ParameterBridge> bridge;
    juce::String loadError;
    double sampleRate = 44100.0;
    int blockSize = 512;
    int numChannels = 2;

    void loadFromEnvironment();
    bool hasLoadedPlugin() const;
    ToolCallOutcome pluginNotLoaded() const;
    nlohmann::json pluginInfo() const;
    nlohmann::json parameterDescriptorToJson(int index, bool includeValues) const;
    std::optional<int> resolveAssistantParameter(const RunAssistantArgs& args) const;
    ToolCallOutcome renderInputAudio(const std::string& inputAudioPath,
                                     double analysisSeconds,
                                     int& renderedSamples);
};

std::unique_ptr<OzonePluginBackend> createOzonePluginBackend();

} // namespace standalone_mcp
} // namespace more_phi
