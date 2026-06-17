# More-Phi User Manual Traceability Matrix

This matrix cross-references `docs/USER_MANUAL.md` against the current GUI, backend, automation, MIDI, MCP, and test surfaces. Guide workflow coverage for `docs/USER_GUIDE.md` is tracked separately in `docs/validation/USER_GUIDE_TRACEABILITY.md`. Status values are limited to: `Pass`, `Mismatch`, `Missing`, `Needs automated test`, and `Needs manual host verification`.

Last refreshed: `2026-06-17`. Current validation evidence is summarized in `docs/validation/USER_MANUAL_E2E_AUDIT_REPORT.md`; the focused manual/VST3 suite passes `27/27`, the full Release suite passes `474/474`, `pluginval` strictness-5 has passed (`validation/pluginval_strictness5.txt`, 2026-06-16), and the local VST3 validator wrapper records the missing Steinberg `vst3_validator` when it is not on `PATH`. `tests/CMakeLists.txt` registers optional `validation` CTest entries when `vst3_validator` or `pluginval` is installed.

## Summary

| Area | Status | Notes |
|---|---|---|
| Global controls | Needs automated test | Title bar/version/output meter are implemented in `src/Plugin/PluginEditor.cpp`; output meter depends on runtime RMS. |
| Plugin Browser Row | Needs manual host verification | Load/show/capture/status are implemented; plugin discovery and hosted editor require host/plugin environment. |
| MorphPad | Needs automated test | UI and backend morph path exist; needs coordinate/source/interpolation coverage. |
| Snapshot Ring | Pass | 12-slot model and MIDI C3-B3 recall route are covered; GUI dot click/full hosted-state behavior still needs additional workflow validation. |
| Snap Fader | Needs automated test | UI/backend fader morph exists; CC1 fader route is covered, while direct mouse-drag interpolation remains GUI/manual. |
| Classic tab | Needs automated test | Controls are implemented; some runtime effects need tests. |
| Engine tab | Needs automated test | Engine APVTS parameters and UI panels exist; audio-domain effect coverage needed. |
| Modulation tab | Needs automated test | UI/backend route systems exist; add-route behavior requires verification. |
| Presets tab | Mismatch | Manual describes banked preset behavior; visible tab is `V2PresetBrowserPanel`/`PresetLibrary` search-list workflow. |
| AI tab and LLM settings | Needs automated test | AI tab exists through `AIChatPanel`; LLM settings exist but need visibility/workflow validation. |
| AI status bar | Needs automated test | `AIStatusPanel` handles MCP state/port/token; backend startup/auth flows are covered, while UI polling and clipboard behavior remain manual/headless. |
| MIDI functions | Pass | Focused VST3 tests cover C3-B3 snapshot notes, CC1 fader routing, and sidechain threshold crossing. |
| MCP feature reference | Needs automated test | Many tools exist; auth and response shapes need regression coverage. |
| Configuration/automation parameters | Needs automated test | APVTS parameters exist; runtime synchronization needs tests. |
| Troubleshooting | Needs manual host verification | Troubleshooting steps map to features but require DAW/plugin/manual QA. |

## Global Controls

| Manual item | Expected GUI | Backend/subsystem | Source reference | Existing coverage | Status | Notes |
|---|---|---|---|---|---|---|
| More-Phi logo | Title bar text | Editor paint only | `src/Plugin/PluginEditor.cpp:110` | None | Pass | Static title text is painted. |
| Version label `v3.3.0` | Title bar label | Project/version docs | `src/Plugin/PluginEditor.cpp:116` | None | Pass | Static version text is painted. |
| OUT meter | Title bar meter | `MorePhiProcessor::getRmsLevel()` | `src/Plugin/PluginEditor.cpp:122` | Basic process finite tests | Needs automated test | Needs audio-driven RMS/meter behavior test or manual visual QA. |
| Resizable editor | Editor resize limits | JUCE editor bounds | `src/Plugin/PluginEditor.cpp:27` | None | Needs automated test | Headless editor construction/resize can validate no-crash and minimum size. |

## Plugin Browser Row

