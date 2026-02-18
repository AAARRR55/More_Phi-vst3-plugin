/*
 * MorphSnap — MIDI/MIDIRouter.cpp
 */
#include "MIDIRouter.h"

namespace morphsnap {

void MIDIRouter::processMidi(const juce::MidiBuffer& midi, juce::MidiBuffer& filtered)
{
    for (const auto metadata : midi)
    {
        const auto msg = metadata.getMessage();
        bool consumed = false;

        if (msg.isNoteOn())
        {
            const int note = msg.getNoteNumber();
            const int trigBase = triggerOctave_ * 12;  // C of trigger octave

            if (note >= trigBase && note < trigBase + 12)
            {
                const int slot = note - trigBase;
                if (onSnapshot_) onSnapshot_(slot);
                consumed = true;  // Don't pass trigger notes to hosted synth
            }
        }
        else if (msg.isController())
        {
            const int cc = msg.getControllerNumber();
            if (cc == 1 && onMorph_)  // Mod wheel → morph position
            {
                onMorph_(msg.getControllerValue() / 127.0f);
            }
        }

        if (!consumed)
            filtered.addEvent(msg, metadata.samplePosition);
    }
}

} // namespace morphsnap
