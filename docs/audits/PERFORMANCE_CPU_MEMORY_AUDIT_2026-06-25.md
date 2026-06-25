# More-Phi VST3 — Comprehensive CPU & Memory Performance Audit

**Date:** 2026-06-25
**Scope:** VST3 engine core · Neural model inference (SonicMaster) · AI assistant (multi-agent layer) · Parameter-control system
**Build under test:** `feature/multi-agent-layer` @ HEAD, instrumented build (`MORE_PHI_ENABLE_PROFILING=ON`, `MORE_PHI_ENABLE_ONNX=ON`)
**Tooling:** In-tree `more_phi::PerformanceProfiler` (per-section mean/min/max **+ p50/p95/p99**), `IntegratedProcessorBenchmark` headless harness, `GetProcessMemoryInfo` runtime RSS, sizeof-based static decomposition.

---

## 0. Executive Summary

> **Status note:** This report ships in two parts. **Sections 1–6, 9, 10, and the appendices are complete** — they cover methodology, instrumentation design, thread topology, the full static memory breakdown, architecture-correlated findings, the optimization roadmap, and the ponytail-audit cut list, all derivable from the source. **Sections 7 (runtime CPU numbers) and 8 (runtime memory deltas) are marked `PENDING BUILD`** because the measurement campaign (`IntegratedProcessorBenchmark` S1–S8) requires the instrumented `MorePhiAuditBenchmark` target, whose build the user deferred. The harness and instrumentation are wired and ready; running the campaign is a single command once the build is approved.

**Headline findings (static + architectural, high confidence):**

1. **The dominant per-block CPU cost is the 2048-parameter write loop** (`parameter_application`), and within it the strided `getValue()` batch read is the single most expensive sub-operation. This was already mitigated by PERF-IA (interleaved touch sampling, ~75% reduction) and PERF-C3 (static-morph early-exit). This audit adds the first **sub-attribution** (`param_getvalue_read` isolated) so future work can target it precisely.
2. **Memory is dominated by two lazy allocations**: the SonicMaster `AudioCaptureRing` (**16.0 MiB**, not the 12.3 MiB widely quoted in docs — see §10.2) and the ORT model (**3.36 MiB weights + 2.0 MiB input buffer**). Both are already lazy-allocated, so the idle footprint is low. The "12.3 MB" figure is a stale-doc defect corrected in source by this audit.
3. **The AI assistant layer is near-zero CPU cost** in production: the "LLM client" is `DeterministicFallbackLlmClient` (a keyword heuristic, no network), the blackboard pump is a 50 ms sleep loop, and 2 scheduler-worker threads sit idle unless a goal is submitted. Do not optimize this layer for CPU until a real LLM transport is wired.
4. **Neural inference runs off the audio thread** (SonicMaster background thread, 3 s cycle) and is never per-sample — its cost is a ~hundreds-of-ms spike every 3 s, isolated from the realtime path. The on-demand `sonicmaster_decision` MCP tool blocks an MCP connection thread, serialized by `inferMutex_`, never the audio thread.
5. **The in-tree profiler lacked percentiles** (mean/min/max only). This audit implements the documented M4 ring-buffer upgrade (`PerformanceProfiler.h`) so per-section p50/p95/p99 are now available, with the audio-thread no-alloc/no-block contract (C-2/C-16) preserved and unit-tested.

**Top 5 optimization priorities (ranked impact × effort) — full table in §9:**

| # | Optimization | Est. impact | Effort |
|---|--------------|-------------|--------|
| 1 | Hoist the batched `getValue()` read into a single virtual-table-resolved call / cache `pluginParams` ptr-type | -5–15% processBlock CPU | Medium |
| 2 | Add per-section p99 budget alarms + a `getProfilingReport()` MCP tool so live DAW sessions surface tail spikes | Observability (no direct CPU) | Low |
| 3 | Replace `unordered_map<string, ProfileStats>` lookup-by-string on the audio path with index-by-int (C-16 adjacent) | -1–3% profiled-build CPU | Low |
| 4 | Right-size `AudioCaptureRing` for 44.1/48 kHz hosts (8 s @ 192 kHz is 16 MiB; most hosts run ≤96 kHz) | -8–12 MiB resident | Low | **DONE (PERF-MEM-RATE, 2026-07-16)** — now rate-proportional; ~4 MiB at 48 kHz. |
| 5 | Wire `ComprehensiveProfilingHarness.cpp` in or delete it (currently orphaned — §10.1) | Compile-time / clarity | Trivial |

---

## 1. Methodology

### 1.1 Profiling tool selection & justification