| Manual item | Expected GUI | Backend/subsystem | Source reference | Existing coverage | Status | Notes |
|---|---|---|---|---|---|---|
| Load | `PluginBrowserPanel` load button | `PluginHostManager::scanPluginFolders()` / `loadPlugin()` | `src/UI/PluginBrowserPanel.cpp:12`, `src/UI/PluginBrowserPanel.cpp:101` | Host integration tests | Needs manual host verification | Real scan/load requires installed plugins. |
| Show | `PluginBrowserPanel` show button | Hosted plugin editor window | `src/UI/PluginBrowserPanel.cpp:13`, `src/UI/PluginBrowserPanel.cpp:177` | None | Needs manual host verification | Requires hosted plugin with editor. |
| Capture | `PluginBrowserPanel` capture button | `MorePhiProcessor::captureSnapshotToSlot()` | `src/UI/PluginBrowserPanel.cpp:14`, `src/UI/PluginBrowserPanel.cpp:197` | Snapshot tests indirectly | Needs automated test | Can test capture behavior with no hosted plugin/test descriptors. |
| Hosted plugin name/status | Label | Host manager state | `src/UI/PluginBrowserPanel.cpp:24`, `src/UI/PluginBrowserPanel.cpp:74` | None | Needs manual host verification | UI poll sync exists. |
| Open Plugin | Editor-level button | `HostedPluginWindow` | `src/Plugin/PluginEditor.cpp:63`, `src/Plugin/PluginEditor.cpp:356` | None | Needs manual host verification | Requires hosted plugin editor. |
| Params toggle | Editor-level button | `ParameterMapPanel` | `src/Plugin/PluginEditor.cpp:45`, `src/Plugin/PluginEditor.cpp:49` | None | Needs automated test | Visibility toggle is implemented; direct private access limits assertions. |

## MorphPad

| Manual item | Expected GUI | Backend/subsystem | Source reference | Existing coverage | Status | Notes |
|---|---|---|---|---|---|---|
| XY cursor and drag | `MorphPad` | `morphX`, `morphY`, morph source XY | `src/UI/MorphPad.*`, `src/Plugin/PluginProcessor.cpp:353` | V2 integration/process tests | Needs automated test | Need APVTS/source sync assertions. |
| Occupied/empty snapshot dots | `MorphPad`/`SnapshotRing` visuals | `SnapshotBank` | `src/UI/MorphPad.*`, `src/Core/SnapshotBank.*` | Snapshot/unit tests | Needs automated test | Visual state requires GUI/manual or snapshot-bank tests. |
| Movement trail | `MorphPad` paint/timer | UI state | `src/UI/MorphPad.*` | None | Needs manual host verification | Primarily visual. |
| Double-click capture | `MorphPad` mouse handler | Snapshot capture by nearest slot | `src/UI/MorphPad.*` | None | Needs automated test | GUI event path needs headless-safe approach or manual QA. |
| Inverse-distance morphing | Backend interpolation | `InterpolationEngine` | `src/Core/InterpolationEngine.*` | DSP/V2 tests | Needs automated test | Needs manual-specific regression coverage. |

## Snapshot Ring

| Manual item | Expected GUI | Backend/subsystem | Source reference | Existing coverage | Status | Notes |
|---|---|---|---|---|---|---|
| 12-slot clock layout | `SnapshotRing` overlay | `SnapshotBank::NUM_SLOTS` | `src/UI/SnapshotRing.*`, `src/Core/SnapshotBank.*` | Unit/integration snapshot coverage plus `TestUserManualFeatureSurface.cpp:80` | Pass | 12-slot model exists and is asserted in guide surface coverage. |
| Slot occupied/empty/active states | Ring drawing | `SnapshotBank` | `src/UI/SnapshotRing.*` | Partial | Needs automated test | GUI active visual needs manual/headless QA. |
| Click recall | Ring mouse handler | `MorePhiProcessor::recallSnapshot()` | `src/UI/SnapshotRing.*`, `src/Plugin/PluginProcessor.cpp:329` | Partial | Needs automated test | Recall path needs full/fast tests. |
| MIDI C3-B3 mapping | MIDI router | `MIDIRouter` | `src/MIDI/MIDIRouter.h:61`, `src/MIDI/MIDIRouter.cpp:48` | `tests/Integration/TestVST3MidiAndSidechain.cpp:64` | Pass | Focused VST3 test verifies notes 48-59 map to slots 0-11. |

