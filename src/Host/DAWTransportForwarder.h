/*
 * More-Phi — Host/DAWTransportForwarder.h
 * Lock-free (seqlock) audio→message snapshot of DAW transport state.
 *
 * The hosted plugin already receives full transport sample-accurately via
 * plugin->setPlayHead() on the audio thread (unchanged). This forwarder does
 * NOT feed the plugin — it publishes a coherent, versioned TransportState so
 * message-thread consumers (UI transport readout, agent context, MCP) can read
 * BPM/time-sig/loop/position in one shot without per-field tearing.
 */
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <cstdint>
#include <optional>

namespace more_phi {

/**
 * Immutable snapshot of DAW transport state. Trivially copyable.
 * Published on the audio thread, read coherently on the message thread.
 */
struct TransportState
{
    double   bpm = 120.0;
    int      timeSigNumerator = 4;
    int      timeSigDenominator = 4;
    bool     isPlaying = false;
    bool     isLooping = false;
    double   ppqPosition = 0.0;
    double   timeInSeconds = 0.0;
    uint32_t version = 0;            /**< bumped on every publish */
};

/**
 * Producer/consumer transport snapshot bus.
 *
 *   // Audio thread (processBlock):
 *   forwarder.updateFromAudioThread(playHead);
 *
 *   // Message thread (UI timer, agent context, MCP tool):
 *   if (auto snap = forwarder.getSnapshot()) { ... use snap->bpm ... }
 */
class DAWTransportForwarder
{
public:
    DAWTransportForwarder() = default;

    /** Producer — audio thread only. Reads the AudioPlayHead and publishes. */
    void updateFromAudioThread(const juce::AudioPlayHead* playHead) noexcept;

    /**
     * Consumer — message thread only. Returns the latest coherent snapshot, or
     * std::nullopt if no state has ever been published. Retries on seqlock
     * mismatch so a torn read is impossible.
     */
    std::optional<TransportState> getSnapshot() const noexcept;

private:
    // Seqlock: even sequence ⇒ stable, readable; odd ⇒ write in progress.
    // Producer bumps to odd before writing the payload, to even after.
    mutable std::atomic<uint32_t> sequence_{ 0 };
    TransportState state_{};

    // Padding to prevent false sharing with adjacent members on the cache line.
    char padding_[64 - sizeof(std::atomic<uint32_t>) - sizeof(TransportState)];
};

} // namespace more_phi
