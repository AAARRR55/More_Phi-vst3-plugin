# MorphSnap - Architecture Document

**Date:** 2026-03-04
**Version:** 3.3.0
**Type:** Desktop Audio Plugin (VST3/AU)
**Pattern:** Layered plugin architecture with strict thread-domain separation

## Executive Summary

MorphSnap is a JUCE 8 C++20 audio plugin that hosts other VST3/AU plugins and provides a rich morphing engine to interpolate between captured parameter snapshots. The architecture is organized into 7 clearly separated modules: Plugin (entry), Core (DSP), Host (plugin management), AI (MCP server), MIDI (routing), Preset (persistence), and UI (visual components). The design prioritizes real-time safety through lock-free data structures, strict thread-domain boundaries, and zero-allocation audio paths.

## Technology Stack

| Category | Technology | Version | Justification |
|----------|-----------|---------|---------------|
| Language | C++ | C++20 | Required by JUCE 8; provides constexpr, concepts, ranges |
| Framework | JUCE | 8.0.4 | Industry-standard audio plugin framework; VST3/AU hosting |
| Build | CMake | 3.24+ | Cross-platform builds with FetchContent for dependency management |
| JSON | nlohmann/json | 3.11.3 | Header-only JSON for MCP server protocol |
| Testing | Catch2 | v3.4.0 | Modern C++ test framework with BDD sections |
| CI | GitHub Actions | — | Multi-platform: Windows MSVC, macOS Universal, Linux ASAN/UBSAN |

## Architecture Pattern

MorphSnap follows a **layered plugin architecture** where:

1. **Plugin layer** (entry point) owns all subsystem instances as member variables
2. **Core layer** provides pure computation (audio-thread-safe, no I/O)
3. **Host layer** manages the hosted plugin lifecycle and parameter I/O
4. **AI layer** provides network I/O (MCP server) isolated on its own thread
5. **UI layer** renders state and enqueues commands via the LockFreeQueue

```
┌─────────────────────────────────────────────────┐
│                  Plugin Layer                     │
│         MorphSnapProcessor (owns all)             │
├──────────┬──────────┬───────────┬────────────────┤
│  Core    │  Host    │   AI      │   UI            │
│ (DSP)    │ (VST3)   │  (MCP)    │  (JUCE GUI)     │
│          │          │           │                  │
│ Morph    │ PlugHost │ MCPServer │ MorphPad         │
│ Physics  │ ParamBr. │ ToolHndlr│ SnapFader        │
│ Genetic  │ Scanner  │ TokenOpt │ SnapshotRing     │
│ Spectral │          │ InstReg  │ V2TabBar         │
│ Granular │          │ LinkBcst │ ModMatrixPanel   │
│ ModEng   │          │ Dataset  │ PresetBrowser    │
├──────────┴──────────┴───────────┴────────────────┤
│              MIDI Layer + Preset Layer             │
│         MIDIRouter  │  MetaPresetManager          │
└─────────────────────┴─────────────────────────────┘
```

## Threading Model

Three thread domains with strict boundaries:

### Audio Thread
- **Entry:** `MorphSnapProcessor::processBlock()`
- **Classes:** `MorphProcessor`, `InterpolationEngine`, `PhysicsEngine`, `GeneticEngine`, `SnapshotBank` (read side), `MIDIRouter`, `ModulationEngine`, `SpectralMorphEngine`, `GranularMorphEngine`
- **Contract:** All methods `noexcept`, zero allocations after `prepare()`, no locks, no I/O
- **Data flow:** Drains `LockFreeQueue` → processes MIDI → computes morph → applies to hosted plugin via `ParameterBridge`

### Message Thread
- **Entry:** JUCE message loop
- **Classes:** All UI components, Timer callbacks, deferred plugin loading
- **Contract:** Standard JUCE rules. May write to `SnapshotBank` (serialized via SpinLock), `ModulationMatrix` (double-buffer publish)

### MCP Thread
- **Entry:** `MCPServer::run()` (TCP accept loop)
- **Classes:** `MCPServer`, `MCPToolHandler`, `MCPToolsExtended`
- **Contract:** JSON-RPC I/O. Enqueues parameter changes to audio thread via `LockFreeQueue`. Reads snapshot data via seqlock.

### Concurrency Primitives

| Primitive | Location | Purpose |
|-----------|----------|----------|
| Seqlock | `SnapshotBank` | Lock-free audio reads with retry; UI/MCP writes via SpinLock |
| SPSC LockFreeQueue | `MorphSnapProcessor` | ParamCommand ring buffer (8192), UI/MCP → audio |
| Double-buffer | `ModulationMatrix` | Atomic route publish with mirror-copy |
| `std::atomic<bool>` | `GranularMorphEngine::active_` | Thread-safe enable/disable |
| `std::atomic<int>` | `ModulationMatrix::readIndex_` | Buffer swap index |
| `memory_order_relaxed` atomics | Various | Morph position, physics mode, toggle flags |

## Processing Pipeline

