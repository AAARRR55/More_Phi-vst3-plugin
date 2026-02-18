/* MorphSnap — UI/SnapFader.cpp */
#include "SnapFader.h"
#include "Plugin/PluginProcessor.h"

namespace morphsnap {

SnapFader::SnapFader(MorphSnapProcessor& p) : proc_(p) {}

void SnapFader::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    float trackX = bounds.getCentreX();
    float trackTop = bounds.getY() + 10;
    float trackBot = bounds.getBottom() - 10;
    float trackH = trackBot - trackTop;

    // Background
    g.setColour(juce::Colour(0xff0d1b2a));
    g.fillRoundedRectangle(bounds, 6.0f);

    // Track groove
    g.setColour(juce::Colour(0xff1a2742));
    g.fillRoundedRectangle(trackX - 2.5f, trackTop, 5, trackH, 2.5f);

    // Slot markers along the track
    auto& bank = proc_.getSnapshotBank();
    int occupied = 0;
    for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
        if (bank.isOccupied(i)) occupied++;

    if (occupied > 1)
    {
        int idx = 0;
        for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
        {
            if (!bank.isOccupied(i)) continue;
            float frac = static_cast<float>(idx) / static_cast<float>(occupied - 1);
            float markerY = trackTop + (1.0f - frac) * trackH;

            // Tick mark
            g.setColour(juce::Colour(0xff533483));
            g.fillRoundedRectangle(trackX - 8, markerY - 1.5f, 16, 3, 1.5f);

            // Slot number
            g.setColour(juce::Colour(0xff8b95a5));
            g.setFont(8.0f);
            g.drawText(juce::String(i + 1), static_cast<int>(trackX + 10),
                       static_cast<int>(markerY - 6), 16, 12,
                       juce::Justification::centredLeft);
            ++idx;
        }
    }

    // Filled portion (from bottom to thumb)
    float faderPos = proc_.faderPos.load(std::memory_order_relaxed);
    float thumbY = trackTop + (1.0f - faderPos) * trackH;

    g.setColour(juce::Colour(0xffec415d).withAlpha(0.6f));
    g.fillRoundedRectangle(trackX - 2.5f, thumbY, 5, trackBot - thumbY, 2.5f);

    // Thumb glow
    g.setColour(juce::Colour(0xffec415d).withAlpha(0.15f));
    g.fillEllipse(trackX - 12, thumbY - 12, 24, 24);

    // Thumb
    g.setColour(juce::Colour(0xffe8eaed));
    g.fillRoundedRectangle(trackX - 10, thumbY - 5, 20, 10, 5.0f);
    g.setColour(juce::Colour(0xffec415d));
    g.fillRoundedRectangle(trackX - 6, thumbY - 1.5f, 12, 3, 1.5f);
}

void SnapFader::mouseDown(const juce::MouseEvent& e) { updateValue(e.position.y); }
void SnapFader::mouseDrag(const juce::MouseEvent& e) { updateValue(e.position.y); }

void SnapFader::updateValue(float yPos)
{
    float trackTop = 10.0f;
    float trackBot = getHeight() - 10.0f;
    float normalised = 1.0f - juce::jlimit(0.0f, 1.0f,
        (yPos - trackTop) / (trackBot - trackTop));

    proc_.faderPos.store(normalised, std::memory_order_relaxed);
    proc_.morphSource.store(1, std::memory_order_relaxed);  // Switch to fader mode
    repaint();
}

} // namespace morphsnap
