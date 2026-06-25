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
    setTooltip(
        "XY Morph Pad: drag the cursor to morph between snapshot positions arranged "
        "around the clock face. Click a numbered dot to recall a snapshot. "
        "Right-click or double-click to capture the current sound to a slot. "
        "The trail shows recent cursor movement.");
    // AUDIT-FIX (accessibility): make the pad keyboard-focusable + announce it.
    // Arrow keys nudge the morph cursor (see keyPressed); screen readers read
    // the name/title.
    setWantsKeyboardFocus(true);
    setComponentID("MorphPad");
    setName("XY Morph Pad");
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

    // Flash message countdown
    if (flashTicks_ > 0)
    {
        --flashTicks_;
        if (flashTicks_ == 0)
            repaint();  // clear the flash
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
    // AUDIT-FIX: return -1 when full rather than silently overwriting slot 0.
    return -1;
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
    flashMessage_ = "Captured to slot " + juce::String(slot + 1);
    flashTicks_ = 90;
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

    // ── Voronoi cell overlay (Phase 3) ──────────────────────────────────────
    // Draw Delaunay/Voronoi cells when Voronoi mode is active and triangulation exists.
    auto& morph = proc_.getMorphProcessor();
    if (morph.getInterpolationMode() == 1)
    {
        const auto& voronoi = morph.getVoronoiEngine();
        if (voronoi.isValid())
        {
            const auto& cells = voronoi.getVoronoiCells();
            for (const auto& cell : cells)
            {
                if (cell.vertices.size() < 3)
                    continue;

                // 12-hue palette: evenly spaced around the colour wheel.
                const float hue = static_cast<float>(cell.slotIndex) / 12.0f;
                const auto fillCol = juce::Colour::fromHSV(hue, 0.50f, 0.55f, 0.18f);
                const auto edgeCol = juce::Colour::fromHSV(hue, 0.65f, 0.70f, 0.40f);

                juce::Path cellPath;
                cellPath.startNewSubPath(
                    centre.x + cell.vertices[0].x * radius,
                    centre.y + cell.vertices[0].y * radius);
                for (size_t vi = 1; vi < cell.vertices.size(); ++vi)
                    cellPath.lineTo(
                        centre.x + cell.vertices[vi].x * radius,
                        centre.y + cell.vertices[vi].y * radius);
                cellPath.closeSubPath();

                g.setColour(fillCol);
                g.fillPath(cellPath);
                g.setColour(edgeCol);
                g.strokePath(cellPath, juce::PathStrokeType(1.0f));
            }
        }
    }

    // ── Waypoint path overlay (Phase 6) ─────────────────────────────────────
    // Draw the waypoint path and current position when waypoints are enabled.
    {
        auto& wp = proc_.getWaypointEngine();
        if (wp.getNumWaypoints() > 1)
        {
            const auto spos = InterpolationEngine::getClockPositions(0.85f);
            // Map waypoint [0,1] coordinates to screen space same as clock layout
            auto wpToScreen = [&](float x, float y) -> juce::Point<float>
            {
                // Waypoints use [0,1] normalized space like morphX/Y
                float sx = centre.x + (x * 2.0f - 1.0f) * radius;
                float sy = centre.y + (y * 2.0f - 1.0f) * radius;
                return {sx, sy};
            };

            const int numWp = wp.getNumWaypoints();
            // Draw the path connecting waypoints
            juce::Path path;
            auto first = wpToScreen(wp.getWaypoint(0).x, wp.getWaypoint(0).y);
            path.startNewSubPath(first);
            for (int i = 1; i < numWp; ++i)
            {
                auto pt = wpToScreen(wp.getWaypoint(i).x, wp.getWaypoint(i).y);
                path.lineTo(pt);
            }
            // Close the loop
            path.closeSubPath();

            g.setColour(accent().withAlpha(0.20f));
            g.strokePath(path, juce::PathStrokeType(1.5f));

            // Draw waypoint dots
            const float wpDotR = radius * 0.025f;
            for (int i = 0; i < numWp; ++i)
            {
                auto pt = wpToScreen(wp.getWaypoint(i).x, wp.getWaypoint(i).y);
                bool isCurrent = wp.isPlaying() && (i == wp.getCurrentIndex());
                g.setColour(isCurrent ? cyanBright().withAlpha(0.7f) : accent().withAlpha(0.35f));
                g.fillEllipse(pt.x - wpDotR, pt.y - wpDotR, wpDotR * 2, wpDotR * 2);
            }

            // Draw current waypoint cursor (if playing)
            if (wp.isPlaying())
            {
                float cpx = wp.getPositionX();
                float cpy = wp.getPositionY();
                auto cp = wpToScreen(cpx, cpy);
                float wpCurR = radius * 0.04f;
                g.setColour(accent().withAlpha(0.50f));
                g.fillEllipse(cp.x - wpCurR, cp.y - wpCurR, wpCurR * 2, wpCurR * 2);
                g.setColour(amber());
                g.drawEllipse(cp.x - wpCurR, cp.y - wpCurR, wpCurR * 2, wpCurR * 2, 1.5f);
            }
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
    // C3 FIX: read each trail point via the atomic getTrailPoint() accessor so
    // the UI never sees a torn {x,y} pair written mid-store by the audio thread.
    int head = morph.getTrailHead();
    constexpr int trailLen = MorphProcessor::TRAIL_SIZE;

    for (int i = 0; i < trailLen - 1; ++i)
    {
        int idx0 = (head - trailLen + i + trailLen * 2) % trailLen;
        int idx1 = (idx0 + 1) % trailLen;

        const auto p0 = morph.getTrailPoint(idx0);
        const auto p1 = morph.getTrailPoint(idx1);

        float alpha = static_cast<float>(i) / static_cast<float>(trailLen) * 0.5f;
        float px0 = centre.x + (p0.x * 2.0f - 1.0f) * radius;
        float py0 = centre.y + (p0.y * 2.0f - 1.0f) * radius;
        float px1 = centre.x + (p1.x * 2.0f - 1.0f) * radius;
        float py1 = centre.y + (p1.y * 2.0f - 1.0f) * radius;

        g.setColour(padTrail().withAlpha(alpha));
        g.drawLine(px0, py0, px1, py1, 1.5f);
    }

    // ── Snapshot nodes (clock layout) ──────────────────────────────────────────
    // Mockup style: numbered ring-nodes around the clock. Filled slots glow cyan
    // with the number inside; empty slots are muted hairline circles.
    // F-12: Active A/B pair (nearest two occupied slots to cursor) get a brighter glow.
    auto positions = InterpolationEngine::getClockPositions(0.85f);
    auto& bank = proc_.getSnapshotBank();
    float nodeR = juce::jmax(radius * 0.075f, 11.0f);
    float nodeFontSize = juce::jmax(radius * 0.06f, 9.0f);

    // Determine A/B slot pair (two nearest occupied slots to morph position)
    int slotA = -1, slotB = -1;
    {
        float bestDistA = std::numeric_limits<float>::max();
        float bestDistB = std::numeric_limits<float>::max();
        float mx = proc_.getMorphX() * 2.0f - 1.0f;
        float my = proc_.getMorphY() * 2.0f - 1.0f;
        for (int i = 0; i < 12; ++i)
        {
            if (!bank.isOccupied(i))
                continue;
            float dist = std::hypot(positions[i].x - mx, positions[i].y - my);
            if (dist < bestDistA) { bestDistB = bestDistA; slotB = slotA; bestDistA = dist; slotA = i; }
            else if (dist < bestDistB) { bestDistB = dist; slotB = i; }
        }
    }
    for (int i = 0; i < 12; ++i)
    {
        float dotX = centre.x + positions[i].x * radius;
        float dotY = centre.y + positions[i].y * radius;
        bool occupied = bank.isOccupied(i);
        bool isActiveA = (i == slotA);
        bool isActiveB = (i == slotB);

        // Glow halo behind filled nodes — A/B slots get a brighter amber halo
        if (occupied)
        {
            float haloR = nodeR * (isActiveA || isActiveB ? 2.2f : 1.8f);
            g.setColour(isActiveA || isActiveB
                ? amber().withAlpha(0.30f)
                : cyan().withAlpha(0.18f));
            g.fillEllipse(dotX - haloR, dotY - haloR, haloR * 2, haloR * 2);
        }

        // Node body — A/B slots use amber tint; regular occupied use cyan
        {
            auto bodyCol = occupied
                ? (isActiveA || isActiveB
                    ? amber().withAlpha(0.25f)
                    : cyan().withAlpha(0.18f))
                : surfaceLit();
            g.setColour(bodyCol);
            g.fillEllipse(dotX - nodeR, dotY - nodeR, nodeR * 2, nodeR * 2);
        }

        // Node ring — A/B slots use amber
        {
            auto ringCol = occupied
                ? (isActiveA || isActiveB ? amber() : cyanBright())
                : border();
            g.setColour(ringCol);
            if (occupied)
                g.drawEllipse(dotX - nodeR, dotY - nodeR, nodeR * 2, nodeR * 2, 1.2f);
            else
            {
                const float dashLen[] = { 2.0f, 2.5f };
                juce::Path dashedCircle;
                dashedCircle.addEllipse(dotX - nodeR, dotY - nodeR, nodeR * 2, nodeR * 2);
                juce::PathStrokeType stroke(1.0f);
                stroke.createDashedStroke(dashedCircle, dashedCircle, dashLen, 2);
                g.strokePath(dashedCircle, stroke);
            }
        }

        // Slot number — A/B slots get amber text with bold weight; regular slots cyan
        {
            auto textCol = isActiveA || isActiveB
                ? amber().withAlpha(0.9f)
                : (occupied ? cyanBright() : textSlot());
            g.setColour(textCol);
            auto fontWeight = isActiveA || isActiveB ? juce::Font::bold : juce::Font::plain;
            g.setFont(juce::Font(nodeFontSize, fontWeight));
            g.drawText(juce::String(i + 1),
                       juce::Rectangle<float>(dotX - nodeR, dotY - nodeR, nodeR * 2, nodeR * 2),
                       juce::Justification::centred);
        }

        // A/B label beneath slot number
        if (isActiveA)
        {
            g.setColour(amber().withAlpha(0.8f));
            g.setFont(juce::Font(nodeFontSize * 0.7f, juce::Font::bold));
            g.drawText("A",
                       juce::Rectangle<float>(dotX - nodeR, dotY + nodeR * 0.7f, nodeR * 2, nodeFontSize),
                       juce::Justification::centred);
        }
        else if (isActiveB)
        {
            g.setColour(amber().withAlpha(0.8f));
            g.setFont(juce::Font(nodeFontSize * 0.7f, juce::Font::bold));
            g.drawText("B",
                       juce::Rectangle<float>(dotX - nodeR, dotY + nodeR * 0.7f, nodeR * 2, nodeFontSize),
                       juce::Justification::centred);
        }
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

        // Anchor (raw input) in screen space
        float rawCx = centre.x + (proc_.getMorphX() * 2.0f - 1.0f) * radius;
        float rawCy = centre.y + (proc_.getMorphY() * 2.0f - 1.0f) * radius;

        if (physMode == 2)  // Drift
        {
            // Tension line — connects anchor to drifted cursor
            g.setColour(accent().withAlpha(0.25f));
            g.drawLine(rawCx, rawCy, cx, cy, 1.0f);

            // Outer glow ring around the drifted cursor (larger, more visible)
            float driftGlowR = radius * 0.12f;
            float driftPulse = 0.7f + 0.3f * std::sin(static_cast<float>(
                juce::Time::getMillisecondCounter() % 1600) / 1600.0f * 6.2832f);
            g.setColour(accent().withAlpha(0.15f * driftPulse));
            g.fillEllipse(cx - driftGlowR, cy - driftGlowR,
                          driftGlowR * 2, driftGlowR * 2);

            // Inner anchor dot — bright, small, representing the fixed anchor
            float anchorR = radius * 0.025f;
            g.setColour(accent().withAlpha(0.6f));
            g.fillEllipse(rawCx - anchorR, rawCy - anchorR,
                          anchorR * 2, anchorR * 2);
            g.setColour(amber().withAlpha(0.4f));
            g.drawEllipse(rawCx - anchorR, rawCy - anchorR,
                          anchorR * 2, anchorR * 2, 1.0f);
        }
        else  // Elastic — simple guide dot
        {
            float guideR = radius * 0.02f;
            g.setColour(padGuide().withAlpha(0.4f));
            g.fillEllipse(rawCx - guideR, rawCy - guideR,
                          guideR * 2, guideR * 2);
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

    // ── Flash message (capture/recall feedback) ────────────────────────────────
    if (flashTicks_ > 0 && flashMessage_.isNotEmpty())
    {
        float flashAlpha = flashTicks_ > 60 ? 1.0f
                         : static_cast<float>(flashTicks_) / 60.0f;  // fade to zero
        float flashY = bounds.getBottom() - 28.0f;
        g.setColour(green().withAlpha(flashAlpha));
        g.setFont(juce::Font(juce::jlimit(10.0f, 13.0f, bounds.getHeight() * 0.07f), juce::Font::bold));
        g.drawText(flashMessage_,
                   juce::Rectangle<float>(bounds.getX(), flashY, bounds.getWidth(), 20.0f),
                   juce::Justification::centred);
    }
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

bool MorphPad::keyPressed(const juce::KeyPress& key)
{
    // AUDIT-FIX (accessibility): arrow-key control of the morph cursor, mirroring
    // a mouse drag through the same APVTS gesture path so DAW automation records
    // it identically. Each press nudges the cursor 2% — fine enough for keyboard
    // / screen-reader users and accelerates when held (host key-repeat).
    constexpr float kStep = 0.02f;
    float dx = 0.0f, dy = 0.0f;

    if      (key.isKeyCode(juce::KeyPress::leftKey))  dx = -kStep;
    else if (key.isKeyCode(juce::KeyPress::rightKey)) dx =  kStep;
    else if (key.isKeyCode(juce::KeyPress::upKey))    dy = -kStep;   // up → smaller morphY
    else if (key.isKeyCode(juce::KeyPress::downKey))  dy =  kStep;   // down → larger morphY
    else return false;

    auto& apvts = proc_.getAPVTS();
    auto* px = apvts.getParameter("morphX");
    auto* py = apvts.getParameter("morphY");
    if (px == nullptr || py == nullptr) return false;

    const float newX = juce::jlimit(0.0f, 1.0f, px->getValue() + dx);
    const float newY = juce::jlimit(0.0f, 1.0f, py->getValue() + dy);

    // One discrete gesture per key press (begin / notify / end) so the host
    // records a distinct automation point, matching a single mouse click-drag.
    px->beginChangeGesture();
    px->setValueNotifyingHost(newX);
    px->endChangeGesture();
    py->beginChangeGesture();
    py->setValueNotifyingHost(newY);
    py->endChangeGesture();

    proc_.setMorphSource(0);  // XY mode
    needsRepaint_ = true;
    repaint();
    return true;
}

std::unique_ptr<juce::AccessibilityHandler> MorphPad::createAccessibilityHandler()
{
    // Announce the pad as a labelled group so screen readers read its name.
    // (Keyboard operability is provided by keyPressed above; a full value
    // interface is not feasible for a 2D pad.)
    return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::group);
}

} // namespace more_phi
