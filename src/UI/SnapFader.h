/*
 * More-Phi — UI/SnapFader.h
 * Vertical fader with slot markers for 1D morphing.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdint>

namespace more_phi {

class MorePhiProcessor;

class SnapFader : public juce::Component,
                  public juce::SettableTooltipClient,
                  private juce::Timer
{
public:
    explicit SnapFader(MorePhiProcessor& p);
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

    // AUDIT-FIX (accessibility): keyboard-operable + screen-reader labelled.
    bool keyPressed(const juce::KeyPress& key) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

#if MORE_PHI_TEST_MODE
    bool isAutoRefreshTimerRunningForTests() const { return isTimerRunning(); }
    bool hasExternalStateChangedForTests() const { return hasExternalStateChanged(); }
#endif

private:
    void updateValue(float yPos);
    void timerCallback() override;
    uint16_t getOccupiedSnapshotMask() const;
    bool hasExternalStateChanged() const;

    MorePhiProcessor& proc_;
    bool dragging_ = false;
    float lastPaintedFaderPos_ = -1.0f;
    int lastPaintedMorphSource_ = -1;
    uint16_t lastSnapshotMask_ = 0xffff;
};

} // namespace more_phi
