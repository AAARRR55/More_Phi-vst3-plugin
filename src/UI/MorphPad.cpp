/* MorphSnap — UI/MorphPad.cpp
 * Optimized for zero allocations during rendering.
 */
#include "MorphPad.h"
#include "Plugin/PluginProcessor.h"
#include "Core/InterpolationEngine.h"

namespace morphsnap {

MorphPad::MorphPad(MorphSnapProcessor& processor)
    : proc_(processor)
{
    // Initialize trail buffer
    trailBuffer_.fill(juce::Point<float>{0.5f, 0.5f});
    startTimerHz(30);  // 30 FPS for smooth animation
}

void MorphPad::resized()
{
    // Reset trail on resize
    trailHead_ = 0;
    trailCount_ = 0;
}

void MorphPad::mouseUp(const juce::MouseEvent&) { dragging_ = false; }

void MorphPad::mouseDoubleClick(const juce::MouseEvent& e)
{
    // Find nearest empty slot and capture
    int slot = findNearestSlotToPoint(e.position);
    captureSnapshotAtSlot(slot);
}

void MorphPad::setGridVisible(bool shouldShow)
{
    showGrid_ = shouldShow;
    repaint();
}

void MorphPad::setPathVisible(bool shouldShow)
{
    showPath_ = shouldShow;
    repaint();
}

void MorphPad::setVisualizationMode(int modeIndex)
{
    visMode_ = static_cast<VisualizationMode>(modeIndex);
    repaint();
}

void MorphPad::timerCallback()
{
    // Update trail points from audio-thread trail
    float x = proc_.morphX.load(std::memory_order_relaxed);
    float y = proc_.morphY.load(std::memory_order_relaxed);
    juce::Point<float> currentPos{x, y};

    if (lastTrailPoint_.getDistanceFrom(currentPos) > 0.01f)
    {
        appendTrailPoint(currentPos);
        lastTrailPoint_ = currentPos;
    }

    repaint();
}

// OPTIMIZATION: Ring buffer implementation - zero allocations
void MorphPad::appendTrailPoint(juce::Point<float> point)
{
    trailBuffer_[trailHead_] = point;
    trailHead_ = (trailHead_ + 1) % maxTrailPoints_;
    if (trailCount_ < maxTrailPoints_)
        ++trailCount_;
}

int MorphPad::findNextEmptySlot() const
{
    auto& bank = proc_.getSnapshotBank();
    for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
    {
        if (!bank.isOccupied(i))
            return i;
    }
    return 0;
}

int MorphPad::findNearestSlotToPoint(juce::Point<float> point) const
{
    auto positions = InterpolationEngine::getClockPositions(0.85f);
    float minDist = std::numeric_limits<float>::max();
    int nearest = 0;

    for (int i = 0; i < 12; ++i)
    {
        // Map clock position to screen coordinates (0-1 range)
        float screenX = (positions[i].x + 1.0f) * 0.5f;
        float screenY = (positions[i].y + 1.0f) * 0.5f;
        float dist = point.getDistanceFrom({screenX, screenY});
        if (dist < minDist)
        {
            minDist = dist;
            nearest = i;
        }
    }
    return nearest;
}

void MorphPad::captureSnapshotAtSlot(int slot)
{
    proc_.getSnapshotBank().capture(slot, proc_.getParameterBridge());
    repaint();
}

void MorphPad::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced(2.0f);
    auto centre = bounds.getCentre();
    float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;

    // Background circle
    g.setColour(juce::Colour(0xff0d1b2a));
    g.fillEllipse(centre.x - radius, centre.y - radius, radius * 2, radius * 2);

    // Grid (optional)
    if (showGrid_)
    {
        g.setColour(juce::Colour(0xff0f3460).withAlpha(0.3f));
        g.drawLine(centre.x - radius, centre.y, centre.x + radius, centre.y, 0.5f);
        g.drawLine(centre.x, centre.y - radius, centre.x, centre.y + radius, 0.5f);
    }

    // Border
    g.setColour(juce::Colour(0xff0f3460));
    g.drawEllipse(centre.x - radius, centre.y - radius, radius * 2, radius * 2, 1.5f);

