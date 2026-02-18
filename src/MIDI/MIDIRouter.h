/*
 * MorphSnap — MIDI/MIDIRouter.h
 * Maps MIDI notes to snapshot triggers, CC to morph, program changes to presets.
 */
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <functional>

namespace morphsnap {

class MIDIRouter
{
public:
    using SnapshotCallback = std::function<void(int slot)>;
    using MorphCallback    = std::function<void(float value)>;

    void setTriggerOctave(int octave) { triggerOctave_ = octave; }
    void setSnapshotCallback(SnapshotCallback cb) { onSnapshot_ = std::move(cb); }
    void setMorphCallback(MorphCallback cb)       { onMorph_ = std::move(cb); }

    // Call from processBlock with incoming MIDI
    void processMidi(const juce::MidiBuffer& midi, juce::MidiBuffer& filtered);

private:
    int triggerOctave_ = 3;  // C3–B3 triggers slots 1–12
    SnapshotCallback onSnapshot_;
    MorphCallback    onMorph_;
};

} // namespace morphsnap
