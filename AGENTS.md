# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## Project Overview

More-Phi is a JUCE 8-based VST3/AU audio plugin (C++20) that hosts other plugins and morphs between parameter snapshots using physics-based interpolation, genetic breeding, and AI integration via an embedded MCP server. Version 3.3.0.

## Build Commands

```bash
# Configure (first time or after CMakeLists.txt changes)
cmake -B build -S . -DMORE_PHI_BUILD_TESTS=ON

# Build Release
cmake --build build --config Release

# Build Debug
cmake --build build --config Debug

# Run all tests
cd build && ctest --build-config Release --output-on-failure --parallel 4

# Run a single test by name (Catch2 test names)
cd build && ctest -R "TestName" --output-on-failure

# Run tests with sanitizers (Clang/GCC only)
cmake -B build -S . -DMORE_PHI_BUILD_TESTS=ON -DMORE_PHI_ENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
cd build && ctest --build-config Debug --output-on-failure
```

CMake options: `MORE_PHI_BUILD_TESTS` (ON/OFF), `MORE_PHI_COPY_PLUGIN_AFTER_BUILD` (OFF by default), `MORE_PHI_ENABLE_SANITIZERS` (ASAN+UBSAN, Clang/GCC only).

Dependencies (all fetched automatically via FetchContent): JUCE 8.0.4, nlohmann/json 3.11.3, Catch2 v3.4.0.

## Architecture

Everything is in the `more_phi` namespace. The plugin entry point is `MorePhiProcessor` (inherits `juce::AudioProcessor`), which owns all subsystems as member variables (no singletons except `InstanceRegistry`).

### Processing Pipeline (audio thread)

```
processBlock() → drain LockFreeQueue commands → MIDIRouter → MorphProcessor → ParameterBridge → hosted plugin
```

1. **LockFreeQueue** (`ParamCommand` ring buffer, 8192 capacity) — UI/MCP threads enqueue parameter changes, audio thread drains them
2. **MIDIRouter** — Notes C3-B3 trigger snapshot recall, CC1 drives fader position
3. **MorphProcessor** — Orchestrates: physics engine → interpolation engine → smoothing → output vector
4. **ParameterBridge** — Applies normalized float vector to hosted plugin's parameters

### Layer Responsibilities

| Layer | Key Classes | Role |
|-------|------------|------|
| `src/Plugin/` | `MorePhiProcessor`, `MorePhiEditor` | JUCE plugin entry points, owns all subsystems |
| `src/Core/` | `MorphProcessor`, `InterpolationEngine`, `PhysicsEngine`, `GeneticEngine`, `SnapshotBank` | Morph computation, all audio-thread-safe |
| `src/Host/` | `PluginHostManager`, `ParameterBridge`, `PluginScanner` | VST3/AU hosting, parameter read/write |
| `src/AI/` | `MCPServer`, `MCPToolHandler`, `TokenOptimizer`, `InstanceRegistry` | JSON-RPC 2.0 server on localhost:30001 |
| `src/MIDI/` | `MIDIRouter` | Note triggers + CC routing |
| `src/Preset/` | `MetaPresetManager`, `PresetSerializer` | Meta-preset save/load |
| `src/UI/` | `MorphPad`, `SnapFader`, `SnapshotRing`, etc. | All UI components |

### Threading Model

Three thread domains with strict boundaries:

- **Audio thread**: `processBlock()`, `MorphProcessor::process()`, `InterpolationEngine`, `PhysicsEngine` — all `noexcept`, zero allocations after `prepare()`, no locks
- **Message thread**: UI components, Timer callbacks (deferred plugin loading), MCP connection handling
- **MCP thread**: `MCPServer::run()` — accepts JSON-RPC connections, enqueues parameter changes via `LockFreeQueue`

### Key Concurrency Primitives

- **Seqlock** in `SnapshotBank` — Audio thread reads snapshot data lock-free with retry; UI/MCP writes serialize via `SpinLock` + sequence counter
- **SPSC LockFreeQueue** — Power-of-2 ring buffer with cache-line-aligned indices; used for `ParamCommand` from UI/MCP → audio thread
- **Atomics** (`memory_order_relaxed`) — All morph position, physics mode, and toggle state transferred between UI and audio threads
- **Touch detection** — Prevents morph from overwriting manual knob changes using per-parameter cooldown counters

### Interfaces for Testability

