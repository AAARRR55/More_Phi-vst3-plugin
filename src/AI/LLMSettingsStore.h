#pragma once

#include "LLMSettings.h"

namespace more_phi {

class LLMSettingsStore
{
public:
    LLMSettingsStore();
    explicit LLMSettingsStore(juce::File configFile);

    static juce::File defaultConfigFile();

    bool load(LLMSettings& settings, juce::String& error) const;
    bool save(const LLMSettings& settings, juce::String& error) const;

    const juce::File& getConfigFile() const noexcept { return configFile_; }

private:
    juce::File configFile_;
};

}
