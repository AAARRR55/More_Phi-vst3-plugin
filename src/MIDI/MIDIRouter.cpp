/*
 * MorphSnap — MIDI/MIDIRouter.cpp
 * MIDI routing + sidechain-triggered snapshot recall.
 */
#include "MIDIRouter.h"
#include <cmath>

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

void MIDIRouter::processSidechain(const juce::AudioBuffer<float>& sidechain)
{
    if (!sidechainEnabled_ || !onSnapshot_)
        return;

    if (sidechain.getNumSamples() == 0)
        return;

    // Compute RMS of the sidechain buffer (all channels summed)
    float sumSq = 0.0f;
    const int numChannels = sidechain.getNumChannels();
    const int numSamples  = sidechain.getNumSamples();

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* data = sidechain.getReadPointer(ch);
        for (int s = 0; s < numSamples; ++s)
            sumSq += data[s] * data[s];
    }

    const float rms = std::sqrt(sumSq / static_cast<float>(numChannels * numSamples));

    // Edge detection: trigger on rising edge (gate was closed, now above threshold)
    if (rms >= sidechainThreshold_)
    {
        if (!sidechainGateOpen_)
        {
            // Rising edge — trigger next snapshot slot
            onSnapshot_(sidechainSlot_);
            sidechainSlot_ = (sidechainSlot_ + 1) % 12;
            sidechainGateOpen_ = true;
        }
    }
    else
    {
        // Below threshold — reset gate for next trigger
        sidechainGateOpen_ = false;
    }
}

} // namespace morphsnap
