/*
 * MorphSnap - Core/SnapshotBank.h
 * 12-slot snapshot storage with capture/recall.
 * Thread-safe for concurrent UI/audio access using seqlock pattern.
 *
 * LOCK-FREE DESIGN:
 * - Audio thread: Never blocks, uses seqlock read with retry
 * - UI thread: Can block briefly during writes using seqlock
 * - Seqlock allows concurrent reads without locks, writers get exclusive access
 *
 * MEMORY:
 * - slots_ is heap-allocated (unique_ptr) to avoid placing ~384 KB of raw
 *   parameter data on the plugin's stack and triggering stack-overflow in
 *   hosts that use small thread stacks (e.g. FL Studio).
 */
#pragma once

#include "ParameterState.h"
#include <juce_core/juce_core.h>
#include <array>
#include <memory>
#include <vector>
#include <atomic>

// Platform-specific pause instruction for spin-wait loops
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#endif

namespace morphsnap {

class ParameterBridge;  // forward

class SnapshotBank
{
public:
    static constexpr int NUM_SLOTS = 12;
    // Maximum retries for audio thread read operations
    static constexpr int MAX_READ_RETRIES = 8;

    // Heap-allocate the large slots array so it never lives on the stack
    SnapshotBank()
        : slots_(std::make_unique<std::array<ParameterState, NUM_SLOTS>>())
    {}

    // Called during prepareToPlay to set parameter upper bound for safe copies.
    void prepare(int maxParamCount);
    void capture(int slot, const ParameterBridge& bridge);
    void captureValues(int slot, const std::vector<float>& values);
    void recall(int slot, ParameterBridge& bridge) const;

    bool isOccupied(int slot) const;
    bool hasAnyOccupied() const;
    int  getOccupiedSlots(std::array<int, NUM_SLOTS>& occupiedSlots) const;
    bool getSlotValuesCopy(int slot, std::vector<float>& outValues) const;

    void clearSlot(int slot);
    void clearAll();

    // Lock-free read access for audio thread
    // Returns false if read failed after MAX_READ_RETRIES (very rare)
    // Fn signature: void(const std::array<ParameterState, NUM_SLOTS>&)
    template <typename Fn>
    bool tryReadLocked(Fn&& fn) const
    {
        // Seqlock read pattern: retry if sequence changed during read
        for (int retry = 0; retry < MAX_READ_RETRIES; ++retry)
        {
            // Load sequence - acquire ensures we see all writes before seqlock increment
            uint32_t seq1 = seqlock_.load(std::memory_order_acquire);

            // If odd, a write is in progress - retry immediately
            if ((seq1 & 1) != 0)
            {
                // Spin pause hint for better performance on x86
                #if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
                _mm_pause();
                #endif
                continue;
            }

            // Copy data - this is the critical read section
            fn(*slots_);

            // Load sequence again - acquire to ensure we see consistent state
            uint32_t seq2 = seqlock_.load(std::memory_order_acquire);

            // If sequence unchanged and even, read was consistent
            if (seq1 == seq2)
                return true;

            // Sequence changed - data was modified during read, retry
        }

        // Exhausted retries - extremely rare, indicates heavy write contention
        return false;
    }

private:
    // RAII helper to guarantee balanced begin/end write markers.
    class WriteScope
    {
    public:
        explicit WriteScope(SnapshotBank& owner) : owner_(owner) { owner_.beginWrite(); }
        ~WriteScope() { owner_.endWrite(); }

    private:
        SnapshotBank& owner_;
    };

    // Seqlock sequence counter:
    // - Even value: no write in progress, data is stable
    // - Odd value: write in progress
    // - Incremented at write start and end, so readers can detect modification
    mutable std::atomic<uint32_t> seqlock_{0};
    // Seqlock requires a single writer. Multiple non-audio writers (UI + MCP)
    // must serialize writes to keep sequence transitions valid.
    mutable juce::SpinLock writeLock_;

    // Heap-allocated to keep SnapshotBank (and its owner MorphSnapProcessor)
    // off the stack. Each ParameterState holds 2048 floats; 12 slots = ~384 KB.
    std::unique_ptr<std::array<ParameterState, NUM_SLOTS>> slots_;
    int preparedParamCount_ = 0;

    // Begin write section - increments seqlock to odd
    void beginWrite()
    {
        writeLock_.enter();
        // Increment to odd value (signals write in progress)
        seqlock_.fetch_add(1, std::memory_order_release);
    }

    // End write section - increments seqlock to even
    void endWrite()
    {
        // Increment to even value (signals write complete)
        // Release ensures all writes to slots_ are visible before seqlock update
        seqlock_.fetch_add(1, std::memory_order_release);
        writeLock_.exit();
    }

    // Helper to copy a single slot with seqlock protection
    // Used by recall() and getSlotValuesCopy()
    bool copySlotValues(int slot, float* outValues, int& outCount) const
    {
        for (int retry = 0; retry < MAX_READ_RETRIES; ++retry)
        {
            uint32_t seq1 = seqlock_.load(std::memory_order_acquire);
            if ((seq1 & 1) != 0)
            {
                #if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
                _mm_pause();
                #endif
                continue;
            }

            // Copy slot data
            bool occupied = (*slots_)[slot].occupied;
            int count = (*slots_)[slot].parameterCount;

            if (!occupied || count <= 0)
            {
                // Still need to check sequence before returning
                uint32_t seq2 = seqlock_.load(std::memory_order_acquire);
                if (seq1 == seq2)
                {
                    outCount = 0;
                    return false;
                }
                continue;
            }

            // Copy values
            std::copy_n((*slots_)[slot].values.begin(),
                        static_cast<size_t>(count),
                        outValues);
            outCount = count;

            uint32_t seq2 = seqlock_.load(std::memory_order_acquire);
            if (seq1 == seq2)
                return true;
        }
        return false;
    }
};

} // namespace morphsnap
