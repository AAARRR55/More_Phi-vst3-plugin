/* More-Phi — Preset/MetaPresetManager.h */
#pragma once
#include <juce_core/juce_core.h>
#include "PresetSerializer.h"

namespace more_phi {

class MetaPresetManager
{
public:
    static constexpr int NUM_BANKS   = 16;
    static constexpr int NUM_PRESETS = 128;

    void savePreset(int bank, int preset, const juce::var& data);
    juce::var loadPreset(int bank, int preset) const;

    void setPresetDirectory(const juce::File& dir) { presetDir_ = dir; }

    // Bank/preset state tracking
    int getCurrentBank() const   { return currentBank_; }
    int getCurrentPreset() const { return currentPreset_; }
    void switchBank(int bank);
    void switchPreset(int preset);
    void switchToNext();
    void switchToPrev();
    juce::String getPresetName(int bank, int preset) const;

    // Callback for preset changes (UI refresh, state load)
    std::function<void(int bank, int preset)> onPresetChanged;

private:
    juce::File presetDir_;
    int currentBank_   = 0;
    int currentPreset_ = 0;
    juce::File getPresetFile(int bank, int preset) const;
};

} // namespace more_phi
