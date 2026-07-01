# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## Project Overview

More-Phi is a JUCE 8-based VST3/AU audio plugin (C++20) that hosts other plugins and morphs between parameter snapshots using physics-based interpolation, genetic breeding, and AI integration via an embedded MCP server. Version 3.4.1.

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

CMake options: `MORE_PHI_BUILD_TESTS` (ON/OFF), `MORE_PHI_COPY_PLUGIN_AFTER_BUILD` (OFF by default), `MORE_PHI_ENABLE_SANITIZERS` (ASAN+UBSAN, Clang/GCC only), `MORE_PHI_MSVC_MP` (MSVC `/MP` process count under the VS generator; defaults to host logical-core count, `0` disables — ignored under Ninja which controls its own parallelism), `MORE_PHI_ENABLE_ONNX` (optional ONNX Runtime for neural mastering), `MORE_PHI_ENABLE_MCP_SERVER` (embedded MCP server, ON by default for Debug/test, OFF for Release), `MORE_PHI_ENABLE_SONICMASTER_HTTP_FALLBACK` (HTTP inference fallback, OFF by default), `MORE_PHI_ENABLE_PROFILING` (performance profiler instrumentation, OFF by default), `MORE_PHI_BUILD_AAX` (AAX Pro Tools format, OFF by default), `MORE_PHI_ENABLE_LTO` (LTO for Release builds, OFF by default), `MORE_PHI_SAFE_BUILD_MODE` (conservative build settings for local stability, ON by default), `MORE_PHI_PROD_ED25519_KEY_HEX` (production Ed25519 public key for licensing, injected via CI).

### Faster builds with Ninja (recommended for Windows/MSVC)

The default Visual Studio generator parallelizes poorly and its `.tlog` tracking
hangs when two builds contend for the same files. Ninja (bundled with VS, no
separate install) schedules compile+link as one stream and is materially faster.

A wrapper script handles the MSVC environment setup (`vcvars64.bat`) and the
VS-bundled `ninja.exe`:

```cmd
build-ninja.bat configure   :: first time — generates build-ninja/ (re-fetches JUCE, ~2 min)
build-ninja.bat build       :: build the DAW-loadable VST3 plugin (default target)
build-ninja.bat tests       :: build + run the full test suite
build-ninja.bat testonly -R "Agent|Realtime" --output-on-failure   :: run a test subset
build-ninja.bat target MorePhi   :: build just the SharedCode lib (faster compile check)
build-ninja.bat target MorePhiCLI MorePhiMcpServer   :: build any specific target(s)
build-ninja.bat clean       :: wipe build-ninja/
```

