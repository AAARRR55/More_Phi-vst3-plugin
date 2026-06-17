/* More-Phi — UI/MorphPad.cpp
 * Optimized for zero allocations during rendering.
 */
#include "MorphPad.h"
#include "Plugin/PluginProcessor.h"
#include "Core/InterpolationEngine.h"
#include "UI/Theme/MorePhiTheme.h"
#include "UI/Bindings/ParameterBinding.h"

namespace more_phi {

using namespace Theme::Colours;

MorphPad::MorphPad(MorePhiProcessor& processor)
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

void MorphPad::mouseUp(const juce::MouseEvent&)
{
    if (dragging_)
    {
        if (auto* px = proc_.getAPVTS().getParameter("morphX"))
            px->endChangeGesture();
        if (auto* py = proc_.getAPVTS().getParameter("morphY"))
            py->endChangeGesture();
    }
    dragging_ = false;
}

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
        else if (numOccupied == 1)
        {
            auto& pos = positions[occupiedSlots[0]];
            x = (pos.x + 1.0f) * 0.5f;
            y = (pos.y + 1.0f) * 0.5f;
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
        needsRepaint_ = true;
    }

    // Only repaint if position actually changed
    if (std::abs(x - lastRepaintX_) > 0.001f ||
        std::abs(y - lastRepaintY_) > 0.001f ||
        snapshotStateDirty_)
    {
        lastRepaintX_ = x;
        lastRepaintY_ = y;
        snapshotStateDirty_ = false;
        needsRepaint_ = true;
    }

    if (needsRepaint_)
    {
        needsRepaint_ = false;
        repaint();
    }
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
    proc_.captureSnapshotToSlot(slot, true);
    repaint();
}

