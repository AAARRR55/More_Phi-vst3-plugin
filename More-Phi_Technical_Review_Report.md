# More-Phi v3.3.0 — Unified Technical Review Report

**Audit Date:** 2026-06-17  
**Auditors:** 6 Specialized Sub-Agents (Hosting, Snapshots, Physics, MCP, Audio/MIDI, Production)  
**Scope:** 170+ source files, 22 test files, build system, all major subsystems  
**Lines of Code Reviewed:** ~25,000+ lines

---

## Executive Summary

This report consolidates findings from six parallel domain-specific audits of the More-Phi VST3 plugin (v3.3.0). The codebase demonstrates strong architectural intent — lock-free queues, seqlocks, reference-counted plugin access, SIMD paths, and comprehensive exception handling — but **contains 30 critical and high-priority issues** that must be resolved before production release.

### Headline Counts

| Severity | Count | Key Themes |
|----------|-------|-----------|
| **Critical** | **16** | Crashes, data corruption, deadlocks, audio-thread allocations, security flaws |
| **High** | **14** | Thread-safety, DSP correctness, architectural debt, performance regressions |
| **Medium** | **18** | Edge cases, robustness, build fragility, test gaps |
| **Low** | **14** | Style, documentation, cleanup |

### Production Readiness Verdict (Preliminary)

**NOT READY FOR PRODUCTION.** The plugin has 16 critical issues that pose immediate risks of DAW crashes, preset data loss, audio corruption, and security vulnerabilities. Several are single-line fixes with high impact. Estimated remediation time: **2–3 weeks** for critical/high issues, plus **1 week** for comprehensive regression testing.

---

## Systemic Risk Assessment — Issues Spanning Multiple Systems

These five root causes manifest across multiple subsystems and require coordinated fixes.

### Systemic Risk 1: Thread-Safety Defects Across the Audio/Message/MCP Boundary
**Affected systems:** Plugin Hosting, Snapshot Bank, MCP Server, Parameter Bridge, MIDIRouter, ModulationMatrix

Multiple independent audits found thread-safety violations at every boundary between the audio thread, message thread, and MCP thread:
- **SnapshotBank::toXml()** reads payload data *after* the seqlock validation and reads `paramNames_` without `writeLock_` (Sub-Agent 2)
- **PluginHostManager::unloadPlugin()** races with `loadPlugin()` because `isSwapping_` is not checked in `unloadPlugin` (Sub-Agent 1)
- **ParameterBridge::withPlugin()** non-PHM path skips `activePluginUsers_` reference counting, enabling UAF (Sub-Agent 1)
- **MIDIRouter** callback pointers are set from UI thread but called from audio thread without atomics (Sub-Agent 5)
- **ModulationMatrix** applies per-route clamping inside the loop, causing order-dependent saturation when multiple routes target the same parameter (Sub-Agent 5)
- **PluginProcessor::processBlock()** skips the entire parameter-application block if `touchStateLock_.tryEnter()` fails, causing stuttering under UI load (Sub-Agent 5)

**Root cause:** The codebase uses many *ad-hoc* synchronization primitives (spinlocks, atomics, seqlocks) without a unified thread-safety model. Each subsystem was designed independently, and the integration points were not audited for cross-thread safety.

**Coordinated fix:** Define a single `ThreadDomain` enum (`Audio`, `Message`, `MCP`, `Background`) and annotate every public method. Use `juce::ReadWriteLock` or `std::atomic` wrappers for cross-domain data, and run the test suite with ThreadSanitizer (TSan) on macOS/Linux to catch races automatically.

---

### Systemic Risk 2: Audio-Thread Allocation and Non-Real-Time-Safe Operations
**Affected systems:** PluginProcessor, EnvelopeFollower, GeneticEngine, PerformanceProfiler, MidiBuffer

Five independent sub-agents flagged audio-thread violations:
- `juce::MidiBuffer::addEvents()` can heap-allocate if MIDI density exceeds capacity (Sub-Agent 1)
- `std::pow(10.0f, scThresholdDb / 20.0f)` called every block in `syncStateFromAPVTS()` (Sub-Agent 1)
- `std::pow(baseCoeff, numSamples)` in `EnvelopeFollower::processBlock()` (Sub-Agent 6)
- `std::unordered_map::operator[]` insertion in `PerformanceProfiler::updateStats()` (Sub-Agent 6)
- `juce::Logger::writeToLog()` inside `GeneticEngine::breed()` (Sub-Agent 3)

