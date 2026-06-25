/*
 * More-Phi — UI/ModeBar.h
 * Physics mode toggles: Direct | Elastic | Drift + smoothing slider.
 * V2: Separates Morph Source (2D Pad | Fader) from Physics Mode.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>

namespace more_phi {

class MorePhiProcessor;

class ModeBar : public juce::Component,
                private juce::Button::Listener,
                private juce::Timer
{
public:
    explicit ModeBar(MorePhiProcessor& p);
    ~ModeBar() override;
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void buttonClicked(juce::Button* b) override;
    void timerCallback() override;
    void updateModeSelection();
    void syncButtonsToState();
    void syncSmoothingToState();
    // R1b: show the Elastic preset combo only while Elastic physics is active.
    void updateElasticPresetVisibility();

    MorePhiProcessor& proc_;

    // Morph source: 2D Pad | Fader
    std::array<juce::TextButton, 2> sourceButtons_;

    // Physics mode: Direct | Elastic | Drift
    std::array<juce::TextButton, 3> modeButtons_;

    juce::Slider smoothSlider_;
    juce::Label  smoothLabel_;
    bool smoothingGestureActive_ = false;

    juce::Label sourceLabel_;
    juce::Label modeLabel_;
    bool syncing_ = false;

    // R1b: Elastic spring-physics preset (Slow / Medium / Heavy). The engine
    // always holds a value (default Medium); the control is only surfaced when
    // Elastic is the active physics mode so it isn't inert on Direct/Drift.
    juce::ComboBox elasticPresetCombo_;
    juce::Label    elasticLabel_;
    bool           elasticPresetSyncing_ = false;
};

} // namespace more_phi
