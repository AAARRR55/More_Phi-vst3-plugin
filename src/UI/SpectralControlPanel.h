/*
 * MorphSnap — UI/SpectralControlPanel.h
 *
 * Horizontal strip panel exposing SpectralMorphEngine controls.
 * Placed in the "Engine" tab of the main tabbed interface.
 *
 * Sections (left → right):
 *   SPECTRAL  — active toggle, FFT size combo
 *   CENTER    — Alpha rotary knob (A/B crossfade in frequency domain)
 *   OPTIONS   — Transient and Formant preserve toggles
 *
 * All engine calls go directly to SpectralMorphEngine; no APVTS binding.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace morphsnap {

class MorphSnapProcessor;

class SpectralControlPanel : public juce::Component
{
public:
    explicit SpectralControlPanel(MorphSnapProcessor& proc);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    // ── Helpers ──────────────────────────────────────────────────────────────
    void setupToggleButton(juce::TextButton& btn, const juce::String& label);
    void drawSectionLabel(juce::Graphics& g, const juce::String& text,
                          juce::Rectangle<int> bounds) const;

    // ── Processor reference ──────────────────────────────────────────────────
    MorphSnapProcessor& proc_;

    // ── Left section: SPECTRAL ───────────────────────────────────────────────
    juce::TextButton activeToggle_  { "Spectral" };
    juce::ComboBox   fftSizeCombo_;
    juce::Label      fftSizeLabel_;

    // ── Center section: Alpha knob ───────────────────────────────────────────
    juce::Slider     alphaKnob_;
    juce::Label      alphaLabel_;

    // ── Right section: OPTIONS ───────────────────────────────────────────────
    juce::TextButton transientToggle_ { "Transient" };
    juce::TextButton formantToggle_   { "Formant"   };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectralControlPanel)
};

} // namespace morphsnap
