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

**C-3 fix (2026-07-15):** The command drain is gated by `commandConsumerLock_` try-lock (`canDrainCommands`). Morph-to-parameter application always proceeds independently — it has its own `touchStateLock_` try-lock. This prevents parameter-write dropouts when the assistant flush path holds `commandConsumerLock_`. `liveEditHold_` reads are now inside the `hasTouchLock` guard.

**C-1 fix (2025-07-21):** `reconfigureAudioDomainProcessing()` uses a 100ms hard-timeout spin-wait for `audioDomainUsers_` to reach zero. On expiry it re-dirties the config flag and returns, retrying on the next timer tick — this prevents message-thread stalls when the audio thread is mid-processing with large FFT sizes.

**W-5 fix (2025-07-21):** `prepareToPlay` stores `prepared` with `memory_order_release`; `processBlock` loads with `memory_order_acquire`. This explicit pairing avoids the implicit `seq_cst` overhead of the old `prepared = false` / `if (!prepared)` pattern. Same convention applies to `releaseResources`.

### Layer Responsibilities

| Layer | Key Classes | Role |
|-------|------------|------|
| `src/Plugin/` | `MorePhiProcessor`, `MorePhiEditor` | JUCE plugin entry points, owns all subsystems |
| `src/Core/` | `MorphProcessor`, `InterpolationEngine`, `PhysicsEngine`, `GeneticEngine`, `SnapshotBank` | Morph computation, all audio-thread-safe |
| `src/Host/` | `PluginHostManager`, `ParameterBridge`, `PluginScanner` | VST3/AU hosting, parameter read/write |
| `src/AI/` | `MCPServer`, `MCPToolHandler`, `TokenOptimizer`, `InstanceRegistry` | JSON-RPC 2.0 server on localhost:30001 |
| `src/AI/` | `SonicMasterAnalysisEngine`, `SonicMasterDecisionRunner`, `SonicMasterDecisionDecoder`, `SonicMasterHttpInferenceSource` | Realtime neural mastering (preview, default OFF). The engine analyses a ~6s window on a 3s cycle on a background thread and feeds the built-in `AutoMasteringEngine` via `applyValidatedPlan`. The primary inference path is the in-process ONNX runner (`SonicMasterDecisionRunner`, loads `masteringbrain_v2_decision.onnx`); the HTTP source (`tools/inference_server/`) is the fallback. See `docs/superpowers/specs/2026-06-21-sonicmaster-vst3-realtime-integration-design.md`. |
| `src/MIDI/` | `MIDIRouter` | Note triggers + CC routing |
| `src/Preset/` | `MetaPresetManager`, `PresetSerializer` | Meta-preset save/load |
| `src/UI/` | `MorphPad`, `SnapFader`, `SnapshotRing`, etc. | All UI components |

### Threading Model

Three thread domains with strict boundaries, plus agent-owned workers:

- **Audio thread**: `processBlock()`, `MorphProcessor::process()`, `InterpolationEngine`, `PhysicsEngine` — all `noexcept`, zero allocations after `prepare()`, no locks
- **Message thread**: UI components, Timer callbacks (deferred plugin loading), MCP connection handling
- **MCP thread**: `MCPServer::run()` — accepts JSON-RPC connections, enqueues parameter changes via `LockFreeQueue`
- **Agent scheduler workers (2 threads)**: `PriorityScheduler::workerLoop()` — executes agent tasks (sync, on workers only). Uses 4-level multi-queue with O(1) operations. Starvation guard 1000ms
- **Blackboard pump thread**: Polls `IntegrationEventBus` every 50ms, fans out to agent subscribers

### Multi-Agent Orchestration Layer

An additive agent layer sits ABOVE the existing `MCPToolHandler` / `AutomationControlPlane`, reusing their permission/ledger/event subsystems rather than duplicating them. Design spec: `docs/superpowers/specs/2026-06-21-multi-agent-orchestration-layer-design.md`. Default config: `config/agents/agent_runtime.default.json`.