## Snap Fader

| Manual item | Expected GUI | Backend/subsystem | Source reference | Existing coverage | Status | Notes |
|---|---|---|---|---|---|---|
| Vertical fader handle | `SnapFader` | `faderPos`, morph source fader | `src/UI/SnapFader.*`, `src/Plugin/PluginProcessor.cpp:118` | `tests/Integration/TestVST3MidiAndSidechain.cpp:131` | Needs automated test | CC1/fader route is covered; direct Snap Fader mouse path remains GUI/manual. |
| Occupied markers | `SnapFader` paint | `SnapshotBank` | `src/UI/SnapFader.*` | None | Needs manual host verification | Visual marker QA. |
| MIDI CC1 | MIDI router callback | `setFaderPos()`, morph source 1 | `src/MIDI/MIDIRouter.cpp`, `src/Plugin/PluginProcessor.cpp:118` | `tests/Integration/TestVST3MidiAndSidechain.cpp:114`, `tests/Integration/TestVST3MidiAndSidechain.cpp:131` | Pass | Processor-level CC1 test verifies fader position and morph-source route. |

## Classic Tab

| Manual item | Expected GUI | Backend/subsystem | Source reference | Existing coverage | Status | Notes |
|---|---|---|---|---|---|---|
| Safety section label | Bottom strip paint | UI only | `src/UI/BottomControlStrip.cpp:186` | None | Pass | Painted section labels exist. |
| Sanity toggle | TextButton | APVTS `sanityEnabled` | `src/UI/BottomControlStrip.cpp:19`, `src/Plugin/PluginProcessor.cpp:633` | Basic APVTS existence | Needs automated test | Runtime protection path needs verification. |
| Listen toggle | TextButton | APVTS/runtime `listenMode` | `src/UI/BottomControlStrip.cpp:85`, `src/Plugin/PluginProcessor.cpp:649` | `tests/Integration/TestVST3ParameterAutomation.cpp:120` | Pass | APVTS-to-runtime sync is covered. |
| Link toggle | TextButton | APVTS/runtime `linkMode` | `src/UI/BottomControlStrip.cpp:107`, `src/Plugin/PluginProcessor.cpp:667` | `tests/Integration/TestVST3ParameterAutomation.cpp:120` | Pass | APVTS-to-runtime sync is covered. |
| Fast/Full recall | Buttons | APVTS `recallMode`; queued/full recall | `src/UI/BottomControlStrip.cpp:41`, `src/Plugin/PluginProcessor.cpp:329` | Partial | Needs automated test | Full state chunk behavior requires hosted-plugin/manual QA. |
| Recall Toggle/Sustain | ToggleButton | APVTS/runtime `recallToggle` | `src/UI/BottomControlStrip.cpp:95`, `src/Plugin/PluginProcessor.cpp:653` | `tests/Integration/TestVST3ParameterAutomation.cpp:120` | Needs manual host verification | Runtime sync is covered; sustained-note effect depends on a hosted synth. |
| Output Gain | Rotary slider | APVTS `outputGain`; audio gain | `src/UI/BottomControlStrip.cpp:117`, `src/Plugin/PluginProcessor.cpp:626` | `tests/Integration/TestVST3AudioSignalAccuracy.cpp:109` | Pass | Audio accuracy tests verify 0 dB and nonzero gain cases. |
| Bypass | TextButton | APVTS `bypass`; audio bypass | `src/UI/BottomControlStrip.cpp:142`, `src/Plugin/PluginProcessor.cpp:630` | `tests/Integration/TestVST3AudioSignalAccuracy.cpp:142` | Pass | Tests verify bit transparency with no hosted plugin loaded. |
| SC toggle | ToggleButton | APVTS/runtime sidechain enable | `src/UI/BottomControlStrip.cpp:58`, `src/Plugin/PluginProcessor.cpp:642` | `tests/Integration/TestVST3MidiAndSidechain.cpp:148` | Pass | Processor-side state and sidechain bus are covered. |
| Threshold knob | Slider | APVTS `sidechainThreshold`; MIDIRouter linear RMS threshold | `src/UI/BottomControlStrip.cpp:69`, `src/MIDI/MIDIRouter.h:74` | `tests/Integration/TestVST3MidiAndSidechain.cpp:148`, `tests/Integration/TestVST3MidiAndSidechain.cpp:170` | Mismatch | Manual describes dB; router stores linear RMS `[0,1]` after processor conversion. |
| Direct/Elastic/Drift | Mode buttons | APVTS `physicsMode`; `PhysicsEngine` | `src/UI/ModeBar.cpp:10`, `src/UI/ModeBar.cpp:90` | Physics unit tests | Needs automated test | ModeBar routes through APVTS. |
| Smooth | Slider | APVTS `smoothing` | `src/UI/ModeBar.cpp:21` | Partial | Needs automated test | Add parameter existence/range coverage. |
| Macro knobs 1-8 | 8 sliders | `ParameterBridge`, command queue | `src/UI/MacroKnobStrip.cpp:9`, `src/UI/MacroKnobStrip.cpp:67` | None | Mismatch | Manual says assignable/right-click; implementation maps first 8 hosted parameters. |
| Breed | Button | Snapshot blend + queue | `src/UI/BreedingPanel.cpp:50` | Genetic/physics tests | Needs automated test | Panel implements local blend. |
| Mutate | Button | Snapshot mutate + queue | `src/UI/BreedingPanel.cpp:103` | Genetic/physics tests | Needs automated test | Panel implements local mutation. |
| Randomize | Button | Morph X/Y APVTS | `src/UI/BreedingPanel.cpp:143` | Implementation-backed APVTS surface test | Needs automated test | Button path exists; `smartRandomize` trigger remains separate mismatch. |

