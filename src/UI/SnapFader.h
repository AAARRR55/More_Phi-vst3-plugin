/*
 * MorphSnap — UI/SnapFader.h
 * Vertical fader with slot markers for 1D morphing.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace morphsnap {

class MorphSnapProcessor;

class SnapFader : public juce::Component
{
public:
    explicit SnapFader(MorphSnapProcessor& p);
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;

private:
    void updateValue(float yPos);
    MorphSnapProcessor& proc_;
};

} // namespace morphsnap
