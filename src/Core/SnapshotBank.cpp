/*
 * MorphSnap — Core/SnapshotBank.cpp
 * Thread-safe snapshot storage with fixed-capacity parameter buffers.
 * Uses seqlock for lock-free reads on audio thread.
 *
 * slots_ is heap-allocated via unique_ptr (see header). All accesses that
 * previously used `slots_[i]` or `for (auto& s : slots_)` now dereference
 * via `(*slots_)[i]` / `for (auto& s : *slots_)`.
 */
#include "SnapshotBank.h"
#include "Host/ParameterBridge.h"
#include <algorithm>

namespace morphsnap {

void SnapshotBank::prepare(int maxParamCount)
{
    WriteScope write(*this);
    preparedParamCount_ = (maxParamCount > MAX_PARAMETERS) ? MAX_PARAMETERS : maxParamCount;
}

void SnapshotBank::capture(int slot, const ParameterBridge& bridge)
{
    if (slot < 0 || slot >= NUM_SLOTS) return;

    const int count = bridge.getParameterCount();
    if (count == 0) return;

    // Use pre-allocated scratch buffer - NO ALLOCATION
    const int limit = (preparedParamCount_ > 0) ? preparedParamCount_ : MAX_PARAMETERS;
    const int safeCount = juce::jmin(count, limit);
    std::array<float, MAX_PARAMETERS> values{};
    for (int i = 0; i < safeCount; ++i)
        values[static_cast<size_t>(i)] = bridge.getParameterNormalized(i);

    WriteScope write(*this);
    (*slots_)[slot].capture(values.data(), safeCount);
}

void SnapshotBank::captureValues(int slot, const std::vector<float>& values)
{
    if (slot < 0 || slot >= NUM_SLOTS) return;
    if (values.empty()) return;

    const int limit = (preparedParamCount_ > 0) ? preparedParamCount_ : MAX_PARAMETERS;
    const int safeCount = juce::jmin(static_cast<int>(values.size()), limit);
    std::array<float, MAX_PARAMETERS> scratch{};
    std::copy_n(values.begin(), static_cast<size_t>(safeCount), scratch.begin());

    WriteScope write(*this);
    (*slots_)[slot].capture(scratch.data(), safeCount);
}

void SnapshotBank::recall(int slot, ParameterBridge& bridge) const
{
    if (slot < 0 || slot >= NUM_SLOTS) return;

    std::array<float, MAX_PARAMETERS> values{};
    int parameterCount = 0;

    // Use lock-free seqlock read
    if (!copySlotValues(slot, values.data(), parameterCount))
        return;

    if (parameterCount > 0)
        bridge.applyParameterState(values.data(), parameterCount);
}

bool SnapshotBank::isOccupied(int slot) const
{
    if (slot < 0 || slot >= NUM_SLOTS) return false;

    // Lock-free read using seqlock
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

        bool occupied = (*slots_)[slot].occupied;

        uint32_t seq2 = seqlock_.load(std::memory_order_acquire);
        if (seq1 == seq2)
            return occupied;
    }
    return false;
}

bool SnapshotBank::hasAnyOccupied() const
{
    // Lock-free read using seqlock
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

        bool anyOccupied = false;
        for (const auto& s : *slots_)
        {
            if (s.occupied)
            {
                anyOccupied = true;
                break;
            }
        }

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
            #if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
            _mm_pause();
            #endif
            continue;
        }

        int occupiedCount = 0;
        for (int i = 0; i < NUM_SLOTS; ++i)
        {
            if ((*slots_)[i].occupied)
                occupiedSlots[occupiedCount++] = i;
        }

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

    // Lock-free read using seqlock
    std::array<float, MAX_PARAMETERS> values{};
    int count = 0;

    if (!copySlotValues(slot, values.data(), count))
        return false;

    if (count <= 0)
        return false;

    outValues.assign(values.begin(), values.begin() + count);
    return true;
}

void SnapshotBank::clearSlot(int slot)
{
    if (slot < 0 || slot >= NUM_SLOTS) return;

    WriteScope write(*this);
    (*slots_)[slot].clear();
}

void SnapshotBank::clearAll()
{
    WriteScope write(*this);
    for (auto& s : *slots_) s.clear();
}

} // namespace morphsnap