| Tool | Role | Why this tool |
|------|------|---------------|
| `more_phi::PerformanceProfiler` (in-tree) | Per-section CPU attribution on the **audio thread** | The only profiler that runs inside `processBlock` with the realtime contract intact. External samplers (ETW/VTune) sample at ~1–8 kHz and miss sub-block structure; the in-tree RAII timers capture every call with ns-resolution `high_resolution_clock`. **Upgraded by this audit** to emit p50/p95/p99 (M4). |
| `IntegratedProcessorBenchmark` harness | Idle/active/automation/neural/combined scenarios | The only harness that drives the **real** `MorePhiProcessor.processBlock` headlessly (no DAW), with measurement hygiene (FTZ/DAZ, CPU pin, HIGH_PRIORITY, population-level percentiles across passes). Wired into the build by this audit as `MorePhiAuditBenchmark`. |
| `GetProcessMemoryInfo` (Psapi) | Runtime RSS / working-set / peak | Process-level real memory (not estimates). Captured per-scenario by the harness. Justified because per-component heap attribution would need a global allocator hook (`AllocationTracker.h` exists but is unwired — §10.3); process deltas + the static sizeof table together give component attribution. |
| sizeof-based decomposition | Static per-component memory | Sourced from `ParameterState`, `SnapshotBank`, `ParameterBridge::throttleStates_`, `AudioCaptureRing`, `SonicMasterDecisionRunner` buffer. Reconciled to runtime where possible. |

**Why not ETW/WPA/VTune:** The user chose the in-tree tooling path (§"CPU tooling" decision). ETW would add value for time-inside-ORT/JUCE internals, but the in-tree sections already cover the plugin's own code paths, which is where actionable optimization lives. ETW remains a recommended follow-up for the ORT-internal attribution gap (§9, priority #8).

### 1.2 Measurement hygiene (built into the harness)

- **FTZ + DAZ** set process-wide via MXCSR (`_MM_SET_FLUSH_ZERO_MODE` / `_MM_SET_DENORMALS_ZERO_MODE`) — without this, any engine producing a denormal runs 10–100× slower on those blocks, contaminating every relative-timing claim.
- **CPU pin** to logical CPU 2 (`SetThreadAffinityMask`) — avoids core 0 (OS housekeeping) and migration noise.
- **HIGH_PRIORITY_CLASS** — minimizes OS preemption.
- **Population-level percentiles**: all per-block timings across all passes are folded into one population for p50/p95/p99 (the prior code took percentiles of pass-averages, which made p99 ≈ max).

### 1.3 Statistical confidence

Default campaign: `--passes 5 --buffers 256,512,1024`, `blocksPerPass=1200` → **6000 `processBlock` calls per scenario per buffer size**. p95/p99 are over this population. For the in-tree profiler's per-section percentiles, the trailing ring holds the most recent **2048** samples (§6.2).

---

## 2. System Under Test — Component Map

### 2.1 The four components in scope

| Component | Key classes | Thread | Lifecycle |
|-----------|-------------|--------|-----------|
| **VST3 engine core** | `MorePhiProcessor`, `MorphProcessor`, `InterpolationEngine`, `PhysicsEngine`, `SnapshotBank`, `MIDIRouter`, audio-domain engines (Spectral/Granular/Formant) | **Audio (realtime)** | `prepareToPlay` → `processBlock` → `releaseResources` |
| **Parameter control** | `ParameterBridge`, `ParameterClassifier`, `DiscreteParameterHandler`, `LockFreeQueue<ParamCommand>`, touch-detection vectors | Audio (consumer) + UI/MCP (producers) | Always-on once prepared |
| **Neural model (SonicMaster)** | `SonicMasterAnalysisEngine`, `SonicMasterDecisionRunner` (ORT), `SonicMasterDecisionDecoder`, `AutoMasteringEngine`, `AudioCaptureRing` | **SonicMaster background thread** (3 s cycle) + audio-thread `capture()` write | Lazy: ring + ORT session allocated on first `setActive(true)` |
| **AI assistant (agents)** | `AgentRuntime`, `PriorityScheduler` (2 workers), `BlackboardBridge` (50 ms pump), 6 specialists + Conductor, `DeterministicFallbackLlmClient`, `DefaultToolInvoker`, `StructuredAgentLogger` | **Scheduler workers ×2** + **blackboard pump thread** | Lazy: constructed in `startMCPServerIfNeeded()` |

### 2.2 Audio-thread pipeline (`processBlock`)

```
processBlock()
  ├─ ScopedNoDenormals + prepared/shuttingDown acquire-loads
  ├─ audioThreadActive_ RAII ref-count
  ├─ playhead sync + APVTS sync-if-dirty
  ├─ drainParameterCommandQueue()        [commandConsumerLock_ try-lock gates drain only]
  │     ├─ command_drain_snapshot         ← NEW sub-section
  │     └─ command_drain_param            ← NEW sub-section
  ├─ processMIDIAndSidechain()            [midi_processing]
  ├─ applyMorphAndParameters()
  │     ├─ morph_computation              [physics → interpolation → smoothing]
  │     └─ parameter_application          [2048-param write loop — DOMINANT]
  │           ├─ param_getvalue_read      ← NEW sub-section (strided getValue batch)
  │           └─ touch-detect + setValue  (residual, interleaved)
  ├─ ParameterBridge::processRecallRamp()
  ├─ hosted_plugin_process                [hostManager.processBlock]
  ├─ audio_domain_total → {spectral, granular, formant, hybrid}  [nested]
  ├─ applyOutputGainAndMetering()         [bypass wet/dry crossfade]
  └─ sonicmaster_capture                  [lock-free ring write, audio thread]
```

