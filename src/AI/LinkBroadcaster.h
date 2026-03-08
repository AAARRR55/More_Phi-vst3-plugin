/*
 * MorphSnap — AI/LinkBroadcaster.h
 *
 * Cross-instance morph position synchronization using shared memory.
 * One instance acts as leader (writes), others as followers (read).
 *
 * DESIGN:
 * - Uses a named shared memory block ("MorphSnap_LinkState")
 * - Layout: [uint32 seqlock] [float x] [float y] [uint32 leaderHash]
 * - Leader writes position every processBlock
 * - Followers read with seqlock retry (lock-free, real-time safe)
 * - JUCE MemoryMappedFile not suitable here (file-backed, not anonymous)
 *   → Use platform-native shared memory instead
 */
#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <cstring>

#if JUCE_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
#elif JUCE_MAC || JUCE_LINUX
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace morphsnap {

/** Shared memory layout for link state. */
struct alignas(64) LinkStateBlock
{
    std::atomic<uint32_t> seqlock{0};
    float morphX    = 0.5f;
    float morphY    = 0.5f;
    uint32_t leaderHash = 0;   // Hash of leader's instanceId
    uint32_t groupId    = 0;   // Link group ID (0 = default group)
};

static_assert(sizeof(LinkStateBlock) <= 64, "LinkStateBlock must fit in one cache line");

class LinkBroadcaster
{
public:
    LinkBroadcaster();
    ~LinkBroadcaster();

    /** Attach to (or create) the shared memory region for a link group. */
    bool attach(uint32_t groupId = 0);

    /** Detach from shared memory. */
    void detach();

    /** Set this instance as leader. */
    void setLeader(bool isLeader, uint32_t instanceHash = 0);
    bool isLeader() const { return isLeader_; }

    /** Write morph position (leader only, called from audio thread). */
    void broadcast(float x, float y) noexcept;

    /** Read morph position (followers, called from audio thread).
     *  Returns false if read failed or no leader is active. */
    bool receive(float& x, float& y) const noexcept;

    /** Check if link is active (shared memory attached). */
    bool isAttached() const { return sharedMem_ != nullptr; }

    /** Enable/disable link mode. */
    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool isEnabled() const { return enabled_; }

private:
    LinkStateBlock* sharedMem_ = nullptr;
    bool isLeader_ = false;
    bool enabled_  = false;
    uint32_t instanceHash_ = 0;
    uint32_t groupId_ = 0;

#if JUCE_WINDOWS
    HANDLE hMapFile_ = nullptr;
#else
    int shmFd_ = -1;
#endif

    static constexpr const char* SHM_NAME_PREFIX = "MorphSnap_Link_";
    juce::String getShmName() const;
};

} // namespace morphsnap
