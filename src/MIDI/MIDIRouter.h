/*
 * More-Phi — MIDI/MIDIRouter.h
 * Maps MIDI notes to snapshot triggers, CC to morph, program changes to presets.
 * Sidechain trigger: RMS envelope follower triggers snapshot transitions.
 */
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include "Core/SnapshotBank.h"

namespace more_phi {

class MIDIRouter
{
public:
    using SnapshotCallback = void(*)(int slot, void* context);
    using MorphCallback    = void(*)(float value, void* context);

    void setTriggerOctave(int octave) { triggerOctave_.store(juce::jlimit(0, 10, octave), std::memory_order_relaxed); }

    // M-17 FIX: Configurable MIDI channel filter (0 = omni, accepts all channels)
    void setMidiChannel(int channel) { midiChannel_.store(juce::jlimit(0, 16, channel), std::memory_order_relaxed); }
    int getMidiChannel() const { return midiChannel_.load(std::memory_order_relaxed); }

    /** Set the snapshot callback. MUST be called before prepareToPlay() and
     *  must NOT be changed during audio processing. The callback pointer is
     *  stored atomically but is NOT guarded by a lock; changing it concurrently
     *  with processMidi() or processSidechain() is a data race. */
    void setSnapshotCallback(SnapshotCallback cb, void* ctx = nullptr) {
        snapshotCb_.store(cb, std::memory_order_release);
        snapshotCtx_.store(ctx, std::memory_order_release);
    }
    /** Set the morph callback. MUST be called before prepareToPlay() and
     *  must NOT be changed during audio processing. The callback pointer is
     *  stored atomically but is NOT guarded by a lock; changing it concurrently
     *  with processMidi() or processSidechain() is a data race. */
    void setMorphCallback(MorphCallback cb, void* ctx = nullptr) {
        morphCb_.store(cb, std::memory_order_release);
        morphCtx_.store(ctx, std::memory_order_release);
    }

    /** Returns the number of MIDI events dropped due to preallocatedStorage_ overflow.
     *  Safe to call from any thread. */
    int getDroppedEventCount() const { return droppedEventCount_.load(std::memory_order_relaxed); }

    /** Resets the dropped-event counter to zero. Safe to call from any thread. */
    void resetDroppedEventCount() { droppedEventCount_.store(0, std::memory_order_relaxed); }

    // Real-time safety: Pre-allocate MIDI buffer storage
    void prepare(int expectedMidiEventsPerBlock = 128);

    /** H4 FIX: Compute sample-rate-dependent sidechain envelope coefficients. */
    void prepare(double sampleRate, int blockSize);

    // Call from processBlock with incoming MIDI
    void processMidi(const juce::MidiBuffer& midi, juce::MidiBuffer& filtered);

    // ── Sidechain Trigger ──────────────────────────────────────────────────
    // Thread-safe: Uses atomic operations for cross-thread access (UI→Audio)
    void setSidechainEnabled(bool enabled)   { sidechainEnabled_.store(enabled, std::memory_order_relaxed); }
    bool isSidechainEnabled() const          { return sidechainEnabled_.load(std::memory_order_relaxed); }
    void setSidechainThreshold(float thresh)  { sidechainThreshold_.store(thresh, std::memory_order_relaxed); }
    float getSidechainThreshold() const       { return sidechainThreshold_.load(std::memory_order_relaxed); }

    /** Process a sidechain bus to detect transients.
     *  When the RMS level exceeds the threshold AND was previously below it,
     *  the next snapshot slot is triggered via the snapshot callback. */
    void processSidechain(const juce::AudioBuffer<float>& sidechain);

private:
    std::atomic<int> triggerOctave_{4};  // C3–B3 triggers slots 0–11 (JUCE: note 48 = C3 = octave 4)
    std::atomic<int> midiChannel_{0};    // M-17: 0=omni, 1-16=specific channel
    std::atomic<SnapshotCallback> snapshotCb_{nullptr};
    std::atomic<void*>            snapshotCtx_{nullptr};
    std::atomic<MorphCallback>    morphCb_{nullptr};
    std::atomic<void*>            morphCtx_{nullptr};

    // Counter incremented on every MIDI event silently dropped due to buffer overflow.
    // Monotonically increasing; reset via resetDroppedEventCount().
    std::atomic<int> droppedEventCount_{0};

    // Sidechain trigger state (thread-safe atomics for cross-thread access)
    std::atomic<bool>  sidechainEnabled_   {false};
    std::atomic<float> sidechainThreshold_ {0.1f};   // RMS threshold [0,1]
    bool  sidechainGateOpen_  = false;   // Edge detector state (audio thread only)
    int   sidechainSlot_      = 0;       // Current slot in round-robin trigger (audio thread only)
    float sidechainEnvelope_  = 0.0f;    // Smoothed RMS envelope (audio thread only)

    // H4: Pre-computed per-block one-pole coefficients (set via prepare(sampleRate, blockSize))
    float scAttackCoeff_      = 0.0f;    // Attack coefficient (1 ms time constant)
    float scReleaseCoeff_     = 0.0f;    // Release coefficient (10 ms time constant)

    // Real-time safe MIDI storage (pre-allocated in prepare())
    // Capacity: 256 events provides 5x safety margin over typical high-density MIDI
    // (max observed in practice: ~50 events/block during dense sequencer playback)
    // Silently drops excess events if exceeded (extremely rare, indicates system stress)
    static constexpr int MAX_MIDI_EVENTS = 256;
    struct PreallocatedMidi {
        juce::MidiMessage messages[MAX_MIDI_EVENTS];
        int samplePositions[MAX_MIDI_EVENTS];
        int count = 0;
    } preallocatedStorage_;
};

} // namespace more_phi
