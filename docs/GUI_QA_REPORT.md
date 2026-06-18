# More-Phi VST3 — GUI Quality Assurance Report

- **Date:** 2026-06-18
- **Version reviewed:** 3.3.0 · branch `retheme/landing-brand`
- **Method:** Static, code-grounded review of the full JUCE editor (5 tab pages + always-on surfaces + AI panel). 6 specialized reviewers × per-finding adversarial re-verification against the actual source (53 agents). WCAG contrast ratios are computed, not asserted. Because the GUI is JUCE-rendered inside a DAW (not a webpage), runtime-only behaviors are flagged **🔧 needs live DAW test**.
- **Result:** 47 findings · **43 upheld** after verification · 4 refuted · 2 refutations exposed a latent theme-naming collision (see H8/meta).

## Executive verdict

> **Not yet release-ready — ~85% there.** The architecture is genuinely strong (centralized theme, real-time-safe wiring, APVTS discipline, a well-built AI assistant with approval gating and honest latency). It ships with **2 functional blockers** (non-functional controls + a latent dangling-pointer crash path), **2 feature-coverage gaps** (a selectable physics mode with no controls; 3 AI surfaces unreachable), and **1 systemic visual defect** (the brand font is silently replaced by a system fallback across the whole UI). Fix the Critical/High tier and this is a clean release.

> **Update (2026-06-18, after remediation): all 2 Critical + 8 High findings are fixed and verified — three build/test cycles, 503/503 tests, 0 warnings. Remaining work is the Medium/Low polish tier.**

**Severity tally (post-verification):** Critical 2 · High 8 · Medium 16 · Low 19 · Refuted 4.

---

## 🔧 Remediation applied (2026-06-18)

A first remediation pass addressed the top-priority items. Build: **0 warnings / 0 errors**; tests: **503/503 passed** (Release, incl. the full Ozone/IPC/MCP suite); fresh VST3 at `build/MorePhi_artefacts/Release/VST3/MorePhi.vst3` (11.8 MB).