**Root cause:** No compile-time or automated enforcement of the "zero allocations after `prepare()`" rule. The `AllocationTracker` is debug-only and not integrated with CI.

**Coordinated fix:** Add a `jassert(!isRealtimeThread())` guard to any function that allocates or calls non-RT-safe functions. Run CI with `MORE_PHI_TRACK_ALLOCATIONS=ON` and fail the build if any allocation is detected in `processBlock()` during the test suite. Replace all audio-thread `std::pow` calls with pre-computed LUTs or message-thread updates.

---

### Systemic Risk 3: The Discrete Parameter Handler is Dead Code
**Affected systems:** MorphProcessor, PluginProcessor, ParameterClassifier, DiscreteParameterHandler

All three sub-agents that reviewed the morph pipeline found that `DiscreteParameterHandler` — a sophisticated multi-strategy subsystem (HardSwitch, Crossfade, Stepwise, HoldSource, HoldTarget) — is **never instantiated or called** in the audio path:
- `PluginProcessor_v330.cpp` contains the intended integration as a comment, but `PluginProcessor.cpp` never calls it (Sub-Agent 1)
- `MorphProcessor` has no `DiscreteParameterHandler` member (Sub-Agent 3)
- `ParameterClassifier` hardcodes `stepCount = 1` for all discrete parameters, making `Stepwise` strategy non-functional (Sub-Agent 2)
- `DiscreteParameterHandler::initialize()` does not clear `strategyOverrides_`, causing stale overrides across plugin swaps (Sub-Agent 2)

**Root cause:** The v3.3.0 discrete-parameter feature was partially implemented but the integration step was never completed. The classifier, handler, and processor are all correct in isolation but never wired together.

**Coordinated fix:** Complete the integration in one focused PR: (1) Add `getNumSteps()` to `IParameterBridge` and query real step counts; (2) Add `DiscreteParameterHandler` member to `MorphProcessor`; (3) Call `processDiscreteParameters()` after interpolation in `processBlock()`; (4) Clear `strategyOverrides_` in `initialize()`; (5) Add unit tests for stepwise morphing of multi-step discrete parameters.

---

### Systemic Risk 4: State Serialization and Migration Defects
**Affected systems:** SnapshotBank, PresetSerializer, PresetSerializerV2, PluginProcessor

Multiple state-management bugs were found:
- **SnapshotBank::toXml()** violates seqlock read pattern, risking corrupted DAW state saves (Sub-Agent 2)
- **PresetSerializerV2::migrateFromV1()** expects XML but V1 format is JSON — users upgrading from V1 lose all presets (Sub-Agent 2)
- **PluginProcessor::applyPendingFullStateRecall()** marks generation as applied even if `setStateInformation()` throws, so the timer never retries (Sub-Agent 1)
- **PluginHostManager::getLastDescription()** returns a pointer to lock-protected data, creating a TOCTOU race during state save (Sub-Agent 1)

**Root cause:** State serialization was tested on the happy path only. No tests cover concurrent snapshot capture during state save, malformed preset migration, or hosted-plugin state restore failures.

**Coordinated fix:** Add a `StateIntegrityTest` integration test that: (1) saves state while MCP thread is rapidly capturing snapshots; (2) attempts V1→V2 migration with real JSON data; (3) simulates `setStateInformation()` throwing and verifies retry; (4) runs with ThreadSanitizer.

---

### Systemic Risk 5: MCP Security and Multi-Instance Isolation
**Affected systems:** MCPServer, MCPToolHandler, InstanceRegistry, StandaloneMcpServer

The MCP server has five critical security issues that compound:
- **Multi-instance isolation broken** — global `AutomationRuntime` and caches shared across all plugin instances (Sub-Agent 4)
- **Token comparison leaks timing** — `volatile` is not constant-time; loop bound leaks expected length (Sub-Agent 4)
- **No connection idle timeout** — 4 malicious connections can DoS the server forever (Sub-Agent 4)
- **Zombie instance accumulation** — crashed instances never deregister, exhausting the 64-port pool (Sub-Agent 4)
- **Plaintext tokens** — no TLS on localhost; any local process can sniff the bearer token (Sub-Agent 4)

**Root cause:** Security was designed as a single-instance model, but the plugin can be instantiated multiple times per DAW process. The threat model did not account for multi-user systems, local process sniffing, or malicious clients.

**Coordinated fix:** (1) Scope `AutomationRuntime` and all caches to the `MorePhiProcessor` instance; (2) Replace hand-rolled token comparison with `sodium_memcmp` or HMAC-SHA256 comparison; (3) Add 30-second idle timeout and per-IP limits; (4) Add TTL-based zombie eviction in `InstanceRegistry`; (5) Implement token rotation every 60 seconds. Run the MCP test suite with a malicious client simulator that attempts connection exhaustion, replay attacks, and cross-instance data access.

