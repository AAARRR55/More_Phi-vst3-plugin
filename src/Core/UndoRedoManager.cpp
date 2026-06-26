#include "UndoRedoManager.h"
#include "SnapshotBank.h"

namespace more_phi {

void UndoRedoManager::pushSnapshotState(int slot, const ParameterState& before,
                                        const juce::String& label)
{
    if (slot < 0 || slot >= SnapshotBank::NUM_SLOTS)
        return;

    // If we are not at the end, truncate redo history
    if (static_cast<size_t>(undoPos_) < records_.size())
        records_.resize(static_cast<size_t>(undoPos_));

    UndoRecord rec;
    rec.slot = slot;
    rec.before = before;
    rec.label = label.isNotEmpty() ? label
                                   : ("Snapshot " + juce::String(slot + 1));

    if (records_.size() >= static_cast<size_t>(kMaxDepth))
        records_.erase(records_.begin());

    records_.push_back(std::move(rec));
    undoPos_ = static_cast<int>(records_.size());

    if (onChange_)
        onChange_();
}

bool UndoRedoManager::undo(SnapshotBank& bank)
{
    if (undoPos_ <= 0)
        return false;

    --undoPos_;
    auto& rec = records_[static_cast<size_t>(undoPos_)];
    bank.captureValuesWithNames(rec.slot, rec.before.values.data(),
                                rec.before.parameterCount,
                                {});

    if (onChange_)
        onChange_();
    return true;
}

bool UndoRedoManager::redo(SnapshotBank& bank)
{
    if (static_cast<size_t>(undoPos_) >= records_.size())
        return false;

    auto& rec = records_[static_cast<size_t>(undoPos_)];
    ++undoPos_;
    bank.captureValuesWithNames(rec.slot, rec.before.values.data(),
                                rec.before.parameterCount,
                                {});

    if (onChange_)
        onChange_();
    return true;
}

void UndoRedoManager::clear()
{
    records_.clear();
    undoPos_ = 0;
    if (onChange_)
        onChange_();
}

} // namespace more_phi
