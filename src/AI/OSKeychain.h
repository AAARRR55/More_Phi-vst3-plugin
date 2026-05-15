#pragma once

#include <juce_core/juce_core.h>

namespace more_phi {

class OSKeychain
{
public:
    bool storeSecret(const juce::String&, const juce::String&) { return false; }
    juce::String loadSecret(const juce::String&) const { return {}; }
};

} // namespace more_phi