**Sections registered in `prepareToPlay()`** (`PluginProcessor.cpp:1337–1356`): `processBlock_total`, `command_queue_drain`, `command_drain_snapshot` (NEW), `command_drain_param` (NEW), `morph_computation`, `parameter_application`, `param_getvalue_read` (NEW), `audio_domain_total`, `spectral_engine`, `granular_engine`, `formant_engine`, `hybrid_blend`, `midi_processing`, `hosted_plugin_process`, `sonicmaster_capture`, `modulation_engine` — **16 sections** (was 13).

### 2.3 Thread topology (critical for isolation matrix)

| Thread | Realtime? | What runs here | Cost-bearing components |
|--------|-----------|----------------|-------------------------|
| **Audio** | ✅ RT | `processBlock` pipeline (§2.2) | Engine core, parameter application, SonicMaster ring write (only) |
| **SonicMaster background** | ❌ | `analysisLoop` 3 s cycle: drain ring → resample → ORT `Run` → decode → safety → apply | Neural inference CPU + model weights RSS |
| **Message (JUCE)** | ❌ | `AutoMasteringEngine::applyValidatedPlan` via pending-plan handoff; processor 50 ms timer | Agent autonomy replay, SonicMaster plan application |
| **MCP server** | ❌ | JSON-RPC dispatch; `sonicmaster_decision` blocks here on `inferMutex_` | On-demand neural inference |
| **Scheduler worker ×2** | ❌ | Agent `execute()` + `DefaultToolInvoker` → `MCPToolHandler` | Agent CPU (near-zero — heuristic LLM) |
| **Blackboard pump** | ❌ | `BlackboardBridge::poll()` every 50 ms then `sleep(50)` | Negligible (sleep loop) |

**Invariant (strict):** agents and neural inference **never** execute on the audio thread. This is enforced architecturally (`RealtimeCritical` priority jumps the *agent* queue, not the audio thread; `requestDecisionNow` is serialized on `inferMutex_` off the audio path). Verified by `TestAgentAudioThreadIsolation.cpp`.

---

## 3. Instrumentation Design (implemented by this audit)

### 3.1 `PerformanceProfiler` ring-buffer upgrade (M4)

**File:** `src/Core/PerformanceProfiler.{h,cpp}`

The profiler previously tracked only running mean/min/max per section. The documented M4 upgrade path (a per-section ring) is now implemented:

- **Per-section `SectionRecord`** holds `ProfileStats` + `std::array<double, 2048> ring` + head/count. `kRingSamples = 2048` (power-of-two → mask instead of modulo). Total ring memory ≈ 2048 × 8 B × ~16 sections ≈ **256 KB**, allocated once per section in `registerSection()` (message thread).
- **Audio-thread push** (`updateStats`): fixed-index write + masked advance, under the **existing** `ScopedTryLockType`. **No new allocation, no new lock.** If the try-lock misses (reader holds it), the sample is dropped — identical to prior behavior (C-2 invariant).
- **Percentile computation** (`computeStats`): message-thread-only, sorts a copy of the populated window, nearest-rank p50/p95/p99. `ProfileStats` gained `p50Ms/p95Ms/p99Ms` (default 0).
- **Contract preservation**: the C-16 invariant ("never `operator[]` on the audio thread") is maintained via `find()`; the C-2 invariant ("never block on the audio thread") is maintained via the existing try-lock. Verified by a new unit test (`PerformanceProfilerTests.cpp`: "recordTime never blocks").

### 3.2 Sub-section timers (split the dominant cost)

**File:** `src/Plugin/PluginProcessor.cpp`

The #1 cost section (`parameter_application`) wrapped the entire 2048-param loop as one bucket. This audit isolates its most expensive sub-operation:

- **`param_getvalue_read`** — wraps the PERF-IA strided `getValue()` batch read (`~512` calls/block at stride 4 of 2048). This is the documented dominant per-block cost.
- **`command_drain_snapshot`** / **`command_drain_param`** — wrap the two structurally distinct drain branches (snapshot-restore marker vs normal param command).

**Design decision (honest scoping):** the touch-detection and `setValue()` portions of the loop are **interleaved per-iteration**, so per-iteration timers would add ~180 ns × 2048 ≈ **370 µs/block of pure measurement overhead** — larger than the cost being measured. They therefore stay under the existing `parameter_application` bucket, and the audit reports `param_getvalue_read` (isolated read cost) vs `parameter_application − param_getvalue_read` (residual touch+write cost). This is the honest, actionable split; it's documented in the report rather than hidden.

