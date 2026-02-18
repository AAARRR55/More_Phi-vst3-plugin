/*
 * MorphSnap — UI/AIStatusPanel.h
 * Bottom bar: MCP server status, port, clients, token copy.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace morphsnap {

class MorphSnapProcessor;

class AIStatusPanel : public juce::Component,
                      private juce::Timer,
                      private juce::Button::Listener
{
public:
    explicit AIStatusPanel(MorphSnapProcessor& p);
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void timerCallback() override;
    void buttonClicked(juce::Button* b) override;

    MorphSnapProcessor& proc_;
    juce::Label statusLabel_;
    juce::Label portLabel_;
    juce::Label clientsLabel_;
    juce::TextButton toggleBtn_{"Start MCP"};
    juce::TextButton copyTokenBtn_{"Copy Token"};
};

} // namespace morphsnap