Build artifacts land in `build-ninja/` (separate from the VS-generator `build/`).
The DAW-loadable VST3 binary is at
`build-ninja\MorePhi_artefacts\Release\VST3\MorePhi.vst3\Contents\x86_64-win\MorePhi.vst3`.
The two build dirs coexist; do not mix generators on the same dir. Verified:
clean full build ~5 min on 8 cores; targeted agent/host-manager tests pass.

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
| `src/Host/` | `PluginHostManager`, `ParameterBridge` | VST3/AU hosting, parameter read/write (AUDIT-DOC: a prior revision listed a `PluginScanner` class in this layer; no such file exists in `src/Host/`.) **Float-only precision** (no `kSample64`/double-precision path — see `AUDIT_VST3_COMPLIANCE_2026-06-30.md` R1). **Hosted-plugin latency changes are detected** via an `AudioProcessorListener` (`PluginHostManager::LatencyListener`) wired in `loadPlugin` → fires `latencyChangedCallback_` → owning processor dirties `latencyConfigDirty_` + kicks the maintenance timer so `updateReportedLatency()` re-rolls the aggregate (LA1 fix, 2026-06-30). Listener removed in `unloadPluginInternal` before the plugin is destroyed. |
| `src/AI/` | `MCPServer`, `MCPToolHandler`, `TokenOptimizer`, `InstanceRegistry` | JSON-RPC 2.0 server on localhost:30001 |
| `src/AI/` | `SonicMasterAnalysisEngine`, `SonicMasterDecisionRunner`, `SonicMasterDecisionDecoder`, `SonicMasterHttpInferenceSource` | Realtime neural mastering (preview, default OFF). The engine analyses a **5.94 s** window (`kSonicMasterSegmentFrames = 262138` @ 44.1 kHz — *not* 6 s) on a 3 s cycle on a background thread and feeds the built-in `AutoMasteringEngine` via `applyValidatedPlan`. **The model is loudness-blind**: the input is peak-normalized to −1 dBFS before inference, so the model cannot measure absolute input LUFS — the decoded loudness slot is a caller-supplied *target*, not a measurement (see `SonicMasterMeasurementSnapshot` for the genuine BS.1770-4 telemetry reported alongside). The primary inference path is the in-process ONNX runner (`SonicMasterDecisionRunner`); the ONNX model (`masteringbrain_v2_decision.onnx`) is **embedded in the binary** via JUCE `BinaryData` (no runtime file I/O). The HTTP source (`tools/inference_server/`) is the fallback. Resampling uses `resamplePolyphase` (not linear). Capture ring is **rate-proportional** and lazily allocated (see PERF-MEM above). **Mono capture supported** (`channelCount == 1` downmix path). Plan application uses a **pending-plan atomic-flag pattern** (the analysis thread stores the plan in `pendingPlan_` + sets `pendingApplication_`; the processor's message-thread timer polls `hasPendingApplication()` and applies it via `processPendingApplication()` → `applyValidatedPlan` — no `callAsync`). **C2 FIX (2026-06-30):** `applyValidatedPlan` arms a `pendingReverify_` flag and records the submitted plan id; the same timer polls `hasPendingReverify()` and, once the audio thread's `getLastDrainedPlanId()` catches up, calls `processPendingReverify()` to re-read the hosted values and recompute `lastApplyWasPartial_` with truthful post-drain landing data (the pre-drain read-back treats `mismatched` as a timing artifact, not genuine drift). See `docs/superpowers/specs/2026-06-21-sonicmaster-vst3-realtime-integration-design.md` and the **SonicMaster Audit Notes** below. |
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
| `src/AI/Agents/Conductor/` | `ConductorAgent` | Goal decomposition via `WorkflowOrchestrator` + an `ILlmClient`; the ONLY agent that may delegate followUps. **Production LLM transport (AUDIT-DOC correction):** `ConductorAgent::decomposeGoal` calls `ctx_->llm->complete(...)` when an `ILlmClient` is wired (`ConductorAgent.cpp:34-57`). `MorePhiProcessor::startMCPServerIfNeeded()` constructs a **`RestLlmClient`** (which DOES implement `ILlmClient`) when a provider API key is **configured** (non-empty key + selected model — `RestLlmClient::isConfigured`), and falls back to **`DeterministicFallbackLlmClient`** (the Analysis→Memory→Optimization heuristic with warm/bright/sparkle keyword flags) otherwise (`PluginProcessor.cpp:3779-3788` — selection line moved during the AUDIT A1 reconciliation; a prior revision cited `3478-3488`). **Note (AUDIT A1, 2026-06-25):** the selection gates on *configured*, not *validated* — a live `LLMConnectionValidator::testConnectionAsync` round-trip is intentionally NOT performed at startup (it is async/UI-panel-only and would risk stalling MCP server init on a network call). A configured-but-invalid key still wires `RestLlmClient` and fails lazily to `http_401` on the first `complete()` (which `RestLlmClient` retries then surfaces — see `TestRestLlmClientHardening.cpp`). A prior revision of this doc claimed the heuristic was always the shipping behavior and the LLM branch was dead — that was wrong; the REST path is live when configured. (`LLMChatClient`, used by `UI/AIChatPanel`, is a separate REST client that does NOT implement `ILlmClient` — do not conflate the two.) |
| `src/AI/Agents/Agents/` | `AnalysisAgent`, `OptimizationAgent`, `CreativeAgent`, `RealtimeControlAgent`, `QualitySafetyAgent`, `MemoryAgent` | Six specialists; cross-delegation is dropped with `agents.delegation_rejected` |
| `src/AI/Agents/Blackboard/` | `BlackboardBridge` | Typed pub/sub OVER `IntegrationEventBus`; a pump thread drains + fans out |
| `src/AI/Agents/Tooling/` | `DefaultToolInvoker` | Wraps `MCPToolHandler::handle`; enforces per-agent capability scope (fail-closed) + rate budget + attribution |
| `src/AI/Agents/Logging/` | `NullAgentLogger`, `StructuredAgentLogger` | JSONL file logger with in-memory ring fallback |
| `src/AI/Agents/Llm/` | `ILlmClient`, `DeterministicFallbackLlmClient`, `RestLlmClient`, `NullLlmClient` | LLM seam (Risk R1). **AUDIT-DOC correction:** the production client is selected at runtime by `MorePhiProcessor` — `RestLlmClient` (implements `ILlmClient`, OpenAI/Anthropic/OpenAI-compatible REST) when a provider API key is **configured** (non-empty key + selected model), else `DeterministicFallbackLlmClient`. Selection gates on *configured*, not *validated* — see the Conductor row note (AUDIT A1) for why live validation is deferred to first `complete()`. `NullLlmClient` is the test stub. (`LLMChatClient`, used by `UI/AIChatPanel`, is a separate REST client that does NOT implement `ILlmClient` — it drives the chat panel, not the agent layer. A prior revision of this doc implied no real transport was wired; that was wrong.) |

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
- **PERF-IA: Interleaved touch sampling** (2026-07-16) — Instead of calling `getValue()` on all 4,096 parameters every block (the dominant CPU cost), only samples 1/`kTouchSamplingStride` (4) params per block using a rotating `touchSamplingPhase_`. Touch detection is gated to sampled params only. Cooldown tick-down and morph setValue still run for all params. Reduces getValue virtual-call cost by ~75%, touch detection latency ~20ms (well within ~200ms cooldown).
- **PERF-CPU: CPU Saver mode** (2026-07-16) — `cpuSaver` APVTS bool parameter. When enabled, halves audio-domain FFT size (min 512) and caps oversampling at ×2. Reduces audio-domain CPU by ~40-60%. Applied in both `prepareToPlay` and `syncStateFromAPVTS`. **Default flipped OFF→ON (2026-06-30, PERF-CPU-DEFAULT):** FL Studio users on the Master bus were hitting CPU-saturation dropouts/crashes with the prior OFF default (FFT 2048/4096, oversampling uncapped). Backward compat preserved — APVTS persists the bool, so saved sessions recall their stored value; only NEW instantiations get the new default. Disable manually for offline render/bounce where max spectral resolution is needed.
- **PERF-OLA: Circular overlap-add drain** (2026-06-30) — `SpectralMorphEngine`'s output buffer drain converted from a per-block `memmove` of `(outLen − numSamples)` floats/channel to an O(1) circular ring with a read head (`outReadPos`) chasing the write head (`outWritePos`). At FFT 2048 / hop 512 / block 256 this removed ~2,304 floats shifted per channel per block — the dominant spectral-engine cost under small-buffer hosts (FL Studio). Proven **bit-identical** to the linear-buffer engine via a dedicated regression suite (`tests/Unit/TestSpectralMorphEngineRegression.cpp`, 11,269 assertions, two independent runs produce the same float bits before and after the refactor). The ring is sized `2·fftSize_ + 2·maxBlockSize + 2·hopSize_` so the OLA write region never collides with live (written-but-not-yet-drained) data.
- **PERF-MEM: SonicMaster rate-proportional capture ring** (2026-07-16, updated 2026-06-27) — The `AudioCaptureRing` size is **rate-proportional** (`8.0 * sampleRate`, clamped to `[2×44100, 32×192000]` frames), then power-of-2 rounded by `AudioCaptureRing`'s constructor. At 44.1/48 kHz this yields ~4 MiB (vs the old fixed 16 MiB); at 192 kHz it still reaches ~16 MiB. **The ring is allocated EAGERLY in `prepare()`** (`SonicMasterAnalysisEngine.cpp:320-321`, CAPTURE-DECOUPLE fix) — the earlier "lazy" scheme (allocate on first `setActive`/`requestDecisionNow`) was abandoned because it broke on-demand capture (the ring was null during playback until the first decision request, by which point the window was empty). `ensureRing()` remains only as a defensive idempotent allocator for tests that skip `prepare()`. `flushCaptureRing()` is called on hosted plugin load to discard stale audio.
- **PERF-MEM: throttleStates_ reduction** (2026-07-16) — Reduced from 8192 to 4096 entries (~64 KB saved).
- **RT-SAFETY: Bypass crossfade scratch pre-allocation** (2026-07-01) — `wetGainScratch_` and `dryGainScratch_` are now pre-allocated to `samplesPerBlock` in `prepareToPlay()` (`PluginProcessor.cpp:~1904-1908`). The audio-thread path in `applyOutputGainAndMetering()` writes into the first `ns` elements and never calls `resize()` — eliminating the last remaining heap allocation path in `processBlock()`. Previously, `std::vector::resize()` could heap-allocate on every bypass transition if the block size exceeded the vector's capacity. The old comment claiming "stack-allocated" was incorrect (`std::vector` always uses the heap).
- **SMART-DISABLE: isActive_ state tracking** (2026-07-01) — Added `isActive_` flag (`PluginProcessor.h:~900`) set true at end of `prepareToPlay()`, false at end of `releaseResources()`. Provides infrastructure for future lightweight Smart Disable optimization. The `SonicMasterAnalysisEngine::setActive()` already handles pause/resume efficiently (no thread join/re-spawn).
- **MEM-LEAK: PluginHostManager destructor bounded wait** (2026-07-01) — Destructor now does a bounded 100ms wait for audio-thread leases to release, then calls `drainDeferredDoomedPlugins(true)` (new `force` parameter) to destroy any remaining deferred plugins. Previously, if leases were never released, plugins in the deferred doom queue would leak until process exit.
- **MEM-LEAK: MCP ConnectionThread stop timeout** (2026-07-01) — Changed `stopThread(-1)` (infinite wait) to `stopThread(5000)` (5s timeout) in `ConnectionThread::~ConnectionThread()`. Prevents destructor hangs on stuck sockets while still giving the thread time to exit cleanly.

### SonicMaster Audit Notes (2026-06-25)

The AI mastering path has **two edit entry points with intentionally aligned safety properties**. Both funnel through `MorePhiProcessor::enqueueParameterSet` → `LockFreeQueue` → audio-thread drain → `ParameterBridge`.

| Property | MCP/agent path (`setParameter`) | Neural path (`applyValidatedPlan` → `OzonePlanApplicator`) |
|----------|---------------------------------|-------------------------------------------------------------|
| ParamID resolution | Live re-query via `ParameterBridge::resolveParameter` (robust to plugin swap) | `OzoneParameterMap` positional indices, **re-validated by name** in `enqueueIfMapped` (`OzonePlanApplicator.cpp`) — stale indices from a swapped plugin are skipped, not misrouted |
| Hold against morph | `holdAgainstMorph=true` | `holdAgainstMorph=true` (P2.4) |
| Read-back verification | `classifyVerification` → `success`/`value_drift`/`morph_overwrite_risk` | `getLastApplyVerification()` / `lastApplyWasPartial()` (P1.2) — `applyValidatedPlan` consults the applicator's read-back and classifies Applied vs AppliedPartial. **C2 FIX (2026-06-30):** the pre-drain read-back treats `mismatched` as a not-yet-drained timing artifact (excluded from `confirmedFraction`); `processPendingReverify()` re-reads after the audio thread drains the plan (`getLastDrainedPlanId` ≥ submitted id) so `mismatched` becomes a truthful drift/loss signal. Driven by the message-thread timer via `hasPendingReverify()`. |
| Action-ledger record | `MCPToolHandler.cpp:451` | `AutoMasteringEngine::applyValidatedPlan` records `neural_mastering.apply_validated_plan` via `setActionLedger` (P2.5); ledger cap raised 256 → 4096 |

**DSP accuracy status:**
- **True-peak estimator is measurement-grade** (R3 fix, 2026-07-16): 4-phase × 64-tap polyphase FIR derived from a 256-tap linear-phase low-pass (fc=fs/8, Kaiser β=8.6, ~80 dB stopband), matching the reference reconstruction in `TestTruePeakEstimator.cpp`. The estimator now tracks the reference to within ~0.02 dBTP across DC, step, near-Nyquist, and two-tone signals (was a ~25 dB near-Nyquist under-read with the prior 48-tap β=5.0 prototype). Regenerate coefficients with `python3 scripts/generate_truepeak_coeffs.py`.
- **DC-offset removal** in the spectrum/crest/THD path (R5a fix, 2026-07-16): the frame mean is subtracted before peak/RMS/crest, so a constant-DC signal no longer inflates low-bin energy or the crest factor. LUFS continues to use its RLB filter independently.
- **Missing metrics** (AUDIT Q5 reconciliation, 2026-06-27): muddiness, harshness, low/high energy ratio, and transient/attack/punch-as-meter are not computed. Noise floor and SNR **are** computed (`RealtimeSpectrumAnalyzer.cpp:389-439`, DSP-3 Phase 4a) — a prior revision of this line listed them as missing.
- **Transition guard** (P2.8): `enqueueParameterSet` arms `SonicMasterAnalysisEngine::notifyHostedParameterChanged()`; the analysis cycle discards a capture window that straddles a hosted-parameter change and flushes the ring, so the model never analyzes a hybrid state. A 0.5 s settle period (configurable via `setParamTransitionSettleSeconds`) lets plugin tails flush first.
- **Plan atomicity** (P3.10): `OzonePlanApplicator::apply` closes each plan with a `enqueuePlanBoundary` marker; the audio thread stamps `lastDrainedPlanId_` when it drains the boundary, so callers can detect a partial (not-yet-drained) plan via `getLastDrainedPlanId()`. True cross-block all-or-nothing buffering is intentionally NOT implemented (risk); the marker is an observability/commit signal.
- **OptimizationAgent scoring** (P3.9): the dry-run `mastering.render_batch` path now populates a real `lufs_error` per candidate (`|targetLUFS − measuredLUFS|`), so `OptimizationAgent`'s `min(lufs_error)` selection is no longer a no-op (previously every candidate read infinity → zero proposed actions).
- **Safety target bounds widened** (H1 fix, 2026-06-30): `NeuralMasteringSafetyPolicy::defaultConfig` now admits loudness targets in `[-1.5, +1.0]` (image of `(lufs+14)/6` over `[-23,-8]` LUFS) and limiter targets in `[-4.0, +1.8]` (image of `(ceiling+1)/0.5` over `[-3,-0.1]` dBTP). The prior blanket `[-1,1]` bound hard-rejected valid low-LUFS targets via `TargetOutOfRange`, which forced the decoder to *saturate* the model's decision to fit (a fidelity regression: a −23 LUFS target silently became −20 LUFS on apply). The decoder clamps were reverted; per-cycle delta caps (`maxDeltaPerPlan`) are independent and still rate-limit loudness slew to ~0.6 LU/cycle.
- **HTTP fallback lockout bounded** (C1 fix, 2026-06-30): `SonicMasterHttpInferenceSource`'s `inFlight_` guard previously never cleared on a 5 s timeout (the comment claimed `refreshProbe()` would, but it didn't), so one wedged POST permanently disabled the fallback source. `refreshProbe()` now clears `inFlight_` on a successful `/health`, and `infer()` force-clears if a pending request is older than `kInFlightMaxAgeMs` (30 s). A detached worker leak is now bounded rather than permanent.
- **ONNX output NaN guard** (H5 fix, 2026-06-30): `SonicMasterDecisionRunner::runDecision` rejects the whole 44-float decision on any non-finite value (treated as `InferenceRejected`, engaging failover) instead of `copy_n`-ing NaN/Inf into the decoder. The decoder's `clamp()` helper already finite-coerces the dynamics sidecar via `finiteOr`.
- **Queue overflow observable** (M1 fix, 2026-06-30): `LockFreeQueue` now counts dropped pushes (`getDroppedCount()`), surfaced via `MorePhiProcessor::getCommandQueueDroppedCount()` and the MCP diagnostics `queue.dropped_count` field. Push-when-full was previously silent at the queue layer.

