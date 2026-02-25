/*
 * MorphSnap — UI/BottomControlStrip.h
 * Bottom control strip from Stitch-enhanced UI design.
 * Contains: SanityMode, Listen Mode, RecallMode, Recall Toggle, Link Mode, Sidechain controls.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

namespace morphsnap {

class MorphSnapProcessor;

class BottomControlStrip : public juce::Component
{
public:
    explicit BottomControlStrip(MorphSnapProcessor& p);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    MorphSnapProcessor& processor;

    // SanityMode toggle
    juce::ToggleButton sanityToggle_{"Sanity Mode"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> sanityAttach_;

    // RecallMode selector
    juce::TextButton recallFastBtn_{"Fast"};
    juce::TextButton recallFullBtn_{"Full"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> recallAttach_;

    // Sidechain controls
    juce::ToggleButton sidechainToggle_{"SC"};
    juce::Slider       thresholdKnob_;
    juce::Label        thresholdLabel_{"", "Threshold"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> scEnableAttach_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> scThreshAttach_;

    // Listen Mode toggle
    juce::ToggleButton listenToggle_{"Listen"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> listenAttach_;

    // Recall Toggle (sustain notes across snapshot switches)
    juce::ToggleButton recallToggle_{"Sustain"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> recallToggleAttach_;

    // RecallMode manual wiring (Choice params don't have ButtonAttachment)
    void updateRecallButtons();

    // Link Mode toggle
    juce::ToggleButton linkToggle_{"Link"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> linkAttach_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BottomControlStrip)
};

} // namespace morphsnap
