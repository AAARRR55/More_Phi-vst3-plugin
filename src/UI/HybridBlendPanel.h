/*
 * More-Phi — UI/HybridBlendPanel.h
 *
 * Horizontal strip panel for hybrid blend weight control.
 * Placed in the "Engine" tab of the main tabbed interface.
 *
 * Sections (left → right):
 *   BLEND MODE  — Audio domain toggle, oversampling combo
 *   CENTER      — Three vertical sliders: Direct / Spectral / Granular weights
 *                 A summary label shows the current normalized percentages.
 *   RIGHT       — Alpha knob (morph crossfade)
 *
 * The three blend weight sliders visually indicate they sum to 1.0.
 * When any slider changes, a "D:X% S:X% G:X%" label updates in real time.
 *
 * All calls go directly to MorePhiProcessor atomics; no APVTS binding.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace more_phi {

class MorePhiProcessor;

class HybridBlendPanel : public juce::Component
{
public:
    explicit HybridBlendPanel(MorePhiProcessor& proc);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    // ── Helpers ──────────────────────────────────────────────────────────────
    void setupToggleButton(juce::TextButton& btn, const juce::String& label);
    void setupVerticalSlider(juce::Slider& slider, double defaultVal);
    void beginBlendGesture();
    void endBlendGesture();
    void onBlendWeightChanged();
    void updateBlendLabel();
    void drawSectionLabel(juce::Graphics& g, const juce::String& text,
                          juce::Rectangle<int> bounds) const;

    // ── Processor reference ──────────────────────────────────────────────────
    MorePhiProcessor& proc_;

    // ── Left section: BLEND MODE ──────────────────────────────────────────────
    juce::TextButton audioDomainToggle_ { "Audio Domain" };
    juce::ComboBox   oversamplingCombo_;
    juce::Label      oversamplingLabel_;

    // ── Center section: BLEND WEIGHTS ─────────────────────────────────────────
    juce::Slider paramSlider_;      // Direct weight
    juce::Slider spectralSlider_;   // Spectral weight
    juce::Slider granularSlider_;   // Granular weight
    bool blendGestureActive_ = false;

    juce::Label  paramLabel_;
    juce::Label  spectralLabel_;
    juce::Label  granularLabel_;

    juce::Label  blendSummaryLabel_; // "D:100% S:0% G:0%"

    // ── Right section: Alpha knob ─────────────────────────────────────────────
    juce::Slider alphaKnob_;
    juce::Label  alphaLabel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HybridBlendPanel)
};

} // namespace more_phi