### Genre-Conditioned Priors — Ozone §3.2 "Stage 1" (2026-06-26)

The SonicMaster ONNX decision takes only the waveform as input — it cannot condition on genre. Two genre-derived priors are now folded into the decode path via the message-thread timer in `MorePhiProcessor::timerCallback` (the rendezvous that owns both `autoMasteringEngine_` and `sonicMasterEngine_`):

- **Stage 1 — target LUFS prior**: `GenreMasteringProfile.h` maps each of the 12 `GenreClassifier` genre slots to a target LUFS (values mirror `ChainPlanExecutor::kGenreLUFS[12]`) and a `MasteringTargetCurve*`. The processor pushes the active genre's LUFS into `SonicMasterAnalysisEngine::setGenreTargetLufs`. Decode precedence on the background cycle is: **closed-loop feedback > genre prior > model default**; on the on-demand path, **explicit `targetLufs` > genre prior > model default**. The sentinel `kUseModelTargetLufs` clears the prior.
- **Stage 2 — tonal-balance residual**: `TonalBalanceExtractor.h` integrates the spectrum snapshot into the 8 EQ band frequencies (level-invariant, mean-subtracted — same shape as `RuleBasedMasteringResolver`). `decodeSonicMasterDecision` takes an optional `GenreEqPrior{curve, measuredBandDb, residualBlend}`; when set, it blends `clamp(target − measured, ±6 dB) * residualBlend` into each EQ band *before* the ±12 dB clamp. The processor scales `residualBlend` by `getTopConfidence()` (capped 0.5×) and only asserts a curve when confidence ≥ 0.5.

