/* MorphSnap — Preset/MetaPresetManager.cpp */
#include "MetaPresetManager.h"

namespace morphsnap {

juce::File MetaPresetManager::getPresetFile(int bank, int preset) const
{
    return presetDir_.getChildFile("bank_" + juce::String(bank))
                     .getChildFile("preset_" + juce::String(preset) + ".json");
}

void MetaPresetManager::savePreset(int bank, int preset, const juce::var& data)
{
    auto file = getPresetFile(bank, preset);
    file.getParentDirectory().createDirectory();
    file.replaceWithText(juce::JSON::toString(data));
}

juce::var MetaPresetManager::loadPreset(int bank, int preset) const
{
    auto file = getPresetFile(bank, preset);
    if (!file.existsAsFile()) return {};
    return juce::JSON::parse(file.loadFileAsString());
}

} // namespace morphsnap
