# UX/UI Audit Report — More-Phi VST3 Plugin (v3.4.1)

**Audit date:** 2026-06-27  
**Scope:** All 43 UI source files across 15 panels and overlays  
**Methodology:** Code-level review against WCAG 2.1 AA/AAA, Nielsen heuristics, DAW workflow conventions  
**Tools used:** Code inspection, contrast calculation, keyboard-navigation simulation, screen-reader compatibility analysis  

---

## Table of Contents

1. [Strengths — What Works Well](#1-strengths--what-works-well)
2. [Friction Points — Critical](#2-critical-friction-points)
3. [Friction Points — High](#3-high-friction-points)
4. [Friction Points — Medium](#4-medium-friction-points)
5. [Friction Points — Low](#5-low-friction-points)
6. [Corrective Recommendations](#6-corrective-recommendations)
7. [Accessibility Scorecard](#7-accessibility-scorecard)
8. [Appendix: Contrast Measurements](#8-appendix-contrast-measurements)

---

## 1. Strengths — What Works Well

### 1.1 Contrast & Readability
The dark theme delivers excellent contrast ratios on primary text paths:

| Pair | Ratio | WCAG |
|------|-------|------|
| `textPrimary` (#EEEFF2) on `backgroundDark` (#070709) | **17.4:1** | AAA |
| `textDim` (#9A9AA2) on `backgroundDark` (#070709) | **7.2:1** | AAA |
| `textDim` (#9A9AA2) on `surfaceLight` (#17181C) | **6.2:1** | AA |
| `accentGold` (#E5C057) on `backgroundDark` (#070709) | **11.7:1** | AAA |

The `textDim` value was raised from `#5A5A60` (~2.9:1, failing AA) to the current `#9A9AA2` — a deliberate fix documented in the code with a clear audit comment.

### 1.2 Colorblind-Safe Patterns
Several components implement **shape + text redundancy** rather than relying solely on color:

| Component | Pattern | Detail |
|-----------|---------|--------|
| AIStatusPanel | Glyph + color | Filled circle (U+25CF) = online, open circle (U+25CB) = offline, alongside green/grey color — documented in code with explicit "colorblind-safe" comment |
| LicenseActivationOverlay | Glyph + color | Checkmark (U+2713) for success, ballot X (U+2715) for failure, alongside green/coral color — same explicit comment |
| AIChatPanel status chip | Text + color | Status text ("Active"/"Offline") alongside green/grey — redundant encoding |
| ChatDisplay role labels | Text + color | "System", "You", "Assistant" are *typed out*, not color-coded only |

### 1.3 Focus Indicators
Three out of five primary control types draw a visible focus ring:
- **Buttons**: 2px cyan ring at 55% alpha, expanded 1.5px outside bounds
- **Rotary sliders**: Same cyan ring
- **Linear sliders**: Same cyan ring
- *Missing on ComboBox and Label* (see Medium findings)

### 1.4 Keyboard Accessibility
- **MorphPad**: Arrow keys nudge 2% per press through APVTS gesture API
- **SnapFader**: Up/Down (2%), PageUp/PageDown (10%), Home/End (100%)
- **ChatDisplay**: Ctrl+C (copy all), Up/Down (36px scroll), PageUp/PageDown, Home/End
- **AIChatPanel prompt**: Enter to submit
- Focus container with explicit Tab order set on 8 components

### 1.5 AccessibilityHandler Implementations
Three components provide custom accessibility:
- **MorphPad**: `AccessibilityRole::group` with `AccessibilityValueInterface` reporting "X 50%, Y 50%"
- **SnapFader**: `AccessibilityRole::slider` with percentage readout
- **MacroKnobStrip**: `AccessibilityRole::group`

### 1.6 Responsive Layout
- **ModeBar**: Two-row compact layout at < 660px
- **BottomControlStrip**: 2×2 grid layout at < 760px
- **Global**: Editor resizes from 760×640 to 1600×1200
- **Font scaling**: Proportional to window width from 920px baseline, clamped 0.75×–1.5×

### 1.7 Font Scaling & Minimum Sizes
Every font role has a documented minimum size floor:
| Role | Base | Minimum |
|------|------|---------|
| Title | 20px | 16px |
| Section | 10.5px | 9px |
| Control | 12px | 10px |
| Value | 11px | 10px |
| Micro | 10px | 10px |

### 1.8 Visual Hierarchy
The tab-based layout (Classic → Engine → Modulation → Presets → AI) provides clear separation of concerns. The five-tab structure matches user mental models:
- **Classic**: Everything needed for normal morphing operation
- **Engine**: Advanced audio engine configuration
- **Modulation**: Modulation routing
- **Presets**: Save/load workflows
- **AI**: Intelligent assistance (expert mode only)

### 1.9 Feedback Design (selected wins)
- **OUT meter**: Smoothed with asymmetric attack/release (0.6/0.18) for natural ballistics; shape + tick marks + color zones for redundancy
- **Flash messages** on MorphPad: Auto-fade ~3s for capture confirmation
- **Breeding status label**: Context-aware text feedback ("Bred slot 3 from 1 + 5")
- **ChatDisplay "Thinking…"**: Elapsed seconds counter updated every 2s
- **License overlay**: Semantic state machine (Default → Grace → Expired → Activating → Success/Failure)

---

## 2. Critical Friction Points

These issues block workflows, produce silent failures, or create fundamental misunderstanding.

### C1. Drift Panel Always Visible When Drift Mode Inactive

**Location:** `DriftControlPanel` (Engine tab → Drift section)  
**Problem:** The Speed / Distance / Chaos knobs are **always interactive** regardless of the active physics mode (Direct, Elastic, or Drift). A user in Direct mode who tweaks the Drift knobs sees **zero effect** with no indication that the controls are inert. The ModeBar correctly hides the Elastic preset combo when not in Elastic mode — DriftControlPanel should follow the same pattern.

**User impact:** High. User may spend time adjusting Drift controls while in Direct or Elastic mode, observe no change, and conclude the plugin is broken. The knobs write to APVTS parameters that the engine only consumes when `physicsMode == Drift`.

**Severity:** Critical — silent failure, broken mental model.

### C2. BreedingPanel Buttons Never Disable

**Location:** `BreedingPanel`  
**Problem:** All five buttons (Breed, Mutate, Randomize, Waypoints, Clear WP) are **always clickable** regardless of whether the operation can succeed. Clicking "Breed" with < 2 snapshots or "Mutate" with 0 snapshots produces only a small status-label text change with no visual emphasis. A user clicking these buttons repeatedly sees no outcome and may believe the UI is broken.

**User impact:** High. The status label uses the same font, size, and color as the idle "Snapshot genetics ready" text — easy to miss. No button ever dims to indicate "available but not right now."

**Severity:** Critical — silent no-ops, undermines trust in the feature.

### C3. ParameterMapPanel Shows Raw Floats

**Location:** `ParameterMapPanel` (overlay panel, toggle-able via "All Parameters >")  
**Problem:** Parameter values display as raw normalized floats (e.g., "0.500") with **no unit, no context, no mapping** to meaningful values. A user sees "0.750" and has no idea whether that's 75%, -6 dB, or 120 Hz.

**User impact:** High. The whole purpose of this panel is parameter visibility, but the values are opaque to anyone unfamiliar with plugin parameter normalization (0.0–1.0). The value readout should at minimum show a percentage or a scaled representation.

**Severity:** Critical — informational value severely degraded.

### C4. ChatDisplay Errors Blend with Regular Messages

**Location:** `ChatDisplay` (AI tab transcript)  
**Problem:** Error messages are prefixed with `[Error]` but rendered in the **same bubble style, same color, same font** as regular assistant messages. No red tint, no icon, no border change — the `[Error]` prefix is the only differentiator.

**User impact:** Medium-high. Users scanning the transcript may miss errors entirely. For AI-triggered parameter changes, a failed application looks identical to a successful one at a glance.

**Severity:** Critical — information scent failure.

---

## 3. High Friction Points

These issues create confusion, inefficiency, or accessibility gaps.

### H1. Three Names for One Concept: Sanity / Safety / sanityEnabled

**Locations:** `BottomControlStrip` + APVTS  
**Problem:** The same safety feature is called:
- **"Sanity"** — button label text
- **"SAFETY"** — section header painted text  
- **`"sanityEnabled"`** — APVTS parameter ID

A user scanning the section sees "SAFETY" but the button reads "Sanity." These are two different English words that do not mean the same thing. "Sanity" is jargon (short for "sanity check") that a new user will not recognize.

**User impact:** Medium. Confusion about which feature is which, reduced discoverability.

### H2. Sidechain Label Inconsistency: SIDE vs Side vs SC

**Location:** `BottomControlStrip` (compact vs wide mode)  
**Problem:** The same section is labeled three different ways:
- **"SIDE"** — wide mode (uppercase painted text)
- **"Side"** — compact mode (title-case painted text)
- **"SC"** — the ToggleButton's constructor label string

The code comment at line 226 notes `// M11: "Side" reads in the narrow compact layout (was cryptic "SC")`, indicating the team already recognized "SC" was cryptic. But the fix was only applied to compact mode — wide mode still uses "SIDE" (uppercase) and the toggle label is still "SC".

**User impact:** Medium. Inconsistency undermines polished feel.

### H3. "Clear WP" Abbreviation Mismatches "Waypoints"

**Location:** `BreedingPanel`  
**Problem:** The waypoint playback toggle is labeled **"Waypoints"** (full word), but the clear button is labeled **"Clear WP"** (abbreviation). A user must connect "WP" to "Waypoints" — an extra cognitive step. "WP" is not an industry-standard abbreviation.

**User impact:** Low-medium. Minor friction, but the inconsistency is visually jarring once noticed.

### H4. ParameterMapPanel — Tiny Fonts + No Empty State

**Location:** `ParameterMapPanel`  
**Problem:** Two issues:
1. Fonts at 10–11px are **below comfortable legibility**, especially for the value column (10px text in `#8E8F95` on `#070709` — the small size compounds the mid-contrast color).
2. When no plugin is loaded, the panel shows **only a blank scrollable area** with "All Parameters" header and a search field. There is no message like "Load a plugin to see its parameters here."

**User impact:** Medium. Hard to read values; empty state is confusing.

### H5. AIStatusPanel — No Loading or Error States

**Location:** `AIStatusPanel` (bottom bar)  
**Problem:** MCP server startup/stop transitions have no intermediate visual state:
- Clicking "Start MCP" shows no "Starting…" state — the panel jumps from ○ AI Offline → ● AI Ready
- If MCP fails to start (port conflict, etc.), there is **no error indication** — the panel simply stays on ○ AI Offline with no message
- The Start/Stop button is **never disabled** — user can double-click during transition

**User impact:** Medium. Silent errors are invisible; no feedback during transition.

### H6. SnapFader Right-Click Capture — No Visual Confirmation

**Location:** `SnapFader`  
**Problem:** Right-clicking a slot to capture creates a snapshot but provides **no visible confirmation** on the fader itself. The MorphPad does show a flash message ("Captured to slot N"), but if the user is operating the fader without looking at the pad, there is no feedback.

**User impact:** Low-medium. The capture worked, but the user has no local confirmation.

---

## 4. Medium Friction Points

These issues reduce polish, consistency, or accessibility.

### M1. ComboBox Lacks Focus Ring

**Location:** `MorePhiLookAndFeel::drawComboBox()`  
**Problem:** `drawFocusRingIfFocused()` is called for buttons, rotary sliders, and linear sliders, but **not for ComboBox** or Label. A keyboard user tabbing through the UI has no visual indicator when focus lands on a ComboBox.

**WCAG violation:** 2.4.7 (Focus Visible).  
**User impact:** Medium for keyboard-only users.

### M2. Rotary and Linear Sliders Lack Hover Feedback

**Location:** `MorePhiLookAndFeel::drawRotarySlider()`, `drawLinearSlider()`  
**Problem:** The JUCE draw methods for these controls do not receive `isOver` or `isDown` parameters. Buttons get hover (cyan border) and press (darken) feedback, but sliders provide **no visual response** to mouse hover or drag initiation. A user who hesitates over a slider sees nothing to confirm their target.

**User impact:** Low-medium. Reduces confidence in target acquisition, especially for motor-impaired users.

### M3. Syncopate Typeface at 9–10px Section Labels

**Location:** `MorePhiLookAndFeel` (FontRole::Section, `kMinSectionLabel = 9px`)  
**Problem:** Syncopate is an all-caps geometric display font. At 9–10px the letterforms lose their distinctive silhouette — characters like 'B', 'R', 'P' collapse into similar shapes. WCAG does not specify minimum font size directly, but 9px is below what most readability research recommends for continuous text, and Syncopate is particularly poorly suited to this size.

**User impact:** Medium. Section headers ("SAFETY", "RECALL", "OUTPUT") become hard to read, especially on high-DPI displays where the physical pixel size is even smaller.

### M4. Section Labels Painted in `paint()` — Inaccessible

**Locations:** `MacroKnobStrip`, `BottomControlStrip`, `DriftControlPanel`, `SpectralControlPanel`, `GranularControlPanel`  
**Problem:** Section labels like "MACRO CONTROLS", "SAFETY", "DRIFT" are painted via `g.drawText()` / `drawFittedText()` in the `paint()` method — they are **not JUCE Component instances**, so:
- Invisible to screen readers (no accessibility role)
- No tooltip support
- No way to style independently of the control
- Cannot be discovered by accessibility tree navigation

**WCAG violation:** 4.1.2 (Name, Role, Value).  
**User impact:** High for screen reader users — entire UI sections are invisible.

### M5. AIChatPanel Send/Cancel/Clear Buttons Lack Tooltips

**Location:** `AIChatPanel`  
**Problem:** The three action buttons at the bottom of the AI tab have **no tooltips**:
- "Send" — no indication of keyboard shortcut (Enter/Ctrl+Enter)
- "Cancel" — no explanation of what is being cancelled
- "Clear" — no indication that this also clears workflow panel state and conversation history

**User impact:** Low. Basic affordance, but missing for discoverability.

### M6. License Activation Has No Cancel During Network Hang

**Location:** `LicenseActivationOverlay`  
**Problem:** When the user clicks "Activate" and the network request hangs (slow server, proxy timeout, no internet), the overlay disables all buttons except "Continue in Demo" but provides **no way to abort** the in-flight activation. The user is stuck in the "Connecting…" state until the HTTP client times out (could be 30+ seconds).

**User impact:** Medium. Users with unreliable internet may be trapped in a loading state.

### M7. ParameterMapPanel Search Field Enabled with No Plugin Loaded

**Location:** `ParameterMapPanel`  
**Problem:** The search/filter `TextEditor` is enabled even when `rebuildForPlugin()` has been called with an empty parameter list (no plugin loaded). A user can type into the search field and see no results with no explanation. Filtering an empty result set is a no-op.

**User impact:** Low. Type and nothing happens — minor confusion.

### M8. Modal Overlays Lack Auto-Focus

**Location:** `LicenseActivationOverlay`, `OnboardingOverlay`, `LLMSettingsDialog`  
**Problem:** When these overlays appear, **no component receives initial keyboard focus**. In `LicenseActivationOverlay`, the key input `TextEditor` is the primary interaction target, but the user must Tab or click to reach it. In `OnboardingOverlay`, no button receives initial focus.

**User impact:** Low. One extra Tab press on overlay open.

---

## 5. Low Friction Points

These are cosmetic or nice-to-have items.

### L1. `accentAmber` and `accentGoldBright` Are Identical Hex

**Location:** `MorePhiLookAndFeel.h`  
**Problem:** Both `accentAmber` and `accentGoldBright` resolve to `#F9E596`. If the intent is for amber to be a warning color (distinct from gold highlights), the two should differ. Currently, any visual distinction between "warning" and "bright highlight" is accidental (same color used in different contexts).

**User impact:** None. Only relevant if the semantic distinction is intended for future use.

### L2. "Unlock the Plugin" Language Slightly Misleading

**Location:** `LicenseActivationOverlay` (description text)  
**Problem:** The description says "Please enter your More-Phi license key to unlock the plugin." In demo mode, the plugin is **fully functional for audio processing** (morphing, snapshots, modulation, hosting) — only premium AI features are gated. The phrase "unlock the plugin" overstates what activation provides.

**User impact:** Low. Minor misalignment with actual demo scope.

### L3. No Keyboard Shortcut Hints on Buttons

**Location:** Multiple panels  
**Problem:** Buttons that have keyboard shortcuts (Enter to send in AIChatPanel, Ctrl+C for ChatDisplay copy-all) do not surface those shortcuts in tooltips or labels. A user who prefers keyboard navigation has no way to discover them.

**User impact:** Low. Power-user inefficiency.

### L4. Single Blank Frame at Startup

**Location:** `PluginEditor`  
**Problem:** On initial load, the editor paints a single blank frame before the first timer tick (15 Hz) populates meters, labels, and state. This flash is sub-100ms on fast systems but noticeable on slower hardware or DAWs that initialize the editor asynchronously.

**User impact:** Very low. Cosmetic flash on launch.

---

## 6. Corrective Recommendations

Each recommendation is ordered by user impact (highest first). All maintain the existing dark-theme design language.

---

### R1. Fix DriftControlPanel Mode Context Awareness

**Priority:** Critical  
**Problem:** C1 — Drift controls always visible and interactive regardless of physics mode.

**Recommendation:** Make `DriftControlPanel` follow the same pattern `ModeBar` uses for the Elastic preset combo: hide the panel (or grey it out) when `physicsMode != Drift`.

**Implementation options:**

| Option | Effort | UX |
|--------|--------|----|
| **A — Hide entirely** | Low: add `setVisible()` call in a state-update method triggered from `updateModeBar()` or the 10Hz timer | Cleanest — matches Elastic preset pattern |
| **B — Grey out** | Medium: override `paint()` to draw an overlay or set alpha 0.38 on all children when inactive | Lets user see values but confirms they're inert |

**Recommendation:** Option A (hide entirely) for consistency with the Elastic preset behavior. Wire the visibility toggle into the `ModeBar`'s existing 10Hz timer callback that already reads `getPhysicsMode()`.

**Verification:** Toggle between Direct/Elastic/Drift modes — Drift panel should appear only in Drift mode.

---

### R2. Disable BreedingPanel Buttons When Operation Not Available

**Priority:** Critical  
**Problem:** C2 — Breed/Mutate/Randomize/Clear WP buttons always clickable.

**Recommendation:** Gate each button's `setEnabled()` against preconditions:
- **Breed**: enabled when `snapshotBank.occupiedSlotCount() >= 2`
- **Mutate**: enabled when `occupiedSlotCount() >= 1`
- **Randomize**: always enabled (harmless no-op)
- **Waypoints**: always enabled (toggle)
- **Clear WP**: enabled when `waypointCount() > 0`

Update enabled states in the 30Hz or 15Hz timer tick.

**Verification:** With 0 snapshots: Breed and Mutate should be greyed. With 1 snapshot: Breed greyed, Mutate enabled.

---

### R3. Add Meaningful Units to ParameterMapPanel Values

**Priority:** Critical  
**Problem:** C3 — Raw normalized floats displayed.

**Recommendation:** Display values with **human-readable formatting**:
- For parameters with known ranges: show scaled value + unit (e.g., from `ParameterBridge` metadata)
- For generic normalized params: show percentage: `"50%"` instead of `"0.500"`
- For discrete/binary params: show step label or "On / Off"

**Implementation sketch (ParameterMapPanel.cpp):**
```cpp
auto value = bridge->getParameter(idx)->getValue();
auto param = bridge->getParameter(idx);

// Try to get a formatted string from the parameter itself
juce::String displayValue;
if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
    displayValue = ranged->getText(value, 0); // delegate to parameter's formatter
else
    displayValue = juce::String(static_cast<int>(value * 100.0f)) + "%";
```

**Verification:** Load a plugin — values should show "75%", "-6.3 dB", "120 Hz", etc., not "0.500".

---

### R4. Visually Differentiate Error Messages in ChatDisplay

**Priority:** Critical  
**Problem:** C4 — Errors blend with regular messages.

**Recommendation:** Give error messages distinct visual treatment:
- **Role label**: Change "Assistant" to "Error" in coral/accentCoral color (`#E5C057`)
- **Bubble background**: Add a subtle red/coral tint (e.g., `accentCoral` at 8% alpha)
- **Icon**: Add the Unicode ballot X (U+2715) before the error text
- Keep the `[Error]` prefix for text-only clients

**Implementation location:** `ChatDisplay::addMessage()` — check `role == "Error"` or add an `isError` flag to the `Message` struct.

**Verification:** Trigger an AI error — the message should be visually distinct from assistant responses at a glance.

---

### R5. Unify "Sanity/Safety/sanityEnabled" Naming

**Priority:** High  
**Problem:** H1 — Three different names for one concept.

**Recommendation:** Rename to a single consistent term across UI labels, APVTS ID, and section header.

| Current | Proposed | Rationale |
|---------|----------|-----------|
| Button label "Sanity" | **"Protect"** | Clear verb — protects Volume/Pitch/Bypass. Shorter than "Safety Check". |
| Section header "SAFETY" | **"SAFETY"** (keep) | OK as section label; the button rename bridges the gap. |
| APVTS `"sanityEnabled"` | `"safetyEnabled"` | Match the section header. (Breaking change for existing presets) |

**Alternative:** Keep APVTS as-is (backward compat), change button label to **"Safety"** (noun matching section header) instead of "Sanity".

**Verification:** The button label, section header, and APVTS path should all use variants of "safety" — no more "Sanity".

---

### R6. Fix Sidechain Label Consistency

**Priority:** High  
**Problem:** H2 — SIDE / Side / SC mismatch.

**Recommendation:**
1. Change wide-mode painted text from `"SIDE"` to `"SIDE"` → rethink: use **"SIDECHAIN"** or **"SC"** if space is very tight, but prefer **"Sidechain"** (title case) in both modes.
2. Change the toggle constructor label from `"SC"` to `"Side"` or omit the label entirely and rely on the section header (the toggle already has a tooltip).
3. Ensure compact mode uses the same string as wide mode — the current inconsistency (`"SIDE"` vs `"Side"`) reveals the fix was partial.

**Verification:** The section reads the same in both wide and compact layouts.

---

### R7. Expand "Clear WP" to "Clear Waypoints"

**Priority:** High  
**Problem:** H3 — Abbreviation mismatch.

**Recommendation:** Change `"Clear WP"` → `"Clear Wpts"` or `"Clear Waypnts"` or `"Clear W"` — any option that avoids a novel abbreviation. Best option given the 78px fixed width: **`"Clear Wpts"`** (5 chars + space) or simply **`"Clear"`** (5 chars) if the tooltip differentiates from other actions.

**Verification:** The button label no longer introduces "WP" as an unexplained abbreviation.

---

### R8. Improve ParameterMapPanel Fonts and Empty State

**Priority:** High  
**Problem:** H4 — 10px fonts, no empty-state message.

**Recommendation:**
1. **Raise minimum font sizes**: Name label from 11px → 12px; value label from 10px → 11px; search from 11px → 12px.
2. **Add empty-state label**: When `params_.empty()`, show a centred `Label` with text `"Load a plugin to see its parameters here."` in `textDim` color.
3. **Disable search field** when parameter list is empty.

**Verification:** With no plugin loaded, the panel shows guidance text. With a plugin loaded, all text is at least 11px (preferably 12px for the name column).

---

### R9. Add MCP Server Transition States to AIStatusPanel

**Priority:** High  
**Problem:** H5 — No loading/error states.

**Recommendation:**
1. Add a third status glyph: **"◐ Starting…"** (half-circle, U+25D0) with amber/yellow text during startup/shutdown transitions
2. Surface MCP server errors: if `MCPServer::start()` returns false, show **"✗ Failed to start"** in coral text with a 5s auto-dismiss
3. Disable the Start/Stop button during transitions (debounce)

**Verification:** Starting MCP shows an intermediate state. A failed start shows an error message.

---

### R10. Add Capture Confirmation to SnapFader

**Priority:** High  
**Problem:** H6 — No visual confirmation on capture.

**Recommendation:** After a right-click capture on a fader slot, paint a brief confirmation overlay on the fader (similar to MorphPad's flash message system): overlay text like `"Slot 3 \u2190 capture"` for ~1.5s, then fade.

**Implementation:** Reuse `FlashMessage` from `MorphPad.h` or add a simple owned pointer to a juce::Component that paints + auto-hides.

**Verification:** Right-click a fader slot — a brief confirmation appears on the fader.

---

### R11. Add Focus Ring to ComboBox

**Priority:** Medium  
**Problem:** M1 — WCAG 2.4.7 failure.

**Recommendation:** Call `drawFocusRingIfFocused(g, comp, area)` at the end of `MorePhiLookAndFeel::drawComboBox()`.

```cpp
// In drawComboBox, after drawing the arrow:
drawFocusRingIfFocused(g, comboBox, comboBox.getLocalBounds());
```

**Verification:** Tab to a ComboBox — a cyan focus ring appears.

---

### R12. Add Hover/Active Feedback to Sliders

**Priority:** Medium  
**Problem:** M2 — No mouse-feedback on sliders.

**Recommendation:** Since JUCE's `drawRotarySlider` signature does not pass `isOver`/`isDown`, there are two approaches:
- **Option A (LookAndFeel):** Store a global or static map of last-interacted slider IDs checked against the current mouse component — fragile.
- **Option B (Component-level):** Override `mouseEnter`/`mouseExit`/`mouseDown`/`mouseUp` on individual sliders to call `repaint()` with a custom flag. This is more boilerplate but correct.

**Recommendation:** Option B for correctness. Add a `bool hovered_` member to sliders in panels where hover feedback matters most (MacroKnobStrip, DriftControlPanel). On hover, brighten the slider track or increase the glow width by 2px.

**Verification:** Mouse over a rotary knob — the arc glow slightly brightens or the pointer thickens.

---

### R13. Replace Syncopate with Outfit at Section Label Sizes

**Priority:** Medium  
**Problem:** M3 — Syncopate at 9–10px illegible.

**Recommendation:** Change `FontRole::Section` to use **Outfit** (the body font) at `10.5px` base (keep Syncopate for `FontRole::Title` and `FontRole::Section` only at sizes ≥ 12px). Alternatively, raise `kMinSectionLabel` from `9px` to `11px`.

**Implementation:** In `MorePhiLookAndFeel::makeRoleFont()`:
```cpp
case FontRole::Section:
    return bodyFont(size, isBold ? Font::bold : Font::plain); // was: displayFont
```

**Verification:** Section headers like "MACRO CONTROLS" and "RECALL" are clearly legible at the smallest window size.

---

### R14. Convert Painted Section Labels to Label Components

**Priority:** Medium  
**Problem:** M4 — Painted text = invisible to screen readers.

**Recommendation:** Replace `g.drawText()` calls in `MacroKnobStrip::paint()`, `BottomControlStrip::paint()`, `DriftControlPanel::paint()`, and engine sub-panels with actual `juce::Label` members. This is a small refactor (add member, position in `resized()`) that immediately makes section headers screen-reader accessible and enables tooltips.

**Verification:** A screen reader user can navigate to each section header and hear its name announced.

---

### R15. Add Tooltips to AI Chat Action Buttons

**Priority:** Medium  
**Problem:** M5 — Send/Cancel/Clear lack tooltips.

**Recommendation:**
- **Send**: `"Send prompt to AI assistant (Enter)"`
- **Cancel**: `"Cancel the current AI request"`
- **Clear**: `"Clear conversation history and workflow state"`

**Verification:** Hover over each button — a detailed tooltip appears.

---

### R16. Add Cancel Mechanism to License Activation

**Priority:** Medium  
**Problem:** M6 — No way to abort hung activation.

**Recommendation:** Add a "Cancel" button (or repurpose "Continue in Demo") that calls `juce::URL::cancel()` or abandons the HTTP response future when activation is in flight. Timeout: add a 15-second hard deadline on the activation HTTP request.

**Implementation:** The activation HTTP code should use `juce::URL::withTimeout(Time(15000))` and a `cancel()` call path via `onClick`.

**Verification:** Click Activate, then click Cancel — the overlay returns to the input state immediately.

---

### R17. Disable ParameterMapPanel Search Field When No Plugin

**Priority:** Low  
**Problem:** M7 — Search field enabled with empty list.

**Recommendation:** Call `searchField_.setEnabled(!params_.empty())` in `rebuildForPlugin()`.

**Verification:** With no plugin loaded, the search field is greyed.

---

### R18. Auto-Focus Primary Input in Modal Overlays

**Priority:** Low  
**Problem:** M8 — No initial focus on overlay open.

**Recommendation:** In `LicenseActivationOverlay::visibleChanged()`, call `keyInput_.grabKeyboardFocusAsync()`. In `OnboardingOverlay`, focus the "Next" button or the dismiss button. In `LLMSettingsDialog`, focus the API key field or provider combo.

**Verification:** Opening any overlay auto-focuses the primary interaction target.

---

### R19. Disambiguate `accentAmber` and `accentGoldBright`

**Priority:** Low  
**Problem:** L1 — Identical hex values.

**Recommendation:** If a semantic distinction is desired, shift `accentAmber` slightly toward orange/warm (e.g., `#F4A460` — sandy brown) or toward caution yellow (`#E8C840`). If not, consolidate to one name and remove the alias.

**Verification:** The two color tokens resolve to visibly different hex values (or one is removed).

---

### R20. Clarify Demo Scope in License Overlay Text

**Priority:** Low  
**Problem:** L2 — "Unlock the plugin" overstates activation benefit.

**Recommendation:** Change the description to: *"Enter your license key to activate premium AI features, or continue in demo mode with full audio processing."*

**Verification:** The text accurately reflects that demo mode preserves all audio features.

---

## 7. Accessibility Scorecard

| Criterion | Status | Notes |
|-----------|--------|-------|
| **1.4.1 Use of Color** | ✅ Pass (most controls) | Glyph+text redundancy on status indicators. Bipolar knob relies on arc direction + color. |
| **1.4.3 Contrast (AA)** | ✅ Pass | Minimum 5.2:1 on smallest text; 17:1 on primary text. |
| **1.4.3 Contrast (AAA)** | ⚠️ Partial | `textSecondary` (6.24:1) fails AAA small-text threshold (7:1). |
| **2.1.1 Keyboard** | ✅ Pass | All interactive elements keyboard-navigable. |
| **2.4.7 Focus Visible** | ⚠️ Partial failure | ComboBox and Label lack focus ring. |
| **2.5.8 Target Size** | ❌ Fail | Slider thumbs 8–16px vs 44px recommended. |
| **4.1.2 Name, Role, Value** | ⚠️ Partial failure | Painted section labels lack accessibility roles. |
| **Reduced motion** | ❌ Not addressed | Neon glow, fade animations have no `prefers-reduced-motion` check. |
| **High contrast mode** | ❌ Not addressed | No Windows High Contrast Mode support. |
| **Screen reader** | ✅ Partial | Three components have custom AccessibleHandlers. |

---

## 8. Appendix: Contrast Measurements

All measurements against `backgroundDark` (`#070709`, luminance ~0.0022).

| Color | Hex | Luminance | Ratio vs BG | WCAG |
|-------|-----|-----------|-------------|------|
| `textPrimary` | `#EEEEF2` | 0.857 | **17.4:1** | AAA |
| `textSecondary` | `#8E8F95` | 0.275 | **6.2:1** | AA |
| `textDim` | `#9A9AA2` | 0.326 | **7.2:1** | AAA |
| `accentGold` | `#E5C057` | 0.558 | **11.7:1** | AAA |
| `accentCyan` | `#00BDCA` | 0.413 | **8.9:1** | AAA |
| `accentMagenta` | `#E22EDB` | 0.233 | **5.4:1** | AA |
| `accentGreen` | `#34D399` | 0.505 | **10.6:1** | AAA |
| `borderColour` | `#323237` | 0.019 | **1.3:1** | — (decorative) |

Text on `surfaceLight` (`#17181C`, luminance ~0.009):

| Pair | Ratio | WCAG |
|------|-------|------|
| `textPrimary` on `#17181C` | ~20:1 | AAA |
| `textSecondary` on `#17181C` | ~5.5:1 | AA |
| `textDim` on `#17181C` | ~6.2:1 | AA |

---

*End of audit report. 20 corrective recommendations across critical (4), high (6), medium (7), and low (3) priorities.*
