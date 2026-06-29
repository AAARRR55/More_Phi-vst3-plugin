/*
 * More-Phi — Host/DAWTransportForwarder.cpp
 */
#include "DAWTransportForwarder.h"

namespace more_phi {

void DAWTransportForwarder::updateFromAudioThread(const juce::AudioPlayHead* playHead) noexcept
{
    if (playHead == nullptr)
        return;

    const auto optInfo = playHead->getPosition();   // JUCE 8: Optional<PositionInfo>
    if (! optInfo.hasValue())
        return;

    const auto& info = *optInfo;

    // Begin write: mark sequence odd so a concurrent reader retries.
    // Single-producer (audio thread) is the only writer. fetch_add(1) flips an
    // even sequence to odd (write in progress). Use relaxed since no data
    // is being published yet — we just need to mark the start.
    const uint32_t seqBegin = sequence_.fetch_add(1, std::memory_order_relaxed);

    // Compiler barrier: prevent the payload writes from being reordered before
    // the sequence increment above. On x86-64 the hardware won't reorder, but
    // the compiler might. atomic_thread_fence is sufficient.
    std::atomic_thread_fence(std::memory_order_release);

    state_.bpm = info.getBpm().orFallback(120.0);
    if (const auto ts = info.getTimeSignature())
    {
        state_.timeSigNumerator   = ts->numerator;
        state_.timeSigDenominator = ts->denominator;
    }
    else
    {
        state_.timeSigNumerator   = 4;
        state_.timeSigDenominator = 4;
    }
    state_.isPlaying     = info.getIsPlaying();
    state_.isLooping     = info.getIsLooping();
    state_.ppqPosition   = info.getPpqPosition().orFallback(0.0);
    state_.timeInSeconds = info.getTimeInSeconds().orFallback(0.0);
    state_.version       = seqBegin / 2 + 1;   // monotonic per publish

    // End write: mark sequence even again (release — payload visible to reader).
    // A full release store ensures that all payload writes above are visible
    // to any thread that observes the new even sequence value.
    std::atomic_thread_fence(std::memory_order_release);
    sequence_.store(seqBegin + 2, std::memory_order_relaxed);
}

std::optional<TransportState> DAWTransportForwarder::getSnapshot() const noexcept
{
    for (int attempts = 0; attempts < 16; ++attempts)
    {
        const uint32_t seq1 = sequence_.load(std::memory_order_relaxed);
        if ((seq1 & 1u) != 0u)
            continue;   // write in progress — retry

        // Acquire fence: ensure the payload copy below is ordered after
        // the sequence read. Without this, the compiler or CPU could
        // reorder the struct copy to happen before we read the sequence,
        // producing a torn read even when seq1 == seq2.
        std::atomic_thread_fence(std::memory_order_acquire);

        // Copy payload.
        TransportState copy = state_;

        // Second acquire fence: ensure seq2 load happens after the copy.
        std::atomic_thread_fence(std::memory_order_acquire);

        const uint32_t seq2 = sequence_.load(std::memory_order_relaxed);
        if (seq1 != seq2)
            continue;   // changed under us — retry

        // First-ever state has version 0 (state_ default); treat as "not yet".
        if (copy.version == 0u)
            return std::nullopt;

        return copy;
    }
    return std::nullopt;   // contended; caller may retry next tick
}

} // namespace more_phi