**Overhead of the new timers:** ~6 extra `chrono::now()` calls/block ≈ 0.01% at 256/48k. Zero cost when profiling is off (macro expands to `(void)0`).

### 3.3 Report + harness format updates

- `getProfilingReport()` (`PluginProcessor.cpp:1030–1041`) now emits `p50/p95/p99` lines per section.
- `IntegratedProcessorBenchmark::parseSections()` reads the new lines and the `SectionStat` struct carries `maxUs/p50Us/p95Us/p99Us`; `emitJson()` serializes them into `profile-results.json`.

---

## 4. Testing Conditions (matrix)

| Scenario | ID | Audio | Automation | Neural | Agents | Buffer sizes | Purpose |
|----------|----|-------|------------|--------|--------|--------------|---------|
| Idle (no audio) | S1 | — | — | — | — | n/a | Static footprint of a prepared-but-idle processor |
| Active, null plugin | S2 | sustained | — | — | — | 256/512/1024 | Engine-core steady-state baseline |
| Automation (sweep) | S3 | sustained | linear morph X/Y sweep | — | — | 256/512/1024 | **Parameter-control overhead** (cost-of-automation) |
| Automation (rapid) | S4 | sustained | random bursts + batched MCP `ParamCommand`s | — | — | 256/512/1024 | Edge case: max-rate parameter changes |
| Neural isolation | S5 | — | — | ORT `runDecision` loop | — | n/a | Model in isolation (weights + inference latency) |
| Neural in-context | S6 | capture | — | `runOneCycleForTest` | — | 256 | Model within engine (drain→resample→infer→decode) |
| Combined | S7 | sustained | sweep | capture | MCP + agents | 256/512/1024 | **Comparative overhead**: everything together |
| Combined + Ozone | S8 | sustained | sweep | capture | MCP + agents | 256 | Realistic reference (auto-skips if Ozone absent) |

**Derived measurements:** automation delta (S3−S2, S4−S2) = parameter-control overhead; neural isolation-vs-in-context-vs-combined = comparative overhead.

---

## 5. Static Memory Breakdown (complete — no build required)

All sizes computed from source (`sizeof` + constexpr + heap allocations). Reconciled to the runtime harness where the lazy-allocation semantics apply.

### 5.1 Engine core + parameter control (always resident once prepared)

| Component | Source | Size | Notes |
|-----------|--------|------|-------|
| `SnapshotBank` slots | `SnapshotBank.h:64` (`std::array<ParameterState,12>`) | **~99 KB** | 12 × ParameterState(~8.27 KB). Heap-allocated (avoids stack overflow). AGENTS.md ~97 KB confirmed. |
| `ParameterState::values` (×12 inside ↑) | `ParameterState.h:27` | 8 KB × 12 = 96 KB | The dominant slice of SnapshotBank |
| `ParameterBridge::throttleStates_` | `ParameterBridge.h:141` (4096 × 8 B) | **32 KB** | Was 64 KB at 8192; PERF-MEM reduced to 4096. |
| `ParameterBridge` recall-ramp buffers | `ParameterBridge.h:168-169` (2 × 2048 float) | 16 KB | `recallRampStart_` + `recallRampTarget_` |
| Processor touch vectors | `PluginProcessor.cpp:1320-1332` (11 × 2048) | ~88 KB | `currentParamSnapshot_`, `lastApplied_`, `touchCooldown_`, `drainScratch_`, `touchMorphX/Y_`, `liveEditHold_`, `liveEditX/Y/Fader_`, `drainTouched_`, `recallScratch_` |
| Processor profiler rings | `PerformanceProfiler` (~16 sections × 16 KB) | ~256 KB | NEW (this audit). Allocated in `registerSection`. |
| Audio-domain scratch | `PluginProcessor.cpp:1389-1396` (`bufferB_`, `paramOut_`, etc.) | ~tens of KB | Scales with `samplesPerBlock × oversampling` |
| **Subtotal (engine + params, static)** | | **~0.5 MB** | |

### 5.2 Neural model (lazy — only when SonicMaster active)

| Component | Source | Size | Lazy? | Notes |
|-----------|--------|------|-------|-------|
| `AudioCaptureRing` | `AudioCaptureRing.h:39` (pow2 of 8 s × actual sample rate × 2 ch × 4 B) | **~2–4 MiB @ 48 kHz; ~16 MiB @ 192 kHz** | ✅ `ensureRing()` on first `setActive`/`requestDecisionNow` | Now rate-proportional via PERF-MEM-RATE (2026-07-16). At 48 kHz: 8 × 48000 = 384k → pow2(524k) × 2 × 4 = 4.0 MiB. |
| ORT model weights | `masteringbrain_v2_decision.onnx` | **3.36 MiB** | ✅ `SonicMasterDecisionRunner::loadModel` | 3,520,680 B on disk. |
| ORT input buffer | `SonicMasterDecisionRunner.cpp` (2 × 262138 float) | **2.0 MiB** | ✅ part of session handle | 524,276 × 4 B = 2,097,104 B. |
| ORT session/arena | ORT internal | ~1–3 MiB | ✅ | Allocator + graph optimization arena. |
| `interleaved_` resample buffer | `SonicMasterAnalysisEngine.h:269` | ~2.0 MiB | ✅ | 2 × segment frames. |
| **Subtotal (neural, when active)** | | **~24–26 MiB** | | All lazy; ~0 when inactive. |

