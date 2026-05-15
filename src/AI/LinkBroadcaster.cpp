/*
 * More-Phi — AI/LinkBroadcaster.cpp
 *
 * Platform-native shared memory for cross-instance morph sync.
 * Windows: CreateFileMappingW / MapViewOfFile
 * macOS/Linux: shm_open / mmap
 *
 * The seqlock protocol ensures real-time safe reads:
 * - Leader increments seqlock to odd before write, increments to even after.
 * - Followers read seqlock, copy data, re-read seqlock — retry if changed.
 * - No mutexes, no allocations, no system calls in the hot path.
 */
#include "LinkBroadcaster.h"
#include <cstring>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

namespace more_phi {

LinkBroadcaster::LinkBroadcaster() = default;

LinkBroadcaster::~LinkBroadcaster()
{
    detach();
}

juce::String LinkBroadcaster::getShmName() const
{
    return juce::String(SHM_NAME_PREFIX) + juce::String(groupId_);
}

// ── Platform: Windows ─────────────────────────────────────────────────────────
#if JUCE_WINDOWS

bool LinkBroadcaster::attach(uint32_t groupId)
{
    if (sharedMem_) return true;  // Already attached
    groupId_ = groupId;

    auto name = getShmName();
    auto wName = name.toWideCharPointer();

    // Try opening existing, or create new
    hMapFile_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wName);
    if (hMapFile_ == nullptr)
    {
        hMapFile_ = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, sizeof(LinkStateBlock), wName);
    }

    if (hMapFile_ == nullptr)
    {
        DBG("LinkBroadcaster: failed to create/open shared memory");
        return false;
    }

    sharedMem_ = static_cast<LinkStateBlock*>(
        MapViewOfFile(hMapFile_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LinkStateBlock)));

    if (sharedMem_ == nullptr)
    {
        CloseHandle(hMapFile_);
        hMapFile_ = nullptr;
        DBG("LinkBroadcaster: failed to map view of shared memory");
        return false;
    }

    DBG("LinkBroadcaster: attached to shared memory group " + juce::String(groupId));
    return true;
}

void LinkBroadcaster::detach()
{
    // H-10 FIX: Disable flags BEFORE unmapping to prevent other threads
    // from accessing shared memory during teardown. The release fence
    // ensures the flag writes are visible before the unmap proceeds.
    enabled_ = false;
    isLeader_ = false;
    std::atomic_thread_fence(std::memory_order_release);

    if (sharedMem_)
    {
        // If we were the leader, clear the leader hash
        uint32_t expected = instanceHash_;
        sharedMem_->leaderHash.compare_exchange_strong(expected, 0u,
            std::memory_order_release, std::memory_order_relaxed);

        UnmapViewOfFile(sharedMem_);
        sharedMem_ = nullptr;
    }
    if (hMapFile_)
    {
        CloseHandle(hMapFile_);
        hMapFile_ = nullptr;
    }
}

// ── Platform: macOS / Linux ───────────────────────────────────────────────────
#else

bool LinkBroadcaster::attach(uint32_t groupId)
{
    if (sharedMem_) return true;
    groupId_ = groupId;

    auto name = "/" + getShmName();
    auto nameUtf8 = name.toRawUTF8();

    // Try opening existing, or create new
    shmFd_ = shm_open(nameUtf8, O_RDWR, 0666);
    if (shmFd_ < 0)
    {
        shmFd_ = shm_open(nameUtf8, O_CREAT | O_RDWR, 0666);
        if (shmFd_ < 0)
        {
            DBG("LinkBroadcaster: shm_open failed");
            return false;
        }
        if (ftruncate(shmFd_, sizeof(LinkStateBlock)) != 0)
        {
            close(shmFd_);
            shmFd_ = -1;
            shm_unlink(nameUtf8);
            DBG("LinkBroadcaster: ftruncate failed");
            return false;
        }
    }

    sharedMem_ = static_cast<LinkStateBlock*>(
        mmap(nullptr, sizeof(LinkStateBlock), PROT_READ | PROT_WRITE,
             MAP_SHARED, shmFd_, 0));

    if (sharedMem_ == MAP_FAILED)
    {
        sharedMem_ = nullptr;
        close(shmFd_);
        shmFd_ = -1;
        DBG("LinkBroadcaster: mmap failed");
        return false;
    }

    DBG("LinkBroadcaster: attached to shared memory group " + juce::String(groupId));
    return true;
}

void LinkBroadcaster::detach()
{
    // H-10 FIX: Disable flags BEFORE unmapping
    enabled_ = false;
    isLeader_ = false;
    std::atomic_thread_fence(std::memory_order_release);

    if (sharedMem_)
    {
        uint32_t expected = instanceHash_;
        sharedMem_->leaderHash.compare_exchange_strong(expected, 0u,
            std::memory_order_release, std::memory_order_relaxed);

        munmap(sharedMem_, sizeof(LinkStateBlock));
        sharedMem_ = nullptr;
    }
    if (shmFd_ >= 0)
    {
        close(shmFd_);
        shmFd_ = -1;
    }
}

#endif  // Platform selection

// ── Shared logic ──────────────────────────────────────────────────────────────

void LinkBroadcaster::setLeader(bool isLeader, uint32_t instanceHash)
{
    isLeader_ = isLeader;
    instanceHash_ = instanceHash;

    if (sharedMem_ && isLeader)
        sharedMem_->leaderHash.store(instanceHash, std::memory_order_release);
}

void LinkBroadcaster::broadcast(float x, float y) noexcept
{
    if (!enabled_ || !isLeader_ || !sharedMem_) return;

    // 1. Update heartbeat (using thread-safe millisecond counter)
    sharedMem_->lastActivity.store(
        static_cast<uint64_t>(juce::Time::currentTimeMillis()), 
        std::memory_order_relaxed);

    // 2. Seqlock write: increment to odd (signals write in progress)
    sharedMem_->seqlock.fetch_add(1, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);

    sharedMem_->morphX.store(x, std::memory_order_relaxed);
    sharedMem_->morphY.store(y, std::memory_order_relaxed);

    // 3. Increment to even (signals write complete)
    std::atomic_thread_fence(std::memory_order_release);
    sharedMem_->seqlock.fetch_add(1, std::memory_order_release);
}

bool LinkBroadcaster::receive(float& x, float& y) const noexcept
{
    if (!enabled_ || isLeader_ || !sharedMem_) return false;
    if (sharedMem_->leaderHash.load(std::memory_order_acquire) == 0) return false;

    // Check heartbeat: if leader hasn't updated in 100ms, assume stale
    const uint64_t now = static_cast<uint64_t>(juce::Time::currentTimeMillis());
    const uint64_t last = sharedMem_->lastActivity.load(std::memory_order_relaxed);
    if (now > last + 100) return false;

    // Seqlock read with retry
    for (int retry = 0; retry < 16; ++retry)
    {
        uint32_t seq1 = sharedMem_->seqlock.load(std::memory_order_acquire);
        if ((seq1 & 1) != 0)
        {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
            _mm_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
            __asm__ volatile("yield");
#endif
            continue;
        }

        std::atomic_thread_fence(std::memory_order_acquire);
        float rx = sharedMem_->morphX.load(std::memory_order_relaxed);
        float ry = sharedMem_->morphY.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);

        uint32_t seq2 = sharedMem_->seqlock.load(std::memory_order_acquire);
        if (seq1 == seq2)
        {
            x = rx;
            y = ry;
            return true;
        }
    }

    return false;
}

} // namespace more_phi
