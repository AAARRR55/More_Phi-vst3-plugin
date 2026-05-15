/*
 * More-Phi — UI/SnapFader.h
 * Vertical fader with slot markers for 1D morphing.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace more_phi {

class MorePhiProcessor;

class SnapFader : public juce::Component
{
public:
    explicit SnapFader(MorePhiProcessor& p);
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    void updateValue(float yPos);
    MorePhiProcessor& proc_;
    bool dragging_ = false;
};

} // namespace more_phi
