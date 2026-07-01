# More-Phi - Architecture Document

**Date:** 2026-07-01 (updated)  
**Version:** 3.4.1  
**Audit Score:** 7.9/10 — See [VST3_TECHNICAL_AUDIT_AND_MARKET_ANALYSIS.md](../VST3_TECHNICAL_AUDIT_AND_MARKET_ANALYSIS.md)
**Type:** Desktop Audio Plugin (VST3/AU)
**Pattern:** Layered plugin architecture with strict thread-domain separation

## Executive Summary

More-Phi is a JUCE 8 C++20 audio plugin that hosts other VST3/AU plugins and provides a rich morphing engine to interpolate between captured parameter snapshots. The architecture is organized into 7 clearly separated modules: Plugin (entry), Core (DSP), Host (plugin management), AI (MCP server + multi-agent orchestration + neural mastering), MIDI (routing), Preset (persistence), and UI (visual components). The AI module includes a dedicated Orchestrator sub-layer (`AgentOrchestrator`, `EcosystemConfig`, `SecurityValidator`, `McpProtocol`) that wires the processor to the agent runtime and MCP server, and a SonicMaster neural mastering subsystem (ONNX model embedded in binary via `BinaryData`, rate-proportional capture ring, polyphase resampling, pending-plan application pattern). The design prioritizes real-time safety through lock-free data structures, strict thread-domain boundaries, and zero-allocation audio paths.

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

More-Phi follows a **layered plugin architecture** where:

1. **Plugin layer** (entry point) owns all subsystem instances as member variables
2. **Orchestrator layer** (facade) wires `MorePhiProcessor` → `AgentRuntime` → `MCPServer` via `AgentOrchestrator`, `EcosystemConfig`, `SecurityValidator`, and `McpProtocol`
3. **Core layer** provides pure computation (audio-thread-safe, no I/O)
4. **Host layer** manages the hosted plugin lifecycle and parameter I/O
5. **AI layer** provides network I/O (MCP server) isolated on its own thread
6. **UI layer** renders state and enqueues commands via the LockFreeQueue

```
┌─────────────────────────────────────────────────┐
│                  Plugin Layer                     │
│         MorePhiProcessor (owns all)             │
├─────────────────────────────────────────────────┤
│              Orchestrator Layer                   │
│  AgentOrchestrator · EcosystemConfig · SecurityValidator · McpProtocol │
├──────────┬──────────┬───────────┬────────────────┤
│  Core    │  Host    │   AI      │   UI            │
│ (DSP)    │ (VST3)   │  (MCP)    │  (JUCE GUI)     │
│          │          │           │                  │
│ Morph    │ PlugHost │ MCPServer │ MorphPad         │
│ Physics  │ ParamBr. │ ToolHndlr│ SnapFader        │
│ Genetic  │          │ TokenOpt │ SnapshotRing     │
│ Spectral │          │ InstReg  │ V2TabBar         │
│ Granular │          │ LinkBcst │ ModMatrixPanel   │
│ ModEng   │          │ Dataset  │ PresetBrowser    │
├──────────┴──────────┴───────────┴────────────────┤
│              MIDI Layer + Preset Layer             │
│         MIDIRouter  │  PresetSerializer            │
└─────────────────────┴─────────────────────────────┘
```

## Threading Model

Three thread domains with strict boundaries, plus two agent-owned worker threads:

### Audio Thread
- **Entry:** `MorePhiProcessor::processBlock()`
- **Classes:** `MorphProcessor`, `InterpolationEngine`, `PhysicsEngine`, `GeneticEngine`, `SnapshotBank` (read side), `MIDIRouter`, `ModulationEngine`, `SpectralMorphEngine`, `GranularMorphEngine`
- **Contract:** All methods `noexcept`, zero allocations after `prepare()`, no locks, no I/O
- **ThreadPool:** `ThreadPool::workerThread()` uses `ActiveTaskGuard` RAII to guarantee `activeTasks_` is decremented even if the task throws. The `enqueue()` lambda no longer decrements the counter.
- **Data flow:** Drains `LockFreeQueue` (gated by `commandConsumerLock_` try-lock) → processes MIDI → computes morph → applies to hosted plugin via `ParameterBridge` (always proceeds, independent of drain gate — C-3 fix)

### Message Thread
- **Entry:** JUCE message loop
- **Classes:** All UI components, Timer callbacks, deferred plugin loading
- **Contract:** Standard JUCE rules. May write to `SnapshotBank` (serialized via SpinLock), `ModulationMatrix` (double-buffer publish)

