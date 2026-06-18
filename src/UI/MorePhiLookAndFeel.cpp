/* More-Phi — UI/MorePhiLookAndFeel.cpp */
#include "MorePhiLookAndFeel.h"
#include "BinaryData.h"
#include <cmath>

namespace more_phi {

namespace {
// Cached typeface family names resolved from the embedded TTFs.
juce::String g_displayTypeface;  // Syncopate
juce::String g_bodyTypeface;     // Outfit
bool g_fontsRegistered = false;
}

void MorePhiLookAndFeel::ensureFontsRegistered()
{
    if (g_fontsRegistered)
        return;
    g_fontsRegistered = true;

    auto registerFace = [](const void* data, int size) -> juce::String
    {
        if (data == nullptr || size <= 0)
            return {};
        auto tf = juce::Typeface::createSystemTypefaceFor(data, static_cast<size_t>(size));
        return tf != nullptr ? tf->getName() : juce::String{};
    };

    // Syncopate (display) — bold weight is the brand wordmark look.
    g_displayTypeface = registerFace(BinaryData::SyncopateBold_ttf,
                                     BinaryData::SyncopateBold_ttfSize);
    registerFace(BinaryData::SyncopateRegular_ttf, BinaryData::SyncopateRegular_ttfSize);

    // Outfit (body / values).
    g_bodyTypeface = registerFace(BinaryData::OutfitRegular_ttf,
                                  BinaryData::OutfitRegular_ttfSize);
    registerFace(BinaryData::OutfitSemiBold_ttf, BinaryData::OutfitSemiBold_ttfSize);

    if (g_displayTypeface.isEmpty()) g_displayTypeface = "Syncopate";
    if (g_bodyTypeface.isEmpty())    g_bodyTypeface = "Outfit";
}

const juce::String& MorePhiLookAndFeel::displayTypefaceName()
{
    ensureFontsRegistered();
    return g_displayTypeface;
}

const juce::String& MorePhiLookAndFeel::bodyTypefaceName()
{
    ensureFontsRegistered();
    return g_bodyTypeface;
}

juce::Font MorePhiLookAndFeel::bodyFont(float size, int style)
{
    return juce::Font(juce::FontOptions(bodyTypefaceName(), size, style));
}

MorePhiLookAndFeel::MorePhiLookAndFeel()
{
    ensureFontsRegistered();

    // Global defaults — gold primary, cyan interactive accents
    setColour(juce::ResizableWindow::backgroundColourId, backgroundDark);
    setColour(juce::TextButton::buttonColourId, surfaceColour);
    setColour(juce::TextButton::buttonOnColourId, accentGold);
    setColour(juce::TextButton::textColourOffId, textPrimary);
    setColour(juce::TextButton::textColourOnId, backgroundDark);
    setColour(juce::ComboBox::backgroundColourId, surfaceColour);
    setColour(juce::ComboBox::textColourId, textPrimary);
    setColour(juce::ComboBox::outlineColourId, borderColour);
    setColour(juce::PopupMenu::backgroundColourId, surfaceLight);
    setColour(juce::PopupMenu::textColourId, textPrimary);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, accentCyan.withAlpha(0.22f));
    setColour(juce::Label::textColourId, textSecondary);
    setColour(juce::Slider::thumbColourId, accentGold);
    setColour(juce::Slider::trackColourId, borderColour);
    setColour(juce::Slider::rotarySliderFillColourId, accentGold);
    setColour(juce::Slider::rotarySliderOutlineColourId, surfaceColour);
    setColour(juce::Slider::textBoxTextColourId, textSecondary);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);

    // Default body font: Outfit (matches the landing page's clean grotesk body)
    setDefaultSansSerifTypefaceName(g_bodyTypeface);
}

// ── Font Scaling ─────────────────────────────────────────────────────────────

float MorePhiLookAndFeel::getScaledFontSize(float baseSize, float editorWidth, float minSize)
{
    const float scale = editorWidth / kBaselineWidth;
    // H6: raised upper clamp from 1.3 to 1.5 so text grows on large/HiDPI canvases
    // (at 1600px the natural scale is ~1.74; capping at 1.5 keeps fixed-height rows
    // from clipping while no longer looking undersized).
    const float clampedScale = juce::jlimit(0.75f, 1.5f, scale);
    return juce::jmax(minSize, baseSize * clampedScale);
}