| Layer | Key Classes | Role |
|-------|------------|------|
| `src/AI/Agents/` | `AgentRuntime`, `AgentRegistry`, `PriorityScheduler` | Container: registers agents, owns 2-worker pool, fans out blackboard events |
| `src/AI/Agents/Conductor/` | `ConductorAgent` | Goal decomposition via `WorkflowOrchestrator` + `DeterministicFallbackLlmClient`; the ONLY agent that may delegate followUps. **Production LLM transport:** the in-process runner is the `DeterministicFallbackLlmClient` (a fixed Analysis→Memory→Optimization heuristic with warm/bright/sparkle keyword flags), NOT a live LLM. `decomposeGoal` always falls through to it. The `ILlmClient` seam exists for a future real adapter (`LLMChatClient` does not yet implement `ILlmClient`); treat the heuristic as the shipping behavior until that adapter lands. |
| `src/AI/Agents/Agents/` | `AnalysisAgent`, `OptimizationAgent`, `CreativeAgent`, `RealtimeControlAgent`, `QualitySafetyAgent`, `MemoryAgent` | Six specialists; cross-delegation is dropped with `agents.delegation_rejected` |
| `src/AI/Agents/Blackboard/` | `BlackboardBridge` | Typed pub/sub OVER `IntegrationEventBus`; a pump thread drains + fans out |
| `src/AI/Agents/Tooling/` | `DefaultToolInvoker` | Wraps `MCPToolHandler::handle`; enforces per-agent capability scope (fail-closed) + rate budget + attribution |
| `src/AI/Agents/Logging/` | `NullAgentLogger`, `StructuredAgentLogger` | JSONL file logger with in-memory ring fallback |
| `src/AI/Agents/Llm/` | `ILlmClient`, `DeterministicFallbackLlmClient`, `NullLlmClient` | LLM seam (Risk R1). **The deterministic fallback IS the production client** — no real transport is wired into the agent layer yet. `LLMChatClient` (used by `UI/AIChatPanel`) does not implement `ILlmClient`. The "broken transport" framing is legacy; ship the heuristic, add the adapter only when a planning benchmark demands it. |

**Threading invariant (strict):** agents execute on scheduler workers ONLY — never on the audio thread. `RealtimeCritical` priority jumps the *agent* queue, not the audio thread. `RealtimeControlAgent` writes corrections through `LockFreeQueue` / `MCPToolHandler::handle`, exactly like the UI/MCP paths.

**MCP surface (7 tools):** `agents.list`, `agents.run_goal`, `agents.run_task`, `agents.run_status`, `agents.run_cancel`, `agents.blackboard.recent`, `agents.set_autonomy` — dispatched in `MCPToolHandler::handle`; classified in `PermissionKernel::classifyTool`.

