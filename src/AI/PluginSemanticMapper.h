#pragma once

#include "Host/ParameterBridge.h"
#include <juce_core/juce_core.h>

namespace more_phi {

class IPluginHostManager;
class ParameterClassifier;

class PluginSemanticMapper
{
public:
    struct Options
    {
        bool includeRawParameters = false;
        int maxControls = 128;
    };

    static juce::String buildSemanticMapJson(IPluginHostManager& host,
                                             const ParameterBridge& bridge,
                                             const ParameterClassifier& classifier,
                                             const Options& options);
};

} // namespace more_phi
