/*
 * More-Phi — UI/BottomControlStrip.h
 * Bottom control strip from Stitch-enhanced UI design.
 * Contains: SanityMode, Listen Mode, RecallMode, Recall Toggle, Link Mode, Sidechain controls.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

namespace more_phi {

class MorePhiProcessor;

class BottomControlStrip : public juce::Component
{
public:
    explicit BottomControlStrip(MorePhiProcessor& p);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    MorePhiProcessor& processor;

    // Safety mode selectors
    juce::TextButton sanityToggle_{"Sanity"};
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
    juce::TextButton listenToggle_{"Listen"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> listenAttach_;

    // Recall Toggle (sustain notes across snapshot switches)
    juce::ToggleButton recallToggle_{"Sustain"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> recallToggleAttach_;

    // RecallMode manual wiring (Choice params don't have ButtonAttachment)
    void updateRecallButtons();

    // Link Mode toggle
    juce::TextButton linkToggle_{"Link"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> linkAttach_;

    // Output gain + bypass
    juce::Slider       outputGainKnob_;
    juce::Label        outputGainLabel_{"", "Gain"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gainAttach_;
    juce::TextButton   bypassBtn_{"Bypass"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttach_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BottomControlStrip)
};

} // namespace more_phi
