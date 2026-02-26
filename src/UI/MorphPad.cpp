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
    showGrid_ = true;  // Enable grid by default (Stitch design)
    startTimerHz(30);   // 30 FPS for smooth animation
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
    const auto bounds = getLocalBounds().toFloat();
    const juce::Point<float> normalizedPoint{
        juce::jlimit(0.0f, 1.0f, e.position.x / bounds.getWidth()),
        juce::jlimit(0.0f, 1.0f, e.position.y / bounds.getHeight())
    };
    int slot = findNearestSlotToPoint(normalizedPoint);
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
    // Compute current cursor position based on morph source
    float x, y;
    int morphSrc = proc_.getMorphSource();

    if (morphSrc == 1)
    {
        // Fader mode: derive XY from fader position along clock positions
        float faderPos = proc_.getFaderPos();
        auto positions = InterpolationEngine::getClockPositions(0.85f);
        auto& bank = proc_.getSnapshotBank();

        int occupiedSlots[SnapshotBank::NUM_SLOTS];
        int numOccupied = 0;
        for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
            if (bank.isOccupied(i))
                occupiedSlots[numOccupied++] = i;

        if (numOccupied >= 2)
        {
            float index = faderPos * static_cast<float>(numOccupied - 1);
            int lo = juce::jlimit(0, numOccupied - 2, static_cast<int>(index));
            float t = index - static_cast<float>(lo);

            auto& posA = positions[occupiedSlots[lo]];
            auto& posB = positions[occupiedSlots[lo + 1]];

            // Clock positions are in [-1,1] range, convert to [0,1] for trail
            x = ((posA.x * (1.0f - t) + posB.x * t) + 1.0f) * 0.5f;
            y = ((posA.y * (1.0f - t) + posB.y * t) + 1.0f) * 0.5f;
        }
        else
        {
            x = 0.5f;
            y = 0.5f;
        }
    }
    else
    {
        x = proc_.getMorphX();
        y = proc_.getMorphY();
    }

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
        // Map clock position to normalized coordinates (0..1)
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

    // Grid (enhanced Stitch style)
    if (showGrid_)
    {
        g.setColour(juce::Colour(0xff0f3460).withAlpha(0.2f));
        // Crosshairs
        g.drawLine(centre.x - radius, centre.y, centre.x + radius, centre.y, 0.5f);
        g.drawLine(centre.x, centre.y - radius, centre.x, centre.y + radius, 0.5f);
        // Diagonal guides
        float diag = radius * 0.707f;
        g.setColour(juce::Colour(0xff0f3460).withAlpha(0.1f));
        g.drawLine(centre.x - diag, centre.y - diag, centre.x + diag, centre.y + diag, 0.5f);
        g.drawLine(centre.x + diag, centre.y - diag, centre.x - diag, centre.y + diag, 0.5f);
        // Concentric rings
        for (float r : {0.33f, 0.66f})
        {
            float ringR = radius * r;
            g.drawEllipse(centre.x - ringR, centre.y - ringR, ringR * 2, ringR * 2, 0.5f);
        }
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

    // ── Cursor position ────────────────────────────────────────────────────────
    // In Direct mode (0): use raw XY input for immediate responsiveness.
    // In Fader mode (1): derive position from faderPos along snapshot positions.
    // In physics modes (Elastic=2, Drift=3): use physics-processed output.
    float cx, cy;
    int physMode = proc_.getPhysicsMode();
    int morphSrc = proc_.getMorphSource();

    if (physMode >= 2)
    {
        // Physics modes: show processed output as main cursor
        float procX = morph.getProcessedX();
        float procY = morph.getProcessedY();
        cx = centre.x + procX * radius;
        cy = centre.y + procY * radius;

        // Also show raw input as faint guide dot
        float rawCx = centre.x + (proc_.getMorphX() * 2.0f - 1.0f) * radius;
        float rawCy = centre.y + (proc_.getMorphY() * 2.0f - 1.0f) * radius;
        g.setColour(juce::Colour(0xff888888).withAlpha(0.4f));
        g.fillEllipse(rawCx - 3, rawCy - 3, 6, 6);
    }
    else if (morphSrc == 1)
    {
        // Fader mode: interpolate cursor along occupied snapshot clock positions
        float faderPos = proc_.getFaderPos();

        // Collect occupied slot indices (reuse 'bank' from snapshot dots above)
        int occupiedSlots[SnapshotBank::NUM_SLOTS];
        int numOccupied = 0;
        for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
            if (bank.isOccupied(i))
                occupiedSlots[numOccupied++] = i;

        if (numOccupied >= 2)
        {
            // Map faderPos [0,1] to a fractional index across occupied slots
            float index = faderPos * static_cast<float>(numOccupied - 1);
            int lo = juce::jlimit(0, numOccupied - 2, static_cast<int>(index));
            int hi = lo + 1;
            float t = index - static_cast<float>(lo);

            auto& posA = positions[occupiedSlots[lo]];
            auto& posB = positions[occupiedSlots[hi]];

            float interpX = posA.x * (1.0f - t) + posB.x * t;
            float interpY = posA.y * (1.0f - t) + posB.y * t;

            cx = centre.x + interpX * radius;
            cy = centre.y + interpY * radius;
        }
        else if (numOccupied == 1)
        {
            cx = centre.x + positions[occupiedSlots[0]].x * radius;
            cy = centre.y + positions[occupiedSlots[0]].y * radius;
        }
        else
        {
            cx = centre.x;
            cy = centre.y;
        }
    }
    else
    {
        // Direct mode: use raw mouse position [0,1] -> [-1,1] -> screen
        float rawX = proc_.getMorphX() * 2.0f - 1.0f;
        float rawY = proc_.getMorphY() * 2.0f - 1.0f;
        cx = centre.x + rawX * radius;
        cy = centre.y + rawY * radius;
    }

    // Radial glow (Stitch-enhanced pulsing effect)
    float glowPhase = static_cast<float>(juce::Time::getMillisecondCounter() % 2000) / 2000.0f;
    float glowAlpha = 0.12f + 0.06f * std::sin(glowPhase * 6.2832f);
    g.setColour(juce::Colour(0xffec415d).withAlpha(glowAlpha));
    g.fillEllipse(cx - 24, cy - 24, 48, 48);
    g.setColour(juce::Colour(0xffec415d).withAlpha(glowAlpha * 0.5f));
    g.fillEllipse(cx - 32, cy - 32, 64, 64);

    // Glow ring
    g.setColour(juce::Colour(0xffe94560).withAlpha(0.3f));
    g.drawEllipse(cx - 12, cy - 12, 24, 24, 1.0f);

    // Cursor dot
    g.setColour(juce::Colour(0xffe94560));
    g.fillEllipse(cx - 5, cy - 5, 10, 10);

    // Mode label (top-left corner of pad)
    const char* modeNames[] = {"2D Pad", "Fader", "Elastic", "Drift"};
    int modeIdx = juce::jlimit(0, 3, proc_.getPhysicsMode());
    g.setColour(juce::Colour(0xff8b95a5).withAlpha(0.7f));
    g.setFont(juce::Font(juce::FontOptions(10.0f)));
    g.drawText(juce::String("Mode: ") + modeNames[modeIdx],
               bounds.toNearestInt().withHeight(16).translated(8, 4),
               juce::Justification::centredLeft);
}

void MorphPad::mouseDown(const juce::MouseEvent& e) { updatePosition(e.position); }
void MorphPad::mouseDrag(const juce::MouseEvent& e) { updatePosition(e.position); }

void MorphPad::updatePosition(juce::Point<float> pos)
{
    auto bounds = getLocalBounds().toFloat();
    float x = juce::jlimit(0.0f, 1.0f, pos.x / bounds.getWidth());
    float y = juce::jlimit(0.0f, 1.0f, pos.y / bounds.getHeight());
    proc_.setMorphX(x);
    proc_.setMorphY(y);
    proc_.setMorphSource(0);  // XY mode
    repaint();
}

} // namespace morphsnap
