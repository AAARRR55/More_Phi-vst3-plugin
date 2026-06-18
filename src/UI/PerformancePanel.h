/*
 * More-Phi — UI/PerformancePanel.h
 *
 * Horizontal strip of opt-in CPU/performance toggles for morphing and
 * hosted-parameter application. Lives in the Engine tab, below the blend strip.
 *
 * Toggles (all default OFF — preserve current behaviour):
 *   Coarse Writes        — raise the apply-loop write deadband (fewer setValue calls)
 *   Disable Touch Detect — skip the per-block batch getValue() read + touch logic
 *   Throttle Commits     — push setValue to the hosted plugin only every Nth block
 *
 * Bound to APVTS so they support DAW automation and persist with state.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace more_phi {

class MorePhiProcessor;

class PerformancePanel : public juce::Component
{
public:
    explicit PerformancePanel(MorePhiProcessor& proc);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void setupToggleButton(juce::TextButton& btn);
    void drawSectionLabel(juce::Graphics& g, const juce::String& text,
                          juce::Rectangle<int> bounds) const;

    MorePhiProcessor& proc_;

    static constexpr int kLabelWidth = 96;

    juce::TextButton coarseWritesToggle_    { "Coarse Writes" };
    juce::TextButton disableTouchToggle_    { "Disable Touch Detect" };
    juce::TextButton throttleCommitsToggle_ { "Throttle Commits" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PerformancePanel)
};

} // namespace more_phi
