#pragma once

#include <juce_core/juce_core.h>

namespace more_phi {

class LLMProvider
{
public:
    virtual ~LLMProvider() = default;
    virtual juce::String getName() const { return "local"; }
};

} // namespace more_phi
