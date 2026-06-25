/*
 * More-Phi — UI/Icons.h
 *
 * Tiny vector-icon helpers (kept here so every panel draws glyphs consistently).
 * All helpers take a (Graphics, area, ...) signature and draw within `area`.
 *
 * Threading: paint helpers — call only from juce::Component::paint (message
 * thread). No allocations after the static typeface is registered.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace more_phi::Icons
{

/*
 * Draw a 5-point star centred in `area`.
 *   filled = true  → solid fill with `colour`
 *   filled = false → 1.0 px outline stroke with `colour`
 *
 * M4: the preset browser previously drew filled/empty circles for ratings,
 * which read as generic dots. Real stars remove the ambiguity.
 */
inline void drawStar(juce::Graphics& g, juce::Rectangle<float> area,
                     bool filled, juce::Colour colour)
{
    const auto cx = area.getCentreX();
    const auto cy = area.getCentreY();
    const auto r  = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;
    if (r <= 0.5f)
        return;

    // Outer radius r, inner radius r * 0.382 (golden-ratio derived 5-point star).
    constexpr float kInnerRatio = 0.382f;
    const float inner = r * kInnerRatio;

    juce::Path star;
    for (int i = 0; i < 10; ++i)
    {
        const float radius = (i % 2 == 0) ? r : inner;
        // Start at top (-90°) and step 36° per vertex.
        const float angle = juce::MathConstants<float>::halfPi
                          + static_cast<float>(i) * juce::MathConstants<float>::pi / 5.0f;
        const float x = cx + std::sin(angle) * radius;
        const float y = cy + std::cos(angle) * radius;
        if (i == 0)
            star.startNewSubPath(x, y);
        else
            star.lineTo(x, y);
    }
    star.closeSubPath();

    g.setColour(colour);
    if (filled)
        g.fillPath(star);
    else
    {
        juce::PathStrokeType stroke(1.0f, juce::PathStrokeType::curved);
        g.strokePath(star, stroke);
    }
}

} // namespace more_phi::Icons
