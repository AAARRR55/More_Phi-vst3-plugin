/*
 * MorphSnap — MIDI/MIDIRouter.h
 * Maps MIDI notes to snapshot triggers, CC to morph, program changes to presets.
 * Sidechain trigger: RMS envelope follower triggers snapshot transitions.
 */
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <functional>
#include <atomic>

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

    // ── Sidechain Trigger ──────────────────────────────────────────────────
    void setSidechainEnabled(bool enabled)   { sidechainEnabled_ = enabled; }
    bool isSidechainEnabled() const          { return sidechainEnabled_; }
    void setSidechainThreshold(float thresh)  { sidechainThreshold_ = thresh; }
    float getSidechainThreshold() const       { return sidechainThreshold_; }

    /** Process a sidechain bus to detect transients.
     *  When the RMS level exceeds the threshold AND was previously below it,
     *  the next snapshot slot is triggered via onSnapshot_. */
    void processSidechain(const juce::AudioBuffer<float>& sidechain);

private:
    int triggerOctave_ = 3;  // C3–B3 triggers slots 1–12
    SnapshotCallback onSnapshot_;
    MorphCallback    onMorph_;

    // Sidechain trigger state
    bool  sidechainEnabled_   = false;
    float sidechainThreshold_ = 0.1f;   // RMS threshold [0,1]
    bool  sidechainGateOpen_  = false;   // Edge detector state
    int   sidechainSlot_      = 0;       // Current slot in round-robin trigger
};

} // namespace morphsnap
