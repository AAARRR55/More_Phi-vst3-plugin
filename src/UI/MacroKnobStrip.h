/*
 * More-Phi — UI/MacroKnobStrip.h
 * 8 rotary knobs mapped to hosted plugin parameters, with labels.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>

namespace more_phi {

class MorePhiProcessor;

class MacroKnobStrip : public juce::Component,
                        private juce::Timer
{
public:
    explicit MacroKnobStrip(MorePhiProcessor& p);
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void timerCallback() override;
    void syncKnobsToPlugin();

    MorePhiProcessor& proc_;
    std::array<juce::Slider, 8> knobs_;
    std::array<juce::Label, 8> labels_;
    bool syncing_ = false;
};

} // namespace more_phi
