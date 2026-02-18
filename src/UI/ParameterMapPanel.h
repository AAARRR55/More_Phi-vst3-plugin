/*
 * MorphSnap — UI/ParameterMapPanel.h
 * Scrollable panel showing ALL hosted plugin parameters with sliders.
 * Supports learn-mode to whitelist which params are included in morphing.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <set>

namespace morphsnap {

class MorphSnapProcessor;

// A single parameter row: checkbox + name + slider + value readout
class ParameterRow : public juce::Component
{
public:
    ParameterRow(int paramIndex, MorphSnapProcessor& proc);
    void resized() override;
    void refresh();

    int getParamIndex() const { return index_; }
    bool isMorphEnabled() const { return morphToggle_.getToggleState(); }

private:
    int index_;
    MorphSnapProcessor& proc_;
    juce::ToggleButton morphToggle_;
    juce::Label nameLabel_;
    juce::Slider slider_;
    juce::Label valueLabel_;
    bool syncing_ = false;
};

// The full scrollable panel
class ParameterMapPanel : public juce::Component,
                           private juce::Timer
{
public:
    explicit ParameterMapPanel(MorphSnapProcessor& proc);
    void resized() override;
    void paint(juce::Graphics& g) override;

    // Returns the set of parameter indices enabled for morphing
    std::set<int> getMorphEnabledParams() const;

    // Rebuild rows when plugin changes
    void rebuildForPlugin();

private:
    void timerCallback() override;

    MorphSnapProcessor& proc_;
    juce::Viewport viewport_;
    juce::Component rowContainer_;
    std::vector<std::unique_ptr<ParameterRow>> rows_;
    juce::Label headerLabel_;
    juce::TextButton selectAllBtn_{"All"};
    juce::TextButton selectNoneBtn_{"None"};
    int lastParamCount_ = 0;
};

} // namespace morphsnap