```
                    ┌─ UI Thread ──────────┐     ┌─ MCP Thread ────────┐
                    │  User drags MorphPad  │     │  AI sets parameter   │
                    │  → enqueue ParamCmd   │     │  → enqueue ParamCmd  │
                    └──────────┬───────────┘     └──────────┬──────────┘
                               │                            │
                               ▼                            ▼
                    ┌─────── LockFreeQueue (SPSC, 8192) ──────────┐
                               │
                    ┌──────────▼───────────────────────────────────┐
                    │            Audio Thread                       │
                    │                                               │
                    │  1. Drain LockFreeQueue commands              │
                    │  2. MIDIRouter: note triggers → snapshots    │
                    │     CC1 → fader position                      │
                    │  3. MorphProcessor::process()                 │
                    │     a. PhysicsEngine (elastic/drift/direct)   │
                    │     b. InterpolationEngine (IDW or linear)    │
                    │     c. DiscreteParameterHandler (snap steps)  │
                    │     d. Smoothing filter                       │
                    │  4. ModulationEngine (LFOs, env, sequencer)  │
                    │  5. ParameterBridge → hosted plugin params    │
                    │  6. HostManager.processBlock(buffer, midi)    │
                    └──────────────────────────────────────────────┘
```

## State Persistence

`getStateInformation()` / `setStateInformation()` serialize:

1. **APVTS parameters** — JUCE AudioProcessorValueTreeState XML
2. **Snapshot bank** — base64-encoded float arrays in XML
3. **Hosted plugin description** — `PluginDescription` XML for re-loading
4. **Hosted plugin state** — opaque VST3/AU state chunk (binary blob)
5. **Modulation routes** — XML via `ModulationMatrix::toXml()`

Plugin reload on state restore uses Timer-based deferred loading with retry logic (max 10 attempts) to handle DAW threading constraints.

## Key Subsystem Details

### SnapshotBank (Seqlock Pattern)
- 12 slots, each holding a `ParameterState` (fixed `std::array<float, 2048>`)
- Heap-allocated slot array (~384 KB) to avoid stack overflow
- Audio thread reads via seqlock (retry on torn read)
- UI/MCP writes serialize via SpinLock + sequence counter increment

### Physics Modes
- **Direct:** Raw cursor position → interpolation (no physics)
- **Elastic:** Spring-damper system (`ElasticState` with position + velocity), three presets
- **Drift:** Perlin noise wandering around target with speed/distance/chaos controls

### Genetic Engine
- `breed()` crosses two snapshots with configurable crossover ratio and mutation
- `SanityConfig` protects dangerous parameters (Volume, Pitch, Bypass)
- `smartRandomize()` only mutates user-learned parameters

### Parameter Classification (v3.3.0)
- `ParameterClassifier` categorizes: Continuous, Discrete, Binary, Frequency, Decibel, Enumeration
- `DiscreteParameterHandler` snaps discrete/binary params to valid steps during morphing
- `TokenOptimizer` manages AI token budgets by selecting which parameters to expose

### ModulationMatrix (Double-Buffer)
- Two `RouteBuffer` instances (up to 64 routes each)
- Message thread mutates write buffer, then publishes via `readIndex_.store(release)`
- Audio thread reads from `readIndex_.load(acquire)` — never writes
- After publish, write buffer is mirrored from the just-published state

### V2 Audio Engines
- **SpectralMorphEngine:** FFT-based overlap-add vocoder with magnitude/phase interpolation
- **GranularMorphEngine:** Grain pool with position/size/density randomization
- **FormantMorphEngine:** Formant-preserving spectral envelope morphing
- **VAEMorphEngine:** Variational autoencoder latent-space morphing
- **HybridBlend:** Coordinates multiple engines with configurable blend weights

## Testing Strategy

- **Unit tests** (Catch2): Core engines, physics, genetics, sidechain, SIMD, spectral, granular, modulation
- **Integration tests**: Plugin lifecycle (load/unload/state), MCP server tool invocations
- **Performance benchmarks**: CPU usage, audio processing throughput
- **DAW test matrix**: Manual test docs for Ableton, FL Studio, Logic, Reaper
- **Automated scripts**: Audio quality, real-time safety, VST3 validator (pluginval strictness 5)
- **Sanitizers**: ASAN + UBSAN via Clang on Linux CI

Tests compile with `MORPHSNAP_TEST_MODE=1` and `JUCE_STANDALONE_APPLICATION=0`.

## Deployment Architecture

- **CI/CD:** GitHub Actions with 3 jobs:
  1. `build-windows`: MSVC 2022 x64 → VST3 + pluginval + tests
  2. `build-macos`: Universal Binary (x64+arm64) → VST3 + AU + pluginval + tests
  3. `sanitizer-tests`: Linux Clang 17 with ASAN/UBSAN
- **Release:** Automatic GitHub Release on `v*` tags with Windows and macOS zip artifacts
- **Plugin locations:**
  - Windows: `C:\Program Files\Common Files\VST3\`
  - macOS: `/Library/Audio/Plug-Ins/VST3/` (VST3), `/Library/Audio/Plug-Ins/Components/` (AU)

## Platform Notes

- Windows: `/STACK:4194304` (4 MB) for FL Studio plugin-in-plugin hosting
- `cmake/PatchJuceForMSVC.cmake` patches JUCE headers conflicting with Windows macros
- AU format only on macOS; Windows builds VST3 only
- macOS deployment target: 11.0 (Big Sur)
- macOS builds Universal Binary (x86_64 + arm64)

---

_Generated using BMAD Method `document-project` workflow_