juce::Font MorePhiLookAndFeel::makeScaledFont(float baseSize, float editorWidth,
                                                float minSize, int style)
{
    return juce::Font(juce::FontOptions(bodyTypefaceName(),
                                         getScaledFontSize(baseSize, editorWidth, minSize),
                                         style));
}

float MorePhiLookAndFeel::getScaledFontSize(float baseSize) const
{
    return getScaledFontSize(baseSize, editorWidth_, kMinControlLabel);
}

float MorePhiLookAndFeel::getScaledFontSize(float baseSize, float minSize) const
{
    return getScaledFontSize(baseSize, editorWidth_, minSize);
}

juce::Font MorePhiLookAndFeel::makeScaledFont(float baseSize, int style) const
{
    return makeScaledFont(baseSize, editorWidth_, kMinControlLabel, style);
}

juce::Font MorePhiLookAndFeel::makeScaledFont(float baseSize, float minSize, int style) const
{
    return makeScaledFont(baseSize, editorWidth_, minSize, style);
}

juce::Font MorePhiLookAndFeel::makeRoleFont(FontRole role, int style) const
{
    // Title + Section labels use Syncopate (display); everything else uses Outfit.
    auto withFamily = [this, style](const juce::String& family, float base, float minSize)
    {
        return juce::Font(juce::FontOptions(
            family, getScaledFontSize(base, minSize), style));
    };

    switch (role)
    {
        case FontRole::Title:   return withFamily(displayTypefaceName(), 20.0f, 16.0f);
        case FontRole::Section: return withFamily(displayTypefaceName(), 10.5f, kMinSectionLabel);
        case FontRole::Control: return makeScaledFont(12.0f, kMinControlLabel, style);
        case FontRole::Value:   return makeScaledFont(11.0f, kMinValueLabel, style);
        case FontRole::Micro:   return makeScaledFont(10.0f, kMinModeLabel, style);
    }

    return makeScaledFont(12.0f, kMinControlLabel, style);
}

// ── Buttons ──────────────────────────────────────────────────────────────────

void MorePhiLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                                  const juce::Colour&,
                                                  bool isOver, bool isDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    bool toggled = button.getToggleState();

    // Background gradient
    juce::Colour bg = toggled
        ? button.findColour(juce::TextButton::buttonOnColourId)
        : button.findColour(juce::TextButton::buttonColourId);
    if (bg.isTransparent())
        bg = toggled ? accentGold : surfaceLight;
    if (isDown) bg = bg.darker(0.2f);
    else if (isOver) bg = bg.brighter(0.08f);

    // Subtle vertical gradient
    g.setGradientFill(juce::ColourGradient(
        bg.brighter(0.05f), 0, bounds.getY(),
        bg.darker(0.05f),   0, bounds.getBottom(), false));
    g.fillRoundedRectangle(bounds, cornerRadius);

    // Border — cyan on hover, gold when active
    g.setColour(toggled ? accentGold.brighter(0.2f)
                        : (isOver ? accentCyan.withAlpha(0.7f) : borderColour));
    g.drawRoundedRectangle(bounds, cornerRadius, 1.0f);

    // Glow for active/toggled buttons
    if (toggled)
    {
        g.setColour(borderGlow);
        g.drawRoundedRectangle(bounds.expanded(1), cornerRadius + 1, 2.0f);
    }
}

void MorePhiLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button,
                                            bool /*isOver*/, bool /*isDown*/)
{
    auto font = makeRoleFont(FontRole::Control);
    font.setFallbackEnabled(true);
    g.setFont(font);
    g.setColour(button.getToggleState()
        ? button.findColour(juce::TextButton::textColourOnId)
        : button.findColour(juce::TextButton::textColourOffId));
    g.drawText(button.getButtonText(), button.getLocalBounds(),
               juce::Justification::centred);
}

// ── Rotary Knobs ─────────────────────────────────────────────────────────────

void MorePhiLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
                                              float sliderPos, float, float,
                                              juce::Slider& slider)
{
    auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
                                         static_cast<float>(w), static_cast<float>(h)).reduced(4.0f);
    const float diameter = juce::jmin(bounds.getWidth(), bounds.getHeight());
    const float radius = diameter * 0.5f;
    const auto centre = bounds.getCentre();
    const float cx = centre.x;
    const float cy = centre.y;

    if (radius <= 6.0f)
        return;

    constexpr float startAngle = juce::MathConstants<float>::pi * 1.25f;
    constexpr float endAngle   = juce::MathConstants<float>::pi * 2.75f;
    const float clampedPos = juce::jlimit(0.0f, 1.0f, sliderPos);
    const float angle = startAngle + clampedPos * (endAngle - startAngle);
    const float alpha = slider.isEnabled() ? 1.0f : 0.38f;
    const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;

    const juce::Colour fillColour =
        slider.findColour(juce::Slider::rotarySliderFillColourId).withMultipliedAlpha(alpha);
    const juce::Colour outlineColour =
        slider.findColour(juce::Slider::rotarySliderOutlineColourId).withMultipliedAlpha(alpha);
    const juce::Colour pointerColour = textPrimary.withMultipliedAlpha(alpha);
    const float stroke = juce::jlimit(3.0f, 5.5f, radius * 0.12f);

    juce::Path bgArc;
    bgArc.addCentredArc(cx, cy, radius - stroke, radius - stroke, 0.0f, startAngle, endAngle, true);
    g.setColour(outlineColour);
    g.strokePath(bgArc, juce::PathStrokeType(stroke, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));

    if (bipolar)
    {
        const float centreNorm = static_cast<float>(
            (0.0 - slider.getMinimum()) / (slider.getMaximum() - slider.getMinimum()));
        const float centreAngle = startAngle + centreNorm * (endAngle - startAngle);
        if (std::abs(clampedPos - centreNorm) > 0.001f)
        {
            juce::Path valueArc;
            if (clampedPos >= centreNorm)
                valueArc.addCentredArc(cx, cy, radius - stroke, radius - stroke, 0.0f,
                                        centreAngle, angle, true);
            else
                valueArc.addCentredArc(cx, cy, radius - stroke, radius - stroke, 0.0f,
                                        angle, centreAngle, true);

            const juce::Colour arcColour =
                clampedPos >= centreNorm ? fillColour : accentPurple.withMultipliedAlpha(alpha);
            // Neon glow: wide, low-alpha passes underneath the crisp value arc.
            g.setColour(arcColour.withMultipliedAlpha(0.18f));
            g.strokePath(valueArc, juce::PathStrokeType(stroke * 3.0f, juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));
            g.setColour(arcColour.withMultipliedAlpha(0.30f));
            g.strokePath(valueArc, juce::PathStrokeType(stroke * 1.9f, juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));
            g.setColour(arcColour);
            g.strokePath(valueArc, juce::PathStrokeType(stroke, juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));
        }
    }
    else if (clampedPos > 0.001f)
    {
        juce::Path valueArc;
        valueArc.addCentredArc(cx, cy, radius - stroke, radius - stroke, 0.0f,
                                startAngle, angle, true);
        // Neon glow: wide, low-alpha passes underneath the crisp value arc.
        g.setColour(fillColour.withMultipliedAlpha(0.18f));
        g.strokePath(valueArc, juce::PathStrokeType(stroke * 3.0f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
        g.setColour(fillColour.withMultipliedAlpha(0.30f));
        g.strokePath(valueArc, juce::PathStrokeType(stroke * 1.9f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
        g.setColour(fillColour);
        g.strokePath(valueArc, juce::PathStrokeType(stroke, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
    }

    const float insetRadius = radius * 0.62f;
    g.setColour(surfaceColour.withMultipliedAlpha(alpha));
    g.fillEllipse(cx - insetRadius, cy - insetRadius, insetRadius * 2.0f, insetRadius * 2.0f);
    g.setColour(borderColour.withMultipliedAlpha(alpha));
    g.drawEllipse(cx - insetRadius, cy - insetRadius, insetRadius * 2.0f, insetRadius * 2.0f, 0.5f);

    juce::Path pointer;
    const float pointerLength = radius * 0.68f;
    const float pointerWidth = juce::jlimit(2.0f, 3.0f, radius * 0.07f);
    pointer.addRectangle(-pointerWidth * 0.5f, -pointerLength,
                         pointerWidth, pointerLength + radius * 0.16f);
    pointer.applyTransform(juce::AffineTransform::rotation(angle).translated(cx, cy));
    g.setColour(pointerColour);
    g.fillPath(pointer);

    const float tipRadius = pointerWidth * 0.75f;
    const float tipDist = pointerLength + radius * 0.08f;
    const float tipX = cx + std::sin(angle) * tipDist;
    const float tipY = cy - std::cos(angle) * tipDist;
    g.setColour(fillColour);
    g.fillEllipse(tipX - tipRadius, tipY - tipRadius, tipRadius * 2.0f, tipRadius * 2.0f);
}

// ── Linear Slider ────────────────────────────────────────────────────────────

void MorePhiLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h,
                                              float sliderPos, float /*minSliderPos*/,
                                              float /*maxSliderPos*/,
                                              juce::Slider::SliderStyle style,
                                              juce::Slider&)
{
    const bool isVertical = (style == juce::Slider::LinearVertical ||
                              style == juce::Slider::LinearBarVertical);

    if (isVertical)
    {
        float trackX = x + w * 0.5f;
        float trackTop = static_cast<float>(y + 4);
        float trackBot = static_cast<float>(y + h - 4);

        // Track
        g.setColour(surfaceLight);
        g.fillRoundedRectangle(trackX - 2, trackTop, 4, trackBot - trackTop, 2.0f);

        // Filled portion
        g.setColour(accentCoral);
        g.fillRoundedRectangle(trackX - 2, sliderPos, 4, trackBot - sliderPos, 2.0f);

        // Thumb
        g.setColour(textPrimary);
        g.fillRoundedRectangle(trackX - 8, sliderPos - 4, 16, 8, 4.0f);
    }
    else
    {
        float trackY = y + h * 0.5f;
        float trackLeft = static_cast<float>(x + 4);
        float trackRight = static_cast<float>(x + w - 4);

        // Track
        g.setColour(surfaceLight);
        g.fillRoundedRectangle(trackLeft, trackY - 2, trackRight - trackLeft, 4, 2.0f);

        // Filled portion
        g.setColour(accentCoral);
        g.fillRoundedRectangle(trackLeft, trackY - 2, sliderPos - trackLeft, 4, 2.0f);

        // Thumb
        g.setColour(textPrimary);
        g.fillEllipse(sliderPos - 5, trackY - 5, 10, 10);
    }
}

// ── ComboBox ─────────────────────────────────────────────────────────────────

void MorePhiLookAndFeel::drawComboBox(juce::Graphics& g, int w, int h, bool isDown,
                                          int, int, int, int, juce::ComboBox&)
{
    auto bounds = juce::Rectangle<float>(0, 0, static_cast<float>(w), static_cast<float>(h));
    g.setColour(isDown ? surfaceLight.brighter(0.1f) : surfaceLight);
    g.fillRoundedRectangle(bounds, cornerRadius);
    g.setColour(borderColour);
    g.drawRoundedRectangle(bounds.reduced(0.5f), cornerRadius, 1.0f);

    // Arrow
    juce::Path arrow;
    float arrowX = w - 16.0f, arrowY = h * 0.5f - 2.0f;
    arrow.addTriangle(arrowX, arrowY, arrowX + 8, arrowY, arrowX + 4, arrowY + 5);
    g.setColour(textSecondary);
    g.fillPath(arrow);
}

// ── Popup Menu ───────────────────────────────────────────────────────────────

void MorePhiLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int w, int h)
{
    g.setColour(surfaceLight);
    g.fillRoundedRectangle(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), cornerRadius);
    g.setColour(borderColour);
    g.drawRoundedRectangle(0.5f, 0.5f, w - 1.0f, h - 1.0f, cornerRadius, 1.0f);
}

// ── Labels ───────────────────────────────────────────────────────────────────

void MorePhiLookAndFeel::drawLabel(juce::Graphics& g, juce::Label& label)
{
    g.setColour(label.findColour(juce::Label::textColourId));
    auto font = label.getFont();
    if (font.getHeight() < kMinValueLabel)
        font.setHeight(kMinValueLabel);
    g.setFont(font);

    auto textArea = label.getBorderSize().subtractedFrom(label.getLocalBounds());
    g.drawFittedText(label.getText(), textArea, label.getJustificationType(),
                      juce::jmax(1, static_cast<int>(textArea.getHeight() / 12)),
                      1.0f);
}

} // namespace more_phi