    // ── Cursor trail (from audio thread) ───────────────────────────────────────
    auto& morph = proc_.getMorphProcessor();
    const auto& trail = morph.getTrail();
    int head = morph.getTrailHead();
    constexpr int trailLen = MorphProcessor::TRAIL_SIZE;

    for (int i = 0; i < trailLen - 1; ++i)
    {
        int idx0 = (head - trailLen + i + trailLen * 2) % trailLen;
        int idx1 = (idx0 + 1) % trailLen;

        float alpha = static_cast<float>(i) / static_cast<float>(trailLen) * 0.5f;
        float px0 = centre.x + (trail[idx0].x * 2.0f - 1.0f) * radius;
        float py0 = centre.y + (trail[idx0].y * 2.0f - 1.0f) * radius;
        float px1 = centre.x + (trail[idx1].x * 2.0f - 1.0f) * radius;
        float py1 = centre.y + (trail[idx1].y * 2.0f - 1.0f) * radius;

        g.setColour(juce::Colour(0xffe94560).withAlpha(alpha));
        g.drawLine(px0, py0, px1, py1, 1.5f);
    }

    // ── Snapshot dots (clock layout) ───────────────────────────────────────────
    auto positions = InterpolationEngine::getClockPositions(0.85f);
    auto& bank = proc_.getSnapshotBank();
    for (int i = 0; i < 12; ++i)
    {
        float dotX = centre.x + positions[i].x * radius;
        float dotY = centre.y + positions[i].y * radius;
        float dotR = bank.isOccupied(i) ? 6.0f : 4.0f;

        g.setColour(bank.isOccupied(i) ? juce::Colour(0xffe94560) : juce::Colour(0xff533483));
        g.fillEllipse(dotX - dotR, dotY - dotR, dotR * 2, dotR * 2);

        // Slot number
        g.setColour(juce::Colour(0xffe0e0e0).withAlpha(0.6f));
        g.setFont(9.0f);
        g.drawText(juce::String(i + 1), static_cast<int>(dotX - 6), static_cast<int>(dotY + 8),
                   12, 10, juce::Justification::centred);
    }

    // ── Physics-processed cursor position ──────────────────────────────────────
    float procX = morph.getProcessedX();
    float procY = morph.getProcessedY();
    // Map [-1,1] → screen coordinates
    float cx = centre.x + procX * radius;
    float cy = centre.y + procY * radius;

    // Glow ring
    g.setColour(juce::Colour(0xffe94560).withAlpha(0.15f));
    g.fillEllipse(cx - 16, cy - 16, 32, 32);
    g.setColour(juce::Colour(0xffe94560).withAlpha(0.3f));
    g.drawEllipse(cx - 12, cy - 12, 24, 24, 1.0f);

    // Cursor dot
    g.setColour(juce::Colour(0xffe94560));
    g.fillEllipse(cx - 5, cy - 5, 10, 10);

    // Raw input position (faint, shows where you're dragging)
    if (proc_.physicsMode.load() > 0)  // Only in physics modes
    {
        float rawCx = centre.x + (proc_.morphX.load() * 2.0f - 1.0f) * radius;
        float rawCy = centre.y + (proc_.morphY.load() * 2.0f - 1.0f) * radius;
        g.setColour(juce::Colour(0xff888888).withAlpha(0.4f));
        g.fillEllipse(rawCx - 3, rawCy - 3, 6, 6);
    }
}

void MorphPad::mouseDown(const juce::MouseEvent& e) { updatePosition(e.position); }
void MorphPad::mouseDrag(const juce::MouseEvent& e) { updatePosition(e.position); }

void MorphPad::updatePosition(juce::Point<float> pos)
{
    auto bounds = getLocalBounds().toFloat();
    float x = juce::jlimit(0.0f, 1.0f, pos.x / bounds.getWidth());
    float y = juce::jlimit(0.0f, 1.0f, pos.y / bounds.getHeight());
    proc_.morphX.store(x, std::memory_order_relaxed);
    proc_.morphY.store(y, std::memory_order_relaxed);
    proc_.morphSource.store(0, std::memory_order_relaxed);  // XY mode
    repaint();
}

} // namespace morphsnap
