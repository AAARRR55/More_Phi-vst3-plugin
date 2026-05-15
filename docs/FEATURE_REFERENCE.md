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

All gates pass (Release build, AVX+SSE enabled):

| Metric | Measured | Target |
|--------|----------|--------|
| Scalar Interp (256 params) | 0.11 µs | < 100 µs |
| SIMD Interp (256 params) | 0.125 µs | < 30 µs |
| Elastic Physics | 0.01 µs | < 1 µs |
| Drift Physics | 0.21 µs | < 5 µs |
| 2D IDW (256 params) | 0.525 µs | < 50 µs |
| Memory Footprint | 14.2 KB | < 10 MB |
| **RT CPU (48kHz/256)** | **0.012%** | **< 2%** |

---

## Test Coverage

59 test cases, 962 assertions — all passing.

| Module | Tests | Tags |
|--------|-------|------|
| LockFreeQueue | 5 | `[lockfree]` |
| ParameterState | 5 | `[param]` |
| InterpolationEngine (1D) | 4 | `[interp]` |
| InterpolationEngine (2D) | 3 | `[interp][2d]` |
| SnapshotBank (basic) | 6 | `[bank]` |
| SnapshotBank (FastMode) | 4 | `[bank][fastmode]` |
| PhysicsEngine | 10 | `[physics]` |
| GeneticEngine | 8 | `[genetic]` |
| SanityMode | 3 | `[genetic][sanity]` |
| MIDIRouter Sidechain | 7 | `[midi][sidechain]` |
| SIMD Operations | 4 | `[simd]` |

---

## 4. Listen Mode

**Purpose:** Prevents clicks/pops by excluding discrete parameters (toggles, dropdowns, waveform selectors) from morphing.

### APVTS Parameter
| ID | Type | Default |
|----|------|---------|
| `listenMode` | Bool | `false` |

### How It Works
1. `ParameterBridge::isDiscrete()` classifies params using JUCE's `isDiscrete()` + step count ≤ 32 heuristic
2. `MorphProcessor::applyListenFilter()` marks discrete params with `SKIP_SENTINEL` (-1.0f)
3. `processBlock()` skips sentinel-marked values when applying to hosted plugin

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