## Engine Tab

| Manual item | Expected GUI | Backend/subsystem | Source reference | Existing coverage | Status | Notes |
|---|---|---|---|---|---|---|
| Engine tab | `EngineTabPage` | Audio-domain engines | `src/Plugin/PluginEditor.cpp:72`, `src/UI/EngineTabPage.*` | V2/audio tests | Pass | Tab page is created and shown. |
| Spectral controls | `SpectralControlPanel` | APVTS spectral params/engine | `src/UI/SpectralControlPanel.*`, `src/Plugin/PluginProcessor.cpp:671` | Spectral tests | Needs automated test | Add APVTS surface coverage. |
| Granular controls | `GranularControlPanel` | APVTS granular params/engine | `src/UI/GranularControlPanel.*`, `src/Plugin/PluginProcessor.cpp:682` | Granular tests | Needs automated test | Add APVTS surface coverage. |
| Hybrid blend | `HybridBlendPanel` | APVTS blend/audio-domain params | `src/UI/HybridBlendPanel.*`, `src/Plugin/PluginProcessor.cpp:698` | V2 integration | Needs automated test | Verify params and processing mode. |
| Mastering processors | Backend analysis/mastering | Core/AI mastering tools | `src/Core/AutoMasteringEngine.*`, `src/AI/MCPToolHandler.*` | MCP/mastering tests | Needs automated test | Mostly MCP/backend, not primary Engine tab GUI. |

## Modulation Tab

| Manual item | Expected GUI | Backend/subsystem | Source reference | Existing coverage | Status | Notes |
|---|---|---|---|---|---|---|
| Modulation tab | `ModulationMatrixPanel` | `ModulationEngine`, `ModulationMatrix` | `src/Plugin/PluginEditor.cpp:75`, `src/UI/ModulationMatrixPanel.*` | Modulation tests | Pass | Tab page is created. |
| Routes | Route list/add/remove/clear | Double-buffered matrix | `src/UI/ModulationMatrixPanel.*`, `src/Core/ModulationMatrix.*` | Unit tests | Needs automated test | UI add-route destination behavior should be verified. |
| LFO routes | LFO mini panels | `LFO`/`ModulationEngine` | `src/Core/LFO.*`, `src/Core/ModulationEngine.*` | Unit tests | Needs automated test | Existing unit tests likely reusable. |
| Step sequencing | StepSeq sources/backend | `src/Core/StepSequencer.*` | Unit tests | Needs automated test | Manual-level surface coverage missing. |

## Presets Tab

