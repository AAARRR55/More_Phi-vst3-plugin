#pragma once

#include <juce_core/juce_core.h>

namespace more_phi {

class ParameterContextBuilder
{
public:
    juce::String buildEmptyContext() const { return {}; }
};

} // namespace more_phi
