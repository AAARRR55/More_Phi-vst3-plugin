/*
 * More-Phi — UI/GranularControlPanel.h
 *
 * Horizontal strip panel exposing GranularMorphEngine controls.
 * Placed in the "Engine" tab of the main tabbed interface.
 *
 * Sections (left → right):
 *   LEFT    — Granular active toggle
 *   CENTER  — Four rotary knobs: Size, Density, Pitch Rand, Scatter
 *
 * All controls route through APVTS for DAW automation support.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>

namespace more_phi {

class MorePhiProcessor;

class GranularControlPanel : public juce::Component
{
public:
    explicit GranularControlPanel(MorePhiProcessor& proc);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    // ── Helpers ──────────────────────────────────────────────────────────────
    void setupToggleButton(juce::TextButton& btn);
    void setupKnob(juce::Slider& knob, double min, double max,
                   double defaultVal, const juce::String& suffix);
    void drawSectionLabel(juce::Graphics& g, const juce::String& text,
                          juce::Rectangle<int> bounds) const;
    void updateEnabledState();  // H5: enable/disable sub-controls from the active toggle

    // ── Processor reference ──────────────────────────────────────────────────
    MorePhiProcessor& proc_;

    // ── Left section ──────────────────────────────────────────────────────────
    juce::TextButton activeToggle_ { "Granular" };

    // ── Center section: 4 knobs ───────────────────────────────────────────────
    // Order: GrainSize, Density, PitchRand, PosRand
    std::array<juce::Slider, 4> knobs_;
    std::array<juce::Label,  4> knobLabels_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GranularControlPanel)
};

} // namespace more_phi
