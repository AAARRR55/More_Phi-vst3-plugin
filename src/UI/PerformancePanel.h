/*
 * More-Phi — UI/PerformancePanel.h
 *
 * Horizontal strip of opt-in CPU-saver toggles for morphing and
 * hosted-parameter application. Lives in the Engine tab, below the blend strip.
 *
 * H7: labels + tooltips are written in producer language (these trade some
 * morph smoothness for CPU); the section is headed "CPU SAVER" to make the
 * trade-off explicit. All default OFF (preserve current behaviour).
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

    // F-8: "Lower Write Rate" removed — merged into "Throttle Writes".
    juce::TextButton disableTouchToggle_    { "Pure Morph" };
    juce::TextButton throttleCommitsToggle_ { "Throttle Writes" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PerformancePanel)
};

} // namespace more_phi