### MCP Thread
- **Entry:** `MCPServer::run()` (TCP accept loop)
- **Classes:** `MCPServer`, `MCPToolHandler`, `MCPToolsExtended`
- **Contract:** JSON-RPC I/O. Enqueues parameter changes to audio thread via `LockFreeQueue`. Reads snapshot data via seqlock.
- **Security:** Instance-scoped `AutomationRuntime` (no global static). Cache keys prefixed with `instanceId + ":"`. Constant-time token comparison using `volatile uint8_t diff`. 30-second idle timeout on connections. TTL-based zombie eviction in `InstanceRegistry`.

### Agent Scheduler Workers (2 threads)
- **Entry:** `PriorityScheduler::workerLoop()`
- **Classes:** `ConductorAgent`, `AnalysisAgent`, `OptimizationAgent`, `CreativeAgent`, `RealtimeControlAgent`, `QualitySafetyAgent`, `MemoryAgent`
- **Contract:** Agents execute synchronously on scheduler workers — NEVER on the audio thread. `RealtimeCritical` priority jumps the *agent* queue only. `RealtimeControlAgent` writes corrections through `LockFreeQueue` / `DefaultToolInvoker` → `MCPToolHandler::handle`, exactly like the UI/MCP paths.
- **Scheduler:** 4 per-priority-level `std::queue<Entry>` with O(1) operations. Starvation guard promotes Background→Normal after 1000ms (H-1/M-4 fix).

### Blackboard Pump Thread
- **Entry:** `AgentRuntime` lambda (50ms polling interval)
- **Classes:** `BlackboardBridge`
- **Contract:** Polls `IntegrationEventBus::listRecent()` every 50ms, fans out new events to matching agent subscribers. Subscriber callbacks run on this thread.

### Orchestrator Thread
- **Entry:** `AgentOrchestrator::initialize()` (single-initialization facade)
- **Classes:** `AgentOrchestrator`, `EcosystemConfig`, `SecurityValidator`, `McpProtocol`
- **Contract:** Wires `MorePhiProcessor` → `AgentRuntime` → `MCPServer` during startup. `EcosystemConfig` provides unified JSON configuration for plugin, agents, MCP, and security settings. `SecurityValidator` provides MCP message sanitization, auth validation, and rate limiting. `McpProtocol` defines explicit JSON-RPC 2.0 message schemas (`McpRequest`, `McpResponse`, `McpNotification`, `McpError`). All orchestrator components live in `src/AI/Orchestrator/` and compile into the `MorePhi` shared-code target.

### Concurrency Primitives

| Primitive | Location | Purpose |
|-----------|----------|----------|
| Seqlock | `SnapshotBank` | Lock-free audio reads with retry; UI/MCP writes via SpinLock. `paramNames_` and `stateChunks_` are read under their respective SpinLocks (`writeLock_` and `chunksLock_`) **before** the seqlock read of the primary payload |
| SPSC LockFreeQueue | `MorePhiProcessor` | ParamCommand ring buffer (8192), UI/MCP → audio. Multi-producer push serialized via `juce::SpinLock`; single-consumer pop is lock-free |
| SpinLock try-lock gate | `MorePhiProcessor::commandConsumerLock_` | Serializes audio-thread command drain vs assistant flush. Try-lock on audio path (non-blocking); only gates drain — morph application always proceeds (C-3 fix) |
| SpinLock try-lock | `MorePhiProcessor::touchStateLock_` | Protects `lastApplied_`, `touchCooldown_`, `liveEditHold_` during morph application and drain. Try-lock on audio path |
| Multi-queue scheduler | `PriorityScheduler` | 4 per-priority `std::queue<Entry>` with O(1) push/pop/starvation-promotion. Starvation guard 1000ms (H-1/M-4 fix) |
| Double-buffer | `ModulationMatrix` | Atomic route publish with mirror-copy |
| `std::atomic<bool>` | `GranularMorphEngine::active_` | Thread-safe enable/disable |
| `std::atomic<int>` | `ModulationMatrix::readIndex_` | Buffer swap index |
| `memory_order_relaxed` atomics | Various | Morph position, physics mode, toggle flags |
| Security primitives | `MCPServer` / `InstanceRegistry` | Instance-scoped runtime, constant-time token compare, idle timeouts, TTL eviction |
| Orchestrator security | `SecurityValidator` / `AgentOrchestrator` | MCP message sanitization, auth validation, rate limiting, unified JSON config via `EcosystemConfig` |

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

**Step 3c** — `DiscreteParameterHandler` snaps discrete/binary parameters to valid steps during morphing, preventing invalid intermediate values.