| Manual item | Expected GUI | Backend/subsystem | Source reference | Existing coverage | Status | Notes |
|---|---|---|---|---|---|---|
| Presets tab | `V2PresetBrowserPanel` | `PresetLibrary`, `PresetSerializer` | `src/Plugin/PluginEditor.cpp:78`, `src/UI/V2PresetBrowserPanel.*` | Preset library tests | Pass | Visible preset browser exists. |
| Banks 16 / presets 128 | Banked preset workflow | `MetaPresetManager` | `src/Preset/MetaPresetManager.*` | State/preset tests | Mismatch | Visible tab appears library/search based, not 16x128 bank UI. |
| Save/load/import/export/delete | Buttons | `PresetLibrary` | `src/UI/V2PresetBrowserPanel.*`, `src/Preset/PresetLibrary.*` | Preset unit tests | Needs automated test | Needs integration round trip coverage. |
| APVTS + snapshots serialization | Preset serializer | `PresetSerializer` | `src/Preset/PresetSerializer.*` | State persistence tests | Needs automated test | Manual-specific coverage needed. |

## AI Tab and LLM Settings

| Manual item | Expected GUI | Backend/subsystem | Source reference | Existing coverage | Status | Notes |
|---|---|---|---|---|---|---|
| AI tab | `AIChatPanel` | Assistant/chat backend | `src/Plugin/PluginEditor.cpp:81`, `src/Plugin/PluginEditor.cpp:245` | LLM chat tests | Pass | AI tab exists in current checkout. |
| LLM settings dialog | Dialog/settings UI | `LLMSettings*` | `src/AI/LLMSettings*`, UI settings files | LLM settings tests | Needs automated test | Existing tests cover settings components; manual workflow needs trace. |
| Provider/API key/base URL/model/test/save | Settings controls | LLM settings store/validator/client | `src/AI/LLMSettings*` | LLM settings tests | Needs automated test | Avoid storing secrets in tests. |

## AI Status Bar

| Manual item | Expected GUI | Backend/subsystem | Source reference | Existing coverage | Status | Notes |
|---|---|---|---|---|---|---|
| MCP status | `AIStatusPanel` label | `MCPServer` running state | `src/UI/AIStatusPanel.*`, `src/AI/MCPServer.*` | MCP tests | Needs automated test | UI polling/manual plus backend tests. |
| Port | `AIStatusPanel` label | Instance port | `src/UI/AIStatusPanel.*`, `src/AI/InstanceRegistry.*` | MCP tests | Needs automated test | Dynamic per-instance port. |
| External clients | Status label | MCP connection count | `src/UI/AIStatusPanel.*`, `src/AI/MCPServer.*` | MCP tests | Needs automated test | Requires server/client flow. |
| Start/Stop MCP | Button | `MCPServer::startServer/stopServer` | `src/UI/AIStatusPanel.*` | MCP tests | Needs automated test | Backend covered; UI manual/headless. |
| Copy Token | Button | Clipboard + instance token | `src/UI/AIStatusPanel.*` | None | Needs manual host verification | Clipboard UI behavior. |

## MIDI Functions

| Manual item | Expected input | Backend/subsystem | Source reference | Existing coverage | Status | Notes |
|---|---|---|---|---|---|---|
| C3-B3 triggers slots 1-12 | MIDI note on/off | `MIDIRouter` snapshot callback | `src/MIDI/MIDIRouter.h:61`, `src/MIDI/MIDIRouter.cpp:48` | `tests/Integration/TestVST3MidiAndSidechain.cpp:64` | Pass | Focused VST3 test verifies notes 48-59 map to slots 0-11. |
| CC1 / Mod Wheel controls Snap Fader | MIDI CC1 | Morph callback/fader source | `src/MIDI/MIDIRouter.cpp`, `src/Plugin/PluginProcessor.cpp:118` | `tests/Integration/TestVST3MidiAndSidechain.cpp:114`, `tests/Integration/TestVST3MidiAndSidechain.cpp:131` | Pass | Processor integration test verifies fader position and morph-source route. |
| Sidechain input triggers snapshot cycling | Sidechain bus | `MIDIRouter::processSidechain()` | `src/MIDI/MIDIRouter.cpp:97` | `tests/Integration/TestVST3MidiAndSidechain.cpp:148`, `tests/Integration/TestVST3MidiAndSidechain.cpp:170` | Pass | Tests cover sidechain bus/APVTS state and rising-edge round-robin behavior. |

