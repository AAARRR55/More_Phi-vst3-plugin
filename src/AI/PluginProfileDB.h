#pragma once

#include <juce_core/juce_core.h>

namespace more_phi {

class IPluginHostManager;
class ParameterBridge;

class PluginProfileDB
{
public:
    static juce::String detectPluginKind(const juce::String& pluginName);
    static juce::String makeProfileId(const juce::String& pluginName, int parameterCount);
    static juce::String buildAuditJson(IPluginHostManager& host, const ParameterBridge& bridge);
    static juce::File getProfileDirectory();
    static juce::File getProfileFile(const juce::String& profileId);
};

} // namespace more_phi
