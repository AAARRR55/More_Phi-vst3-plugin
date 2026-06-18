/*
 * More-Phi — UI/ParameterMapPanel.h
 * Scrollable panel showing ALL hosted plugin parameters with sliders
 * and live value readouts.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

namespace more_phi {

class MorePhiProcessor;

// A single parameter row: name + slider + value readout
class ParameterRow : public juce::Component
{
public:
    ParameterRow(int paramIndex, MorePhiProcessor& proc);
    void resized() override;
    void refresh();

    int getParamIndex() const { return index_; }

private:
    int index_;
    MorePhiProcessor& proc_;
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
    explicit ParameterMapPanel(MorePhiProcessor& proc);
    void resized() override;
    void paint(juce::Graphics& g) override;

    // Rebuild rows when plugin changes
    void rebuildForPlugin();

private:
    void timerCallback() override;

    MorePhiProcessor& proc_;
    juce::Viewport viewport_;
    juce::Component rowContainer_;
    std::vector<std::unique_ptr<ParameterRow>> rows_;
    juce::Label headerLabel_;
    int lastParamCount_ = 0;
};

} // namespace more_phi
