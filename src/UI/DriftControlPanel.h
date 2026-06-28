/*
 * More-Phi — UI/DriftControlPanel.h
 *
 * Horizontal strip exposing the Drift physics-mode parameters
 * (driftSpeed, driftDistance, driftChaos). These take effect whenever the
 * ModeBar physics mode is set to "Drift". Placed in the Engine tab.
 *
 * All controls route through APVTS for DAW automation support.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>

namespace more_phi {

class MorePhiProcessor;

class DriftControlPanel : public juce::Component,
                           private juce::Timer
{
public:
    explicit DriftControlPanel(MorePhiProcessor& proc);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void setupKnob(juce::Slider& knob, double min, double max,
                   double defaultVal, const juce::String& suffix);
    void drawSectionLabel(juce::Graphics& g, const juce::String& text,
                          juce::Rectangle<int> bounds) const;

    MorePhiProcessor& proc_;

    // Speed, Distance, Chaos
    std::array<juce::Slider, 3> knobs_;
    std::array<juce::Label,  3> knobLabels_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DriftControlPanel)
};

} // namespace more_phi