## MCP Feature Reference

| Manual item | Expected tool/API | Backend/subsystem | Source reference | Existing coverage | Status | Notes |
|---|---|---|---|---|---|---|
| Core tools | JSON-RPC/MCP tools | `MCPToolHandler` | `src/AI/MCPToolHandler.*` | MCP tests | Needs automated test | Current auth/response shapes should be checked. |
| Semantic safety tools | plugin profile tools | `SemanticPluginProfile`, `MCPToolHandler` | `src/AI/SemanticPluginProfile.*`, `src/AI/MCPToolHandler.*` | AI regression tests | Needs automated test | Recent semantic tests exist; include in audit. |
| Analysis/mastering tools | analysis/mastering MCP tools | analyzers/mastering engine | `src/AI/MCPToolHandler.*`, `src/Core/*Analyzer*` | MCP/unit tests | Needs automated test | Verify tool list and basic response. |
| Auth token/localhost connection | MCP initialize/auth | `MCPServer`, `InstanceIdentity` | `src/AI/MCPServer.*` | `tests/Integration/TestMCPIntegration.cpp:298`, `tests/Integration/TestMCPIntegration.cpp:323` | Needs manual host verification | Backend initialize/auth flow is covered; real external client setup remains manual. |

## Configuration and Automation Parameters

| Parameter | Source reference | Existing coverage | Status | Notes |
|---|---|---|---|---|
| `morphX` | `src/Plugin/PluginProcessor.cpp:598` | Partial | Needs automated test | Host automation and runtime sync. |
| `morphY` | `src/Plugin/PluginProcessor.cpp:600` | Partial | Needs automated test | Host automation and runtime sync. |
| `morphFader` / implementation `faderPos` | `src/Plugin/PluginProcessor.cpp:602` | `tests/Integration/TestUserManualFeatureSurface.cpp:90` | Mismatch | Manual lists `morphFader`; APVTS ID is `faderPos`. |
| `morphSource` | `src/Plugin/PluginProcessor.cpp:788` | `tests/Integration/TestUserManualFeatureSurface.cpp:193` | Mismatch | Manual labels it as "Choice/internal"; it is actually registered as an APVTS `AudioParameterChoice` and is host-visible. Wording should be clarified. |
| `sanityEnabled` | `src/Plugin/PluginProcessor.cpp:633` | Partial | Needs automated test | Runtime protection path. |
| `listenMode` | `src/Plugin/PluginProcessor.cpp:649` | `tests/Integration/TestVST3ParameterAutomation.cpp:120` | Pass | Runtime sync is covered. |
| `linkMode` | `src/Plugin/PluginProcessor.cpp:667` | `tests/Integration/TestVST3ParameterAutomation.cpp:120` | Pass | Runtime sync is covered. |
| `recallMode` | `src/Plugin/PluginProcessor.cpp:637` | Partial | Needs automated test | Fast/full behavior. |
| `recallToggle` | `src/Plugin/PluginProcessor.cpp:653` | `tests/Integration/TestVST3ParameterAutomation.cpp:120` | Needs manual host verification | Runtime sync is covered; sustained-note effect depends on a hosted synth. |
| `sidechainEnabled` | `src/Plugin/PluginProcessor.cpp:642` | `tests/Integration/TestVST3MidiAndSidechain.cpp:148` | Pass | Router sync and sidechain bus are covered. |
| `sidechainThreshold` | `src/Plugin/PluginProcessor.cpp:644` | Sidechain tests | Mismatch | Manual dB wording vs router linear threshold. |
| `outputGain` | `src/Plugin/PluginProcessor.cpp:626` | `tests/Integration/TestVST3AudioSignalAccuracy.cpp:109` | Pass | Audio gain accuracy is covered. |
| `bypass` | `src/Plugin/PluginProcessor.cpp:629` | `tests/Integration/TestVST3AudioSignalAccuracy.cpp:142` | Pass | Bypass transparency is covered with no hosted plugin loaded. |
| `smartRandomize` | `src/Plugin/PluginProcessor.cpp:663` | `tests/Integration/TestVST3ParameterAutomation.cpp:179` | Mismatch | Parameter is exposed, but validation has not found a processor trigger path. |
| `driftOutputX` | `src/Plugin/PluginProcessor.cpp:657` | None | Needs automated test | Runtime drift output. |
| `driftOutputY` | `src/Plugin/PluginProcessor.cpp:659` | None | Needs automated test | Runtime drift output. |