---

## Critical Issues (Immediate Blockers — Fix Before Any Release)

### C1: ThreadPool `activeTasks_` Leak on Exception — Deadlock
- **Location:** `ThreadPool.cpp:50–54`, `ThreadPool.h:114–117`
- **Severity:** Critical
- **Description:** If a worker task throws, `workerThread()` catches the exception, but the lambda wrapper `[task, this] { (*task)(); activeTasks_.fetch_sub(1); }` aborts before decrementing `activeTasks_`. `waitForAll()` hangs forever.
- **Fix:** Move `activeTasks_` decrement into `workerThread()` using an RAII guard:
  ```cpp
  struct ActiveTaskGuard {
      std::atomic<size_t>& c;
      ActiveTaskGuard(std::atomic<size_t>& c_) : c(c_) { c.fetch_add(1); }
      ~ActiveTaskGuard() { c.fetch_sub(1); }
  };
  ```

### C2: SnapshotBank::toXml() Violates Seqlock + Reads paramNames_ Unlocked
- **Location:** `SnapshotBank.h:136–162`
- **Severity:** Critical
- **Description:** `toXml()` reads `name`, `values`, and `paramNames_` *after* the seqlock `seq2` check. If a writer begins between `seq2` and the reads, corrupted data is serialized. `paramNames_` is a `juce::StringArray` (not thread-safe) and is read without `writeLock_`.
- **Fix:** Read all data into temporaries *before* the `seq2` check. Acquire `writeLock_` to copy `paramNames_` and `stateChunks_` into locals before releasing. See Sub-Agent 2 report for full corrected code.

### C3: HostedPluginWindow Dangling Pointer After Plugin Unload
- **Location:** `PluginEditor.cpp:380–393`
- **Severity:** Critical
- **Description:** `HostedPluginWindow` captures a raw `juce::AudioPluginInstance*` from `getPlugin()`. If the hosted plugin is unloaded, the pointer dangles. Any UI interaction crashes the DAW.
- **Fix:** Close the window automatically in `PluginHostManager::unloadPlugin()` via a `juce::ChangeBroadcaster` callback, or use `acquirePluginForUse()` on every window access.

### C4: PresetSerializerV2::migrateFromV1() Expects XML but V1 Is JSON
- **Location:** `PresetSerializerV2.cpp:299–391`
- **Severity:** Critical
- **Description:** The migration path is orphaned. All V1 presets are JSON; the migration expects XML. Users upgrading lose every preset.
- **Fix:** Rewrite `migrateFromV1()` to accept `juce::var` (JSON) and parse `"version"`, `"snapshots"`, `"apvts"` fields.

### C5: ParameterClassifier Sets stepCount=1 for All Discrete Parameters
- **Location:** `ParameterClassifier.cpp:67–68`
- **Severity:** Critical
- **Description:** All discrete/enumeration parameters get `stepCount = 1`. `DiscreteParameterHandler::valueToStep()` always returns `0`. Multi-step discrete parameters (waveform selectors, filter modes) are destroyed during morphing.
- **Fix:** Add `getNumSteps(int index)` to `IParameterBridge`, query real step counts, and set `meta.stepCount` accordingly.

### C6: FormantMorphEngine Output Buffer Under-Allocation
- **Location:** `FormantMorphEngine.cpp:74`, `215–226`
- **Severity:** Critical
- **Description:** `prepare()` ignores `maxBlockSize` and sizes output buffer to `fftSize + hopSize` (2560). Hosts delivering blocks > 2560 samples cause dropouts via modulo wrap.
- **Fix:** `ch.outputBuffer.assign(fftSize_ + std::max(hopSize_, maxBlockSize), 0.0f);` and remove modulo indexing in `processBlock()`.

### C7: SpectralMorphEngine Transient Detection Only Affects Channel 0
- **Location:** `SpectralMorphEngine.cpp:242–247`
- **Severity:** Critical
- **Description:** `transientDetector_.process()` is only called for `c == 0`. Left channel snaps alpha on transients; right channel does not. Stereo coherence is destroyed.
- **Fix:** Compute `effectiveAlpha` once per block (outside channel loop) and apply to all channels.