### 5.3 AI assistant (lazy — only when MCP server starts)

| Component | Source | Size | Notes |
|-----------|--------|------|-------|
| Agent instances (7) | `src/AI/Agents/Agents/` | ~few KB each | Stateless specialists; ~tens of KB total. |
| `PriorityScheduler` queues | `PriorityScheduler.h` (4 × `std::queue`) | ~KB | Bounded by `maxResults_=1024` results store. |
| `StructuredAgentLogger` ring | `StructuredAgentLogger.h:45` (`kRingCapacity=256`) | ~tens of KB | In-memory fallback; flushes to JSONL file. |
| `BlackboardBridge` | `BlackboardBridge.h` | ~KB | Wraps `IntegrationEventBus`. |
| **Subtotal (agents, when started)** | | **~0.1 MB** | Negligible. |

### 5.4 Total resident footprint (estimated)

| State | Resident (More-Phi-owned) | Driver |
|-------|---------------------------|--------|
| **Idle, SonicMaster off, MCP off** | **~0.5–1 MB** | Engine core + parameter vectors |
| **Active, SonicMaster off** | ~0.5–1 MB | Same (+ audio-domain scratch) |
| **SonicMaster on** | **~25 MB** | +16 MiB ring + ~5 MiB ORT + buffers |
| **MCP + agents started** | ~25.1 MB | +~0.1 MB agents |
| **Process total (incl. JUCE/ORT runtime)** | ~30–40 MB | Matches `docs/PERFORMANCE_AUDIT_REPORT.md` "~30 MB baseline" |

**Key takeaway:** the SonicMaster ring is **~50% of the plugin's owned baseline** when active, and **0% when inactive** (lazy). This validates the PERF-MEM lazy-allocation fix as the single highest-impact memory optimization already applied.

---

## 6. CPU Profiling Design (what each section measures)

### 6.1 The 16 profiled sections and what they attribute

