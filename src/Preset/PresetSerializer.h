/* MorphSnap — Preset/PresetSerializer.h */
#pragma once
#include <juce_core/juce_core.h>
namespace morphsnap {
class PresetSerializer
{
public:
    static juce::var serialize(const juce::var& state);
    static bool deserialize(const juce::var& json, juce::var& outState);
};
} // namespace morphsnap