void MorphPad::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced(2.0f);
    auto centre = bounds.getCentre();
    float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;

    // Background circle
    g.setColour(bg());
    g.fillEllipse(centre.x - radius, centre.y - radius, radius * 2, radius * 2);

    // Grid (enhanced Stitch style)
    if (showGrid_)
    {
        g.setColour(padGrid().withAlpha(0.2f));
        // Crosshairs
        g.drawLine(centre.x - radius, centre.y, centre.x + radius, centre.y, 0.5f);
        g.drawLine(centre.x, centre.y - radius, centre.x, centre.y + radius, 0.5f);
        // Diagonal guides
        float diag = radius * 0.707f;
        g.setColour(padGrid().withAlpha(0.1f));
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
    g.setColour(padGrid());
    g.drawEllipse(centre.x - radius, centre.y - radius, radius * 2, radius * 2, 1.5f);

    // Center "MORPH" watermark label (mockup style) — sits behind the cursor
    {
        float labelFont = juce::jlimit(8.0f, 11.0f, radius * 0.055f);
        g.setColour(accent().withAlpha(0.45f));
        g.setFont(juce::Font(labelFont, juce::Font::bold));
        g.drawText("MORPH",
                   juce::Rectangle<float>(centre.x - radius * 0.4f, centre.y - labelFont,
                                          radius * 0.8f, labelFont * 2.0f),
                   juce::Justification::centred);
    }

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

        g.setColour(padTrail().withAlpha(alpha));
        g.drawLine(px0, py0, px1, py1, 1.5f);
    }

    // ── Snapshot nodes (clock layout) ──────────────────────────────────────────
    // Mockup style: numbered ring-nodes around the clock. Filled slots glow cyan
    // with the number inside; empty slots are muted hairline circles.
    auto positions = InterpolationEngine::getClockPositions(0.85f);
    auto& bank = proc_.getSnapshotBank();
    float nodeR = juce::jmax(radius * 0.075f, 11.0f);
    float nodeFontSize = juce::jmax(radius * 0.06f, 9.0f);
    for (int i = 0; i < 12; ++i)
    {
        float dotX = centre.x + positions[i].x * radius;
        float dotY = centre.y + positions[i].y * radius;
        bool occupied = bank.isOccupied(i);

        // Glow halo behind filled nodes
        if (occupied)
        {
            float haloR = nodeR * 1.8f;
            g.setColour(cyan().withAlpha(0.18f));
            g.fillEllipse(dotX - haloR, dotY - haloR, haloR * 2, haloR * 2);
        }

        // Node body
        g.setColour(occupied ? cyan().withAlpha(0.18f) : surfaceLit());
        g.fillEllipse(dotX - nodeR, dotY - nodeR, nodeR * 2, nodeR * 2);

        // Node ring
        g.setColour(occupied ? cyanBright() : border());
        g.drawEllipse(dotX - nodeR, dotY - nodeR, nodeR * 2, nodeR * 2, 1.2f);

        // Slot number centered inside the node
        g.setColour(occupied ? cyanBright() : textSlot());
        g.setFont(juce::Font(nodeFontSize, juce::Font::bold));
        g.drawText(juce::String(i + 1),
                   juce::Rectangle<float>(dotX - nodeR, dotY - nodeR, nodeR * 2, nodeR * 2),
                   juce::Justification::centred);
    }

    // ── Cursor position ────────────────────────────────────────────────────────
    // Source determines the target; physics only modifies XY-pad targets in the
    // audio thread. Therefore the UI must check the source first so Fader mode
    // is displayed correctly even when a physics mode is active.
    //   MorphSource: 0 = 2D Pad, 1 = Fader
    //   MorphMode:   0 = Direct, 1 = Elastic, 2 = Drift
    float cx, cy;
    int physMode = proc_.getPhysicsMode();
    int morphSrc = proc_.getMorphSource();

    if (morphSrc == 1)
    {
        // Fader mode: interpolate cursor along occupied snapshot clock positions.
        // This branch must take precedence over physics so the cursor is not
        // misplaced when Fader source and Drift/Elastic physics are combined.
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
    else if (physMode >= 1)
    {
        // Elastic (1) and Drift (2) physics modes use processed output.
        float procX = morph.getProcessedX();
        float procY = morph.getProcessedY();
        cx = centre.x + procX * radius;
        cy = centre.y + procY * radius;

        // Also show raw input as faint guide dot
        float rawCx = centre.x + (proc_.getMorphX() * 2.0f - 1.0f) * radius;
        float rawCy = centre.y + (proc_.getMorphY() * 2.0f - 1.0f) * radius;
        float guideR = radius * 0.02f;
        g.setColour(padGuide().withAlpha(0.4f));
        g.fillEllipse(rawCx - guideR, rawCy - guideR, guideR * 2, guideR * 2);
    }
    else
    {
        // Direct mode: use raw mouse position [0,1] -> [-1,1] -> screen
        float rawX = proc_.getMorphX() * 2.0f - 1.0f;
        float rawY = proc_.getMorphY() * 2.0f - 1.0f;
        cx = centre.x + rawX * radius;
        cy = centre.y + rawY * radius;
    }

    // ── Dashed connection lines (cursor → filled snapshots) ────────────────────
    // Mockup style: faint cyan dashed links from the puck to each occupied slot.
    {
        const float dashes[] = { 3.0f, 4.0f };
        g.setColour(cyan().withAlpha(0.22f));
        for (int i = 0; i < 12; ++i)
        {
            if (!bank.isOccupied(i))
                continue;
            float dotX = centre.x + positions[i].x * radius;
            float dotY = centre.y + positions[i].y * radius;
            juce::Line<float> link(cx, cy, dotX, dotY);
            g.drawDashedLine(link, dashes, 2, 1.0f);
        }
    }

    // Radial glow (Stitch-enhanced pulsing effect) — scaled to radius
    float glowPhase = static_cast<float>(juce::Time::getMillisecondCounter() % 2000) / 2000.0f;
    float glowAlpha = 0.12f + 0.06f * std::sin(glowPhase * 6.2832f);
    float glowR = radius * 0.16f;
    g.setColour(accent().withAlpha(glowAlpha));
    g.fillEllipse(cx - glowR, cy - glowR, glowR * 2, glowR * 2);
    float outerGlowR = radius * 0.21f;
    g.setColour(accent().withAlpha(glowAlpha * 0.5f));
    g.fillEllipse(cx - outerGlowR, cy - outerGlowR, outerGlowR * 2, outerGlowR * 2);

    // Glow ring
    float ringR = radius * 0.08f;
    g.setColour(accent().withAlpha(0.35f));
    g.drawEllipse(cx - ringR, cy - ringR, ringR * 2, ringR * 2, 1.0f);

    // Puck (mockup style: gold fill with bright gold rim + soft halo)
    float puckR = radius * 0.038f;
    float puckHaloR = puckR * 2.4f;
    g.setColour(accent().withAlpha(0.25f));
    g.fillEllipse(cx - puckHaloR, cy - puckHaloR, puckHaloR * 2, puckHaloR * 2);
    g.setColour(accent());
    g.fillEllipse(cx - puckR, cy - puckR, puckR * 2, puckR * 2);
    g.setColour(amber());
    g.drawEllipse(cx - puckR, cy - puckR, puckR * 2, puckR * 2, 1.5f);

    // Mode label (top-left corner of pad) — now shows source + physics mode
    const char* sourceNames[] = {"2D Pad", "Fader"};
    const char* physNames[] = {"Direct", "Elastic", "Drift"};
    int srcIdx = juce::jlimit(0, 1, proc_.getMorphSource());
    int physIdx = juce::jlimit(0, 2, proc_.getPhysicsMode());
    float modeFontSize = juce::jlimit(10.0f, 13.0f, bounds.getHeight() * 0.08f);
    g.setColour(textDim().withAlpha(0.8f));
    g.setFont(modeFontSize);
    g.drawText(juce::String(sourceNames[srcIdx]) + " \u00B7 " + juce::String(physNames[physIdx]),
               bounds.toNearestInt().withHeight(20).translated(8, 4),
               juce::Justification::centredLeft);
}

