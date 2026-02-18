/*
 * MorphSnap — UI/MacroKnobStrip.h
 * 8 rotary knobs mapped to hosted plugin parameters, with labels.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>

namespace morphsnap {

class MorphSnapProcessor;

class MacroKnobStrip : public juce::Component,
                        private juce::Timer
{
public:
    explicit MacroKnobStrip(MorphSnapProcessor& p);
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void timerCallback() override;
    void syncKnobsToPlugin();

    MorphSnapProcessor& proc_;
    std::array<juce::Slider, 8> knobs_;
    std::array<juce::Label, 8> labels_;
    bool syncing_ = false;
};

} // namespace morphsnap
