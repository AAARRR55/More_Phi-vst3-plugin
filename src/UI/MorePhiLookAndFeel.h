/*
 * More-Phi — UI/MorePhiLookAndFeel.h
 * Premium dark theme derived from Stitch design system.
 * Palette: Deep navy, coral accents, purple highlights, glassmorphic surfaces.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace more_phi {

class MorePhiLookAndFeel : public juce::LookAndFeel_V4
{
public:
    MorePhiLookAndFeel();

    // ── Color Palette (Stitch-derived) ───────────────────────────────────────
    static constexpr float cornerRadius = 8.0f;

    // Baseline editor width for font scaling calculations
    static constexpr float kBaselineWidth = 920.0f;

    // Minimum font sizes — nothing goes below these, ever
    static constexpr float kMinSectionLabel  = 9.0f;
    static constexpr float kMinControlLabel  = 10.0f;
    static constexpr float kMinValueLabel    = 10.0f;
    static constexpr float kMinSlotNumber    = 9.0f;
    static constexpr float kMinModeLabel     = 10.0f;

    enum class FontRole
    {
        Title,
        Section,
        Control,
        Value,
        Micro
    };

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

    // ── Font Scaling Helpers ─────────────────────────────────────────────────
    // Scales a base font size proportionally to editor width, with a floor.
    // Call with current editor width; returns size clamped to [minSize, baseSize*1.3].
    static float getScaledFontSize(float baseSize, float editorWidth, float minSize);

    // Convenience: creates a scaled juce::Font with the given typeface and style.
    static juce::Font makeScaledFont(float baseSize, float editorWidth,
                                      float minSize, int style = juce::Font::plain);

    // Non-static convenience that uses the global editor width reference.
    float getScaledFontSize(float baseSize, float minSize) const;
    float getScaledFontSize(float baseSize) const;
    juce::Font makeScaledFont(float baseSize, float minSize,
                              int style = juce::Font::plain) const;
    juce::Font makeScaledFont(float baseSize, int style = juce::Font::plain) const;
    juce::Font makeRoleFont(FontRole role, int style = juce::Font::plain) const;

    // Settable reference width — updated by MorePhiEditor::resized().
    void setEditorWidth(float w) { editorWidth_ = w; }

private:
    mutable float editorWidth_ = 920.0f;
};

} // namespace more_phi