**Lifecycle:** `MorePhiProcessor::startMCPServerIfNeeded()` lazily constructs `agentRuntime_` (after the MCP server's `AutomationRuntime` exists), registers the full cast, starts the runtime. Destructor resets `agentRuntime_` BEFORE stopping the MCP server so workers join before the runtime they reference is torn down.

### Key Concurrency Primitives

- **Seqlock** in `SnapshotBank` — Audio thread reads snapshot data lock-free with retry; UI/MCP writes serialize via `SpinLock` + sequence counter. `findParameterIndex()` uses `ScopedTryLockType` to avoid blocking the audio thread.
- **SPSC LockFreeQueue** — Power-of-2 ring buffer with cache-line-aligned indices; multi-producer safe via internal `SpinLock` on push. Used for `ParamCommand` from UI/MCP → audio thread.
- **Multi-queue PriorityScheduler** — 4 per-priority-level `std::queue` for agent task scheduling with O(1) push/pop/starvation-promotion (H-1/M-4 fix, 2026-07-15). Starvation guard: 1000ms.
- **SpinLock try-lock gates** — `commandConsumerLock_` try-lock gates only command drain (not morph application — C-3 fix). `touchStateLock_` try-lock protects touch detection vectors during morph apply. Both non-blocking on audio thread.
- **Atomics** (`memory_order_relaxed`) — All morph position, physics mode, and toggle state transferred between UI and audio threads. See Memory Ordering table above for the full convention.
- **Timer-deferred notification** — `morphPositionNotifyPending_` flag + `requestMessageThreadMaintenance()` replaces `callAsync()` for APVTS morph-position sync. Timer fires reliably on message thread even with editor closed.
- **Touch detection** — Prevents morph from overwriting manual knob changes using per-parameter cooldown counters. Dynamic cooldown duration (~200ms) computed from sample rate / block size in `prepareToPlay()`.
- **PERF-IA: Interleaved touch sampling** (2026-07-16) — Instead of calling `getValue()` on all 2,048 parameters every block (the dominant CPU cost), only samples 1/`kTouchSamplingStride` (4) params per block using a rotating `touchSamplingPhase_`. Touch detection is gated to sampled params only. Cooldown tick-down and morph setValue still run for all params. Reduces getValue virtual-call cost by ~75%, touch detection latency ~20ms (well within ~200ms cooldown).
- **PERF-CPU: CPU Saver mode** (2026-07-16) — New `cpuSaver` APVTS bool parameter (default OFF). When enabled, halves audio-domain FFT size (min 512) and caps oversampling at ×2. Reduces audio-domain CPU by ~40-60%. Applied in both `prepareToPlay` and `syncStateFromAPVTS`.
- **PERF-MEM: SonicMaster lazy ring allocation** (2026-07-16) — The 12.3 MB `AudioCaptureRing` is now allocated lazily via `ensureRing()` on first `setActive(true)`, `requestDecisionNow()`, or `runCycle()`. `prepare()` resets the ring to nullptr. Saves ~60% of More-Phi's owned memory when SonicMaster is inactive.
- **PERF-MEM: throttleStates_ reduction** (2026-07-16) — Reduced from 8192 to 4096 entries (~64 KB saved).

### Interfaces for Testability

`IPluginHostManager`, `IParameterBridge`, `IMCPServer` (all in `Host/IPluginHostManager.h`) — abstract interfaces that enable mock injection in tests.

## Coding Conventions (2025-07-21 audit)

### Memory Ordering

**Rule:** Always use explicit `.load(memory_order)` / `.store(value, memory_order)` on `std::atomic<T>`. Never rely on implicit `operator T()` / `operator=(T)` which default to `seq_cst`.

**Default choices by use case:**
| Use case | Load order | Store order | Rationale |
|----------|-----------|-------------|-----------|
| UI→audio hints (morph XY, physics mode, toggles) | `relaxed` | `relaxed` | No dependent data; eventual visibility is fine |
| Lifecycle flags (`prepared`, `shuttingDown_`) | `acquire` | `release` | Pairs with prepare/release → processBlock handoff |
| Pending-work flags (`mcpStartPending_`, `morphPositionNotifyPending_`) | `acquire` | `release` | Ensure payload writes are visible when flag is observed |
| Reference counts (`audioThreadActive_`, `audioDomainUsers_`) | `acq_rel` (fetch_add/sub) | — | Modify-and-read in one operation |

Avoid `seq_cst` unless multiple atomics must be globally ordered across threads — it's the most expensive barrier.

### Logging

- **`DBG()` is the only approved logging macro.** Never use `std::cout`, `printf`, `std::cerr`, or `Logger::writeToLog` directly.
- On potentially-audio-thread paths (e.g. `getStateInformation` during offline render), wrap `DBG()` calls in `#if JUCE_DEBUG` blocks so Release builds emit zero code — even an empty `if` branch with a no-op `DBG()` triggers MSVC warning C4390.
- Example pattern:
```cpp
if (!isAudioThread)  // runtime guard — skip all logging on audio thread
{
#if JUCE_DEBUG
    DBG("message: " + someString);
#endif
}
```

### Profiling

- **`MORE_PHI_PROFILE(profiler_, "section_name")`** — RAII timer macro (opt-in via `MORE_PHI_ENABLE_PROFILING=ON`). Expands to no-op in Release builds without the flag. **CRITICAL:** Sections MUST be registered via `profiler_.registerSection("section_name")` in `prepareToPlay()` BEFORE audio starts — otherwise all timing data is silently dropped (the audio-thread `updateStats()` path skips unregistered sections to avoid allocation).
- **13 sections currently registered** (2026-07-16): `processBlock_total`, `command_queue_drain`, `midi_processing`, `morph_computation`, `modulation_engine`, `parameter_application`, `hosted_plugin_process`, `sonicmaster_capture`, `audio_domain_total`, `spectral_engine`, `granular_engine`, `formant_engine`, `hybrid_blend`.
- **`MorePhiProcessor::getProfilingReport()`** — Returns a formatted string with per-section avg/max/percentage. Called from MCP tools and UI diagnostics.
- **`MorePhiDiagnostics`** — 250ms watchdog timer detects message-thread stalls. Enabled with `MORE_PHI_ENABLE_PROFILING`. Writes to `diagnostics-<pid>.log`.

### Thread-Safe Patterns

- **Prefer Timer-deferred notification over `MessageManager::callAsync()`.** `callAsync()` can silently drop callbacks in headless hosts (FL Studio, Linux) or when the editor is closed. Instead: set an atomic flag, call `requestMessageThreadMaintenance()`, and handle the work in `timerCallback()` on the message thread. See `morphPositionNotifyPending_` for the canonical example.
- **Spin-waits on the message thread MUST have a hard timeout.** See `reconfigureAudioDomainProcessing()` — 100ms deadline with re-dirty + retry on next timer tick. Unbounded spin-waits cause UI jank.
- **`pendingStateMutex_` is a spinlock that protects a `MemoryBlock` copy which may heap-allocate.** It is only acquired on the message thread (never audio). This is acceptable because contention is effectively zero (single-threaded message loop).
- **`hostManager` is `mutable`** — `acquirePluginForUse()`/`releasePluginFromUse()` only mutate atomics and are logically const. This eliminates `const_cast` in `getTailLengthSeconds()` and similar const methods.

### Interface Documentation

- **`IPluginHostManager::getPlugin()` returns a raw pointer whose lifetime is bound to the owning `PluginHostManager`.** It is NOT stable across plugin load/unload cycles. For audio-thread and parameter-bridge access, use `acquirePluginForUse()`/`releasePluginFromUse()` which provide ref-counted safety. `getPlugin()` is provided primarily for test stubs and single-call check-then-use with no intervening yield.

### State Persistence

`getStateInformation`/`setStateInformation` serialize: APVTS parameters + snapshot bank (base64-encoded floats in XML) + hosted plugin description + opaque VST3 state chunk. Plugin reload on state restore uses Timer-based deferred loading with retry logic (max 10 attempts) to handle DAW threading constraints.

**Thread-safety note:** `getStateInformation()` detects audio-thread callers (offline render/export) via `MessageManager::isThisTheMessageThread()`. On the audio thread it skips `beginExclusivePluginUse()` entirely and falls back to the buffered pending state copy from `pendingStateMutex_`. All `DBG()` calls in this function are guarded by `if (!isAudioThread)` + `#if JUCE_DEBUG`.

**VST3 Program Interface:** Exposes all 12 snapshot slots as DAW "programs" for preset-browser integration. Empty slots appear as "Empty N" and are selectable (no-op on recall). `setCurrentProgram()` calls `recallSnapshotQueued()` which enqueues parameter writes through the multi-producer-safe `LockFreeQueue`.

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
- `tests/Unit/TestAgent*.cpp`, `TestRealtimeReactive.cpp`, `TestStructuredAgentLogger.cpp` — Agent layer (unit + E2E + RT-isolation invariants)
- `tests/Integration/` — Plugin lifecycle and MCP integration
- `tests/Performance/` — Benchmark suite (opt-in via `MORE_PHI_BUILD_BENCHMARKS`)

Tests compile with `MORE_PHI_TEST_MODE=1` and `JUCE_STANDALONE_APPLICATION=0`.

## Platform Notes

- Windows builds set `/STACK:4194304` (4 MB) for FL Studio compatibility with plugin-in-plugin hosting
- `cmake/PatchJuceForMSVC.cmake` patches JUCE headers that conflict with Windows macros
- AU format only built on macOS; Windows builds VST3 only
- `ParameterState` uses fixed `std::array<float, 2048>` (no heap allocation) for real-time safety
- `SnapshotBank` heap-allocates its 12-slot array (~97 KB: 12 × 2048 × 4 B + overhead) to avoid stack overflow in hosts with small thread stacks
- `SnapshotBank::toXml()` contains a `static_assert` verifying the local `nameBuf[64]` matches `ParameterState::name[64]` to prevent silent truncation

<!-- SPECKIT START -->
For additional context about technologies to be used, project structure,
shell commands, and other important information, read
`specs/004-dataset-curation/plan.md`.
<!-- SPECKIT END -->