### Mastering-chain routing (important clarification)

The pipeline above is the **default real-time audio path** and terminates at the
hosted plugin. The mastering processors — `BrickwallLimiter`,
`MultibandDynamicsProcessor`, `AdaptiveEQ`, `StereoImager`, `HarmonicExciter`,
`LoudnessNormalizer`, `AutoMasteringEngine` — are owned by `MorePhiProcessor`
but are **not** in this default signal path:

- In `processBlock`, `AutoMasteringEngine::analyzeBlock(buffer)` runs as
  **measurement only** (metering + neural-mastering input). It does not alter the
  buffer. See the in-source comment: *"measures the final audio … without running
  the autonomous mastering processor."*
- The mastering processors **apply** audio changes only when a **validated
  neural-mastering plan** is executed (`AutoMasteringEngine`, gated by
  `NeuralMasteringSafetyPolicy`), on the message/background thread — never on the
  default audio path.

Consequences (verified 2026-06-16, see `validation/2026-06-16_headless_validation_findings.md`):

- Default-operation CPU is morph + modulation + analysis + output gain (+ hosted
  plugin); the heavy mastering/oversampling DSP engages **only when a plan runs**.
- Reported latency is **0** in the default state; mastering processors add latency
  only when a plan engages them.
- A default headless render with **no hosted inner plugin** produces bit-exact
  silence (the hosted-plugin stage has nothing to route through). Exercising the
  mastering chain requires a hosted plugin + a triggered plan (DAW/MCP context),
  not a bare render.