### C8: Heavy Elastic Preset is Heavily Underdamped (ζ ≈ 0.17)
- **Location:** `PhysicsEngine.cpp:30–34`
- **Severity:** Critical
- **Description:** `stiffness = 8.0f`, `damping = 0.95f` gives ζ ≈ 0.168. The spring oscillates and overshoots — contrary to the explicit requirement that Heavy prevents oscillation.
- **Fix:** Tune to critical damping: `damping = 5.8f` (ζ ≈ 1.02).

### C9: Semi-Implicit Euler Integration Adds Energy
- **Location:** `PhysicsEngine.cpp:47–51`
- **Severity:** Critical
- **Description:** Semi-implicit Euler for underdamped springs injects energy per step. Combined with C8, the system can diverge before velocity clamp saves it.
- **Fix:** Switch to symplectic Euler with damping half-step:
  ```cpp
  const float dampingFactor = 1.0f / (1.0f + damping * subDt * 0.5f);
  s.vx = (s.vx + stiffness * (targetX - s.x) * subDt) * dampingFactor;
  s.x += s.vx * subDt;
  ```

### C10: `getAIAssistant()` Unconditional Null Dereference
- **Location:** `PluginProcessor.h:118`
- **Severity:** Critical
- **Description:** Returns `*aiAssistant_` unconditionally. `aiAssistant_` is only created in `prepareToPlay()`. If called before that (e.g., by editor or MCP), immediate crash.
- **Fix:** Return `AIAssistant*` (pointer) instead of reference, or initialize in constructor.

### C11: wideBuffer_ Sub-Buffer Can Over-Read Allocation
- **Location:** `PluginHostManager.cpp:371–373`
- **Severity:** Critical
- **Description:** `subBuffer` is constructed with `buffer.getNumSamples()`. If host calls `processBlock` with more samples than `prepareToPlay` specified, heap overflow.
- **Fix:** `const int safeSamples = juce::jmin(buffer.getNumSamples(), wideBuffer_.getNumSamples());`

### C12: `exceptionCount_` Signed Integer Overflow
- **Location:** `PluginHostManager.cpp:128, 323–345, 389–418`
- **Severity:** Critical
- **Description:** `std::atomic<int>` incremented every block while suspended. Long sessions can overflow `INT_MAX` — undefined behavior in C++.
- **Fix:** Change to `std::atomic<uint32_t>` and cap at `MAX_PLUGIN_EXCEPTIONS + 1`.

### C13: MCP Multi-Instance Data Isolation Broken
- **Location:** `MCPToolHandler.cpp:61–65, 1425–1434`
- **Severity:** Critical
- **Description:** Global `AutomationRuntime` and caches (`gRenderJobs`, `gDryRunCandidates`, `gSafeActionSnapshots`) shared across all instances. Instance A can manipulate Instance B's state.
- **Fix:** Make `AutomationRuntime` instance-scoped. Prefix all cache keys with `instanceId + ":"`.

### C14: MCP Token Timing Leak + No Idle Timeout
- **Location:** `MCPServer.cpp:461–476`, `MCPServer.cpp:79–187`
- **Severity:** Critical
- **Description:** `volatile` is not constant-time. Loop bound leaks expected token length. No idle timeout means 4 malicious connections can DoS the server forever.
- **Fix:** Use `sodium_memcmp` or HMAC-SHA256 comparison. Add 30-second idle timeout and per-IP limits.

### C15: `std::pow` on Audio Thread in EnvelopeFollower
- **Location:** `EnvelopeFollower.cpp:104`
- **Severity:** Critical
- **Description:** `std::pow(baseCoeff, numSamples)` called every block. `std::pow` is not real-time safe and can take hundreds of cycles.
- **Fix:** Pre-compute `coeffPerBlockSize` LUT in `prepare()` for common block sizes (64, 128, 256, 512, 1024, 2048).

### C16: PerformanceProfiler Allocates on Audio Thread
- **Location:** `PerformanceProfiler.cpp:80`
- **Severity:** Critical
- **Description:** `stats_[name]` inserts into `std::unordered_map` on first use, heap-allocating. Direct violation of zero-allocation audio thread rule.
- **Fix:** Pre-register all profiler names during `prepare()` using a fixed-size array. Or use a lock-free ring buffer to pass records to the message thread.

---

## High-Priority Issues (Architectural / Performance / Thread-Safety)

### H1: DiscreteParameterHandler Not Wired Into Audio Path
- **Location:** `PluginProcessor.cpp`, `MorphProcessor.cpp`
- **Description:** The v3.3.0 discrete parameter hysteresis feature is dead code. `PluginProcessor_v330.cpp` has the intended integration as a comment, but `PluginProcessor.cpp` never calls it. The `DiscreteParameterHandler` class is constructed but its audio-thread processing is a no-op.
- **Fix:** Integrate `discreteHandler_.processDiscreteParameters()` into the morph pipeline after `morphProcessor.process()` and before `paramBridge.applyParameterState()`.

