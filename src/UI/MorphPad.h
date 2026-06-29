/*
 * More-Phi — UI/MorphPad.h
 * 2D XY Morph Pad with clock-layout snapshot dots.
 * Optimized for zero allocations during rendering.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>

namespace more_phi {

class MorePhiProcessor;

enum class VisualizationMode
{
    Standard = 0,
    Heatmap = 1,
    Timeline = 2
};

class MorphPad : public juce::Component,
                 public juce::SettableTooltipClient,
                 private juce::Timer
{
public:
    explicit MorphPad(MorePhiProcessor& p);
    ~MorphPad() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;

    // AUDIT-FIX (accessibility): keyboard-operable + screen-reader labelled.
    bool keyPressed(const juce::KeyPress& key) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    // Visualization options
    void setGridVisible(bool shouldShow);
    void setPathVisible(bool shouldShow);
    void setVisualizationMode(int modeIndex);

    // Dirty flag for efficient repaint scheduling
    void markDirty() { snapshotStateDirty_ = true; }

    /** Show a brief status message on the pad (e.g. capture/recall confirmation). */
    void showFlash(const juce::String& message)
    {
        flashMessage_ = message;
        flashTicks_ = 90;  // ~3 seconds at 30Hz
    }

private:
    void timerCallback() override;
    void updatePosition(juce::Point<float> pos);
    int findNextEmptySlot() const;
    int findNearestSlotToPoint(juce::Point<float> point) const;
    void captureSnapshotAtSlot(int slot);

    // OPTIMIZATION: Fixed-size ring buffer for trail (no allocations)
    void appendTrailPoint(juce::Point<float> point);

    MorePhiProcessor& proc_;

    // Trail ring buffer (fixed size - no dynamic allocation)
    static constexpr int maxTrailPoints_ = 64;
    std::array<juce::Point<float>, maxTrailPoints_> trailBuffer_{};
    int trailHead_ = 0;
    int trailCount_ = 0;

    juce::Point<float> lastTrailPoint_{-1.0f, -1.0f};
    bool dragging_ = false;
    bool showGrid_ = true;
    bool showPath_ = true;
    VisualizationMode visMode_ = VisualizationMode::Standard;

    // Flash message for capture/recall feedback
    juce::String flashMessage_;
    int flashTicks_ = 0;

    float lastRepaintX_ = -1.0f;
    float lastRepaintY_ = -1.0f;
    bool snapshotStateDirty_ = true;
    bool needsRepaint_ = true;
};

} // namespace more_phi
