# More-Phi User Manual and VST3 E2E Validation Report

Generated: `2026-05-17`

## Verdict

The manual/VST3 validation pass is complete for the deterministic local scope. The refreshed focused suite passes `24/24`, the full Release Ninja suite passes `375/375`, and the VST3 artifact builds successfully.

External VST3 standards validation remains blocked locally because neither `vst3_validator` nor `pluginval` is installed on `PATH` or in common Windows install locations, so CTest registers no `validation`-label tests in this environment. The repo validator wrapper now records that blocker as JSON/JUnit evidence, and CTest will register validator tests automatically when those external tools are installed. Host-only workflows remain manual blockers: DAW discovery, hosted editor display, real host automation lanes, clipboard token copy, audible hosted-plugin morph/recall, and opaque third-party plugin state chunks.

The validation pass intentionally reports product/documentation drift rather than fixing product behavior or manual wording.

## Coverage Changes

| Area | Evidence | Result |
|---|---|---|
| Manual feature surface | `tests/Integration/TestUserManualFeatureSurface.cpp:28`, `:59`, `:90`, `:99`, `:125`, `:150` | APVTS controls, guide workflow controls, documented automation ID drift, Engine-tab APVTS controls, five editor tabs, headless editor construction/resize. |
| VST3 parameter automation | `tests/Integration/TestVST3ParameterAutomation.cpp:60`, `:112`, `:126`, `:151`, `:168`, `:185` | Host-visible documented parameters, automatable flags, gesture updates, APVTS-to-runtime sync, output gain, bypass, smart-randomize documented mismatch. |
| VST3 MIDI/sidechain | `tests/Integration/TestVST3MidiAndSidechain.cpp:64`, `:85`, `:114`, `:131`, `:148`, `:170` | C3-B3 slots, trigger note filtering, CC1 fader route, sidechain bus/state, rising-edge sidechain cycling. |
| VST3 audio accuracy | `tests/Integration/TestVST3AudioSignalAccuracy.cpp:80`, `:93`, `:109`, `:142`, `:162`, `:182` | Finite output, silence preservation, output gain accuracy, bypass bit transparency, deterministic neutral processing, stereo consistency. |
| Test stability | `tests/Unit/TestLLMSettingsStore.cpp:12`, `tests/Unit/TestStandaloneMcpServer.cpp:950` | LLM settings temp directories are unique per test process; fake IPC ring-wrap test uses a deterministic headless timeout. |
| Validator automation | `tests/CMakeLists.txt:184`, `:190`, `:204`; `tests/scripts/run_vst3_validator.py:38`, `:208`, `:244` | CTest uses the repo wrapper and JUCE VST3 bundle path when `vst3_validator` is installed; pluginval strictness 5 is registered when `pluginval` is installed; missing-validator runs emit ASCII-safe console output and machine-readable JSON/JUnit error artifacts on Windows. |

## Validation Commands and Results

### Primary build tree attempts

```powershell
cmake -B build -S . -DMORE_PHI_BUILD_TESTS=ON
```

Result: timed out after `304.1s`. Stale `cmake`/`MSBuild`/`cl` processes were stopped.

```powershell
cmake --build build --config Release --target MorePhiTests --parallel 2
```

Result: timed out after `2445.8s`. The Visual Studio generator path left idle build processes and was not used for final validation.

### Successful Release Ninja build path

```powershell
cmd /c "`"C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat`" -arch=x64 -host_arch=x64 && cmake --build build\codex-ninja-vs18d --target MorePhiTests --parallel 2"
```

Result: passed in `65.2s`.

```powershell
cmd /c "`"C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat`" -arch=x64 -host_arch=x64 && cmake --build build\codex-ninja-vs18d --target MorePhiMcpServerTests --parallel 2"
```

Result: passed in `23.8s`.

```powershell
cmd /c "`"C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat`" -arch=x64 -host_arch=x64 && cmake -S . -B build\codex-ninja-vs18d -DMORE_PHI_BUILD_TESTS=ON"
```

Result after validator-harness changes: passed in `7.4s`.

### Focused manual, guide, and VST3 suite

```powershell
ctest --test-dir build\codex-ninja-vs18d -R "User manual feature surface|User guide feature surface|VST3 parameter automation|VST3 smart randomize|VST3 MIDI handling|VST3 sidechain|VST3 audio signal accuracy" --output-on-failure
```

Result: `24/24` passed.

```text
100% tests passed, 0 tests failed out of 24
Total Test time (real) = 6.54 sec
```

### Full regression suite

```powershell
ctest --test-dir build\codex-ninja-vs18d --output-on-failure --parallel 4
```

Result: `375/375` passed after fixing the test-owned IPC timing flake.

```text
100% tests passed, 0 tests failed out of 375
Total Test time (real) = 38.22 sec
```

The prior flaky failure was isolated as:

```powershell
ctest --test-dir build\codex-ninja-vs18d -R "Standalone MCP Ozone IPC assistant handles ring wrap-around write" --output-on-failure --parallel 1
```

Result after timeout adjustment: `1/1` passed.

### VST3 artifact and validator status

```powershell
cmd /c "`"C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat`" -arch=x64 -host_arch=x64 && cmake --build build\codex-ninja-vs18d --target MorePhi_VST3 --parallel 2"
```