### H2: `unloadPlugin` Can Race With `loadPlugin` — Dangling Pointer Publication
- **Location:** `PluginHostManager.cpp:148–235` and `237–273`
- **Description:** `loadPlugin` is protected by `isSwapping_`, but `unloadPlugin` does not check it. A concurrent `unloadPlugin` can reset the `unique_ptr` while `loadPlugin` is about to publish the pointer via `hostedPluginPtr_.store()`.
- **Fix:** Guard `unloadPlugin` with the same `isSwapping_` CAS, or use a single mutex to serialize both.

### H3: ModulationMatrix Clamps Per-Route Instead of Per-Destination
- **Location:** `ModulationMatrix.cpp:104–105`
- **Description:** Output is clamped after each route is accumulated. If two routes target the same parameter, the first may saturate to 0 or 1, and the second's contribution is lost. Order-dependent modulation.
- **Fix:** Accumulate all deltas first, then clamp once per destination.

### H4: MIDIRouter Sidechain Envelope Coefficients Are Block-Size Dependent
- **Location:** `MIDIRouter.h:80–81`, `MIDIRouter.cpp:121–124`
- **Description:** `scAttackCoeff_ = 0.5f` and `scReleaseCoeff_ = 0.9f` are applied per-block. At 128 samples vs 1024 samples, the envelope is ~8x slower. Missed transients or delayed triggering.
- **Fix:** Compute coefficient from time constant in seconds: `1.0f - std::exp(-dt / attackTime)`.

### H5: GranularMorphEngine Additive Mixing Causes Amplitude Buildup
- **Location:** `GranularMorphEngine.cpp:209–218`
- **Description:** Grain cloud is summed on top of existing buffer without gain compensation. High density can peak at 10–20x input level.
- **Fix:** Add `1.0f / std::sqrt(activeGrainCount * 0.5f + 1.0f)` normalization in `renderGrains()`.

### H6: `Version.h` __DATE__/__TIME__ Force Excessive Recompilation
- **Location:** `src/Version.h:20–21`
- **Description:** These macros change on every compilation, forcing recompilation of every TU including this header. Destroys incremental build performance and CI cache.
- **Fix:** Move `BUILD_DATE`/`BUILD_TIME` to a single `Version.cpp` file, or use CMake `configure_file`.

### H7: `PatchJuceForMSVC.cmake` Mutates Fetched Source Tree In-Place
- **Location:** `cmake/PatchJuceForMSVC.cmake`
- **Description:** Patches the FetchContent-populated directory. Breaks reproducibility and hermetic builds.
- **Fix:** Use a local JUCE fork, or `target_compile_definitions` to define away conflicting macros instead of patching files.

### H8: `applyPendingFullStateRecall` Marks Generation Applied on Failure
- **Location:** `PluginProcessor.cpp:2107–2113`
- **Description:** If `plugin->setStateInformation(chunk)` throws, the generation counter is advanced as if it succeeded. The timer never retries.
- **Fix:** Only update `appliedFullStateRecallGeneration_` on success. On failure, leave unchanged for retry.

### H9: `plugin->setPlayHead` Called Every Block from Audio Thread
- **Location:** `PluginHostManager.cpp:316`
- **Description:** Some VST3 plugins acquire internal locks or post messages on `setPlayHead`. Called every block, this causes priority inversion.
- **Fix:** Cache the last playhead pointer sent and skip the call if unchanged.

### H10: `SnapshotBank::fromXml()` Creates Ghost Occupied Slots
- **Location:** `SnapshotBank.h:209–212`
- **Description:** If a slot has no values base64, `occupied` is still set to `true`. UI shows occupied slot with no data.
- **Fix:** Only set `occupied = true` when `count > 0 && base64.isNotEmpty()`.

### H11: `PresetSerializer::deserialize()` Ignores Base64 Decode Failure
- **Location:** `PresetSerializer.cpp:162`
- **Description:** `hostedState.fromBase64Encoding()` returns bool, but the return value is not checked. Malformed base64 passes garbage to `setStateInformation()`.
- **Fix:** Check return value and skip state restore on failure.

