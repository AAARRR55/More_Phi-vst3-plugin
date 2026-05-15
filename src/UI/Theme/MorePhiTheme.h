#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace more_phi::Theme::Colours {

inline juce::Colour background() { return juce::Colour(0xff0d1b2a); }
inline juce::Colour bg() { return juce::Colour(0xff0a1628); }
inline juce::Colour surface() { return juce::Colour(0xff16213e); }
inline juce::Colour surfaceLit() { return juce::Colour(0xff1a2742); }
inline juce::Colour border() { return juce::Colour(0xff1e3a5f); }
inline juce::Colour accent() { return juce::Colour(0xffec415d); }
inline juce::Colour purple() { return juce::Colour(0xff533483); }
inline juce::Colour green() { return juce::Colour(0xff4ade80); }
inline juce::Colour amber() { return juce::Colour(0xfffbbf24); }
inline juce::Colour textBright() { return juce::Colour(0xffe8eaed); }
inline juce::Colour textDim() { return juce::Colour(0xff8b95a5); }
inline juce::Colour textSlot() { return juce::Colour(0xff8b95a5); }
inline juce::Colour padGrid() { return juce::Colour(0xff2f4f75); }
inline juce::Colour padTrail() { return juce::Colour(0xffec415d); }
inline juce::Colour padDot() { return juce::Colour(0xff4ade80); }
inline juce::Colour padDotEmpty() { return juce::Colour(0xff4a5568); }
inline juce::Colour padGuide() { return juce::Colour(0xffe8eaed); }

} // namespace more_phi::Theme::Colours

namespace more_phi::Theme::Metrics {

inline constexpr float cornerRadius = 8.0f;
inline constexpr int panelPadding = 8;

} // namespace more_phi::Theme::Metrics
