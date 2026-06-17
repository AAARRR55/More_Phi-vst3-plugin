/* More-Phi — UI/SnapFader.cpp */
#include "SnapFader.h"
#include "Plugin/PluginProcessor.h"
#include "UI/Bindings/ParameterBinding.h"

#include <cmath>

namespace more_phi {

SnapFader::SnapFader(MorePhiProcessor& p) : proc_(p)
{
    startTimerHz(15);
}

void SnapFader::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    float trackX = bounds.getCentreX();
    float trackTop = bounds.getY() + 10;
    float trackBot = bounds.getBottom() - 10;
    float trackH = trackBot - trackTop;
    if (trackH <= 0.0f) trackH = 1.0f;

    // Background
    g.setColour(juce::Colour(0xff070709));
    g.fillRoundedRectangle(bounds, 6.0f);

    // Track groove
    g.setColour(juce::Colour(0xff17181c));
    g.fillRoundedRectangle(trackX - 2.5f, trackTop, 5, trackH, 2.5f);

    // Slot markers along the track
    const uint16_t snapshotMask = getOccupiedSnapshotMask();
    int occupied = 0;
    for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
        if ((snapshotMask & (uint16_t{1} << i)) != 0) occupied++;

    if (occupied > 1)
    {
        int idx = 0;
        for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
        {
            if ((snapshotMask & (uint16_t{1} << i)) == 0) continue;
            float frac = static_cast<float>(idx) / static_cast<float>(occupied - 1);
            float markerY = trackTop + (1.0f - frac) * trackH;

            // Tick mark
            g.setColour(juce::Colour(0xffe22edb));
            g.fillRoundedRectangle(trackX - 8, markerY - 1.5f, 16, 3, 1.5f);

            // Slot number — with minimum font size for readability
            g.setColour(juce::Colour(0xff8e8f95));
            float slotFont = juce::jmax(bounds.getWidth() * 0.16f, 10.0f);
            g.setFont(slotFont);
            g.drawText(juce::String(i + 1), static_cast<int>(trackX + 10),
                       static_cast<int>(markerY - 6), 20, 14,
                       juce::Justification::centredLeft);
            ++idx;
        }
    }

    // Filled portion (from bottom to thumb)
    float faderPos = proc_.getFaderPos();
    lastPaintedFaderPos_ = faderPos;
    lastPaintedMorphSource_ = proc_.getMorphSource();
    lastSnapshotMask_ = snapshotMask;
    float thumbY = trackTop + (1.0f - faderPos) * trackH;

    g.setColour(juce::Colour(0xffe5c057).withAlpha(0.6f));
    g.fillRoundedRectangle(trackX - 2.5f, thumbY, 5, trackBot - thumbY, 2.5f);

    // Thumb glow
    g.setColour(juce::Colour(0xffe5c057).withAlpha(0.15f));
    g.fillEllipse(trackX - 12, thumbY - 12, 24, 24);

    // Thumb
    g.setColour(juce::Colour(0xffeeeef2));
    g.fillRoundedRectangle(trackX - 10, thumbY - 5, 20, 10, 5.0f);
    g.setColour(juce::Colour(0xffe5c057));
    g.fillRoundedRectangle(trackX - 6, thumbY - 1.5f, 12, 3, 1.5f);
}

void SnapFader::mouseDown(const juce::MouseEvent& e)
{
    dragging_ = true;
    if (auto* p = proc_.getAPVTS().getParameter("faderPos"))
        p->beginChangeGesture();
    updateValue(e.position.y);
}
void SnapFader::mouseDrag(const juce::MouseEvent& e) { updateValue(e.position.y); }
void SnapFader::mouseUp(const juce::MouseEvent&)
{
    if (dragging_)
        if (auto* p = proc_.getAPVTS().getParameter("faderPos"))
            p->endChangeGesture();
    dragging_ = false;
}

void SnapFader::updateValue(float yPos)
{
    float trackTop = 10.0f;
    float trackBot = getHeight() - 10.0f;
    float normalised = 1.0f - juce::jlimit(0.0f, 1.0f,
        (yPos - trackTop) / (trackBot - trackTop));

    // Route through APVTS so DAW automation captures the change.
    if (auto* p = proc_.getAPVTS().getParameter("faderPos"))
        p->setValueNotifyingHost(normalised);

    ParameterBinding::setChoiceIndexWithGesture(proc_.getAPVTS(), "morphSource", 1, 2);
    proc_.setMorphSource(1);  // Switch to fader mode
    repaint();
}

uint16_t SnapFader::getOccupiedSnapshotMask() const
{
    uint16_t mask = 0;
    auto& bank = proc_.getSnapshotBank();
    for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
        if (bank.isOccupied(i))
            mask = static_cast<uint16_t>(mask | (uint16_t{1} << i));
    return mask;
}

void SnapFader::timerCallback()
{
    if (hasExternalStateChanged())
        repaint();
}

bool SnapFader::hasExternalStateChanged() const
{
    return std::abs(proc_.getFaderPos() - lastPaintedFaderPos_) > 0.001f
        || proc_.getMorphSource() != lastPaintedMorphSource_
        || getOccupiedSnapshotMask() != lastSnapshotMask_;
}

} // namespace more_phi
