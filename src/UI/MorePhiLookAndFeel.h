/*
 * MorphSnap — UI/MorphSnapLookAndFeel.h
 * Premium dark theme derived from Stitch design system.
 * Palette: Deep navy, coral accents, purple highlights, glassmorphic surfaces.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace morphsnap {

class MorphSnapLookAndFeel : public juce::LookAndFeel_V4
{
public:
    MorphSnapLookAndFeel();

    // ── Color Palette (Stitch-derived) ───────────────────────────────────────
    static constexpr float cornerRadius = 8.0f;

    // Backgrounds
    const juce::Colour backgroundDark  {0xff0d1b2a};  // Deep navy
    const juce::Colour surfaceColour   {0xff16213e};  // Surface navy
    const juce::Colour surfaceLight    {0xff1a2742};  // Elevated surface
    const juce::Colour padBackground   {0xff0a1628};  // XY pad inner

    // Accents
    const juce::Colour accentCoral     {0xffec415d};  // Primary (Stitch: #ec415d)
    const juce::Colour accentPurple    {0xff533483};  // Secondary
    const juce::Colour accentGreen     {0xff4ade80};  // Status: online
    const juce::Colour accentAmber     {0xfffbbf24};  // Status: warning

    // Text
    const juce::Colour textPrimary     {0xffe8eaed};
    const juce::Colour textSecondary   {0xff8b95a5};
    const juce::Colour textDim         {0xff4a5568};

    // Borders
    const juce::Colour borderColour    {0xff1e3a5f};
    const juce::Colour borderGlow      {0x40ec415d};  // Coral glow (25% alpha)

    // ── LookAndFeel Overrides ────────────────────────────────────────────────
    void drawButtonBackground(juce::Graphics&, juce::Button&,
                               const juce::Colour&, bool isOver, bool isDown) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&,
                         bool isOver, bool isDown) override;
    void drawRotarySlider(juce::Graphics&, int x, int y, int w, int h,
                           float sliderPos, float startAngle, float endAngle,
                           juce::Slider&) override;
    void drawLinearSlider(juce::Graphics&, int x, int y, int w, int h,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    void drawComboBox(juce::Graphics&, int w, int h, bool isDown,
                       int bx, int by, int bw, int bh, juce::ComboBox&) override;
    void drawPopupMenuBackground(juce::Graphics&, int w, int h) override;
    void drawLabel(juce::Graphics&, juce::Label&) override;
};

} // namespace morphsnap
