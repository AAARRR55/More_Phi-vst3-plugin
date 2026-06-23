# More-Phi VST3 Plugin — UX/UI Audit Report

**Version:** v3.3.0  
**Date:** 2026-07-16  
**Auditor:** Automated UX/UI audit against DAW plugin interface best practices  
**Methodology:** Source-level analysis of all UI components, their labeling, tooltips, layout, visual hierarchy, accessibility, and discoverability. Grounded in real DAW workflows (studio producers, sound designers, mix engineers).

---

## 1. Strengths — What Works Well

### 1.1 Cohesive Dark Theme
The More-Phi LookAndFeel delivers a premium ultra-dark palette (near-black surfaces #070709, gold primary #e5c057, cyan accent #00bdca, magenta secondary #e22edb) that is easy on the eyes during long studio sessions. The glassmorphic panel backgrounds (`surface().withAlpha(0.85f)`) and subtle border glows create clear visual separation between controls without harsh dividers. This is a strong, professional design language.

### 1.2 Typography System
Embedded Syncopate (display/headings) and Outfit (body/values) typefaces with the `FontRole` enum (`Title`, `Section`, `Control`, `Value`, `Micro`) provide a consistent, scalable typographic hierarchy. The minimum-font-size floor (`kMinSectionLabel=9.0f`, `kMinControlLabel=10.0f`, `kMinValueLabel=10.0f`) prevents text from becoming illegible at any window size — a rare and commendable attention to accessibility.

### 1.3 Tabbed Architecture
The V2TabBar (Classic | Engine | Modulation | Presets | AI) cleanly separates concerns. Core morphing controls live on "Classic"; audio-domain DSP on "Engine"; modulation routing on "Modulation"; preset management on "Presets"; and AI chat on "AI." This follows familiar DAW plugin patterns (e.g., FabFilter tab strips, iZotope module tabs) and reduces cognitive load.

### 1.4 Engine Panel Consistency
The SpectralControlPanel, GranularControlPanel, HybridBlendPanel, PerformancePanel, and DriftControlPanel share a consistent visual language: rounded rectangle background, section labels in all-caps dim text, vertical/horizontal dividers between sections, and active-toggle → sub-control enable/disable gating (the H-5 pattern). A user who learns one engine panel can navigate all of them.

### 1.5 Tooltips on Performance-Critical Controls
The PerformancePanel toggles have clear, technical tooltips explaining *what* each toggle does at the engine level ("Larger write deadband: fewer setValue() calls…", "Skip the per-block batch parameter read…", "Push parameter updates… only every 4th block"). The License overlay has self-explanatory copy. The AIChatPanel's workflow/approval buttons are tooltipped. This is the right target for tooltip coverage.

### 1.6 Responsive Layout
Compact-width code paths (`getWidth() < 760` / `< 660`) reflow controls into stacked rows rather than allowing them to clip. The MorphPad square constrains itself to `jlimit(96, 320, availableSide)` and centers in the available space. The tab content height adapts for the AI tab (`jlimit(baseTabContentHeight, area.getHeight() - 120, aiTabContentHeight)`). This shows awareness of host-window resize use cases.

### 1.7 Font Scaling
The `getScaledFontSize()` system scales font sizes proportionally to editor width with a floor and a 1.5x ceiling, ensuring text remains readable on both small laptop screens and large 4K monitors. The `editorWidth_` is updated in `resized()` and shared across all components via the L&F.

---

## 2. Friction Points — Specific Usability Issues

### 2.1 Critical: Snapshot Capture/Recall Is Undiscoverable

**Location:** MorphPad + SnapshotRing (`MorphPad.cpp`, `SnapshotRing.cpp`)

**Problem:** The 12 snapshot slots on the MorphPad clock-face are the plugin's core workflow: capture parameter states, morph between them. But there are zero visible hints about how to interact with them:

- **No visible labels on snapshot dots.** The dots are painted as small circles on the pad, but nothing tells the user which is slot 1 vs. slot 7, whether a slot is empty or filled, or what's stored in it.
- **Right-click to capture is invisible.** `SnapshotRing::mouseDown()` uses right-click for capture. This is a hidden gesture — there's no tooltip, no context menu, no status-bar hint. Most DAW users expect a "Capture" button or a `Ctrl+Click` modifier, not an unadvertised right-click.
- **Double-click to capture is equally hidden.** `MorphPad::mouseDoubleClick()` captures to the nearest empty slot. This is never communicated.
- **No feedback on capture/recall.** Neither `SnapshotRing::mouseDown()` nor `MorphPad::mouseDoubleClick()` shows any visual confirmation — no flash, no toast, no status-bar message. The user clicks and hopes something happened.
- **Recall mode ambiguity.** The user doesn't know whether left-clicking a dot does a "Fast" or "Full" recall — that's set elsewhere in the BottomControlStrip with no cross-reference.

**Impact (HIGH):** New users cannot figure out how to use snapshots — the entire morphing workflow — without reading external documentation or watching a tutorial. This is the single biggest onboarding failure.

### 2.2 High: BottomControlStrip Labels Are Cryptic

**Location:** `BottomControlStrip.cpp`

**Problem:** Several buttons use single-word labels that assume domain knowledge:

| Button Label | What a new user thinks | What it actually does |
|---|---|---|
| "Sanity" | ?? (mental health?) | Enables safety mode: protects Volume/Pitch/Bypass params during morph |
| "Listen" | Audio monitoring? | Learn Mode: automatically maps touched hosted-plugin params |
| "Link" | Stereo link? | Links snapshot A/B selection for paired morphing |
| "Sustain" | Envelope sustain? | Retains MIDI notes across snapshot switches |
| "Fast" / "Full" | Speed? | Snapshot recall mode: fast=params only, full=params+state |

None of these have tooltips. The section labels painted in `paint()` ("SAFETY", "RECALL", "OUTPUT", "SC") are dim (#704a5568) 10px text — easily missed and not interactive.

**Impact (HIGH):** Users can't confidently use these controls without trial-and-error. In a studio context, pressing "Sanity" without understanding it could lead to unexpected parameter protection that confuses the user further.

### 2.3 High: ModeBar Physics Modes Have No Explanation

**Location:** `ModeBar.cpp`

**Problem:** The three physics mode buttons — "Direct", "Elastic", "Drift" — have no tooltips or inline help. These are domain-specific terms:

- "**Direct**" — Raw cursor position → interpolation. No physics simulation.
- "**Elastic**" — Spring-damper system with three presets (Slow/Medium/Heavy). Feels like momentum/friction.
- "**Drift**" — Perlin noise wandering around target with speed/distance/chaos controls.

A new user has no idea what these modes do, how they differ, or why they'd choose one over another. The Drift parameters (Speed, Distance, Chaos) are buried on the Engine tab with no cross-reference from the ModeBar. If a user clicks "Drift" and doesn't see the Engine tab, they'll think nothing happened.

**Impact (HIGH):** Physics modes are a headline differentiator for More-Phi. If users can't understand them, they won't use them — and they won't appreciate the plugin's value.

### 2.4 Medium-High: MorphPad XY Pad Has No Labeling or Guidance

**Location:** `MorphPad.cpp`

**Problem:** The MorphPad — the central visual element — has several discoverability gaps:

- **No axis labels.** The pad is an abstract 2D square. What do X and Y represent? Which snapshot is where? There's no legend.
- **The cyan trail line** (`padTrail = #00bdca`) shows recent cursor movement, but there's no indication of what the trail means or that it can be toggled.
- **Three visualization modes** (Standard, Heatmap, Timeline) exist via `setVisualizationMode()` but there's no UI to switch between them. They appear to be programmatic-only.
- **Grid toggle** and **path toggle** exist programmatically but have no UI controls.
- **No tooltip** on the pad itself.

**Impact (MEDIUM):** Users can drag the cursor and see dots, but can't form a mental model of what the pad represents or how to use it effectively.

### 2.5 Medium: Macro Knobs Show Generic "P1"–"P8" Until Plugin Loads

**Location:** `MacroKnobStrip.cpp`

**Problem:** The 8 macro knobs initialize with labels "P1" through "P8" and default values of 0.5. These are placeholders. When a plugin is loaded, the timer syncs them to the first 8 hosted parameters — but:

- **No indication that these are "macro" controls** rather than just the first 8 parameters.
- **The tooltip** (`"<paramName>  (macro 1 of 8 - macros map to hosted params 1-8)"`) is informative once it appears, but only after plugin load.
- **Hardcoded to parameters 0–7** with no user-configurable mapping. The code itself admits this in a comment: `"TODO (still pending, larger feature): user-configurable mapping"`.
- **No visual distinction** between macro knobs and regular parameter knobs — they look identical to the knobs in the ParameterMapPanel.

**Impact (MEDIUM):** Power users who want to map specific "performance" parameters to these 8 knobs can't. New users don't know why these 8 knobs exist separately from the ParameterMapPanel.

### 2.6 Medium: Sidechain Threshold Knob Labeled "SC" With No Context

**Location:** `BottomControlStrip.cpp`

**Problem:** The sidechain section uses a toggleButton with text "SC" (SideChain) and a threshold knob labeled "Threshold." There's no tooltip explaining that this is an audio sidechain input for envelope-following morph modulation. The section label "SC" is cryptic — many producers know "SC" as "SideChain" from compressors, but the connection to morph modulation is non-obvious.

**Impact (MEDIUM):** Users may enable sidechain and hear no effect because they don't understand what it modulates or how to set the threshold.

### 2.7 Medium: Breeding Panel Buttons Lack Tooltips

**Location:** `BreedingPanel.cpp`

**Problem:** The three buttons — "Breed", "Mutate", "Randomize" — have no tooltips:

- **"Breed"** crosses two snapshots with configurable crossover/mutation. Which two? The last two selected? The two nearest the cursor? This is unclear.
- **"Mutate"** perturbs parameters of a single snapshot. Which snapshot? How much mutation?
- **"Randomize"** randomizes the morph position. This is the most self-explanatory but still lacks confirmation feedback.

There's a `statusLabel_` in the class but it's not visible in the constructor — it may only appear after an action.

**Impact (MEDIUM):** Genetic breeding is a unique, powerful feature. Without any guidance, users won't risk clicking these buttons, which sound destructive.

### 2.8 Medium: ParameterMapPanel Toggle Button Ambiguous

**Location:** `PluginEditor.cpp` (lines 33, 54–66)

**Problem:** The "Params >" / "< Params" toggle button opens a side panel showing all hosted plugin parameters. The `>` / `<` arrow convention is a reasonable discoverability hint, but:

- The button lives at the far right of the plugin browser row, next to "Open Plugin UI" — its purpose isn't obvious from spatial context.
- When the panel opens, it pushes the MorphPad leftward, changing the layout. This can be disorienting.
- The panel shows ALL parameters in a scrollable list, which for a plugin with 500+ parameters becomes unwieldy.

**Impact (LOW-MEDIUM):** The feature works, but the toggle's location and the layout shift create minor friction.

### 2.9 Low-Medium: SonicMaster Toggle Has a Good Tooltip But Mixed Visual Signals

**Location:** `PluginEditor.cpp` (lines 69–86)

**Problem:** The SonicMaster toggle has an excellent tooltip — one of the best in the plugin. However:

- The status label shows verbose technical states like "held (low confidence)" and "error - see log." An average user doesn't know what "low confidence" means or where "the log" is.
- When the feature is unavailable ("no model"), the toggle is disabled with no explanation of *why* — is the model not installed? Not supported on this platform?
- The toggle and status label take a full 28px-high row below the main content, eating vertical space even when the feature is unavailable.

**Impact (LOW-MEDIUM):** Users who try the feature get confusing status messages. Users who can't use it don't know why.

### 2.10 Low: AI Status Bar Shows Technical Details

**Location:** `AIStatusPanel.cpp`, `PluginEditor.cpp` (line 300)

**Problem:** The bottom bar shows MCP server port, client count, and a "Copy Token" button. This is developer-facing information:

- Most musicians don't know what an "MCP server" is or why it needs a port.
- "Clients: 0" is the default state — it looks like an error to someone unfamiliar with the architecture.
- The "Copy Token" button copies a bearer token — useful for AI integration, but unexplained.

**Impact (LOW):** The bar occupies 32px of vertical space permanently. For users who never use the AI features, it's dead space showing cryptic information.

### 2.11 Low: "Deactivate License" Button Placement

**Location:** `PluginEditor.cpp` (lines 93, 278)

**Problem:** The deactivate button is positioned at `(getWidth() - 250, 10, 120, 28)` — overlapping the title bar area near the RMS meter. For a licensed user, this button is always visible in the title chrome, which is unusual for a "destructive" administrative action. Most plugins bury license management in a settings/hamburger menu.

**Impact (LOW):** Visual clutter in the title bar. Slight risk of accidental clicks, though the confirmation dialog mitigates this.

### 2.12 Low: Color-Only Status Indicators

**Location:** Multiple components (`AIStatusPanel`, `SonicMaster status`, `MorphPad slot dots`)

**Problem:** Several status indicators rely solely on color:

- **MorphPad slot dots:** Filled slots = gold (#e5c057), empty = dark (#3a3a40). For red-green colorblind users, the gold/dark distinction may be sufficient, but the gold/cyan distinction (trail vs. filled dot) could be ambiguous.
- **RMS meter:** Green → gold → red transition. Red-green colorblind users (~8% of males) can't distinguish the green zone from the red zone.
- **AI status:** Green (#34d399) = online, amber (#f9e596) = warning. Blue-yellow colorblind users (tritanopia) may struggle.

**Impact (LOW):** The ultra-dark theme already limits color usage, but the few places where color conveys meaning could benefit from shape/text fallbacks.

### 2.13 Low: Missing Tooltips on Several Common Controls

The following controls have no tooltips:
- **BottomControlStrip:** Sanity, Listen, Link, Fast, Full, Sustain, Bypass, SC toggle — all un-tooltipped
- **ModeBar:** Direct, Elastic, Drift, 2D Pad, Fader, Smooth — all un-tooltipped
- **BreedingPanel:** Breed, Mutate, Randomize — all un-tooltipped
- **MorphPad:** The pad itself has no tooltip
- **SnapshotRing:** The overlay has no tooltip
- **PluginBrowserPanel:** "Capture" button has no tooltip (does it capture to next empty slot? to a specific slot?)
- **EngineTabPage sub-panels:** Most knobs and toggles have labels but no tooltips. Spectral "Transient" and "Formant" toggles are un-tooltipped — what do they preserve?
- **HybridBlendPanel:** "Audio Domain" toggle, blend weight sliders — no tooltips
- **DriftControlPanel:** Speed, Distance, Chaos knobs — no tooltips

---

## 3. Corrective Recommendations

### Priority 1: Fix Snapshot Discoverability (Addresses 2.1)

**Impact:** Highest. This is the core workflow.

1. **Add slot number labels** painted inside or beside each snapshot dot on the clock face, so users can identify slots 1–12.
2. **Show empty vs. filled state clearly:** filled dots = filled gold circle; empty dots = dashed outline circle (not just a dimmer solid circle). This makes occupancy obvious at a glance.
3. **Add a tooltip to the MorphPad** (or a small overlay label): "Click a dot to recall a snapshot. Right-click or double-click to capture the current sound to a slot."
4. **Show a brief "Captured to slot N" toast/status** when a capture succeeds (e.g., a 1.5-second gold flash on the dot, or a status message in the AI status bar area).
5. **Add a small "Recall: Fast" / "Recall: Full" indicator** near the pad (or on the SnapshotRing) that reflects the current BottomControlStrip recall mode, so users know what a click will do.
6. **Consider adding visible capture/recall buttons** next to the MorphPad as an alternative to hidden gestures. Two small buttons: "Capture" (with slot selector) and "Recall" would make the workflow obvious to first-time users.

### Priority 2: Add Tooltips to Cryptic Controls (Addresses 2.2, 2.3, 2.6, 2.7, 2.13)

**Impact:** High. Fix the "what does this button do" problem across ~15 controls.

**BottomControlStrip tooltips:**

| Button | Recommended Tooltip |
|---|---|
| Sanity | "Safety mode: protects Volume, Pitch, and Bypass parameters from being changed during morphing, breeding, or randomization." |
| Listen | "Learn Mode: automatically maps hosted plugin parameters that you manually adjust, so they become part of the morph target set." |
| Link | "Link Mode: couples snapshot A/B selection for synchronized paired morphing across two plugin instances." |
| Fast | "Fast Recall: instantly loads parameter values only (snappiest transitions)." |
| Full | "Full Recall: loads parameters plus plugin internal state (slower but complete)." |
| Sustain | "Sustain: holds currently-playing MIDI notes across snapshot switches instead of retriggering." |
| SC | "Sidechain: uses an external audio input to modulate the morph position based on amplitude envelope." |
| Bypass | "Bypass: temporarily disables all More-Phi processing and passes audio through the hosted plugin unchanged." |

**ModeBar tooltips:**

| Button | Recommended Tooltip |
|---|---|
| 2D Pad | "2D Pad: morph by dragging the cursor on the XY pad between snapshot positions." |
| Fader | "Fader: morph along a single axis using the vertical slider, interpolating between occupied snapshots in clock order." |
| Direct | "Direct mode: cursor position drives morph instantly — no physics simulation." |
| Elastic | "Elastic mode: spring-physics cursor with momentum and inertia. Three presets: Slow, Medium, Heavy." |
| Drift | "Drift mode: Perlin-noise wandering around the target position. Adjust speed, distance, and chaos on the Engine tab." |
| Smooth | "Smoothing: blends morph output over time. 0 = instant jumps, higher values = gradual transitions." |

**BreedingPanel tooltips:**

| Button | Recommended Tooltip |
|---|---|
| Breed | "Breed: crosses the two most recently selected snapshots using genetic crossover, storing the result in the next empty slot." |
| Mutate | "Mutate: applies random perturbation to the most recently selected snapshot's parameters, stored in the next empty slot." |
| Randomize | "Randomize: jumps the morph cursor to a random position on the pad for unexpected sound exploration." |

**Engine Tab tooltips:**
- Spectral "Transient": "Preserve transient details when morphing in the frequency domain."
- Spectral "Formant": "Preserve formant structure (vocal character) when morphing in the frequency domain."
- HybridBlend "Audio Domain": "Enable audio-domain processing (spectral, granular, formant engines) in addition to parameter morphing."
- Drift "Speed": "How fast the drift cursor wanders across the morph surface."
- Drift "Distance": "Maximum radius the drift cursor can wander from the target position."
- Drift "Chaos": "Randomness of the drift path. Low = smooth orbit, High = erratic jumps."

### Priority 3: Add MorphPad Axis Labels and Legend (Addresses 2.4)

**Impact:** Medium. Helps users build a mental model of the pad.

1. **Paint axis labels** on the pad edges: "Snapshot A → Snapshot B" or parameter-group labels along X and Y axes, derived from the snapshot bank contents if available.
2. **Add a small legend** in the corner of the pad: "● = filled slot, ○ = empty slot, — trail = recent movement"
3. **Expose visualization mode toggles** (Standard / Heatmap / Timeline) as three small icon buttons in a corner of the pad, or in a right-click context menu on the pad.
4. **Add a MorphPad tooltip:** "XY Morph Pad — drag to interpolate between snapshot positions around the clock face. Trail shows recent movement."

### Priority 4: Improve Macro Knob UX (Addresses 2.5)

**Impact:** Medium.

1. **Add a section label** above or beside the macro knobs: "MACRO CONTROLS" (matching the all-caps dim section-label convention used in engine panels).
2. **Visually distinguish macro knobs:** use a slightly different knob cap color (e.g., cyan accent instead of gold) or a subtle "M1"–"M8" badge to differentiate them from regular parameter knobs.
3. **Default tooltip when no plugin is loaded:** "Macro 1 — load a plugin to assign its first 8 parameters here."
4. **Long-term (acknowledged in code):** user-configurable mapping so any hosted parameter can be assigned to any macro knob, with a right-click "Assign…" context menu.

### Priority 5: Clarify SonicMaster Status Messages (Addresses 2.9)

**Impact:** Low-Medium.

1. **Simplify status messages** to user-facing language:
   - "collecting audio…" → "Listening… (analyzing your audio)"
   - "held (low confidence)" → "Waiting for clearer signal…"
   - "error - see log" → "Analysis paused — check diagnostics panel"
2. **When unavailable, show why:** "Neural Master: model not installed. Download from [website]." instead of just "unavailable (no model)."
3. **Consider collapsing the row** when the feature is permanently unavailable on a given system (no ONNX runtime, etc.) to reclaim vertical space.

### Priority 6: De-Emphasize Technical AI Status Bar (Addresses 2.10)

**Impact:** Low.

1. **Collapse the AI status bar** into a small expandable chip (e.g., "AI: ● Offline ▸") when the MCP server is not running.
2. **When running**, show a compact status: "AI Ready · Port 30001 · 1 client" with a gear icon for the token/settings.
3. **Move the "Copy Token" action** into a right-click context menu on the status chip or a small "…" button.

### Priority 7: Visual Polish & Accessibility (Addresses 2.11, 2.12)

**Impact:** Low.

1. **Move "Deactivate License"** into a settings gear icon in the title bar (or the existing AI status area), rather than a full-width text button in the title chrome.
2. **Add shape differentiation to the RMS meter:** paint a subtle "0 dB" tick mark and clip indicator line on the meter, so the color-coded zones have a non-color fallback.
3. **For MorphPad slot dots:** add a subtle ring/border to filled dots (gold fill + slightly brighter gold border) so the filled/empty distinction is shape+color, not color alone.
4. **Add a small "?" help icon** in the title bar or status bar that opens a quick-reference overlay showing keyboard shortcuts and gesture hints (right-click = capture, double-click = capture, etc.).

### Priority 8: Parameter Panel UX Refinements (Addresses 2.8)

**Impact:** Low.

1. **Add a search/filter field** at the top of the ParameterMapPanel, so users with 500+ parameter plugins can find specific controls.
2. **Change the toggle label** from "Params >" to "All Parameters ▸" for clarity.
3. **Consider a split-view mode** where the parameter panel opens *below* the MorphPad instead of pushing it sideways, preserving the pad's center position.

---

## 4. Summary of Tooltip Coverage Gaps

| Component | Controls Lacking Tooltips | Priority |
|---|---|---|
| BottomControlStrip | Sanity, Listen, Link, Fast, Full, Sustain, SC, Bypass | **P2** |
| ModeBar | 2D Pad, Fader, Direct, Elastic, Drift, Smooth | **P2** |
| BreedingPanel | Breed, Mutate, Randomize | **P2** |
| MorphPad | The pad itself | **P3** |
| SnapshotRing | The overlay (capture/recall hints) | **P1** |
| PluginBrowserPanel | Capture button | **P2** |
| SpectralControlPanel | Transient toggle, Formant toggle | **P2** |
| HybridBlendPanel | Audio Domain toggle, blend weight sliders | **P2** |
| DriftControlPanel | Speed, Distance, Chaos knobs | **P2** |
| GranularControlPanel | Size, Density, Pitch, Scatter knobs | **P2** |
| MacroKnobStrip | Knobs (tooltipped but could be improved) | **P4** |

---

## 5. Implementation Effort Estimate

| Priority | Changes | Est. Effort |
|---|---|---|
| P1 | Snapshot discoverability: tooltip, labels, feedback toast, slot rendering | ~4 hours |
| P2 | Tooltip audit (~20 controls): string additions only | ~2 hours |
| P3 | MorphPad axis labels + legend + viz mode toggles | ~3 hours |
| P4 | Macro knob labeling + visual distinction | ~1.5 hours |
| P5 | SonicMaster status message simplification | ~30 min |
| P6 | AI status bar compact mode | ~2 hours |
| P7 | License button move, meter tick, slot shape | ~1.5 hours |
| P8 | Parameter panel search + label change | ~2 hours |
| **Total** | | **~16.5 hours** |

**Lowest-effort, highest-impact first step:** Add tooltips to the 20 untooltipped controls (P2). This is pure string work with no layout changes and immediately improves every friction point in section 2.
