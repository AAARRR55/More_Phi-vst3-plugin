/*
 * More-Phi — MIDI/MIDIRouter.cpp
 * MIDI routing + sidechain-triggered snapshot recall.
 */
#include "MIDIRouter.h"
#include <cmath>

namespace more_phi {

void MIDIRouter::prepare(int /*expectedMidiEventsPerBlock*/)
{
    // Reset pre-allocated storage - this ensures we start with clean state
    preallocatedStorage_.count = 0;
    // Note: The storage arrays are already allocated, no dynamic allocation here
}

void MIDIRouter::prepare(double sampleRate, int blockSize)
{
    // H4 FIX: Compute per-block one-pole coefficients from time constants.
    const float dt = static_cast<float>(blockSize) / static_cast<float>(sampleRate);
    const float attackTime  = 0.001f;  // 1 ms
    const float releaseTime = 0.010f;  // 10 ms
    scAttackCoeff_  = 1.0f - std::exp(-dt / attackTime);
    scReleaseCoeff_ = 1.0f - std::exp(-dt / releaseTime);
}

void MIDIRouter::processMidi(const juce::MidiBuffer& midi, juce::MidiBuffer& filtered)
{
    // Clear output buffer (no allocation - just marks it empty)
    filtered.clear();

    // Reset pre-allocated storage count for this block
    preallocatedStorage_.count = 0;

    // First pass: Collect non-consumed events in pre-allocated storage
    for (const auto& metadata : midi)
    {
        const auto msg = metadata.getMessage();
        bool consumed = false;

        // M-17 FIX: Channel filter — skip events that don't match configured channel.
        // When midiChannel_ is 0 (omni, the default), all channels are accepted.
        const int midiCh = midiChannel_.load(std::memory_order_relaxed);
        if (midiCh > 0 && msg.getChannel() != midiCh)
        {
            // Channel doesn't match — pass through without consuming
            if (preallocatedStorage_.count < MAX_MIDI_EVENTS)
            {
                preallocatedStorage_.messages[preallocatedStorage_.count] = msg;
                preallocatedStorage_.samplePositions[preallocatedStorage_.count] = metadata.samplePosition;
                ++preallocatedStorage_.count;
            }
            continue;
        }

        if (msg.isNoteOn() || msg.isNoteOff())
        {
            const int note = msg.getNoteNumber();
            const int trigBase = triggerOctave_.load(std::memory_order_relaxed) * 12;

            if (note >= trigBase && note < trigBase + SnapshotBank::NUM_SLOTS)
            {
                if (msg.isNoteOn() && msg.getVelocity() > 0)
                {
                    const int slot = note - trigBase;
                    auto cb = snapshotCb_.load(std::memory_order_acquire);
                    if (cb) cb(slot, snapshotCtx_.load(std::memory_order_acquire));
                    consumed = true;  // FIX C19: Only consume note-ONs
                }
                // note-OFFs pass through to the hosted plugin
            }
        }
        else if (msg.isController())
        {
            const int cc = msg.getControllerNumber();
            if (cc == 1)  // Mod wheel → morph position
            {
                auto cb = morphCb_.load(std::memory_order_acquire);
                if (cb) cb(msg.getControllerValue() / 127.0f, morphCtx_.load(std::memory_order_acquire));
            }
        }

        // Store non-consumed events in pre-allocated storage (real-time safe - no allocation)
        if (!consumed)
        {
            if (preallocatedStorage_.count < MAX_MIDI_EVENTS)
            {
                preallocatedStorage_.messages[preallocatedStorage_.count] = msg;
                preallocatedStorage_.samplePositions[preallocatedStorage_.count] = metadata.samplePosition;
                ++preallocatedStorage_.count;
            }
            else
            {
                // Storage full — event is silently dropped; track it for diagnostics.
                droppedEventCount_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // Second pass: Add stored events to output buffer
    // This minimizes allocation calls and provides controlled fallback
    for (int i = 0; i < preallocatedStorage_.count; ++i)
    {
        filtered.addEvent(preallocatedStorage_.messages[i], preallocatedStorage_.samplePositions[i]);
    }
}

void MIDIRouter::processSidechain(const juce::AudioBuffer<float>& sidechain)
{
    // Thread-safe atomic loads (may be set from UI thread)
    auto snapshotCb = snapshotCb_.load(std::memory_order_acquire);
    if (!sidechainEnabled_.load(std::memory_order_relaxed) || !snapshotCb)
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

    // H-7 FIX: One-pole envelope follower for smooth sidechain triggering.
    // Without ballistics, instantaneous RMS causes erratic triggering on transients.
    const float coeff = rms > sidechainEnvelope_ ? scAttackCoeff_ : scReleaseCoeff_;
    sidechainEnvelope_ += coeff * (rms - sidechainEnvelope_);

    // Edge detection: trigger on rising edge (gate was closed, now above threshold)
    const float threshold = sidechainThreshold_.load(std::memory_order_relaxed);
    if (sidechainEnvelope_ >= threshold)
    {
        if (!sidechainGateOpen_)
        {
            // Rising edge — trigger next snapshot slot
            snapshotCb(sidechainSlot_, snapshotCtx_.load(std::memory_order_acquire));
            sidechainSlot_ = (sidechainSlot_ + 1) % SnapshotBank::NUM_SLOTS;
            sidechainGateOpen_ = true;
        }
    }
    else
    {
        // Below threshold — reset gate for next trigger
        sidechainGateOpen_ = false;
    }
}

} // namespace more_phi