`IPluginHostManager`, `IParameterBridge`, `IMCPServer` (all in `Host/IPluginHostManager.h`) — abstract interfaces that enable mock injection in tests.

### State Persistence

`getStateInformation`/`setStateInformation` serialize: APVTS parameters + snapshot bank (base64-encoded floats in XML) + hosted plugin description + opaque VST3 state chunk. Plugin reload on state restore uses Timer-based deferred loading with retry logic (max 10 attempts) to handle DAW threading constraints.

### Parameter Classification (v3.3.0)

`ParameterClassifier` categorizes hosted plugin parameters (Continuous, Discrete, Binary, Frequency, Decibel, Enumeration) for Learn Mode. `DiscreteParameterHandler` ensures discrete/binary parameters snap to valid steps during morphing rather than interpolating through invalid intermediate values. `TokenOptimizer` manages AI token budgets by selecting which parameters to expose.

### Dataset Generation (V2/V3)

More-Phi includes a comprehensive dataset generation system for creating synthetic audio training data. **Dataset V3 is always compiled** (`MORE_PHI_ENABLE_DATASET_V3` is retained as a deprecated compatibility flag/no-op).

**V2 Components (Sequential Pipeline):**
- `DatasetGeneratorV2` — Main orchestrator integrating all modules
- `ParameterSampler` — Latin Hypercube Sampling, stratified sampling
- `AudioContentLibrary` — Source audio management with genre classification
- `PluginChainEngine` — Sequential multi-plugin chains (EQ, Dynamics, Mastering)
- `EnhancedRenderPipeline` — Multi-segment rendering (Full/Transient/SteadyState)
- `FeatureExtractor` — MFCC, LUFS, spectral, temporal, perceptual features
- `MetadataWriter` — JSON/CSV/Parquet export
- `ValidationEngine` — KS test, MMD, coverage metrics
- `DatasetOrganizer` — Train/Val/Test splits, directory management
- `DatasetConfig` — CLI interface, JSON schema validation

**V3 Components (Optional Modular Pipeline):**
- `DatasetGeneratorV3` — High-performance async pipeline orchestrator
- `TaskQueue` — MPMC priority queue with backpressure
- `WorkerPool` — Parallel batch processing threads
- `ResourceMonitor` — Adaptive CPU/RAM throttling
- `ProgressTracker` — Real-time progress & ETA
- `CheckpointManager` — Crash recovery
- `WatchdogTimer` — Hung thread detection
- `GenerationLogger` — Structured JSON logging

### Genetic Engine

`GeneticEngine::breed()` crosses two snapshots with configurable crossover ratio and mutation strength. `SanityConfig` protects dangerous parameters (Volume, Pitch, Bypass) from modification during breed/randomize. `smartRandomize()` only mutates user-learned parameters.

### Physics Modes

- **Direct**: Raw cursor position → interpolation (no physics)
- **Elastic**: Spring-damper system (`ElasticState` with position + velocity), three presets (Slow/Medium/Heavy)
- **Drift**: Perlin noise wandering around target with speed/distance/chaos controls, three modes (Free/Locked/Orbit)

## Tests

Tests use Catch2 v3 and link against the `More-Phi` shared-code target. Test sources:

- `tests/Unit/` — Core engine unit tests (interpolation, physics, genetics, sidechain)
- `tests/Integration/` — Plugin lifecycle and MCP integration
- `tests/Performance/` — Benchmark suite (opt-in via `MORE_PHI_BUILD_BENCHMARKS`)

Tests compile with `MORE_PHI_TEST_MODE=1` and `JUCE_STANDALONE_APPLICATION=0`.

## Platform Notes

- Windows builds set `/STACK:4194304` (4 MB) for FL Studio compatibility with plugin-in-plugin hosting
- `cmake/PatchJuceForMSVC.cmake` patches JUCE headers that conflict with Windows macros
- AU format only built on macOS; Windows builds VST3 only
- `ParameterState` uses fixed `std::array<float, 2048>` (no heap allocation) for real-time safety
- `SnapshotBank` heap-allocates its 12-slot array (~384 KB) to avoid stack overflow in hosts with small thread stacks

<!-- SPECKIT START -->
For additional context about technologies to be used, project structure,
shell commands, and other important information, read
`specs/004-dataset-curation/plan.md`.
<!-- SPECKIT END -->