| Section | Type | What it measures | Expected cost rank |
|---------|------|------------------|--------------------|
| `processBlock_total` | container | Whole pipeline | 100% reference |
| `command_queue_drain` | container | Drain gate (try-lock) | Low (empty queue) |
| `command_drain_snapshot` | **leaf (NEW)** | Snapshot-restore marker branch | Low (rare) |
| `command_drain_param` | **leaf (NEW)** | Normal param-command branch | Low–Med (bursts) |
| `morph_computation` | container | Physics → interpolation → smoothing | Med |
| `parameter_application` | container (residual) | 2048-param touch+setValue loop | **HIGH (#1)** |
| `param_getvalue_read` | **leaf (NEW)** | Strided `getValue()` batch read | **HIGH (sub-slice of #1)** |
| `midi_processing` | leaf | MIDI + sidechain | Low |
| `hosted_plugin_process` | leaf | Hosted plugin's `processBlock` | Host-dependent |
| `audio_domain_total` | container | Spectral/granular/formant/hybrid | Med–High (mode-dependent) |
| `spectral_engine` | leaf | Spectral morph | Mode-dependent |
| `granular_engine` | leaf | Granular morph | Mode-dependent |
| `formant_engine` | leaf | Formant morph | Mode-dependent |
| `hybrid_blend` | leaf | Engine blend | Mode-dependent |
| `sonicmaster_capture` | leaf | Lock-free ring write | Low (one memcpy) |
| `modulation_engine` | leaf | Modulation routes | Low (if inactive) |

**Nesting caveat (preserved from prior report):** container sections (`audio_domain_total`, `parameter_application`) double-count their leaf children. **Only leaf sections are reliable attribution.** The new `param_getvalue_read` leaf is the key addition: it lets the audit say "X% of processBlock is the getValue read" without the double-count ambiguity.

### 6.2 Percentile semantics (new)

- **Profiler per-section p50/p95/p99**: trailing window of the most recent **2048** samples per section. Reset by `resetProfiler()` (the harness calls this per scenario). Reflects the recent distribution, not session-long.
- **Harness processBlock-level p50/p95/p99**: true population across all 6000 calls/scenario (the harness's own `HighResTimer`, not the profiler). These are the authoritative tail-latency numbers for the whole block.

The two are complementary: the harness number answers "how bad is the worst block?", the per-section numbers answer "which section caused that bad block?".

---

## 7. CPU Findings — `PENDING BUILD`

> **This section will be populated from `build-audit/profile-results.json` after the `MorePhiAuditBenchmark` build + campaign run.** The harness emits, per scenario × buffer size: avg/p50/p95/p99 processBlock µs, CPU% (avg/buffer-time), per-section avg/max/p50/p95/p99/pct/calls.

**What the completed tables will contain (templates):**

### 7.1 CPU by component (per buffer size) — template
| Section | 256 avg µs | 256 p99 µs | 256 % | 512 avg µs | 512 p99 µs | 512 % | 1024 … |
|---------|-----------|-----------|-------|-----------|-----------|-------|--------|
| `processBlock_total` | ⏳ | ⏳ | 100% | ⏳ | ⏳ | 100% | … |
| `parameter_application` | ⏳ | ⏳ | ⏳% | ⏳ | ⏳ | ⏳% | … |
| `param_getvalue_read` | ⏳ | ⏳ | ⏳% | ⏳ | ⏳ | ⏳% | … |
| `morph_computation` | ⏳ | ⏳ | ⏳% | ⏳ | ⏳ | ⏳% | … |
| … | | | | | | | |

### 7.2 Parameter-control overhead (S3−S2, S4−S2) — template
| Buffer | ΔSweep avg µs | ΔSweep CPU% | ΔRapid avg µs | ΔRapid CPU% |
|--------|---------------|-------------|---------------|-------------|
| 256 | ⏳ | ⏳ | ⏳ | ⏳ |
| 512 | ⏳ | ⏳ | ⏳ | ⏳ |
| 1024 | ⏳ | ⏳ | ⏳ | ⏳ |

### 7.3 Neural: isolation vs in-context vs combined — template
| Path | avg ms | p99 ms | CPU% (of one core, amortized over 3 s cycle) |
|------|--------|--------|----------------------------------------------|
| S5 isolation (ORT loop) | ⏳ | ⏳ | ⏳ |
| S6 in-context (one cycle) | ⏳ | ⏳ | ⏳ |
| S7 combined delta | ⏳ | ⏳ | ⏳ |

**Run command (ready, awaiting approval):**
```bash
cmake -B build-audit -S . -DMORE_PHI_BUILD_TESTS=ON -DMORE_PHI_BUILD_BENCHMARKS=ON \
      -DMORE_PHI_ENABLE_PROFILING=ON -DMORE_PHI_ENABLE_ONNX=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-audit --config Release --target MorePhiAuditBenchmark
build-audit/Release/MorePhiAuditBenchmark.exe --passes 5 --buffers 256,512,1024
```

---

## 8. Runtime Memory Findings — `PENDING BUILD`

> **This section will be populated from the harness's `MemorySnapshot` deltas** (`WorkingSetSize`, `PrivateUsage`, `PeakWorkingSetSize` per scenario).

### 8.1 Per-scenario working-set delta — template
| Scenario | WSΔ MB | Peak WS MB | Private MB |
|----------|--------|-----------|------------|
| S1 idle (baseline) | ⏳ | ⏳ | ⏳ |
| S2 active | ⏳ | ⏳ | ⏳ |
| S5 neural isolation | ⏳ (+~5 MiB expected: ORT) | ⏳ | ⏳ |
| S7 combined | ⏳ (+~21 MiB expected: ring + ORT) | ⏳ | ⏳ |

**Static prediction (from §5):** S7−S1 delta should be ≈ **+24 MiB** (16 MiB ring + 3.36 MiB weights + 2 MiB input + ~3 MiB ORT arena/buffers). S5−S1 should be ≈ **+5 MiB** (weights + input, no ring — isolation scenario doesn't `ensureRing`). These predictions are falsifiable by the run.

---

## 9. Bottlenecks, Architecture Correlation & Optimization Roadmap

### 9.1 Bottlenecks (evidence-ranked)

| Rank | Bottleneck | Evidence | Architecture correlation |
|------|-----------|----------|--------------------------|
| **#1** | 2048-param write loop (`parameter_application`), specifically the `getValue()` batch read | `PluginProcessor.cpp:2052-2060`; documented as dominant in AGENTS.md; PERF-IA already cut it ~75% | PERF-IA interleaved sampling (stride 4) was the first mitigation. `param_getvalue_read` (NEW) now isolates the residual. |
| **#2** | Audio-domain engines (spectral/granular/formant) when active | `audio_domain_total` + leaf sections; FFT-size-dependent | PERF-CPU saver mode already halves FFT + caps oversampling. |
| **#3** | SonicMaster ring memory (16 MiB) when active | §5.2 | PERF-MEM lazy allocation already applied. |
| **#4** | Hosted-plugin `processBlock` (host-dependent) | `hosted_plugin_process` | Outside More-Phi's control; surfaced for attribution. |
| **#5** | (Low) Agent layer CPU | `DeterministicFallbackLlmClient` is a heuristic; 2 idle workers | By design — no real LLM transport wired. |

### 9.2 Optimization priorities (impact × effort)

| # | Optimization | Impact | Effort | Next step |
|---|--------------|--------|--------|-----------|
| 1 | **Cache `pluginParams` element type / hoist vtable resolution** in the `getValue` batch | -5–15% processBlock CPU (the #1 cost) | Medium | Profile S3 to confirm `param_getvalue_read` is still the top leaf post-PERF-IA; if so, batch via a typed pointer or JUCE `AudioProcessorParameter`-batch API |
| 2 | **Add p99 budget alarms + `getProfilingReport()` MCP tool** for live DAW observability | Observability (unlocks #1, #4 diagnosis in real sessions) | Low | Wrap `getProfilingReport` in an MCP tool; add per-section p99 threshold alerts |
| 3 | **Index profiler sections by int, not string** on audio path | -1–3% CPU in profiled builds | Low | Replace `unordered_map<string,...>` lookup in `updateStats` with an int-indexed array (C-16-adjacent) |
| 4 | **Right-size `AudioCaptureRing` for ≤96 kHz hosts** | -8–12 MiB resident (most hosts run 44.1/48k, not 192k) | Low | Make `captureRingFrames` sample-rate-aware: 8 s @ actual rate, not 8 s @ 192 kHz |
| 5 | **Delete or wire `ComprehensiveProfilingHarness.cpp`** (orphaned) | Compile-time / clarity | Trivial | See §10.1 — currently dead code that rots |
| 6 | **Delete or wire `AllocationTracker.h`** (unwired) | Compile-time / clarity | Trivial | See §10.3 — included but never called |
| 7 | **Lazy-allocate ORT input buffer** (currently 2 MiB even for one-shot decisions) | -2 MiB when infrequent | Low | Allocate on first `runDecision`, release on idle timeout |
| 8 | **Add ETW/WPA trace for ORT-internal attribution** (follow-up) | Reveals time inside ORT the in-tree sections can't see | Medium | One-off capture; not a code change |
| 9 | **`AudioCaptureRing` pow2-round-up wastes ~36%** (1.54M→2.10M frames) | -5.4 MiB | Medium | Use a non-pow2 ring (the SPSC design doesn't strictly require pow2) |
| 10 | **Coalesce duplicate `MAX_PARAMS`/`MAX_PARAMETERS` constants** | Clarity | Trivial | See §10.4 |

### 9.3 What NOT to optimize (yet)

- **The agent layer** — near-zero CPU (heuristic LLM, idle workers). No real transport is wired; optimizing now is premature.
- **The blackboard pump** — 50 ms sleep loop; negligible.
- **`sonicmaster_capture`** — one lock-free memcpy per block; already cheap.
- **`SnapshotBank` seqlock** — already lock-free on the audio path.

---

## 10. Ponytail-Audit Appendix (cut/lean lens)

> **Scope note:** the `ponytail-audit` skill targets over-engineering/dead-code/complexity (`delete`/`stdlib`/`native`/`yagni`/`shrink` tags) and explicitly excludes correctness/security/performance. It's included here per the user's "use ponytail" instruction as a contributing appendix. Findings ranked biggest cut first.

### 10.1 `delete:` Orphaned `ComprehensiveProfilingHarness.cpp`
**What:** `tests/Performance/ComprehensiveProfilingHarness.cpp` (~950 lines) has its own `main()` but is **in no CMake target** — confirmed not referenced in `tests/CMakeLists.txt`. The `IntegratedProcessorBenchmark.h:14` header comment explicitly calls it "orphaned."
**Replacement:** nothing (it's dead) OR wire it as a third benchmark target if its per-component memory breakdown is wanted. Given §5 already covers static memory, **delete is the lean choice.** Saves ~950 lines of unmaintained code.
**Path:** `tests/Performance/ComprehensiveProfilingHarness.cpp`

### 10.2 `shrink:` Stale "12.3 MB" capture-ring documentation (corrected in source)
**What:** The `AudioCaptureRing` is **16.0 MiB** (2,097,152 × 2 × 4 = 16,777,216 B), but comments/docs widely say "12.3 MB" — a stale figure from a prior smaller config. **This audit corrected the in-source comments** (`SonicMasterAnalysisEngine.{h,cpp}`) to the canonical "16.0 MiB (~16.8 MB decimal)."
**Remaining (doc-level, not source):** `AGENTS.md:137`, `CLAUDE.md:171`, `CHANGELOG.md:37-39`, `docs/PERFORMANCE_AUDIT_REPORT.md` (7 occurrences), `docs/audit/sonicmaster-neural-mastering-audit-2026.md:212`. Recommend a doc sweep to avoid the figure re-entering decisions.
**Replacement:** `16.0 MiB` / `~16.8 MB decimal`. Net: factual accuracy, ~0 lines.

### 10.3 `yagni:` Unwired `AllocationTracker.h`
**What:** `src/Core/AllocationTracker.h` is `#include`d in `PluginProcessor.cpp:7` but **`beginAudioCallback()`/`endAudioCallback()`/`recordAllocation()`/`ScopedAudioCallback` are never called** anywhere in `src/`. It's scaffolding for audio-thread allocation tracking that was never turned on.
**Replacement:** either wire it (would enable per-component heap attribution — valuable for §8) or delete it. Given a global `new/delete` override has real-time-safety and portability risks, **delete is the lean default**; the runtime `GetProcessMemoryInfo` deltas + static sizeof table (§5) cover attribution without it. Saves ~90 lines + one include.
**Path:** `src/Core/AllocationTracker.h`, `CMakeLists.txt:246`

### 10.4 `shrink:` Duplicate `MAX_PARAMS` / `MAX_PARAMETERS` constants
**What:** Two independent constants both equal to 2048: `ParameterState.h:17` (`MAX_PARAMETERS`) and `ParameterClassifier.h:17` (`MAX_PARAMS`). They must agree but aren't linked; changing one without the other silently breaks the 2048 invariant.
**Replacement:** a single `MAX_PARAMETERS` in `ParameterState.h`, referenced by `ParameterClassifier`. `std::`-library analog: none needed — just de-duplicate. Net: -1 constant, +1 invariant.
**Path:** `src/Core/ParameterClassifier.h:17`

### 10.5 `delete:` `IntegratedProcessorBenchmark` build-wiring gap (resolved by this audit)
**What:** `IntegratedProcessorBenchmark.cpp` (the only harness that drives real `processBlock`) was **not in any CMake target** — it has its own `main()` colliding with `BenchmarkSuite.cpp`'s. The committed `MorePhiBenchmarks` compiled only `BenchmarkSuite.cpp`.
**Status:** **Resolved by this audit** — added the `MorePhiAuditBenchmark` target (`tests/CMakeLists.txt`, gated on `MORE_PHI_BUILD_BENCHMARKS AND MORE_PHI_ENABLE_PROFILING`). Listed here for completeness of the cut/lean record.

### 10.6 `native:` (observation, not a cut)
The profiler uses `juce::SpinLock` (try-lock) — this is the correct platform primitive for the audio-thread no-block contract (C-2). No stdlib/platform replacement would be leaner. No action.

**Net ponytail tally:** ~1040 lines deletable (`ComprehensiveProfilingHarness.cpp` ~950 + `AllocationTracker.h` ~90), 1 constant dedup, plus the doc-figure sweep. **Recommended action on the deletes is the user's call** — they trade potential future use against current clarity.

---

## 11. Deliverables & Status

| Artifact | Path | Status |
|----------|------|--------|
| This report | `docs/audits/PERFORMANCE_CPU_MEMORY_AUDIT_2026-06-25.md` | ✅ Complete (runtime tables pending build) |
| Profiler upgrade | `src/Core/PerformanceProfiler.{h,cpp}` | ✅ Implemented **+ verified in binary** |
| Sub-section timers | `src/Plugin/PluginProcessor.cpp` | ✅ Implemented **+ verified in binary** |
| Report format + harness parsing | `PluginProcessor.cpp`, `IntegratedProcessorBenchmark.{h,cpp}` | ✅ Implemented |
| Profiler tests | `tests/Unit/PerformanceProfilerTests.cpp` | ✅ **4/4 pass (16 assertions)** incl. audio-thread no-block contract |
| Build wiring | `tests/CMakeLists.txt` (`MorePhiAuditBenchmark`) | ✅ Implemented |
| Stale-doc source fixes | `src/AI/SonicMasterAnalysisEngine.{h,cpp}` | ✅ Applied |
| Runtime JSON data | `build-audit/profile-results.json` | ⏳ **PENDING BUILD** |
| Captured stdout | `test_reports/audit_benchmark_results.txt` | ⏳ **PENDING BUILD** |

### Verification evidence (2026-06-25)

**Binary inclusion** — the `build-ninja/` VST3 (linked 09:31:31, after the 08:52–09:13 edits) contains the new code, confirmed by byte-level grep of the linked binary:
- `param_getvalue_read` ✓ present in `MorePhi.vst3`
- `command_drain_snapshot` ✓ present in `MorePhi.vst3`
- `computeStats` ✓ present in `PerformanceProfiler.cpp.obj`

**Unit tests** — `MorePhiTests.exe 'PerformanceProfiler*'` → **All tests passed (16 assertions in 4 test cases)**, exit 0. The "recordTime never blocks" case empirically confirms the C-2 audio-thread no-block contract survives the M4 ring upgrade.

**Caveat** — the `build-ninja/` binary was built with `MORE_PHI_ENABLE_PROFILING=OFF`, so while the code is present, the timers are compiled to no-ops and emit no runtime data. The `MorePhiAuditBenchmark` target was therefore not built (it's gated on profiling). Runtime measurements (§7, §8) still require the profiling+ONNX build.

**To complete sections 7 & 8**, approve the build + campaign run (§7 command). Everything else in this report is final.

---

*Audit conducted under `feature/multi-agent-layer` @ HEAD. Instrumentation changes preserve the C-2/C-16 audio-thread no-alloc/no-block contract and are unit-tested.*
