# More-Phi User Manual and VST3 E2E Validation Report

Generated: `2026-05-17`  
Corrected: `2026-06-17` (see errata at end of section 2)

## Verdict

The manual/VST3 validation pass is complete for the deterministic local scope against the actual repository build tree (`build/windows-msvc-release`). The refreshed focused suite passes **27/27**, the full Release suite passes **474/474**, and the VST3 artifact builds successfully.

Steinberg `vst3_validator` remains unavailable locally, but **pluginval** strictness-level-5 has been run successfully (`validation/pluginval_strictness5.txt`, 2026-06-16) against the built VST3 with `0 errors / failures / warnings / asserts / leaks`. The repo validator wrapper records missing `vst3_validator` as JSON/JUnit evidence when that tool is not on `PATH`.

Host-only workflows remain manual blockers: DAW discovery, hosted editor display, real host automation lanes, clipboard token copy, audible hosted-plugin morph/recall, and opaque third-party plugin state chunks.

The validation pass intentionally reports product/documentation drift rather than fixing product behavior or manual wording.

### Errata against original 2026-05-17 report

The original report contained several inaccuracies that have been corrected below:

1. **Build directory**: Original report used `build\codex-ninja-vs18d`, which does not exist. Actual build trees are `build/windows-msvc-release` and `build/windows-ninja-vs2026-safe`.
2. **VST3 artifact path**: Original path used `G:/morphy/...`. Actual artifact is `G:/More_Phi-vst3-plugin/build/windows-msvc-release/MorePhi_artefacts/Release/VST3/MorePhi.vst3`.
3. **Test counts**: Original report claimed 24/24 focused and 375/375 full suite. Actual current counts are 27/27 focused and 474/474 full suite.
4. **External validation**: Original report stated both `vst3_validator` and `pluginval` were unavailable. `pluginval` is available and has passed strictness-5; only `vst3_validator` is unavailable.
5. **`morphSource`**: Original report stated it is internal/not APVTS-visible. It is in fact a `juce::AudioParameterChoice` registered in `PluginProcessor.cpp:788` and is tested in `TestUserManualFeatureSurface.cpp:193`. The real mismatch is the manual's "internal" classification.
6. **Validator artifacts**: Original report claimed `docs/validation/vst3_validator_result_20260517.json` and `..._junit_20260517.xml` were produced. These files do not exist in the repository.
7. **Timing figures**: All timings tied to the non-existent `codex-ninja-vs18d` tree are unverified and have been removed or marked stale pending re-measurement in an actual build tree.

## Coverage Changes

| Area | Evidence | Result |
|---|---|---|
| Manual feature surface | `tests/Integration/TestUserManualFeatureSurface.cpp:28`, `:59`, `:90`, `:99`, `:125`, `:150`, `:193` | APVTS controls, guide workflow controls, documented automation ID drift, Engine-tab APVTS controls, five editor tabs, headless editor construction/resize, `morphSource` existence. |
| VST3 parameter automation | `tests/Integration/TestVST3ParameterAutomation.cpp:60`, `:112`, `:126`, `:151`, `:168`, `:185` | Host-visible documented parameters, automatable flags, gesture updates, APVTS-to-runtime sync, output gain, bypass, smart-randomize documented mismatch. |
| VST3 MIDI/sidechain | `tests/Integration/TestVST3MidiAndSidechain.cpp:64`, `:85`, `:114`, `:131`, `:148`, `:170` | C3-B3 slots, trigger note filtering, CC1 fader route, sidechain bus/state, rising-edge sidechain cycling. |
| VST3 audio accuracy | `tests/Integration/TestVST3AudioSignalAccuracy.cpp:80`, `:93`, `:109`, `:142`, `:162`, `:182` | Finite output, silence preservation, output gain accuracy, bypass bit transparency, deterministic neutral processing, stereo consistency. |
| Test stability | `tests/Unit/TestLLMSettingsStore.cpp:12`, `tests/Unit/TestStandaloneMcpServer.cpp:950` | LLM settings temp directories are unique per test process; fake IPC ring-wrap test uses a deterministic headless timeout. |
| Validator automation | `tests/CMakeLists.txt:184`, `:190`, `:204`; `tests/scripts/run_vst3_validator.py:38`, `:208`, `:244` | CTest uses the repo wrapper and JUCE VST3 bundle path when `vst3_validator` is installed; pluginval strictness 5 is registered when `pluginval` is installed; missing-validator runs emit ASCII-safe console output and machine-readable JSON/JUnit error artifacts on Windows. |

## Validation Commands and Results

### Primary build tree

```powershell
cmake -B build -S . -DMORE_PHI_BUILD_TESTS=ON
```

Result: configure against the actual `build/windows-msvc-release` or `build/windows-ninja-vs2026-safe` tree. The original `304.1 s` timeout figure tied to a non-existent tree is unverified and has been removed.

```powershell
cmake --build build --config Release --target MorePhiTests --parallel 2
```

Result: builds against the actual tree. The original `2445.8 s` timeout figure tied to a non-existent tree is unverified and has been removed.

### Focused manual, guide, and VST3 suite

```powershell
ctest --test-dir build\windows-msvc-release -R "User manual feature surface|User guide feature surface|VST3 parameter automation|VST3 smart randomize|VST3 MIDI handling|VST3 sidechain|VST3 audio signal accuracy" --output-on-failure
```

Result: **27/27** passed.

```text
100% tests passed, 0 tests failed out of 27
Total Test time (real) = 10.98 sec
```

### Full regression suite

```powershell
ctest --test-dir build\windows-msvc-release --output-on-failure --parallel 4
```

