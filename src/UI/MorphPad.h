/*
 * MorphSnap - UI/MorphPad.h
 * Main XY morphing pad with visualization and interaction.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

namespace morphsnap {

class MorphSnapProcessor;

class MorphPad : public juce::Component,
                 private juce::Timer
{
public:
    enum class VisualizationMode
    {
        twoD = 0,
        threeD = 1,
        spectral = 2
    };

    explicit MorphPad(MorphSnapProcessor& processor);

    void paint(juce::Graphics& g) override;
    void resized() override;

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;

    void setGridVisible(bool shouldShow);
    void setPathVisible(bool shouldShow);
    void setVisualizationMode(int modeIndex);

private:
    void timerCallback() override;
    void updatePosition(juce::Point<float> pos);

    juce::Rectangle<float> getPadBounds() const;
    juce::Point<float> normalizedToPoint(float xNorm, float yNorm) const;
    juce::Point<float> pointToNormalized(juce::Point<float> point) const;

    void setMorphPositionFromPoint(juce::Point<float> point);
    void appendTrailPoint(juce::Point<float> point);

    void drawBackground(juce::Graphics& g, juce::Rectangle<float> padBounds) const;
    void drawGrid(juce::Graphics& g, juce::Rectangle<float> padBounds) const;
    void drawAxes(juce::Graphics& g, juce::Rectangle<float> padBounds) const;
    void drawTrail(juce::Graphics& g) const;
    void drawSnapshotMarkers(juce::Graphics& g, juce::Rectangle<float> padBounds) const;
    void drawPositionIndicator(juce::Graphics& g, juce::Point<float> position) const;
    void drawModeBadge(juce::Graphics& g, juce::Rectangle<float> padBounds) const;

    int findNextEmptySlot() const;
    int findNearestSlotToPoint(juce::Point<float> point) const;
    void captureSnapshotAtSlot(int slot);

    MorphSnapProcessor& proc_;
    bool dragging_ = false;
    bool showGrid_ = true;
    bool showPath_ = true;
    VisualizationMode visMode_ = VisualizationMode::twoD;

    std::vector<juce::Point<float>> trailPoints_;
    juce::Point<float> lastTrailPoint_{ -1.0f, -1.0f };

    static constexpr size_t maxTrailPoints_ = 80;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MorphPad)
};

} // namespace morphsnap
