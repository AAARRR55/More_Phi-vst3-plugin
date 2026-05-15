/*
 * More-Phi — Core/AllocationTracker.h
 * Debug-only allocation tracking for audio thread safety verification.
 *
 * USAGE:
 *   1. In debug builds, this tracks allocations occurring in the audio thread.
 *   2. Call AllocationTracker::beginAudioCallback() at start of processBlock.
 *   3. Call AllocationTracker::endAudioCallback() at end of processBlock.
 *   4. Any allocation during the audio callback will be logged.
 *
 * In release builds, all tracking is disabled for zero overhead.
 */
#pragma once

#include <atomic>

#if JUCE_DEBUG && MORE_PHI_TRACK_ALLOCATIONS
    #define MORE_PHI_TRACK_ALLOC 1
#else
    #define MORE_PHI_TRACK_ALLOC 0
#endif

namespace more_phi {

class AllocationTracker
{
public:
#if MORE_PHI_TRACK_ALLOC

    static void beginAudioCallback()
    {
        inAudioCallback_.store(true, std::memory_order_relaxed);
    }

    static void endAudioCallback()
    {
        inAudioCallback_.store(false, std::memory_order_relaxed);
    }

    static bool isInAudioCallback()
    {
        return inAudioCallback_.load(std::memory_order_relaxed);
    }

    static void recordAllocation(size_t /*size*/, const char* /*location*/)
    {
        if (isInAudioCallback())
        {
            allocationCount_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    static int getAndResetAllocationCount()
    {
        return allocationCount_.exchange(0, std::memory_order_relaxed);
    }

private:
    static inline std::atomic<bool> inAudioCallback_{false};
    static inline std::atomic<int> allocationCount_{0};

#else
    // Release build: no-op stubs with zero overhead
    static void beginAudioCallback() {}
    static void endAudioCallback() {}
    static bool isInAudioCallback() { return false; }
    static void recordAllocation(size_t, const char*) {}
    static int getAndResetAllocationCount() { return 0; }
#endif
};

// ── Scoped Audio Callback Guard ────────────────────────────────────────────────

class ScopedAudioCallback
{
public:
    ScopedAudioCallback()  { AllocationTracker::beginAudioCallback(); }
    ~ScopedAudioCallback() { AllocationTracker::endAudioCallback(); }
};

// ── Custom new/delete operators (debug only) ────────────────────────────────────

#if MORE_PHI_TRACK_ALLOC

// Override global new/delete to track allocations
// Note: This is intrusive and may conflict with JUCE's memory management
// Use with caution in isolated debug builds

inline void* tracked_new(size_t size, const char* location)
{
    AllocationTracker::recordAllocation(size, location);
    return std::malloc(size);
}

inline void tracked_delete(void* ptr)
{
    std::free(ptr);
}

// Macros for location-aware allocation
#define MORE_PHI_NEW tracked_new(__FILE__, __LINE__)

#endif // MORE_PHI_TRACK_ALLOC

} // namespace more_phi
