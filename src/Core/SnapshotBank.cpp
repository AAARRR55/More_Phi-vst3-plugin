/*
 * More-Phi — Core/SnapshotBank.cpp
 * Thread-safe snapshot storage with fixed-capacity parameter buffers.
 * Uses seqlock for lock-free reads on audio thread.
 *
 * slots_ is heap-allocated via unique_ptr (see header). All accesses that
 * previously used `slots_[i]` or `for (auto& s : slots_)` now dereference
 * via `(*slots_)[i]` / `for (auto& s : *slots_)`.
 */
#include "SnapshotBank.h"
#include "Host/IPluginHostManager.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <algorithm>

namespace more_phi {

// Single thread_local scratch buffer shared by all capture/recall functions.
// Thread_local guarantees no data races across threads; using one buffer
// instead of five saves ~32 KB per thread (~128 KB across 4 threads).
// Not re-entrant (none of these functions call each other), so one is safe.
thread_local std::array<float, MAX_PARAMETERS> tlsScratch;

void SnapshotBank::prepare(int maxParamCount)
{
    WriteScope write(*this);
    preparedParamCount_.store((maxParamCount > MAX_PARAMETERS) ? MAX_PARAMETERS : maxParamCount,
                              std::memory_order_release);
}

void SnapshotBank::capture(int slot, const IParameterBridge& bridge)
{
    if (slot < 0 || slot >= NUM_SLOTS) return;

    const int count = bridge.getParameterCount();
    if (count == 0) return;

    const int prepared = preparedParamCount_.load(std::memory_order_acquire);
    const int limit = (prepared > 0) ? prepared : MAX_PARAMETERS;
    const int safeCount = juce::jmin(count, limit);
    tlsScratch.fill(0.0f);

    // PERF-C1+BATCH: Read parameter values AND names via single plugin
    // acquisitions (captureAllNormalized / captureAllNames) instead of one
    // acquirePluginForUse cycle per parameter. For a 2048-param plugin this
    // cuts ~4096 atomic lock cycles to 2 per capture — the contention the
    // PERF-C1 comment blamed for FL Studio stalls. Both reads stay OUTSIDE
    // the WriteScope (do not regress the seqlock-writer hold-time invariant).
    bridge.captureAllNormalized(tlsScratch.data(), safeCount);

    juce::StringArray namesCapture;
    bridge.captureAllNames(namesCapture, safeCount);

    WriteScope write(*this);
    (*slots_)[slot].capture(tlsScratch.data(), safeCount);

    paramNames_[slot] = std::move(namesCapture);
}

void SnapshotBank::captureValues(int slot, const std::vector<float>& values)
{
    if (slot < 0 || slot >= NUM_SLOTS) return;
    if (values.empty()) return;

    const int prepared = preparedParamCount_.load(std::memory_order_acquire);
    const int limit = (prepared > 0) ? prepared : MAX_PARAMETERS;
    const int safeCount = juce::jmin(static_cast<int>(values.size()), limit);
    tlsScratch.fill(0.0f);
    std::copy_n(values.begin(), static_cast<size_t>(safeCount), tlsScratch.begin());

    WriteScope write(*this);
    (*slots_)[slot].capture(tlsScratch.data(), safeCount);
}

void SnapshotBank::captureValuesWithNames(int slot,
                                          const float* values,
                                          int count,
                                          const juce::StringArray& names)
{
    if (slot < 0 || slot >= NUM_SLOTS) return;
    if (values == nullptr || count <= 0) return;

    const int prepared = preparedParamCount_.load(std::memory_order_acquire);
    const int limit = (prepared > 0) ? prepared : MAX_PARAMETERS;
    const int safeCount = juce::jmin(count, limit, MAX_PARAMETERS);

    tlsScratch.fill(0.0f);
    std::copy_n(values, static_cast<size_t>(safeCount), tlsScratch.begin());

    // PERF-C1: Prepare names before WriteScope — StringArray::add/clear can
    // heap-allocate, and the names input is already available outside the lock.
    juce::StringArray namesCapture;
    namesCapture.ensureStorageAllocated(safeCount);
    for (int i = 0; i < safeCount && i < names.size(); ++i)
        namesCapture.add(names[i]);

    WriteScope write(*this);
    (*slots_)[slot].capture(tlsScratch.data(), safeCount);
    paramNames_[slot] = std::move(namesCapture);
}

void SnapshotBank::recall(int slot, IParameterBridge& bridge) const
{
    if (slot < 0 || slot >= NUM_SLOTS) return;

    int parameterCount = 0;

    if (!copySlotValues(slot, tlsScratch.data(), parameterCount))
        return;

    if (parameterCount > 0)
        bridge.applyParameterState(tlsScratch.data(), parameterCount);
}

void SnapshotBank::recallFast(int slot, IParameterBridge& bridge) const
{
    if (slot < 0 || slot >= NUM_SLOTS) return;

    int parameterCount = 0;

    if (!copySlotValues(slot, tlsScratch.data(), parameterCount))
        return;

    if (parameterCount > 0)
        bridge.applyParameterState(tlsScratch.data(), parameterCount);
}

bool SnapshotBank::captureStateChunk(int slot, juce::AudioPluginInstance* plugin)
{
    if (slot < 0 || slot >= NUM_SLOTS || plugin == nullptr) return false;

    juce::MemoryBlock chunk;
    try
    {
        plugin->getStateInformation(chunk);
    }
    catch (const std::exception& e)
    {
#if JUCE_DEBUG
        DBG("SnapshotBank::captureStateChunk — getStateInformation failed: "
            + juce::String(e.what()));
#endif
        return false;
    }
    catch (...)
    {
#if JUCE_DEBUG
        DBG("SnapshotBank::captureStateChunk — getStateInformation failed: unknown exception");
#endif
        return false;
    }

    // Phase 7: cache the plugin UID from the hosted plugin description
    // Lock order: WriteScope (writeLock_) → chunksLock_ (consistent with fromXml).
    const juce::PluginDescription& desc = plugin->getPluginDescription();
    const juce::String uid = desc.name + "|" + desc.manufacturerName;

    WriteScope write(*this);
    {
        const juce::SpinLock::ScopedLockType chunkLock(chunksLock_);
        stateChunks_[static_cast<size_t>(slot)] = std::move(chunk);
        cachedPluginUID_[static_cast<size_t>(slot)] = uid;
    }
    return true;
}

void SnapshotBank::captureStateChunk(int slot, const juce::MemoryBlock& chunk)
{
    if (slot >= 0 && slot < NUM_SLOTS)
    {
        WriteScope write(*this);
        const juce::SpinLock::ScopedLockType chunkLock(chunksLock_);
        stateChunks_[slot] = chunk;
    }
}

bool SnapshotBank::recallStateChunk(int slot, juce::AudioPluginInstance* plugin) const
{
    if (slot < 0 || slot >= NUM_SLOTS || plugin == nullptr) return false;

    // CRITICAL (Finding 4): Protect stateChunks_ read with seqlock.
    // Use tryReadLocked pattern for lock-free read with retry on concurrent write.
    juce::MemoryBlock chunkCopy;
    bool hasChunk = false;

    if (copyStateChunk(slot, chunkCopy))
    {
        hasChunk = true;
    }

    if (hasChunk && chunkCopy.getSize() > 0)
    {
        try
        {
            plugin->setStateInformation(chunkCopy.getData(), static_cast<int>(chunkCopy.getSize()));
        }
        catch (const std::exception& e)
        {
#if JUCE_DEBUG
            DBG("SnapshotBank::recallStateChunk — setStateInformation failed: "
                + juce::String(e.what()));
#endif
            return false;
        }
        catch (...)
        {
#if JUCE_DEBUG
            DBG("SnapshotBank::recallStateChunk — setStateInformation failed: unknown exception");
#endif
            return false;
        }
    }
    return true;
}

bool SnapshotBank::recallStateChunk(int slot, juce::AudioProcessor* plugin) const
{
    if (slot < 0 || slot >= NUM_SLOTS || plugin == nullptr) return false;

    // CRITICAL (Finding 4): Protect stateChunks_ read with seqlock.
    // Use tryReadLocked pattern for lock-free read with retry on concurrent write.
    juce::MemoryBlock chunkCopy;
    bool hasChunk = false;

    if (copyStateChunk(slot, chunkCopy))
    {
        hasChunk = true;
    }

    if (hasChunk && chunkCopy.getSize() > 0)
    {
        try
        {
            plugin->setStateInformation(chunkCopy.getData(), static_cast<int>(chunkCopy.getSize()));
        }
        catch (const std::exception& e)
        {
#if JUCE_DEBUG
            DBG("SnapshotBank::recallStateChunk — setStateInformation failed: "
                + juce::String(e.what()));
#endif
            return false;
        }
        catch (...)
        {
#if JUCE_DEBUG
            DBG("SnapshotBank::recallStateChunk — setStateInformation failed: unknown exception");
#endif
            return false;
        }
    }
    return true;
}

bool SnapshotBank::isOccupied(int slot) const noexcept
{
    if (slot < 0 || slot >= NUM_SLOTS) return false;

    // Lock-free read using seqlock
    for (int retry = 0; retry < MAX_READ_RETRIES; ++retry)
    {
        uint32_t seq1 = seqlock_.load(std::memory_order_acquire);
        if ((seq1 & 1) != 0)
        {
            spinPause();
            continue;
        }

        bool occupied = (*slots_)[slot].occupied;

        // C-1: acquire fence pairs with writer's release fence (seqlock
        // correctness on weakly-ordered CPUs; see tryReadLocked).
        std::atomic_thread_fence(std::memory_order_acquire);
        uint32_t seq2 = seqlock_.load(std::memory_order_acquire);
        if (seq1 == seq2)
            return occupied;
    }
    return false;
}

bool SnapshotBank::hasAnyOccupied() const noexcept
{
    // Lock-free read using seqlock
    for (int retry = 0; retry < MAX_READ_RETRIES; ++retry)
    {
        uint32_t seq1 = seqlock_.load(std::memory_order_acquire);
        if ((seq1 & 1) != 0)
        {
            spinPause();
            continue;
        }

        bool anyOccupied = false;
        for (const auto& s : *slots_)
        {
            if (s.occupied)
            {
                anyOccupied = true;
                break;
            }
        }

        // C-1: acquire fence pairs with writer's release fence (seqlock
        // correctness on weakly-ordered CPUs; see tryReadLocked).
        std::atomic_thread_fence(std::memory_order_acquire);
        uint32_t seq2 = seqlock_.load(std::memory_order_acquire);
        if (seq1 == seq2)
            return anyOccupied;
    }
    return false;
}

int SnapshotBank::getOccupiedSlots(std::array<int, NUM_SLOTS>& occupiedSlots) const
{
    // Lock-free read using seqlock
    for (int retry = 0; retry < MAX_READ_RETRIES; ++retry)
    {
        uint32_t seq1 = seqlock_.load(std::memory_order_acquire);
        if ((seq1 & 1) != 0)
        {
            spinPause();
            continue;
        }

        int occupiedCount = 0;
        for (int i = 0; i < NUM_SLOTS; ++i)
        {
            if ((*slots_)[i].occupied)
                occupiedSlots[occupiedCount++] = i;
        }

        // C-1: acquire fence pairs with writer's release fence (seqlock
        // correctness on weakly-ordered CPUs; see tryReadLocked).
        std::atomic_thread_fence(std::memory_order_acquire);
        uint32_t seq2 = seqlock_.load(std::memory_order_acquire);
        if (seq1 == seq2)
            return occupiedCount;
    }
    return 0;
}

bool SnapshotBank::getSlotValuesCopy(int slot, std::vector<float>& outValues) const
{
    if (slot < 0 || slot >= NUM_SLOTS)
        return false;

    // This public helper allocates into std::vector and is used by UI/MCP/tests,
    // not the audio thread. Serialize it with writers to avoid C++ data races on
    // the non-atomic ParameterState storage while keeping recall() lock-free.
    const juce::SpinLock::ScopedLockType lock(writeLock_);
    const auto& state = (*slots_)[slot];
    if (!state.occupied || state.parameterCount <= 0)
        return false;

    const int count = juce::jmin(state.parameterCount, MAX_PARAMETERS);
    outValues.assign(state.values.begin(), state.values.begin() + count);
    return true;
}

bool SnapshotBank::copyStateChunk(int slot, juce::MemoryBlock& outChunk) const
{
    if (slot < 0 || slot >= NUM_SLOTS)
        return false;

    return copyStateChunkInternal(slot, outChunk);
}

bool SnapshotBank::hasStateChunk(int slot) const
{
    if (slot < 0 || slot >= NUM_SLOTS)
        return false;

    juce::MemoryBlock chunk;
    return copyStateChunkInternal(slot, chunk);
}

// ── Gravity Wells ───────────────────────────────────────────────────────────────

void SnapshotBank::setMass(int slot, float mass)
{
    if (slot < 0 || slot >= NUM_SLOTS) return;
    WriteScope write(*this);
    (*slots_)[slot].mass = std::clamp(mass, 0.1f, 3.0f);
}

float SnapshotBank::getMass(int slot) const noexcept
{
    if (slot < 0 || slot >= NUM_SLOTS) return 1.0f;
    for (int retry = 0; retry < MAX_READ_RETRIES; ++retry)
    {
        uint32_t seq1 = seqlock_.load(std::memory_order_acquire);
        if ((seq1 & 1) != 0) { spinPause(); continue; }
        float m = (*slots_)[slot].mass;
        std::atomic_thread_fence(std::memory_order_acquire);
        uint32_t seq2 = seqlock_.load(std::memory_order_acquire);
        if (seq1 == seq2)
            return m;
    }
    return 1.0f;
}

void SnapshotBank::getMasses(std::array<float, NUM_SLOTS>& masses) const noexcept
{
    masses.fill(1.0f);
    for (int retry = 0; retry < MAX_READ_RETRIES; ++retry)
    {
        uint32_t seq1 = seqlock_.load(std::memory_order_acquire);
        if ((seq1 & 1) != 0) { spinPause(); continue; }
        for (int i = 0; i < NUM_SLOTS; ++i)
            masses[static_cast<size_t>(i)] = (*slots_)[i].mass;
        std::atomic_thread_fence(std::memory_order_acquire);
        uint32_t seq2 = seqlock_.load(std::memory_order_acquire);
        if (seq1 == seq2)
            return;
    }
}

void SnapshotBank::clearSlot(int slot)
{
    if (slot < 0 || slot >= NUM_SLOTS) return;

    WriteScope write(*this);
    (*slots_)[slot].clear();
    paramNames_[slot].clear();
    {
        const juce::SpinLock::ScopedLockType chunkLock(chunksLock_);
        stateChunks_[static_cast<size_t>(slot)].reset();
        cachedPluginUID_[static_cast<size_t>(slot)].clear();
    }
}

void SnapshotBank::clearAll()
{
    WriteScope write(*this);
    for (auto& s : *slots_) s.clear();
    for (auto& n : paramNames_) n.clear();
    {
        const juce::SpinLock::ScopedLockType chunkLock(chunksLock_);
        for (auto& chunk : stateChunks_)
            chunk.reset();
        for (auto& uid : cachedPluginUID_)
            uid.clear();
    }
}

} // namespace more_phi
