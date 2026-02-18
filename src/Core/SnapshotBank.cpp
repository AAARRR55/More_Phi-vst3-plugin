/*
 * MorphSnap — Core/SnapshotBank.cpp
 */
#include "SnapshotBank.h"
#include "Host/ParameterBridge.h"

namespace morphsnap {

void SnapshotBank::capture(int slot, const ParameterBridge& bridge)
{
    if (slot < 0 || slot >= NUM_SLOTS) return;

    const int count = bridge.getParameterCount();
    if (count == 0) return;

    std::vector<float> values(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        values[i] = bridge.getParameterNormalized(i);

    slots_[slot].capture(values.data(), count);
}

void SnapshotBank::recall(int slot, ParameterBridge& bridge) const
{
    if (slot < 0 || slot >= NUM_SLOTS) return;
    if (!slots_[slot].occupied) return;

    bridge.applyParameterState(slots_[slot].values);
}

bool SnapshotBank::hasAnyOccupied() const
{
    for (const auto& s : slots_)
        if (s.occupied) return true;
    return false;
}

void SnapshotBank::clearAll()
{
    for (auto& s : slots_) s.clear();
}

} // namespace morphsnap