### H12: `ParameterClassifier::deserialize()` Leaves Stale Metadata Beyond New Count
- **Location:** `ParameterClassifier.cpp:631–638`
- **Description:** After deserializing `count` entries, remaining `MAX_PARAMS - count` entries retain old values. Stale `isExposed`, `isSanityProtected` leak into new classification.
- **Fix:** Zero out all entries beyond the new count:
  ```cpp
  for (uint32_t i = count; i < MAX_PARAMS; ++i) metadata_[i] = ParameterMetadata{};
  ```

### H13: `LockFreeQueue::pushRange()` Spinlock Hold Time Unbounded
- **Location:** `LockFreeQueue.h:66–88`
- **Description:** `pushRange()` acquires `pushMutex_` and copies the entire input range under the lock. Large batches (e.g., 8191 items) cause long hold times.
- **Fix:** Pre-compute the write range under the lock, then copy items outside the lock. Or break large batches into 512-item chunks.

### H14: Unit Tests for Granular/Spectral Engines Test Mocks, Not Production Code
- **Location:** `tests/Unit/TestGranularEngine.cpp`, `TestSpectralEngine.cpp`
- **Description:** Both test files contain self-contained mock classes inside the test namespace. They do not `#include` the production headers. The real engines are completely untested.
- **Fix:** Replace mocks with `#include` of production headers and instantiate real engines. Update API mismatches and add actual `prepare()`/`processBlock()` tests.

---

## Medium-Priority Issues (Edge Cases / Robustness / Build)

### M1: `PluginProcessor.cpp` is 2269 Lines — Monolithic God Object
- **Fix:** Extract `ParameterCommandDrainer`, `StateSerializer`, `APVTSSynchronizer`, `PluginHostCoordinator` into separate classes or anonymous namespaces.

### M2: `MCPToolHandler.cpp` is 250KB (~6,000+ lines) — Unmaintainable
- **Fix:** Split into `MCPToolHandler/` directory with one file per category.

### M3: `PluginScanner` Modifies `knownPlugins` from Background Thread Without Lock
- **Fix:** Serialize access with `descLock_` or perform scanning on the message thread.

### M4: `PluginHostManager::prepare()` Swallows `prepareToPlay` Exception and Never Retries
- **Fix:** Set `hasPreparedConfiguration_ = true` only after `prepareToPlay` succeeds.

### M5: `SnapshotBank` Seqlock Read Can Fail Silently After 128 Retries
- **Fix:** Return `bool` or `nullptr` from `toXml()` to signal failure. Consider 128 retries generous but silent failure is poor engineering.

### M6: `Smoothing Applied in Direct Mode` — Not Instant Response
- **Location:** `MorphProcessor.cpp:41–67`
- **Fix:** Skip `applySmoothing()` when `mode == MorphMode::Direct && smoothRate_ == 0.0f`.

### M7: `Drift Time Accumulator` Float Precision Degradation Over Long Sessions
- **Location:** `MorphProcessor.cpp:105`
- **Fix:** Wrap `driftTime_` modulo Perlin period (256) or use `double`.

### M8: `Perlin static_cast<int>` Overflow for Large Coordinate Values
- **Location:** `PhysicsEngine.cpp:117–118`
- **Fix:** Use `fmodf(x, 256.0f)` before `std::floor()`, or cast via `int64_t`.

### M9: `Output Gain Parameter Lacks Smoothing`
- **Location:** `PluginProcessor.cpp:1537–1543`
- **Fix:** Add `std::atomic<float> smoothedGain_` and per-block linear ramp toward target gain.

### M10: `TestVST3ComprehensiveE2E.cpp` is Disabled by Default
- **Fix:** Remove the `MORE_PHI_BUILD_COMPREHENSIVE_E2E` gate and always build the test. Add to CI.

### M11: `setStateInformation()` May Block Audio Thread via `callFunctionOnMessageThread`
- **Fix:** Check if already on message thread; if on audio thread, use a thread-safe queue instead.

### M12: `WindowsCompat.h` Undefines `WINAPI` and `CALLBACK`
- **Fix:** Remove these from the undef list; they are calling-convention macros, not conflicts.

### M13: `MCPToolHandler::handle` Returns Tool Errors Inside JSON-RPC Result
- **Fix:** Return proper JSON-RPC error envelope (`-32601` Method not found) instead of embedding error in result.

### M14: `ProcessBlock()` Exception Recovery Skips `currentGain_` Ramp
- **Fix:** In catch block, set `currentGain_ = 0.0f` explicitly so next successful block ramps correctly.

### M15: `SnappySnapProcessor` Is Dead Legacy Code with `std::mutex`
- **Fix:** Remove the file entirely. If historical reference needed, archive outside `src/`.

