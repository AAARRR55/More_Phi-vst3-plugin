/* More-Phi — Preset/MetaPresetManager.cpp */
#include "MetaPresetManager.h"

namespace more_phi {

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

void MetaPresetManager::switchBank(int bank)
{
    currentBank_ = juce::jlimit(0, NUM_BANKS - 1, bank);
    if (onPresetChanged)
        onPresetChanged(currentBank_, currentPreset_);
}

void MetaPresetManager::switchPreset(int preset)
{
    currentPreset_ = juce::jlimit(0, NUM_PRESETS - 1, preset);
    if (onPresetChanged)
        onPresetChanged(currentBank_, currentPreset_);
}

void MetaPresetManager::switchToNext()
{
    int next = currentPreset_ + 1;
    if (next >= NUM_PRESETS)
    {
        next = 0;
        switchBank((currentBank_ + 1) % NUM_BANKS);
    }
    switchPreset(next);
}

void MetaPresetManager::switchToPrev()
{
    int prev = currentPreset_ - 1;
    if (prev < 0)
    {
        prev = NUM_PRESETS - 1;
        switchBank((currentBank_ - 1 + NUM_BANKS) % NUM_BANKS);
    }
    switchPreset(prev);
}

juce::String MetaPresetManager::getPresetName(int bank, int preset) const
{
    auto data = loadPreset(bank, preset);
    if (data.isObject())
    {
        auto name = data.getProperty("name", {});
        if (name.isString()) return name.toString();
    }
    return "Bank " + juce::String(bank) + " / Preset " + juce::String(preset);
}

} // namespace more_phi
