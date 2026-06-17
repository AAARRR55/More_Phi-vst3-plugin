# More-Phi — New Features Reference

## Overview

This document covers features added during the Phase 4–5 hardening sprint.  
All features are wired into `MorePhiProcessor` via APVTS and exposed to DAW automation.

---

## 1. SanityMode

**Purpose:** Protects critical parameters (e.g., output gain, routing) from being randomized by `breed()` or `smartRandomize()`.

### APVTS Parameter
| ID | Type | Default | Range |
|----|------|---------|-------|
| `sanityEnabled` | Bool | `false` | — |

### API (`GeneticEngine.h`)
```cpp
struct SanityConfig {
    bool enabled = false;
    std::set<int> protectedIndices;  // param indices to skip
};
```

### State Persistence
Serialized as `<SANITY_CONFIG enabled="true"><PROTECTED index="0"/>...</SANITY_CONFIG>` in the plugin's XML state.

---

## 2. FastMode (RecallMode)

**Purpose:** Controls whether snapshot recall restores only normalized floats (`Fast`) or also opaque VST3 state chunks (`Full`).

### APVTS Parameter
| ID | Type | Default | Options |
|----|------|---------|---------|
| `recallMode` | Choice | `Fast` | Fast, Full |

### API (`SnapshotBank.h`)
```cpp
enum class RecallMode { Fast = 0, Full = 1 };

void captureStateChunk(int slot, juce::AudioPluginInstance* plugin);
void recallStateChunk(int slot, juce::AudioPluginInstance* plugin) const;
```

- **Fast** — recalls only the `float[]` parameter snapshot through the audio-thread-safe parameter queue
- **Full** — also restores the binary state chunk from the hosted plugin (Kontakt, Serum, etc.) on the message-thread maintenance path

### State Persistence
Serialized as `recallMode="0"` attribute on the root XML element.

---

## 3. Sidechain Trigger

**Purpose:** Audio-driven snapshot recall — detects transients on the sidechain input bus and cycles through snapshots.

### APVTS Parameters
| ID | Type | Default | Range |
|----|------|---------|-------|
| `sidechainEnabled` | Bool | `false` | — |
| `sidechainThreshold` | Float | `-20.0 dB` | `-60.0` to `0.0 dB`, step `0.5` |

### API (`MIDIRouter.h`)
```cpp
void setSidechainEnabled(bool enabled);
void setSidechainThreshold(float threshold);
void processSidechain(const juce::AudioBuffer<float>& sidechain);
```

### Behavior
1. Calculates RMS of the sidechain buffer
2. Rising-edge detection: triggers only when RMS crosses threshold from below
3. Round-robin: cycles slots 0→1→...→11→0
4. No re-trigger while signal stays above threshold

### Bus Layout
`isBusesLayoutSupported()` now accepts sidechain bus in disabled, mono, or stereo configurations.

---

## Performance Benchmarks

Local microbenchmark gates pass in the Release benchmark executable (`build/windows-msvc-release/tests/Release/MorePhiBenchmarks.exe`, AVX+SSE available). Values below are from a single local run; they are **not** a DAW-host CPU or memory profile. Benchmarks include warmup iterations and report percentiles (p50/p95/p99) instead of raw min/max.

> Granular and Spectral engine tests now exercise real production code, not mocks.

| Metric | Scope | Measured Value | Target |
|--------|-------|----------------|--------|
| Scalar Interp (256 params) | Core math | 0.058 µs avg | < 100 µs |
| SIMD Interp (256 params) | Core math | 0.075 µs avg | < 30 µs |
| Elastic Physics | Core math | 0.059 µs avg | < 1 µs |
| Drift Physics | Core math | 0.123 µs avg | < 5 µs |
| 2D IDW (256 params) | Core math | 0.254 µs avg | < 50 µs |
| Neural mastering controller | Core math | 1.521 µs avg | < 100 µs avg / < 1000 µs p99 |
| Memory Footprint | sizeof estimate | See note below | < 10 MB |
| **Core Math RT Load (48kHz/256)** | PhysicsEngine + InterpolationEngine only | **0.00699% simulated** | **< 2%** |
| **Core Math RT Load (44.1kHz/64)** | PhysicsEngine + InterpolationEngine only | **0.0207% simulated** | **< 2%** |

**Memory Footprint Note**: The memory estimate sums `sizeof(SnapshotBank)` inline members plus heap-allocated slot data (12 slots × 2048-float `ParameterState`) plus morph/smoothing buffers. It excludes JUCE overhead, hosted plugin memory, MCP server, modulation matrix, and audio-domain/mastering engine allocations. Runtime heap profiling is a separate release gate.

**Core Math RT Load Note**: These percentages measure only `PhysicsEngine::updateElastic()` + `InterpolationEngine::compute2D()` per audio buffer. The full `processBlock()` pipeline includes MIDI routing, snapshot bank, morph processor, modulation, audio-domain engines, mastering chain, and hosted plugin processing. Full-plugin CPU load requires DAW-hosted profiling.

External DAW profiling, `pluginval`, and Steinberg `vst3_validator` remain separate release gates.

---

## Test Coverage

Current `build/windows-msvc-release` CTest discovery lists 459+ tests. All tests pass successfully.

