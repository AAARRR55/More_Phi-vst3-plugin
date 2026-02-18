/*
 * MorphSnap — UI/ModeBar.h
 * Physics mode toggles: Direct | Elastic | Drift + physics knobs.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>

namespace morphsnap {

class MorphSnapProcessor;

class ModeBar : public juce::Component,
                private juce::Button::Listener
{
public:
    explicit ModeBar(MorphSnapProcessor& p);
    void resized() override;
    void paint(juce::Graphics& g) override;

    // Callbacks for visualization options
    std::function<void(bool)> onGridVisibleChanged;
    std::function<void(bool)> onPathVisibleChanged;
    std::function<void(int)> onVisualizationModeChanged;

private:
    void buttonClicked(juce::Button* b) override;
    void updateSelection();

    MorphSnapProcessor& proc_;
    std::array<juce::TextButton, 3> modeButtons_;
    int currentMode_ = 0;

    juce::Slider smoothSlider_;
    juce::Label  smoothLabel_;
};

} // namespace morphsnap