Result: passed. Artifact:

```text
G:/morphy/build/codex-ninja-vs18d/MorePhi_artefacts/Release/VST3/MorePhi.vst3/Contents/x86_64-win/MorePhi.vst3
```

```powershell
ctest --test-dir build\codex-ninja-vs18d -N -L validation
ctest --test-dir build\codex-ninja-vs18d --build-config Release -L validation --output-on-failure
Get-Command vst3_validator -ErrorAction SilentlyContinue
Get-Command pluginval -ErrorAction SilentlyContinue
where.exe vst3_validator
where.exe pluginval
python tests\scripts\run_vst3_validator.py --plugin "G:\morphy\build\codex-ninja-vs18d\MorePhi_artefacts\Release\VST3\MorePhi.vst3" --output docs\validation\vst3_validator_result_20260517.json --junit docs\validation\vst3_validator_junit_20260517.xml
```

Results:

- `ctest -N -L validation`: `Total Tests: 0` because no external validator tool is installed
- `ctest -L validation`: no tests found
- `vst3_validator`: unavailable on `PATH`
- `pluginval`: unavailable on `PATH`
- Common-location search under `Program Files`, `Program Files (x86)`, and the user `AppData` trees found no `vst3_validator.exe` or `pluginval.exe`
- Validator wrapper result: blocked, `{"status": "error", "error": "VST3 validator not found"}`
- Validator wrapper artifacts: `docs/validation/vst3_validator_result_20260517.json`, `docs/validation/vst3_validator_junit_20260517.xml`

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

## Confirmed Mismatches and Manual Blockers

| Priority | Finding | Evidence | Impact |
|---|---|---|---|
| P1 | Manual lists `morphFader`, but implementation exposes `faderPos`. | `tests/Integration/TestUserManualFeatureSurface.cpp:90` asserts `faderPos` exists and `morphFader` does not. | Host automation users may search for the documented ID and fail. |
| P1 | Manual lists `morphSource` as configuration/automation, but implementation treats it as internal runtime state. | `tests/Integration/TestUserManualFeatureSurface.cpp:90` asserts no APVTS `morphSource`. | Manual overstates DAW-visible automation. |
| P1 | `smartRandomize` is automatable but has no validated processor trigger path for the documented behavior. | `tests/Integration/TestVST3ParameterAutomation.cpp:185`. | Hosts can automate a control that may not trigger randomization. |
| P1 | Macro knobs are documented as assignable, while current UI maps directly to the first eight hosted parameters. | `src/UI/MacroKnobStrip.cpp` inspection and traceability matrix. | Manual workflow does not match visible behavior. |
| P1 | Presets tab documentation describes 16x128 banked behavior; visible UI is library/search CRUD. | `src/UI/V2PresetBrowserPanel.*` inspection and traceability matrix. | Manual workflow does not match current UI. |
| P2 | `sidechainThreshold` is user-facing in dB, while `MIDIRouter` stores linear RMS after processor conversion. | Focused sidechain tests and traceability matrix. | Internally acceptable, but documentation wording should be precise. |
| P2 | Host-only visual/audio workflows cannot be fully proven headlessly. | DAW scan/editor/automation/clipboard/opaque state workflows require real host/plugin setup. | Release sign-off still needs manual DAW QA. |

## Follow-Up

1. Install Steinberg `vst3_validator` or `pluginval` in the release environment, reconfigure CMake so the optional validation tests register, and rerun `ctest -L validation` plus direct validator commands against the built VST3.
2. Decide whether to fix product behavior or documentation for `morphFader`/`faderPos`, `morphSource`, `smartRandomize`, macro assignment, and preset-bank drift.
3. Run manual DAW validation with at least one instrument, one effect, and one custom-editor VST3 plugin.
4. Prefer the `build/codex-ninja-vs18d` + `VsDevCmd.bat` path for local validation until the default Visual Studio generator timeout is diagnosed.