### Test Matrix Summary
| Scope | Result |
|-------|--------|
| Full current Release CTest suite | 459/459 passed |
| Latency, metering, spectrum, stereo field, LUFS, true peak, and analysis metadata | 40/40 passed |
| Dataset-filtered integration/schema tests | 8/8 passed |
| Release benchmark executable gates | 9/9 passed |

### Core DSP and Thread-Safety Regression Tests (v3.3.0)
The hardening sprint added three key regression test categories to verify DSP correctness and audio thread safety under the `[production]` tag:

1. **Discrete Parameter Step Snapping (`[discrete][production]`):**
   - **Target:** `DiscreteParameterHandler::valueToStep`
   - **Assertion:** Asserts that continuous morph positions round correctly to the nearest discrete step. For instance, `0.95f` with 10 steps maps to step 9 (reconstructing to `1.0f`), rather than step 8 (reconstructing to `0.888f`) under the previous truncating static cast.

2. **Real-time Safe Envelope Follower (`[modulation][envelope][production]`):**
   - **Target:** `EnvelopeFollower`
   - **Assertion:** Verifies that time-constant coefficients are pre-calculated as logs on the setting/message thread. Asserts that the audio thread does not call `std::pow()`, using pre-computed block constants when block sizes match, or falling back to `std::exp()` exponentiation when block sizes change.

3. **Vocoder Channel Synchronization (`[spectral][production]`):**
   - **Target:** `SpectralMorphEngine`
   - **Assertion:** Verifies that transient detection is channel-coherent. Validates that modified morph alphas calculated on channel 0 are successfully cached and shared with channel 1, preventing stereo field collapse or asymmetric channel offsets.

Comprehensive E2E test is now enabled and compiles with the current API.

Full-suite evidence: `validation/ctest_full_windows-msvc-release_20260615.md`.

External VST3-validator and DAW-host results should be attached as separate release artifacts when available.

---

## 4. Listen Mode

**Purpose:** Prevents clicks/pops by excluding discrete parameters (toggles, dropdowns, waveform selectors) from morphing.

### APVTS Parameter
| ID | Type | Default |
|----|------|---------|
| `listenMode` | Bool | `false` |

### How It Works
1. `ParameterBridge::isDiscrete()` now uses the hosted plugin's actual step count (not hardcoded `stepCount = 1`).
2. `DiscreteParameterHandler` is now wired into the audio path and snaps discrete values to valid steps in real time.
3. Listen Mode is fully effective for all discrete parameter types.

---

## 5. Recall Toggle

**Purpose:** Sustain synthesizer notes during MIDI-triggered snapshot switches by applying only float params.

### APVTS Parameter
| ID | Type | Default |
|----|------|---------|
| `recallToggle` | Bool | `true` |

### Behavior
- **On (default):** Full recall with state chunks; MIDI-triggered state chunks are scheduled off the audio thread
- **Off:** Calls `recallFast()` — params-only, notes sustain

---

## 5.1 Compatibility and Timing Boundaries

- Internal morph, MIDI-triggered snapshot recall, and MCP morph-position updates are block-accurate.
- Discrete parameter snapping is now block-accurate and occurs after modulation but before the parameter bridge.
- Hosted plugin parameter and audio processing remain serialized through the audio callback; opaque hosted state capture/recall uses exclusive non-audio access, and audio skips hosted processing while that access is pending.
- Mono and stereo main layouts are supported. Surround layouts are rejected/unsupported in this batch.
- The audio-domain morph layer reports latency only for enabled processors. Disabled spectral/granular/oversampling paths do not add PDC.
- External DAW matrix testing, `pluginval`, and Steinberg `vst3_validator` remain release gates to run when those tools or hosts are installed.

---

## 6. Link Mode (Cross-Instance Sync)

**Purpose:** Synchronize morph position across multiple More-Phi instances in the same DAW session.

### APVTS Parameter
| ID | Type | Default |
|----|------|---------|
| `linkMode` | Bool | `false` |

### Architecture
- `LinkBroadcaster` uses platform-native shared memory (Windows: `CreateFileMappingW`, macOS/Linux: `shm_open`)
- Seqlock protocol ensures real-time safe reads (no mutexes in audio thread)
- Leader broadcasts morph X/Y; followers receive and override their morph position

---

## 7. Drift Recording

**Purpose:** Record drift/orbit morph movement as DAW automation.

### APVTS Parameters
| ID | Type | Default | Range |
|----|------|---------|-------|
| `driftOutputX` | Float | `0.5` | `0.0` to `1.0` |
| `driftOutputY` | Float | `0.5` | `0.0` to `1.0` |

Written from `processBlock()` after morph computation. DAWs can record these as automation lanes.

---

## 8. Smart Randomize (DAW Trigger)

**Purpose:** Trigger preset randomization from DAW automation.

### APVTS Parameter
| ID | Type | Default |
|----|------|---------|
| `smartRandomize` | Bool | `false` |

---

## 9. Meta Preset Manager

**Purpose:** Bank/preset navigation with JSON serialization.

### API (`MetaPresetManager.h`)
```cpp
void switchBank(int bank);      // 0–15
void switchPreset(int preset);  // 0–127
void switchToNext();            // Wraps across banks
void switchToPrev();
String getPresetName(int bank, int preset) const;
```

### Serialization (`PresetSerializer`)
Full JSON roundtrip: snapshot bank (12 slots) + APVTS state + version tag.


_Updated 2026-06-18._
