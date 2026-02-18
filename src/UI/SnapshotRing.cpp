/*
 * MorphSnap — UI/SnapshotRing.cpp
 */
#include "SnapshotRing.h"
#include "Plugin/PluginProcessor.h"

namespace morphsnap {

void SnapshotRing::paint(juce::Graphics& g)
{
    // Draw is handled by MorphPad — this overlay adds interactivity only.
    // Optionally draw slot labels or glow effects here for selected slots.
    (void)g;
}

void SnapshotRing::mouseDown(const juce::MouseEvent& e)
{
    int slot = hitTestSlot(e.position);
    if (slot < 0) return;

    auto& bank = proc_.getSnapshotBank();

    if (e.mods.isRightButtonDown())
    {
        // Right-click: capture current state to this slot
        bank.capture(slot, proc_.getParameterBridge());
    }
    else
    {
        // Left-click: recall this slot if occupied
        if (bank.isOccupied(slot))
            bank.recall(slot, proc_.getParameterBridge());
    }

    repaint();
    if (auto* parent = getParentComponent()) parent->repaint();
}

int SnapshotRing::hitTestSlot(juce::Point<float> pos) const
{
    auto bounds = getLocalBounds().toFloat();
    auto centre = bounds.getCentre();
    float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;

    auto positions = InterpolationEngine::getClockPositions(0.85f);
    const float hitRadius = 15.0f;

    for (int i = 0; i < 12; ++i)
    {
        float dotX = centre.x + positions[i].x * radius;
        float dotY = centre.y + positions[i].y * radius;
        float dist = pos.getDistanceFrom({dotX, dotY});
        if (dist < hitRadius)
            return i;
    }
    return -1;
}

} // namespace morphsnap