void MorphPad::mouseDown(const juce::MouseEvent& e)
{
    dragging_ = true;
    if (auto* px = proc_.getAPVTS().getParameter("morphX"))
        px->beginChangeGesture();
    if (auto* py = proc_.getAPVTS().getParameter("morphY"))
        py->beginChangeGesture();
    updatePosition(e.position);
}
void MorphPad::mouseDrag(const juce::MouseEvent& e) { updatePosition(e.position); }

void MorphPad::updatePosition(juce::Point<float> pos)
{
    auto bounds = getLocalBounds().toFloat().reduced(2.0f);
    auto centre = bounds.getCentre();
    float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;

    // Work in centre-relative coordinates and clamp to the circular boundary so
    // the cursor can never leave the round pad (matches the circular visuals).
    juce::Point<float> rel = pos - centre;
    if (radius > 0.0f && rel.getDistanceFromOrigin() > radius)
        rel = rel * (radius / rel.getDistanceFromOrigin());

    // Map clamped [-radius, radius] back to normalized [0, 1] parameter space.
    float x = juce::jlimit(0.0f, 1.0f, (rel.x / radius) * 0.5f + 0.5f);
    float y = juce::jlimit(0.0f, 1.0f, (rel.y / radius) * 0.5f + 0.5f);

    // Route through APVTS so DAW automation captures the change.
    // syncStateFromAPVTS() bridges APVTS → atomics on the audio thread.
    if (auto* px = proc_.getAPVTS().getParameter("morphX"))
        px->setValueNotifyingHost(x);
    if (auto* py = proc_.getAPVTS().getParameter("morphY"))
        py->setValueNotifyingHost(y);

    ParameterBinding::setChoiceIndexWithGesture(proc_.getAPVTS(), "morphSource", 0, 2);
    proc_.setMorphSource(0);  // XY mode
    needsRepaint_ = true;
    repaint();
}

} // namespace more_phi
