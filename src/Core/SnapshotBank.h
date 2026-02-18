/*
 * MorphSnap — Core/SnapshotBank.h
 * 12-slot snapshot storage with capture/recall.
 */
#pragma once

#include "ParameterState.h"
#include <array>

namespace morphsnap {

class ParameterBridge;  // forward

class SnapshotBank
{
public:
    static constexpr int NUM_SLOTS = 12;

    void capture(int slot, const ParameterBridge& bridge);
    void recall(int slot, ParameterBridge& bridge) const;

    const ParameterState& getSlot(int index) const { return slots_[index]; }
    ParameterState&       getSlot(int index)       { return slots_[index]; }

    bool isOccupied(int slot) const { return slots_[slot].occupied; }
    bool hasAnyOccupied() const;

    void clearSlot(int slot) { slots_[slot].clear(); }
    void clearAll();

private:
    std::array<ParameterState, NUM_SLOTS> slots_;
};

} // namespace morphsnap
