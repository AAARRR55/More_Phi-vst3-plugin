/*
 * MorphSnap — Host/ParameterBridge.cpp
 * Uses the legacy AudioProcessor::setParameter() which directly calls
 * VST3's setParamNormalized via the JUCE wrapper.
 */
#include "ParameterBridge.h"

// Suppress deprecation warning for setParameter — it's the only way
// that reliably works for setting hosted VST3 plugin parameters.
JUCE_BEGIN_IGNORE_WARNINGS_MSVC(4996)

namespace morphsnap {

int ParameterBridge::getParameterCount() const
{
    auto* plugin = host_.getPlugin();
    if (!plugin) return 0;
    return plugin->getParameters().size();
}

float ParameterBridge::getParameterNormalized(int index) const
{
    auto* plugin = host_.getPlugin();
    if (!plugin) return 0.0f;
    auto& params = plugin->getParameters();
    if (index < 0 || index >= params.size()) return 0.0f;
    return params[index]->getValue();
}

void ParameterBridge::setParameterNormalized(int index, float value)
{
    auto* plugin = host_.getPlugin();
    if (!plugin) return;
    if (index < 0 || index >= plugin->getParameters().size()) return;

    // Legacy setParameter() directly calls VST3's setParamNormalized
    // via the JUCE VST3 wrapper — the ONLY method that reliably
    // changes hosted plugin state and updates the plugin's GUI.
    plugin->setParameter(index, juce::jlimit(0.0f, 1.0f, value));
}

juce::String ParameterBridge::getParameterName(int index) const
{
    auto* plugin = host_.getPlugin();
    if (!plugin) return {};
    auto& params = plugin->getParameters();
    if (index < 0 || index >= params.size()) return {};
    return params[index]->getName(128);
}

void ParameterBridge::applyParameterState(const std::vector<float>& values)
{
    auto* plugin = host_.getPlugin();
    if (!plugin) return;
    const int count = juce::jmin(static_cast<int>(values.size()),
                                  plugin->getParameters().size());
    for (int i = 0; i < count; ++i)
        plugin->setParameter(i, juce::jlimit(0.0f, 1.0f, values[i]));

    // Refresh host-side display after bulk changes
    plugin->updateHostDisplay(juce::AudioProcessor::ChangeDetails().withParameterInfoChanged(true));
}

std::vector<float> ParameterBridge::captureParameterState() const
{
    auto* plugin = host_.getPlugin();
    if (!plugin) return {};
    auto& params = plugin->getParameters();
    std::vector<float> state(params.size());
    for (int i = 0; i < params.size(); ++i)
        state[i] = params[i]->getValue();
    return state;
}

} // namespace morphsnap

JUCE_END_IGNORE_WARNINGS_MSVC