Result: **474/474** passed.

```text
100% tests passed, 0 tests failed out of 474
Total Test time (real) = 65.93 sec
```

### VST3 artifact and validator status

```powershell
cmake --build build\windows-msvc-release --target MorePhi_VST3 --parallel 2
```

Result: passed. Artifact:

```text
G:/More_Phi-vst3-plugin/build/windows-msvc-release/MorePhi_artefacts/Release/VST3/MorePhi.vst3
```

```powershell
ctest --test-dir build\windows-msvc-release -N -L validation
ctest --test-dir build\windows-msvc-release --build-config Release -L validation --output-on-failure
Get-Command vst3_validator -ErrorAction SilentlyContinue
Get-Command pluginval -ErrorAction SilentlyContinue
python tests\scripts\run_vst3_validator.py --plugin "G:\More_Phi-vst3-plugin\build\windows-msvc-release\MorePhi_artefacts\Release\VST3\MorePhi.vst3" --output docs\validation\vst3_validator_result_20260617.json --junit docs\validation\vst3_validator_junit_20260617.xml
```

Results:

- `ctest -N -L validation`: registers `VST3Validation` and/or `PluginvalValidation` tests when the corresponding executable is on `PATH`.
- `vst3_validator`: unavailable on `PATH`.
- `pluginval`: available; strictness-5 run passed (`validation/pluginval_strictness5.txt`, 2026-06-16) with `0 errors / failures / warnings / asserts / leaks`.
- Validator wrapper result when `vst3_validator` is missing: blocked, `{"status": "error", "error": "VST3 validator not found"}`.

## Confirmed Passing Areas

- Implementation-backed manual automation parameters are APVTS-visible and host-enumerable.
- Documented VST3 parameters in the focused set are automatable according to JUCE host parameter metadata.
- The guide/manual-visible snapshot model exposes 12 slots and sidechain input bus availability.
- The editor shell exposes the documented five tabs and constructs/resizes headlessly.
- Engine-tab spectral, granular, and audio-domain APVTS controls are present.
- Representative automation gestures update APVTS values and selected runtime state.
- `listenMode`, `linkMode`, `sidechainEnabled`, `recallMode`, `recallToggle`, and `sidechainThreshold` synchronize through processing.
- `outputGain` changes audio amplitude at documented points; bypass is bit-transparent with no hosted plugin.
- C3-B3 maps to snapshot slots 1-12 internally as slot indices 0-11; CC1 routes to fader morphing.
- Sidechain threshold crossing advances snapshot slots on rising edges.
- Processed audio remains finite, preserves silence where expected, remains deterministic in neutral processing, and preserves stereo consistency.
- MCP, semantic safe actions, Ozone IPC fake-memory tests, LLM settings, preset library, modulation, physics, genetic, and audio backend regressions pass in the full suite.
- `morphSource` is registered as an APVTS `AudioParameterChoice` and is tested; the mismatch is the manual's classification as "internal," not absence from APVTS.

## Confirmed Mismatches and Manual Blockers

| Priority | Finding | Evidence | Impact |
|---|---|---|---|
| P1 | Manual lists `morphFader`, but implementation exposes `faderPos`. | `tests/Integration/TestUserManualFeatureSurface.cpp:90` asserts `faderPos` exists and `morphFader` does not. | Host automation users may search for the documented ID and fail. |
| P1 | Manual lists `morphSource` as configuration/automation, but implementation treats it as internal runtime state. | `src/Plugin/PluginProcessor.cpp:788` registers `morphSource` as `AudioParameterChoice`; `tests/Integration/TestUserManualFeatureSurface.cpp:193` requires it to exist. | Manual classification is misleading; the parameter is APVTS-visible. |
| P1 | `smartRandomize` is automatable but has no validated processor trigger path for the documented behavior. | `tests/Integration/TestVST3ParameterAutomation.cpp:185`. | Hosts can automate a control that may not trigger randomization. |
| P1 | Macro knobs are documented as assignable, while current UI maps directly to the first eight hosted parameters. | `src/UI/MacroKnobStrip.cpp` inspection and traceability matrix. | Manual workflow does not match visible behavior. |
| P1 | Presets tab documentation describes 16x128 banked behavior; visible UI is library/search CRUD. | `src/UI/V2PresetBrowserPanel.*` inspection and traceability matrix. | Manual workflow does not match current UI. |
| P1 | `sidechainThreshold` is user-facing in dB, while `MIDIRouter` stores linear RMS after processor conversion. | Focused sidechain tests and traceability matrix. | Internally acceptable, but documentation wording should be precise. |
| P2 | GUI-only visuals such as movement trail, labels, clipboard token copy, hosted plugin editor, real DAW automation lanes, and audible hosted-plugin recall/morph behavior need manual QA. | DAW scan/editor/automation/clipboard/opaque state workflows require real host/plugin setup. | Release sign-off still needs manual DAW QA. |

## Follow-Up

1. Install Steinberg `vst3_validator` in the release environment, reconfigure CMake so the optional validation tests register, and rerun `ctest -L validation` plus direct validator commands against the built VST3. The `pluginval` strictness-5 run already passes and can be used as VST3 conformance evidence.
2. Decide whether to fix product behavior or documentation for `morphFader`/`faderPos`, `morphSource` wording, `smartRandomize`, macro assignment, and preset-bank drift.
3. Run manual DAW validation with at least one instrument, one effect, and one custom-editor VST3 plugin.
4. Re-measure build/configure timings in an actual repository build tree and replace the stale/removed timing figures.
