/* MorphSnap — Preset/MetaPresetManager.h */
#pragma once
#include <juce_core/juce_core.h>
#include "PresetSerializer.h"

namespace morphsnap {

class MetaPresetManager
{
public:
    static constexpr int NUM_BANKS   = 16;
    static constexpr int NUM_PRESETS = 128;

    void savePreset(int bank, int preset, const juce::var& data);
    juce::var loadPreset(int bank, int preset) const;

    void setPresetDirectory(const juce::File& dir) { presetDir_ = dir; }

private:
    juce::File presetDir_;
    juce::File getPresetFile(int bank, int preset) const;
};

} // namespace morphsnap