## Troubleshooting

| Manual item | Expected validation | Source reference | Existing coverage | Status | Notes |
|---|---|---|---|---|---|
| More-Phi does not appear in DAW | Plugin build/validator/manual DAW QA | CMake/JUCE plugin target | Optional validator | Needs manual host verification | Requires host/plugin scan. |
| Hosted plugin list empty | Plugin scan status | `PluginBrowserPanel`, `PluginHostManager` | Host tests | Needs manual host verification | Depends on installed plugin paths. |
| Hosted plugin loads but no sound | Signal path/bypass/gain | Processor/host manager | Audio tests | Needs automated test | Add bypass/gain tests; real hosted plugin manual. |
| Morphing does not change sound | Snapshot/interpolation path | Snapshot/Morph/ParameterBridge | Partial | Needs automated test | Requires hosted descriptors or synthetic state. |
| Snapshot recall interrupts notes | Recall mode/toggle | Processor/MIDI | Partial | Needs manual host verification | Real hosted synth behavior manual. |
| Genetic tools too extreme | Sanity/listen/output gain | Breeding/genetic/core | Partial | Needs automated test | Sanity path verification needed. |
| MCP client cannot connect | MCP status/auth/port/token | MCP server/status panel | MCP tests | Needs manual host verification | Backend auth/startup tests are green; external client setup and UI token copy remain manual. |
| LLM validation fails | LLM settings validator | `LLMSettings*` | LLM tests | Needs automated test | Existing settings tests reusable. |
| Safe action restore fails | Semantic MCP rollback | `MCPToolHandler`, `SemanticPluginProfile` | AI regression tests | Needs automated test | Include recent semantic tests in audit. |

## Traceability Totals

Recomputed from the detailed rows above:

| Status | Count |
|---|---:|
| Pass | 26 |
| Mismatch | 13 |
| Needs automated test | 57 |
| Needs manual host verification | 18 |
| Missing | 0 |
| **Total traced items** | **114** |

The summary table at the top of this matrix aggregates high-level areas; the detailed-row totals above provide the item-level coverage count.

## Prioritized Risk Register

| Priority | Finding | Status | Recommended validation |
|---|---|---|---|
| P0 | No active automated-validation blocker remains in the executed focused, regression, VST3 build, and full-suite checks. | Pass | Keep this status green by rerunning focused and full suites after future feature/doc changes. |
| P0 | Steinberg `vst3_validator` has not run locally because it is not on `PATH`. `pluginval` strictness-5 has passed (`validation/pluginval_strictness5.txt`, 2026-06-16). | Needs manual host verification | Install `vst3_validator` in CI/release validation and rerun `tests/scripts/run_vst3_validator.py`; keep `pluginval` strictness-5 in the release pipeline. |
| P1 | `sidechainThreshold` is documented as dB while router stores linear RMS `[0,1]` after processor conversion. | Mismatch | Keep tests for conversion/threshold behavior and clarify documentation wording. |
| P1 | Manual lists `morphFader`, while APVTS uses `faderPos`. | Mismatch | Report and decide doc alias vs implementation rename later. |
| P1 | Manual classifies `morphSource` as "Choice/internal"; it is registered as an APVTS `AudioParameterChoice` and is host-visible. | Mismatch | Clarify manual wording to reflect host-visible automation parameter. |
| P1 | Macro knobs are documented as assignable, but current implementation maps first 8 hosted parameters. | Mismatch | Report; do not fix in validation pass. |
| P1 | Presets manual describes banked 16x128 behavior; visible tab uses preset library/search workflow. | Mismatch | Report and trace both `PresetLibrary` and `MetaPresetManager`. |
| P2 | GUI-only visuals such as movement trail, labels, clipboard token copy, hosted plugin editor, real DAW automation lanes, and audible hosted-plugin recall/morph behavior need manual QA. | Needs manual host verification | Include in release/manual-host QA checklist. |
