/* MorphSnap — Preset/PresetSerializer.cpp */
#include "PresetSerializer.h"
namespace morphsnap {
juce::var PresetSerializer::serialize(const juce::var& state) { return state; }
bool PresetSerializer::deserialize(const juce::var& json, juce::var& outState)
{ outState = json; return !json.isVoid(); }
} // namespace morphsnap