**No-model caveat:** `GenreClassifier` runs a **time-domain heuristic** (low/high band split at 200 Hz + zero-crossing rate → coarse genre guess, confidence 0.5–0.65) out of the box — so the priors are **live and reacting to audio from day one**, not stuck on Streaming. The heuristic covers all 12 slots. When no ONNX genre model is loaded the heuristic is the active path; loading a model upgrades it to neural.

### Pluggable Genre Model — Neural Path (2026-06-27)

`GenreClassifier::loadModel` is a real ONNX path (Phase B), mirroring `SonicMasterDecisionRunner`'s pimpl session pattern. **No model is embedded** — it's pluggable:

- **Search path** (`MorePhiProcessor::initializeGenreClassifier`, called once from `initializeSonicMaster`):
  1. `%APPDATA%/MorePhi/models/genre_classifier.onnx` (user-writable, survives reinstalls)
  2. alongside the plugin binary (dev/drop-in)
- **Expected model shape:** input `[N, 128, T]` float log-mel (`N`∈{1,-1} batch, `128` mel bins — or symbolic, `T` mel frames — concrete or symbolic), output `[N, C]` float softmax over `C` genre classes. If the model's declared `T` is concrete, the mel frontend (`MelSpectrogram.h`) re-`prepare`s to match it; if symbolic, the frontend's 10 s window count is used.
- **Label remap:** the model's `C` output classes are identity-mapped onto the plugin's 12 slots (`genreRemap_[i] = i` for `i < min(C, 12)`); unmapped outputs get `-1`. An argmax that lands on `-1` falls back to the heuristic rather than asserting a wrong genre. A future model can emit `genre_labels` metadata to refine this.
- **Fallback discipline:** any failure (file absent, ORT not compiled, shape mismatch, inference threw, unmapped argmax) silently drops to the heuristic — `runClassification` is fail-soft, never fail-deaf. `unloadModel` does NOT reset `topGenre_`, so the heuristic keeps producing a live guess after a model is unloaded.
- **No SHA pinning:** unlike the hash-pinned SonicMaster model, a user-supplied model's bytes aren't ours to verify.

