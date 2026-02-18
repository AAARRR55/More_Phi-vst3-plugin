/*
 * MorphSnap — UI/SnapshotRing.h
 * Interactive overlay for clicking snapshot dots to capture/recall.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Core/InterpolationEngine.h"

namespace morphsnap {

class MorphSnapProcessor;

class SnapshotRing : public juce::Component
{
public:
    explicit SnapshotRing(MorphSnapProcessor& p) : proc_(p) {}

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    int hitTestSlot(juce::Point<float> pos) const;
    MorphSnapProcessor& proc_;
};

} // namespace morphsnap