Although not in the default audio path, the mastering processors' **audio-correctness
is unit/integration-test-validated** (21/21 PASS, 2026-06-16 — incl. the dBTP-ceiling-
vs-inter-sample-peaks test #24 and bounded/mono-compatible plan transitions #217).
See `validation/2026-06-16_headless_validation_findings.md` §6a. Only subjective
sonic voicing and plan-context latency remain unverified (neither is a defect).

### Neural Mastering Edit Path

The neural mastering subsystem (`SonicMasterAnalysisEngine` → `AutoMasteringEngine::applyValidatedPlan` → `OzonePlanApplicator`) provides a second edit entry point alongside the MCP/agent `setParameter` path. Both paths funnel through `MorePhiProcessor::enqueueParameterSet` → `LockFreeQueue` → audio-thread drain → `ParameterBridge`, with aligned safety properties:

| Property | MCP/agent path | Neural path (`OzonePlanApplicator`) |
|----------|----------------|--------------------------------------|
| ParamID resolution | Live re-query via `resolveParameter` | Positional indices, re-validated by name (`enqueueIfMapped`) |
| Hold against morph | `holdAgainstMorph=true` | `holdAgainstMorph=true` |
| Read-back verification | `classifyVerification` | `getLastApplyVerification()` / `lastApplyWasPartial()` |
| Action-ledger record | Standard MCP ledger | Ledger cap 4096 (`kLedgerMaxTransactions`) |
| Plan atomicity | N/A | `enqueuePlanBoundary()` + `lastDrainedPlanId_` atomic |
| Transition guard | N/A | `notifyHostedParameterChanged()` + ring flush |

**SonicMaster key implementation details:**
- ONNX model embedded in binary via JUCE `BinaryData::getNamedResource()` — no runtime file I/O
- Capture ring is rate-proportional (`8.0 * sampleRate`, clamped `[2×44100, 32×192000]`) and **eagerly allocated** in `prepare()` (CAPTURE-DECOUPLE fix, 2026-06-26). `ensureRing()` remains as a defensive idempotent allocator for tests that skip `prepare()`.
- Resampling uses `resamplePolyphase` (not linear interpolation)
- Mono capture supported (`channelCount == 1`)
- Plan application uses pending-plan atomic-flag pattern (stores in `pendingPlan_`, applies in `runCycle`) — no `callAsync`
- Genre-conditioned priors: target LUFS prior from `GenreMasteringProfile` + tonal-balance residual from `TonalBalanceExtractor`, fed via `setGenreTargetLufs()` on the message-thread timer
- Pluggable ONNX genre model: `GenreClassifier::loadModel()` searches `%APPDATA%/MorePhi/models/genre_classifier.onnx` or alongside the plugin binary; always falls back to time-domain heuristic if absent

### Agent MCP Surface

The multi-agent layer exposes 7 MCP tools dispatched via `MCPToolHandler::handle` and classified in `PermissionKernel::classifyTool`: `agents.list`, `agents.run_goal`, `agents.run_task`, `agents.run_status`, `agents.run_cancel`, `agents.blackboard.recent`, `agents.set_autonomy`.

## State Persistence

`getStateInformation()` / `setStateInformation()` serialize:

1. **APVTS parameters** — JUCE AudioProcessorValueTreeState XML
2. **Snapshot bank** — base64-encoded float arrays in XML
3. **Hosted plugin description** — `PluginDescription` XML for re-loading
4. **Hosted plugin state** — opaque VST3/AU state chunk (binary blob)
5. **Modulation routes** — XML via `ModulationMatrix::toXml()`

Plugin reload on state restore uses Timer-based deferred loading with retry logic (max 10 attempts) to handle DAW threading constraints. `setStateInformation()` uses an atomic `pendingStateRestore_` flag instead of blocking `callFunctionOnMessageThread`.

## Key Subsystem Details

### SnapshotBank (Seqlock Pattern)
- 12 slots, each holding a `ParameterState` (fixed `std::array<float, 4096>`)
- Heap-allocated slot array (~197 KB) to avoid stack overflow
- Audio thread reads via seqlock (retry on torn read). `paramNames_` and `stateChunks_` are read under their respective SpinLocks (`writeLock_` and `chunksLock_`) **before** the seqlock read of the primary payload
- UI/MCP writes serialize via SpinLock + sequence counter increment
- `toXml()` reads `name`/`count` under lock first, then seqlock-protected values, with retry on `seq1 != seq2`

### Physics Modes
- **Direct:** Raw cursor position → interpolation (no physics)
- **Elastic:** Spring-damper system (`ElasticState` with position + velocity), three presets
- **Drift:** Perlin noise wandering around target with speed/distance/chaos controls

### Genetic Engine
- `breed()` crosses two snapshots with configurable crossover ratio and mutation
- `SanityConfig` protects dangerous parameters (Volume, Pitch, Bypass)
- `smartRandomize()` only mutates user-learned parameters

### Parameter Classification (v3.4.0+)
- `ParameterClassifier` categorizes: Continuous, Discrete, Binary, Frequency, Decibel, Enumeration
- `DiscreteParameterHandler` snaps discrete/binary params to valid steps during morphing
- `TokenOptimizer` manages AI token budgets by selecting which parameters to expose

### ModulationMatrix (Double-Buffer)
- Two `RouteBuffer` instances (up to 128 routes each, `MAX_ROUTES = 128`)
- Message thread mutates write buffer, then publishes via `readIndex_.store(release)`
- Audio thread reads from `readIndex_.load(acquire)` — never writes
- After publish, write buffer is mirrored from the just-published state

### V2 Audio Engines
- **SpectralMorphEngine:** FFT-based overlap-add vocoder with magnitude/phase interpolation
- **GranularMorphEngine:** Grain pool with position/size/density randomization
- **FormantMorphEngine:** Formant-preserving spectral envelope morphing
- **HybridBlend:** Coordinates multiple engines with configurable blend weights

## Testing Strategy

- **Unit tests** (Catch2): Core engines, physics, genetics, sidechain, SIMD, spectral, granular, modulation, true-peak, adaptive EQ, neural plan verification, ONNX inference, SonicMaster analysis/decoder, mastering meters, agent layer (unit + E2E + RT-isolation), LLM client, MCP server/protocol, licensing, security
- **Integration tests**: Plugin lifecycle (load/unload/state), MCP server tool invocations
- **Legacy tests**: incompatible Morphy-era tests are documented in `tests/LEGACY_TESTS.md` and excluded from active targets
- **Performance benchmarks**: CPU usage, audio processing throughput. `PerformanceProfiler` uses a pre-allocated circular buffer (`std::array<Sample, 1024>`) with atomic write index — no heap allocation on the audio thread
- **DAW test matrix**: Manual test docs for Ableton, FL Studio, Logic, Reaper
- **Automated scripts**: Audio quality, real-time safety, VST3 validator (pluginval strictness 5)
- **Sanitizers**: ASAN + UBSAN via Clang on Linux CI

Tests compile with `MORE_PHI_TEST_MODE=1` and `JUCE_STANDALONE_APPLICATION=0`.

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
- `src/Version.cpp` is the sole translation unit for `__DATE__`/`__TIME__` to prevent incremental build churn
- Windows macro conflicts are resolved via `/U` compiler flags (not file mutation), making the build hermetic
- AU format only on macOS; Windows builds VST3 only
- macOS deployment target: 11.0 (Big Sur)
- macOS builds Universal Binary (x86_64 + arm64)

---

_Generated using BMAD Method `document-project` workflow_
