#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// ── Brand palette (derived from the More-Phi landing page) ────────────────────
// Ultra-dark luxury theme: near-black surfaces, gold primary, cyan + magenta
// accents. Hex values are sRGB conversions of the landing page's oklch tokens.
namespace more_phi::Theme::Colours {

inline juce::Colour background() { return juce::Colour(0xff070709); } // App base
inline juce::Colour bg() { return juce::Colour(0xff050506); }         // Deepest well
inline juce::Colour surface() { return juce::Colour(0xff0d0d10); }    // Card / panel
inline juce::Colour surfaceLit() { return juce::Colour(0xff17181c); } // Elevated surface
inline juce::Colour border() { return juce::Colour(0xff323237); }     // Hairline border
inline juce::Colour accent() { return juce::Colour(0xffe5c057); }     // Primary: gold
inline juce::Colour purple() { return juce::Colour(0xffe22edb); }     // Bipolar / secondary: magenta
inline juce::Colour cyan() { return juce::Colour(0xff00bdca); }       // Interactive / active: cyan
inline juce::Colour cyanBright() { return juce::Colour(0xff00e2ed); } // Cyan highlight
inline juce::Colour green() { return juce::Colour(0xff34d399); }      // Status: online
inline juce::Colour amber() { return juce::Colour(0xfff9e596); }      // Status: warning (warm gold)
inline juce::Colour textBright() { return juce::Colour(0xffeeeef2); }
inline juce::Colour textDim() { return juce::Colour(0xff8e8f95); }
inline juce::Colour textSlot() { return juce::Colour(0xff8e8f95); }
inline juce::Colour padGrid() { return juce::Colour(0xff2a2a30); }
inline juce::Colour padTrail() { return juce::Colour(0xff00bdca); }   // Morph trail: cyan
inline juce::Colour padDot() { return juce::Colour(0xffe5c057); }     // Filled slot: gold
inline juce::Colour padDotEmpty() { return juce::Colour(0xff3a3a40); }
inline juce::Colour padGuide() { return juce::Colour(0xffeeeef2); }

} // namespace more_phi::Theme::Colours

namespace more_phi::Theme::Metrics {

inline constexpr float cornerRadius = 8.0f;
inline constexpr int panelPadding = 8;

} // namespace more_phi::Theme::Metrics