To enable neural genre: drop a compatible `genre_classifier.onnx` into `%APPDATA%/MorePhi/models/` and restart the plugin. Zero code changes required.

Both priors feed `candidate.targets` (the decoded recommendation); the safety policy's per-cycle delta caps (`maxDeltaPerPlan.loudness ≈ 0.6 LU`) rate-limit the *applied* plan toward them over successive cycles (the same convergence the closed-loop Stage D uses).

### Transient Shaper — Ozone "Impact" (Phase 3, 2026-06-26)

`src/Core/TransientShaper.{h,cpp}` is a single-band transient/sustain shaper (fast 3 ms + slow 150 ms RMS envelope ratio → bounded gain) inserted into the `AutoMasteringEngine::processBlock` chain after dynamics, before EQ. `MasteringControlMask::impact` gates it; `applyValidatedPlan` enables it with a moderate default when the mask is raised (the 44-float decision vector has no impact slot yet — a future decode slot would read the amount from the vector). **Note:** the native chain is dormant today (`intelligenceActive_=false` in the shipped plugin; mastering drives hosted Ozone via `OzonePlanApplicator`), so this module only runs if/when intelligence is enabled. Per-band under `MultibandSplitter` is the noted upgrade path.

### Interfaces for Testability

`IPluginHostManager`, `IParameterBridge` (both in `Host/IPluginHostManager.h`) — abstract interfaces that enable mock injection in tests.

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
- **19 sections currently registered** (audited 2026-06-27): `processBlock_total`, `command_queue_drain`, `morph_computation`, `parameter_application`, `param_getvalue_read`, `param_touch_detect`, `param_setvalue_write`, `command_drain_snapshot`, `command_drain_param`, `audio_domain_total`, `spectral_engine`, `granular_engine`, `formant_engine`, `hybrid_blend`, `midi_processing`, `hosted_plugin_process`, `sonicmaster_capture`, `modulation_engine`, `output_protect`. (A prior revision claimed 13 — stale; the registration list is at `PluginProcessor.cpp:1708-1737`.)
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

