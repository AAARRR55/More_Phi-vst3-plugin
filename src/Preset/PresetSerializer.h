/* MorphSnap — Preset/PresetSerializer.h
 * Serializes/deserializes full MorphSnap state (snapshots, morph config, sanity)
 * into JSON for meta preset storage. */
#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "Core/SnapshotBank.h"
#include "Core/GeneticEngine.h"

namespace morphsnap {

class PresetSerializer
{
public:
    // Serialize full state: 12 snapshot slots + morph/physics config + sanity
    static juce::var serialize(const SnapshotBank& bank,
                               juce::AudioProcessorValueTreeState& apvts);

    // Deserialize into existing bank and APVTS
    static bool deserialize(const juce::var& json,
                            SnapshotBank& bank,
                            juce::AudioProcessorValueTreeState& apvts);
};

} // namespace morphsnap
