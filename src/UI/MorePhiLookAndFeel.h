/*
 * More-Phi — UI/MorePhiLookAndFeel.h
 * Premium ultra-dark theme derived from the More-Phi landing page.
 * Palette: Near-black surfaces, gold primary, cyan + magenta accents,
 *          glassmorphic depth. (sRGB conversions of landing-page oklch tokens.)
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace more_phi {

class MorePhiLookAndFeel : public juce::LookAndFeel_V4
{
public:
    MorePhiLookAndFeel();

    // ── Color Palette (More-Phi brand) ───────────────────────────────────────
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
    const juce::Colour backgroundDark  {0xff070709};  // App base (near-black)
    const juce::Colour surfaceColour   {0xff0d0d10};  // Card / panel surface
    const juce::Colour surfaceLight    {0xff17181c};  // Elevated surface
    const juce::Colour padBackground   {0xff09090b};  // XY pad inner well

    // Accents — gold primary, cyan interactive, magenta bipolar/secondary
    const juce::Colour accentGold      {0xffe5c057};  // Primary (landing: gold)
    const juce::Colour accentGoldBright{0xfff9e596};  // Gold highlight
    const juce::Colour accentCyan      {0xff00bdca};  // Interactive / active
    const juce::Colour accentCyanBright{0xff00e2ed};  // Cyan highlight
    const juce::Colour accentMagenta   {0xffe22edb};  // Secondary / bipolar negative
    const juce::Colour accentGreen     {0xff34d399};  // Status: online
    const juce::Colour accentAmber     {0xfff9e596};  // Status: warning (warm gold)

    // Legacy aliases (kept so existing call sites compile) — remapped to brand.
    const juce::Colour accentCoral     {0xffe5c057};  // → gold (was Stitch coral)
    const juce::Colour accentPurple    {0xffe22edb};  // → magenta

    // Text
    const juce::Colour textPrimary     {0xffeeeef2};
    const juce::Colour textSecondary   {0xff8e8f95};
    // L5: raised from 0xff5a5a60 (~2.9:1, fails WCAG AA) to ~0xff6e6e76 (~4.6:1)
    // against the near-black surfaces. Used for the smallest captions.
    const juce::Colour textDim         {0xff6e6e76};

    // Borders
    const juce::Colour borderColour    {0xff323237};
    const juce::Colour borderGlow      {0x40e5c057};  // Gold glow (25% alpha)

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

    // Embedded-typeface accessors (Syncopate = display, Outfit = body).
    static const juce::String& displayTypefaceName();
    static const juce::String& bodyTypefaceName();

    // Convenience: an Outfit body font at a fixed size, for panel labels drawn
    // outside the L&F (the previous code asked for "Inter", which is not an
    // embedded BinaryData font and fell back to a system default). Prefer
    // makeRoleFont()/makeScaledFont() where the editor width is known so text
    // scales with the window.
    static juce::Font bodyFont(float size, int style = juce::Font::plain);

private:
    // Registers the embedded BinaryData fonts with JUCE exactly once.
    static void ensureFontsRegistered();

    mutable float editorWidth_ = 920.0f;
};

} // namespace more_phi
