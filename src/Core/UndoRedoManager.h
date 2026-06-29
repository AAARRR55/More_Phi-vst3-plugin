#pragma once
#include "ParameterState.h"
#include <juce_core/juce_core.h>
#include <array>
#include <functional>
#include <vector>

namespace more_phi {

class SnapshotBank;

struct UndoRecord
{
    int slot = -1;
    ParameterState before;
    juce::String label;
};

class UndoRedoManager
{
public:
    static constexpr int kMaxDepth = 32;

    using ChangeCallback = std::function<void()>;

    UndoRedoManager() = default;

    void pushSnapshotState(int slot, const ParameterState& before,
                           const juce::String& label = {});
    bool undo(SnapshotBank& bank);
    bool redo(SnapshotBank& bank);
    void clear();

    bool canUndo() const noexcept { return undoPos_ > 0; }
    bool canRedo() const noexcept { return static_cast<size_t>(undoPos_) < records_.size(); }
    int getUndoCount() const noexcept { return undoPos_; }
    int getRedoCount() const noexcept
    {
        return static_cast<int>(records_.size()) - undoPos_;
    }

    void onChange(ChangeCallback cb) { onChange_ = std::move(cb); }

private:
    std::vector<UndoRecord> records_;
    int undoPos_ = 0;

    ChangeCallback onChange_;
};

} // namespace more_phi