### Parameter Classification (v3.4.0+)

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

- `tests/Unit/` — Core engine unit tests (interpolation, physics, genetics, sidechain, spectrum analyzer, true-peak, SIMD, denormals, adaptive EQ, granular/spectral engines, gravity wells, voronoi morph, bypass crossfade, recall ramp, sweep parameter, state persistence, concurrency, stress edge cases, audit fixes, E2E signal path, spec optimizations)
- `tests/Unit/TestAgent*.cpp`, `TestRealtimeReactive.cpp`, `TestStructuredAgentLogger.cpp` — Agent layer (unit + E2E + RT-isolation invariants)
- `tests/Unit/TestNeuralPlanVerification.cpp`, `TestMasteringMeters.cpp`, `TestOnnxNeuralMasteringRunner.cpp`, `TestOnnxRunnerInference.cpp`, `TestSonicMaster*.cpp`, `TestNeuralMastering*.cpp` — Neural mastering verification, ONNX inference, SonicMaster analysis engine/decoder/HTTP
- `tests/Unit/TestLLM*.cpp`, `TestRestLlmClientHardening.cpp` — LLM client and REST transport tests
- `tests/Unit/TestMCPServerUnit.cpp`, `TestMcpProtocol.cpp`, `TestStandaloneMcpServer.cpp` — MCP server and protocol tests
- `tests/Unit/TestLicensing.cpp`, `TestSecurityValidator.cpp` — Licensing and security tests
- `tests/Integration/` — Plugin lifecycle and MCP integration
- `tests/Performance/` — Benchmark suite (opt-in via `MORE_PHI_BUILD_BENCHMARKS`)

Tests compile with `MORE_PHI_TEST_MODE=1` and `JUCE_STANDALONE_APPLICATION=0`.

## Platform Notes

- Windows builds set `/STACK:4194304` (4 MB) for FL Studio compatibility with plugin-in-plugin hosting
- `cmake/PatchJuceForMSVC.cmake` patches JUCE headers that conflict with Windows macros
- AU format only built on macOS; Windows builds VST3 only
- `ParameterState` uses fixed `std::array<float, 4096>` (no heap allocation) for real-time safety
- `SnapshotBank` heap-allocates its 12-slot array (~197 KB: 12 × 4096 × 4 B + overhead) to avoid stack overflow in hosts with small thread stacks
- `SnapshotBank::toXml()` contains a `static_assert` verifying the local `nameBuf[64]` matches `ParameterState::name[64]` to prevent silent truncation

<!-- SPECKIT START -->
For additional context about technologies to be used, project structure,
shell commands, and other important information, read
`specs/004-dataset-curation/plan.md`.
<!-- SPECKIT END -->
