/* MorphSnap — UI/MorphSnapLookAndFeel.cpp */
#include "MorphSnapLookAndFeel.h"

namespace morphsnap {

MorphSnapLookAndFeel::MorphSnapLookAndFeel()
{
    // Global defaults
    setColour(juce::ResizableWindow::backgroundColourId, backgroundDark);
    setColour(juce::TextButton::buttonColourId, surfaceColour);
    setColour(juce::TextButton::textColourOffId, textPrimary);
    setColour(juce::TextButton::textColourOnId, accentCoral);
    setColour(juce::ComboBox::backgroundColourId, surfaceColour);
    setColour(juce::ComboBox::textColourId, textPrimary);
    setColour(juce::ComboBox::outlineColourId, borderColour);
    setColour(juce::PopupMenu::backgroundColourId, surfaceLight);
    setColour(juce::PopupMenu::textColourId, textPrimary);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, accentCoral.withAlpha(0.2f));
    setColour(juce::Label::textColourId, textSecondary);
    setColour(juce::Slider::thumbColourId, accentCoral);
    setColour(juce::Slider::trackColourId, borderColour);
    setColour(juce::Slider::rotarySliderFillColourId, accentCoral);
    setColour(juce::Slider::rotarySliderOutlineColourId, surfaceColour);
    setColour(juce::Slider::textBoxTextColourId, textSecondary);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);

    // Default font
    setDefaultSansSerifTypefaceName("Segoe UI");
}

// ── Buttons ──────────────────────────────────────────────────────────────────

void MorphSnapLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                                  const juce::Colour&,
                                                  bool isOver, bool isDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    bool toggled = button.getToggleState();

    // Background gradient
    juce::Colour bg = toggled ? accentCoral : surfaceLight;
    if (isDown) bg = bg.darker(0.2f);
    else if (isOver) bg = bg.brighter(0.08f);

    // Subtle vertical gradient
    g.setGradientFill(juce::ColourGradient(
        bg.brighter(0.05f), 0, bounds.getY(),
        bg.darker(0.05f),   0, bounds.getBottom(), false));
    g.fillRoundedRectangle(bounds, cornerRadius);

    // Border
    g.setColour(toggled ? accentCoral.brighter(0.2f) : borderColour);
    g.drawRoundedRectangle(bounds, cornerRadius, 1.0f);

    // Glow for active/toggled buttons
    if (toggled)
    {
        g.setColour(borderGlow);
        g.drawRoundedRectangle(bounds.expanded(1), cornerRadius + 1, 2.0f);
    }
}

void MorphSnapLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button,
                                            bool /*isOver*/, bool /*isDown*/)
{
    g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::plain)));
    g.setColour(button.getToggleState() ? juce::Colours::white : textPrimary);
    g.drawText(button.getButtonText(), button.getLocalBounds(),
               juce::Justification::centred);
}

// ── Rotary Knobs ─────────────────────────────────────────────────────────────

void MorphSnapLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
                                              float sliderPos, float startAngle, float endAngle,
                                              juce::Slider&)
{
    const float radius = juce::jmin(w, h) * 0.5f - 4.0f;
    const float cx = x + w * 0.5f;
    const float cy = y + h * 0.5f;
    const float angle = startAngle + sliderPos * (endAngle - startAngle);

    // Background track (full arc)
    juce::Path bgArc;
    bgArc.addCentredArc(cx, cy, radius, radius, 0.0f, startAngle, endAngle, true);
    g.setColour(surfaceLight);
    g.strokePath(bgArc, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));

    // Value arc (filled portion)
    if (sliderPos > 0.0f)
    {
        juce::Path valueArc;
        valueArc.addCentredArc(cx, cy, radius, radius, 0.0f, startAngle, angle, true);
        g.setColour(accentCoral);
        g.strokePath(valueArc, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
    }

    // Center dot
    g.setColour(surfaceColour);
    g.fillEllipse(cx - radius * 0.6f, cy - radius * 0.6f, radius * 1.2f, radius * 1.2f);

    // Pointer line
    juce::Path pointer;
    const float pointerLength = radius * 0.5f;
    pointer.addRectangle(-1.5f, -pointerLength, 3.0f, pointerLength);
    pointer.applyTransform(juce::AffineTransform::rotation(angle).translated(cx, cy));
    g.setColour(textPrimary);
    g.fillPath(pointer);
}

// ── Linear Slider ────────────────────────────────────────────────────────────

void MorphSnapLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h,
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

void MorphSnapLookAndFeel::drawComboBox(juce::Graphics& g, int w, int h, bool isDown,
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

void MorphSnapLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int w, int h)
{
    g.setColour(surfaceLight);
    g.fillRoundedRectangle(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), cornerRadius);
    g.setColour(borderColour);
    g.drawRoundedRectangle(0.5f, 0.5f, w - 1.0f, h - 1.0f, cornerRadius, 1.0f);
}

// ── Labels ───────────────────────────────────────────────────────────────────

void MorphSnapLookAndFeel::drawLabel(juce::Graphics& g, juce::Label& label)
{
    g.setColour(label.findColour(juce::Label::textColourId));
    g.setFont(label.getFont());

    auto textArea = label.getBorderSize().subtractedFrom(label.getLocalBounds());
    g.drawFittedText(label.getText(), textArea, label.getJustificationType(),
                      juce::jmax(1, static_cast<int>(textArea.getHeight() / 12)),
                      1.0f);
}

} // namespace morphsnap