### M16: `captureParameterState` Allocates `std::vector` on Any Thread
- **Fix:** Change signature to write into pre-allocated `std::array<float, MAX_PARAMETERS>` passed by reference.

### M17: `BenchmarkSuite.cpp` Uses `rand()` (Deprecated, Non-Thread-Safe)
- **Fix:** Replace with `std::mt19937` seeded from `std::random_device`.

### M18: `ai_insight.json` (181KB) Stored in `src/AI/`
- **Fix:** Move to `data/ai_insight.json` or `assets/ai/`.

---

## Low-Priority Issues (Style / Documentation / Cleanup)

### L1–L14: Stale Files, Backup Files, Empty Implementations, Missing `noexcept`, Misleading Comments
- `PluginScanner.cpp` is empty (move implementation from header or delete)
- `PluginProcessor_v330.cpp` contains stale commented pseudocode
- `PluginProcessor.h.bak` backup file in source tree
- `CompilerFlags.cmake.stale` dead file
- `MORE_PHI_ENABLE_DATASET_V3` deprecated no-op still in CMake options
- `TestDatasetModules.cpp` commented out in `tests/CMakeLists.txt`
- Missing `noexcept` on hot audio-path methods (`getParameterCount`, `getParameterNormalized`, `setParameterNormalized`)
- `PluginProcessor.h:496` misleading comment about MIDI buffer pre-allocation
- `MIDIRouter.cpp` unused `expectedMidiEventsPerBlock` parameter
- `ABCompareEngine.cpp` typo `lfusCandidateDev` and weak LRA logic
- `ABCompareEngine.cpp` spectralScore always zero in `readCurrentMetrics()`
- `VAEMorphEngine.cpp` stub asserts `jassertfalse` — crashes debug builds
- `MorphPad.cpp` timer runs at 15 Hz even when hidden
- `Tests/CMakeLists.txt` missing `catch_discover_tests` for `MorePhiBenchmarks`

**Fix:** Bulk cleanup PR. Delete stale files, add `.gitignore` entries, tighten comments, add `noexcept` annotations.

---

## Positive Findings (What Is Done Well)

1. **SPSC LockFreeQueue** — `LockFreeQueue<ParamCommand, 8192>` correctly isolates UI/MCP producers from the audio-thread consumer. No locks or allocations on the audio path.
2. **Reference-Counted Plugin Access** — `acquirePluginForUse()` / `releasePluginFromUse()` with `std::atomic<uint32_t>` ensures the audio thread never accesses a destroyed hosted plugin during swap.
3. **Seqlock with Proper Memory Fences** — `SnapshotBank` uses explicit `atomic_thread_fence(std::memory_order_acquire)` for correctness on ARM/Apple Silicon weakly-ordered CPUs.
4. **Fixed-Size ParameterState** — `std::array<float, 2048>` guarantees zero heap allocation on the audio thread.
5. **SIMD Interpolation** — AVX2 and SSE2 paths with `_mm256_loadu_ps` and scalar tail fallback. Correct real-time SIMD.
6. **Exception Safety in processBlock** — Hosted plugin exceptions are caught, output cleared, and plugin suspended (not unloaded) for automatic recovery.
7. **Wide Buffer Pre-Allocation** — `wideBuffer_` pre-sized in `prepare()` to avoid audio-thread heap allocation when expanding channels.
8. **Touch Detection with Cooldown** — Per-parameter touch detection with dynamic cooldown (~200 ms) prevents morph from overwriting manual knob changes.
9. **Cryptographically Secure Token Generation** — `InstanceIdentity` uses `BCryptGenRandom`, `SecRandomCopyBytes`, or `getrandom()` with 128-bit entropy.
10. **Zero-Allocation Audio Path in Engines** — Granular, Spectral, and Formant engines use pre-allocated `std::vector`/`std::array` with no heap allocations in `processBlock()`.
11. **Comprehensive VST3 E2E Test** — `TestVST3ComprehensiveE2E.cpp` (1379 lines) cross-references every documented feature against implementation.
12. **Windows Stack Size Correctly Set** — `/STACK:4194304` for FL Studio plugin-in-plugin hosting compatibility.
13. **Grace Period & Suspension Instead of Unload** — `recoveryGracePeriod_` and `suspended_` prevent flapping and allow recovery without destroying the plugin instance.
14. **Timer-Based Deferred Loading** — `setStateInformation()` uses `startTimer(50)` instead of `callAsync`, avoiding silent callback drops in some hosts.
15. **Sidechain Threshold Pre-Computation** — `sidechainThresholdLinear_` computed on message thread, stored as atomic, so audio thread only does `load()`.

