#pragma once

#include "Host/ParameterBridge.h"

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <vector>

namespace more_phi {

struct SemanticControl
{
    juce::String semanticId;
    juce::String role;
    juce::String group;
    juce::String unit;
    juce::String safety;
    int parameterIndex = -1;
    float safeMin = 0.0f;
    float safeMax = 1.0f;
    float maxStepDb = 0.0f;
    float minDeltaDb = 0.0f;
    float maxDeltaDb = 0.0f;
};

struct SemanticParameterCommand
{
    int parameterIndex = -1;
    float normalizedValue = 0.0f;
};

struct SemanticActionPlan
{
    bool success = false;
    juce::String error;
    juce::String message;
    std::vector<SemanticParameterCommand> commands;
    nlohmann::json actionJson;
};

class SemanticPluginProfile
{
public:
    using ParameterDescriptor = ParameterBridge::ParameterDescriptor;

    static std::vector<SemanticControl> classify(const std::vector<ParameterDescriptor>& descriptors);
    static std::vector<SemanticControl> classify(const juce::String& pluginName,
                                                 const std::vector<ParameterDescriptor>& descriptors);

    static nlohmann::json controlsToJson(const std::vector<ParameterDescriptor>& descriptors,
                                         const std::vector<SemanticControl>& controls);
    static nlohmann::json controlsToJson(const std::vector<SemanticControl>& controls,
                                         const std::vector<ParameterDescriptor>& descriptors);

    static const SemanticControl* findControl(const std::vector<SemanticControl>& controls,
                                              const juce::String& semanticId);

    static SemanticActionPlan planSafeAction(const juce::var& params,
                                             const std::vector<ParameterDescriptor>& descriptors,
                                             const std::vector<SemanticControl>& controls,
                                             const ParameterBridge* bridge = nullptr);
};

} // namespace more_phi