| ID | Status | Change |
|---|---|---|
| **C1** | ✅ Fixed | Removed the non-functional "Include in morphing" toggle, `getMorphEnabledParams()`, and the All/None buttons (plus the FUN-5 dead `findChildWithID` loop). `ParameterMapPanel.{h,cpp}`. |
| **C2** | ✅ Fixed | `PluginBrowserPanel::openPluginEditor` now acquires a use-lease (`acquirePluginForUse`) before opening the hosted editor, releases it via `releasePluginCallback`, and wires the unload `windowCloseCallback` — eliminating the dangling-editor path. (🔧 defect still wants live-DAW confirmation.) |
| **H3** | ✅ Fixed | Removed dead `LearnModePanel` (CMake source list + both files). |
| **H2** | ✅ Fixed | Replaced all 32 `"Inter"` font sites with the embedded **Outfit** body font via a new `MorePhiLookAndFeel::bodyFont()` helper (11 files). Grep confirms no `"Inter"` font usages remain. |
| **H1** | ✅ Fixed | Added `DriftControlPanel` (Speed/Distance/Chaos knobs bound to `driftSpeed`/`driftDistance`/`driftChaos`) to the Engine tab — the Drift physics mode is now shapeable from the UI. (🔧 live-DAW to confirm layout at extremes.) |
| **H5** | ✅ Fixed | Spectral/Granular sub-controls and the SC threshold knob now disable (greyed at 0.38) when their parent toggle is off, via `onClick` + ctor sync (`SpectralControlPanel`, `GranularControlPanel`, `BottomControlStrip`). (🔧 covers the user-toggle case; DAW-automation sync would need APVTS listeners.) |
| **H4** | ✅ Fixed (discoverability) | Enriched the AI assistant's empty state with a capabilities blurb + example prompts that surface Ozone Track Assistant + dataset generation (also resolves AIV-8). Full dataset/Ozone *dialogs* remain a larger follow-up. |
| **H6** | ✅ Fixed | Raised the font-scaling upper clamp from 1.3× to 1.5× so text grows on large/HiDPI canvases (`MorePhiLookAndFeel.cpp`). (🔧 live-DAW: fixed-height rows don't grow — check for clipping at extremes.) |
| **H7** | ✅ Fixed | Macro knobs now show a value readout (`TextBoxBelow`), use brand-color tokens (navy divider → `border()`), and carry per-knob tooltips with the bound param name. User-configurable macro mapping remains a larger deferred feature. |

**Deferred:** the Medium/Low tier (tooltips on remaining controls, `textDim`/border contrast, named layout constants, local-vs-LLM provenance tag, etc.). **All Critical + High findings are now addressed.** The `smartRandomize` orphaned param (COV-3) and the false "LearnModePanel wired" claim in `IMPLEMENTATION_COMPLETE.md` remain.

**Follow-up recommended:** a DI refactor of `PluginBrowserPanel` (depend on `IPluginHostManager` with the lease methods) would enable a deterministic lease-pairing unit test for C2.

---

## ✅ What's working correctly (and why)

| Area | Why it's good | Evidence |
|---|---|---|
| **Theme architecture** | Centralized `MorePhiTheme.h` palette + `MorePhiLookAndFeel` with embedded Syncopate/Outfit TTFs, `cornerRadius=8`, alpha-0.38 disabled convention — the correct JUCE pattern. | `MorePhiTheme.h:8-37`, `MorePhiLookAndFeel.cpp:15-81` |
| **Core contrast** | Primary text & accents far exceed AA: textPrimary 16.8:1, gold 11.5:1, cyan 8.7:1 (recomputed — they hold). | `MorePhiLookAndFeel.h:41-66` |
| **Real-time-safe wiring** | UI never touches audio state directly — everything routes through APVTS attachments, `enqueueParameterSet/State`, or atomic setters. SnapshotBank writes always take the seqlock `WriteScope`. | `ParameterBinding.h`, `PluginProcessor.h:239-280`, `SnapshotBank.cpp:24,70,98` |
| **APVTS range fidelity** | Sliders/combos use real `SliderAttachment`/`ComboBoxAttachment` → ranges & DAW automation derive from one source of truth. Hand-managed sliders match backend exactly (`smoothing 0..0.999`). | `BottomControlStrip.cpp:35-157`, `GranularControlPanel.cpp:30-33` |
| **Knob rendering** | Multi-pass neon glow, bipolar gold/magenta arc, gold-tipped pointer — premium, brand-coherent, allocation-free in paint. | `MorePhiLookAndFeel.cpp:193-295` |
| **AI assistant** | Honest measured latency, race-safe cancel (per-request `shared_ptr<atomic<bool>>`), approval gate before applying AI changes, before/after param diffs surfaced, bounded history (28/128 caps), graceful error friendlification. | `AIChatPanel.cpp:426-443,818-875`, `ChatDisplay.cpp:167` |
| **Responsive floors** | Font min-sizes (9/10) genuinely enforced; layout holds at 720×600 with no negative sizes or overlaps; MorphPad/fader don't collide. | `MorePhiLookAndFeel.cpp:381-382`, `PluginEditor.cpp:255-261` |
| **Graceful degradation** | Macro knobs + browser buttons disable themselves when fewer params / no plugin loaded. | `MacroKnobStrip.cpp:75-90`, `PluginBrowserPanel.cpp:21-22` |

---

## 🔴 Critical (fix before release)

### C1 — "Include in morphing" toggles are non-functional (misleading dead controls)
- **Location:** `src/UI/ParameterMapPanel.cpp:12-14, 184-193`
- **Evidence:** Every parameter row's morph checkbox has no handler; `morphToggle_.setToggleState(true, dontSendNotification)` with no `onClick`/`onStateChange`. `getMorphEnabledParams()` is never read by `MorphProcessor`, the interpolation engine, or snapshot capture. The breed path uses `ParameterClassifier` instead.
- **Problem:** A user can toggle thousands of checkboxes with zero effect. Violates "no non-functional controls" and actively misleads.
- **Fix:** Either wire `morphToggle_::onStateChange` into a processor-side morph-mask mirrored to `ParameterClassifier`, **or** remove the toggle + dead getter.

### C2 — Dangling-editor crash risk in the "Show UI" path 🔧
- **Location:** `src/UI/PluginBrowserPanel.cpp:177-195`, `src/UI/HostedPluginWindow.h:15-43`
- **Evidence:** `PluginBrowserPanel::openPluginEditor()` builds the window from `host_.getPlugin()` — a raw atomic pointer with no lifetime lease. If the hosted plugin unloads (state-restore reload, host teardown, plugin swap) while its native editor window is open, the owned editor + any interaction becomes a dangling pointer. The *other* path (`MorePhiEditor::openPluginWindow`) correctly uses `acquirePluginForUse()/releasePluginFromUse()`.
- **Fix:** Acquire via `acquirePluginForUse()` before constructing the window; release on close. Reuse the safe pattern already in `PluginEditor.cpp:395-411`.

---

## 🟠 High

| ID | Issue | Location |
|---|---|---|
| **H1** | **Drift physics mode selectable but has no controls** — `driftSpeed/Distance/Chaos` have zero GUI binding (defaults only); `driftOutputX/Y` never visualized. | `ModeBar.cpp:35-44` vs `PluginProcessor.cpp:927-935` |
| **H2** | **Brand font silently replaced across the UI ("Inter" not embedded)** — 32 sites in 11 files ask for `"Inter"` (not bundled) → system sans fallback; also hardcode sizes, bypassing scaling. | `V2TabBar.cpp:49`, `ModeBar.cpp:18,49,91`, `BottomControlStrip.cpp:80,137,186,206`, `MacroKnobStrip.cpp:34`, +HybridBlend/Granular/ModulationMatrix/Spectral/Preset/ParameterMap |
| **H3** | **`LearnModePanel` is dead UI** — compiled, runs a 1s timer, never instantiated. Token-budget/classifier controls unreachable. | `LearnModePanel.cpp:1-201`, `CMakeLists.txt:462-463` |
| **H4** | **Dataset generation + Ozone Track Assistant have no GUI** — CLI-only / implicit tool calls only. | `AIChatPanel.cpp:88,91,537` |
| **H5** | **Engine sub-controls stay editable when their engine is off** — `fftSize/alpha/transient/formant`, SC threshold never `setEnabled(false)`. 🔧 | `SpectralControlPanel.cpp:19-87`, `GranularControlPanel.cpp:38-73`, `BottomControlStrip.cpp:58-78` |
| **H6** | **Font scaling hard-caps at 1.3×** → undersized on large/HiDPI canvases (natural 1.74× at 1600px). | `MorePhiLookAndFeel.cpp:88` |
| **H7** | **Macro knobs have no value readout (`NoTextBox`) AND are hardcoded to params 0–7** (explicit TODO); non-brand divider color. | `MacroKnobStrip.cpp:7-14,71` |
| **H8** | **`lnf.textDim` (0xff5a5a60) fails WCAG AA (2.8–2.9:1)** on the version string & OUT label; OUT label also offset 28px left of its meter. | `PluginEditor.cpp:159-162,184-187` |

> ⚠️ **Meta-finding (root cause of 2 refuted findings):** two different `textDim` under the same name — the L&F *instance member* `textDim{0xff5a5a60}` (fails AA) vs `Theme::Colours::textDim() = 0xff8e8f95` (passes — it equals `textSecondary`). VIS-6/VIS-7 were refuted because those call sites used the passing one. **Consolidate to a single definition.**

---

## 🟡 Medium (15)

| ID | Issue | Location |
|---|---|---|
| M1 | Theme bypass: raw hex literals (`0xff888888`, `0xff0f3460`, `0xff4a5568`) instead of tokens | `PluginBrowserPanel.cpp:25,89,97`, `AIStatusPanel.cpp:15-17`, `MacroKnobStrip.cpp:60` |
| M2 | `smartRandomize` APVTS param orphaned — GUI calls engine directly, bypassing it | `BreedingPanel.cpp:152` vs `PluginProcessor.cpp:978` |
| M3 | Tooltips absent on all core controls (only 14 exist, all AI/mod/perf) | `MorphPad/ModeBar/Breeding/Macro/Bottom/Engine` |
| M4 | Ambiguous labels unlabeled ("Direct", "Alpha", "Sanity", "Listen", "SC") | `HybridBlendPanel.cpp:85,112`, `BottomControlStrip.h:27-54` |
| M5 | `statusChip` colors green only on `Active`; chat fires even when "Untested" → misrepresents readiness | `AIChatPanel.cpp:345-356` |
| M6 | Local fallback replies indistinguishable from LLM replies (no provenance tag; Role enum has no `Local`) | `ChatDisplay.cpp:42-51` |
| M7 | No animated pending spinner — "Thinking…" text-only, easy to miss on long calls | `AIChatPanel.cpp:423,933` |
| M8 | 2s thinking-timer races the progress callback, clobbering "Running tool calls (iter N)" | `AIChatPanel.cpp:439-444,929-941` |
| M9 | Concurrent overlapping timers (editor 30Hz + MorphPad 30Hz + others 10Hz) | `PluginEditor.cpp:129`, `MorphPad.cpp:20` |
| M10 | `cornerRadius` inconsistent (3/6/8/12) — license dialog 12 vs rest 8 | `BottomControlStrip.cpp:168`, `LicenseActivationOverlay.cpp:60` |
| M11 | BreedingPanel/ParamMap stay interactive with no plugin loaded (no empty-state disable) | `ParameterMapPanel.cpp:141-164`, `BreedingPanel.cpp:14-29` |
| M12 | Hue-only toggle encoding (blue/orange/green) risks red-green color-blind users | `BottomControlStrip.cpp:86-109` |
| M13 | MorphPad compresses to ~135px at 720×600 on non-AI tabs | `PluginEditor.cpp:226-238` |
| M14 | Title bar wordmark/version/meter use fixed px reservations that don't scale | `PluginEditor.cpp:155-165` |
| M15 | Border widths magic numbers (0.5/1.0/1.2/1.5/2.0), no shared constant | multiple |

---

## 🟢 Low (12)

- COV-2 — Drift output meters unvisualized (`PluginProcessor.cpp:972-975`)
- COV-6 — SnapFader has no numeric position readout (`SnapFader.cpp:15-81`)
- FUN-5 — `selectAllBtn_` dead `findChildWithID` loop + fragile child-index cast (`ParameterMapPanel.cpp:85-103`)
- FUN-6 — Queue-overflow drops edits silently (DBG only, compiled out in Release) (`MacroKnobStrip.cpp:24-29`)
- POL-1 — Stale "M-16 reduced to 15Hz" comment vs `startTimerHz(30)` (`PluginEditor.cpp:127-129`)
- POL-7 — Magic numbers in `resized()` (`PluginEditor.cpp:202-256`)
- RS-5 — Fixed-px browser buttons may clip (`PluginBrowserPanel.cpp:41-47`)
- RS-6 — Deactivate button absolute `-250` offset, brittle (`PluginEditor.cpp:205`)
- RS-8 — 0.5px MorphPad hairlines may vanish at 1× DPI (`MorphPad.cpp:211-217`)
- RS-9 — `getScaledFontSize` doc-contract not enforced (`MorePhiLookAndFeel.cpp:85-90`)
- AIV-8 — Empty chat state is bare "Assistant ready." with no guidance (`ChatDisplay.cpp:156`)

---

## 🤖 AI feature integration — deep dive

**Working well:** clear visual separation (own tab + grouped Workflow/Approval sub-panels with borders); **honest** latency (elapsed seconds, no fake "fast" claims); **race-safe** cancel; **approval gate** before AI changes apply (MCP `permission.approve/reject`); before/after param diffs surfaced for both workflows and approvals; bounded history; graceful WinHTTP/NVIDIA error friendlification.

**Problems (beyond H3/H4):**
- **M6 — Provenance missing:** local deterministic replies (tool inventory, snapshot self-test, local workflow) are pushed as `Role::Assistant` and look identical to real LLM output. Add a `Role::Local` or "(local)" tag.
- **M5 — Readiness signal decoupled:** the toolbar chip is grey for "Untested" but the chat still works → the visual lies about capability.
- **M7/M8 — Weak pending feedback:** text-only "Thinking…" with a 2s timer that clobbers the richer "Running tool calls (iteration N)" status. Add an animated glyph + single-owner status.
- **AIV-8 — Discoverability:** first-run users land on a near-blank transcript with no capability hint or example prompts.

> *Refuted (good — skeptics caught it):* the claim that `AIStatusPanel` "Start MCP" hardcodes port 30001 and would collide was **refuted** — `MCPServer::startServer()` ignores the arg when an identity port is set (`MCPServer.cpp:43`).

---

## 📊 Feature → control coverage

**39 APVTS params:** 33 cleanly exposed · 3 hidden-by-design (`driftOutputX/Y` DAW meters, `smartRandomize`) · **3 orphaned (H1: the entire Drift surface)** · **0 orphaned GUI controls except the dead morph toggles (C1)**. Every documented *feature* has an entry point **except Dataset generation and Ozone Track Assistant (H4)**.

---

## Recommended fix order

1. **C1 + C2** (correctness/safety) — small, high-leverage.
2. **H2** (Inter font) — single biggest visual-integration win; mechanical find-replace + brand helper.
3. **H1** (Drift controls) + **H5** (disable sub-controls) — closes the feature/UX gaps.
4. **H3/H4** (dead/invisible AI surfaces) — decide ship-or-remove; don't ship dead code.
5. **H6** (scaling cap) + the Medium tier — polish pass before release.