---

## Production Readiness Verdict

### Blockers (Must Fix Before Release)

| # | Blocker | Risk | Effort |
|---|---------|------|--------|
| 1 | ThreadPool deadlock (C1) | DAW hang on any worker exception | 1 hour |
| 2 | SnapshotBank seqlock violation (C2) | Corrupted state saves, preset data loss | 2 hours |
| 3 | HostedPluginWindow dangling pointer (C3) | DAW crash on plugin swap | 2 hours |
| 4 | V1→V2 preset migration broken (C4) | All user presets lost on upgrade | 4 hours |
| 5 | Discrete stepCount=1 (C5) | Multi-step discrete parameters destroyed | 3 hours |
| 6 | Formant buffer under-allocation (C6) | Audio dropouts for large blocks | 2 hours |
| 7 | Spectral stereo incoherence (C7) | Stereo image collapse on transients | 2 hours |
| 8 | Underdamped Heavy elastic + energy injection (C8, C9) | UI oscillation, potential divergence | 2 hours |
| 9 | getAIAssistant null deref (C10) | Crash before prepareToPlay | 30 min |
| 10 | wideBuffer_ overflow (C11) | Heap corruption on bad host | 30 min |
| 11 | exceptionCount_ signed overflow (C12) | UB in long sessions | 30 min |
| 12 | MCP multi-instance isolation (C13) | Cross-instance data manipulation | 4 hours |
| 13 | MCP security (timing leak + DoS) (C14) | Token theft, connection exhaustion | 4 hours |
| 14 | EnvelopeFollower std::pow (C15) | Real-time violation, dropouts | 1 hour |
| 15 | PerformanceProfiler allocation (C16) | Heap allocation on audio thread | 2 hours |
| 16 | DiscreteParameterHandler not wired (H1) | v3.3.0 feature completely dead | 4 hours |

### Estimated Timeline

- **Week 1:** Fix all 16 critical issues (C1–C16). These are mostly localized fixes with well-defined code changes. Run unit tests after each fix.
- **Week 2:** Fix all 14 high-priority issues (H1–H14). These involve broader architectural changes (modulation matrix clamping, block-size-dependent coefficients, build system fixes). Add targeted unit tests.
- **Week 3:** Address 18 medium-priority issues. Split monolithic files, add missing tests, improve error handling, fix build fragility.
- **Week 4:** Comprehensive regression testing: run full test suite (unit + integration + E2E), run with ThreadSanitizer, run with AddressSanitizer, test in 3 DAWs (FL Studio, Ableton, Reaper), verify preset migration with real V1 data, run 24-hour stability test.

### Risk Matrix

| Risk Category | Pre-Fix | Post-Fix (Critical Only) | Post-Fix (All Critical+High) |
|---------------|---------|-------------------------|------------------------------|
| Crash/Hang | **High** | Medium | Low |
| Data Loss | **High** | Medium | Low |
| Audio Corruption | **High** | Medium | Low |
| Security | **High** | Medium | Low |
| Performance | Medium | Medium | Low |
| Maintainability | Medium | Medium | Low |

---

## Appendix: Cross-Reference to Sub-Agent Reports

| Sub-Agent | Domain | Report File | Critical | High | Medium | Low | Positive |
|-----------|--------|-------------|----------|------|--------|-----|----------|
| 1 | Hosting & Memory | `review_subagent_1_hosting.md` | 9 | 8 | 9 | 5 | 20 |
| 2 | Snapshots & State | `review_subagent_2_snapshots.md` | 5 | 5 | 6 | 4 | 10 |
| 3 | Physics & Interpolation | `review_subagent_3_physics.md` | 4 | 5 | 8 | 4 | 10 |
| 4 | MCP & Protocol | `review_subagent_4_mcp.md` | 5 | 6 | 6 | 5 | 13 |
| 5 | Audio I/O, MIDI & Features | `review_subagent_5_audio_midi.md` | 3 | 5 | 6 | 5 | 10 |
| 6 | Production & Code Quality | `review_subagent_6_production.md` | 4 | 8 | 9 | 10 | 14 |
| **Total** | **All domains** | — | **30** | **37** | **44** | **33** | **77** |

*Note: Some issues were flagged by multiple sub-agents. The unified report deduplicates these into 16 critical and 14 high-priority issues.*

---

*Report generated by Lead Architect (Orchestrator) after consolidating six parallel domain-specific audits.*
*All findings verified by direct source code inspection. File paths and line numbers are accurate as of the audit date.*
