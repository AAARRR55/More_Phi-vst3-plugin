# Changelog

All notable changes to More-Phi are recorded here. This file is maintained
alongside the audit documents in `docs/audits/` and the production-readiness
gates in `docs/validation/`. Format is loosely Keep-a-Changelog; entries are
grouped by date with severity tags.

## 2026-07-01 — Audio-thread allocation elimination & memory leak fixes (branch `morph-audit-fixes`)

Eliminated the last remaining heap allocation path on the audio thread and fixed two memory leak risks. All changes are performance-only — bit-identical audio output.

### Bypass crossfade scratch pre-allocation (RT-SAFETY)
- **[HIGH] `wetGainScratch_`/`dryGainScratch_` pre-allocation** — These `std::vector<float>` members were previously `resize()`d inside `applyOutputGainAndMetering()` on every bypass transition, which could heap-allocate if the block size exceeded the vector's capacity. Now pre-allocated to `samplesPerBlock` in `prepareToPlay()` and never resized on the audio thread. The old comment claiming "stack-allocated" was incorrect (`std::vector` always uses the heap) and has been updated. `src/Plugin/PluginProcessor.{h,cpp}`.

### Memory leak fixes (MEM-LEAK)
- **[MED] `PluginHostManager` destructor bounded wait + force-drain** — If audio-thread leases were never released, plugins in the deferred doom queue would leak until process exit. The destructor now does a bounded 100ms wait for leases to release, then calls `drainDeferredDoomedPlugins(true)` (new `force` parameter) to destroy any remaining deferred plugins. `src/Host/PluginHostManager.{h,cpp}`.
- **[LOW] `MCPServer::ConnectionThread` stop timeout** — `stopThread(-1)` (infinite wait) changed to `stopThread(5000)` (5s timeout) in `ConnectionThread::~ConnectionThread()`. Prevents destructor hangs on stuck sockets while still giving the thread time to exit cleanly. `src/AI/MCPServer.cpp`.

### Smart Disable state tracking (INFRA)
- **`isActive_` flag** — Added `bool isActive_` member to `MorePhiProcessor`, set true at end of `prepareToPlay()`, false at end of `releaseResources()`. Provides infrastructure for future lightweight Smart Disable optimization. The `SonicMasterAnalysisEngine::setActive()` already handles pause/resume efficiently (no thread join/re-spawn). `src/Plugin/PluginProcessor.{h,cpp}`.

### Documentation updates
- **Updated:** `AGENTS.md` — Added PERF entries for bypass scratch pre-allocation, Smart Disable state tracking, PluginHostManager leak fix, MCP stop timeout.
- **Updated:** `AUDIT_VST3_COMPLIANCE_2026-06-30.md` — Added finding 1.5 (bypass scratch pre-alloc), 10.4 (deferred doom force-drain), 10.5 (MCP stop timeout). Updated Section 14 summary.
- **Updated:** `AUDIT_PONYTAIL.md` — Added finding 11 (withdrawn `std::array` → `std::vector` for scratch arrays).
- **Updated:** `docs/PERFORMANCE_AUDIT_REPORT.md` — Added Batch 2 fixes (2026-07-01), updated Executive Summary.
- **Updated:** `docs/ARCHITECTURE.md` — Added `isActive_` concurrency primitive, new "Real-Time Safety Guarantees" section.

## 2026-07-16 — Documentation overhaul & comprehensive audit

- **New:** `VST3_TECHNICAL_AUDIT_AND_MARKET_ANALYSIS.md` — 39 KB comprehensive
  audit with 8-criterion ratings (7.9/10 overall), 5-competitor analysis,
  market positioning, 26 verifiable claims with exact code locations.
  Based on direct review of ~165 source files across all 7 layers.
- **Updated:** `README.md` — Complete rewrite: features expanded from 10 to
  60+, architecture tree updated, MCP tools documented as 30+ across 8
  categories, technical highlights table added with 9 verifiable claims,
  installation/build instructions clarified, troubleshooting expanded.
- **Updated:** `docs/API_REFERENCE.md` — Added audit score reference and
  expanded tool category overview.
- **Updated:** `docs/TECHNICAL_DOCUMENTATION.md` — Updated date, added
  audit reference, expanded multi-agent description.
- **Updated:** `docs/ARCHITECTURE.md` — Updated date, added audit score.
- **Updated:** `docs/PERFORMANCE_AUDIT_REPORT.md` — Added cross-reference
  to comprehensive audit.

## 2026-07-16 — Performance audit fixes (branch `chore/perf-audit-fixes`)

Comprehensive CPU/memory performance audit (`docs/PERFORMANCE_AUDIT_REPORT.md`)
identified 8 bottlenecks. All 5 priority fixes applied.

### Parameter application (CPU: ~75% reduction in getValue cost)
- **[HIGH] Interleaved touch sampling (PERF-IA)** — `kTouchSamplingStride=4`,
  rotating `touchSamplingPhase_`. Only 1/4 params call `getValue()` per block.
  Touch detection gated to sampled params; cooldowns + setValue still run for
  all. `src/Plugin/PluginProcessor.{h,cpp}`.

### SonicMaster memory (MEM: up to ~16 MiB saved when inactive)
- **[MED] Lazy + rate-proportional capture ring (PERF-MEM)** — `ensureRing()` defers
  `AudioCaptureRing` to first `setActive(true)`, `requestDecisionNow`,
  or `runCycle()`. Ring size is rate-proportional: `round(8.0 × sampleRate)`,
  clamped `[2×44100, 32×192000]`, pow2-rounded — ~4 MiB at 44.1 kHz, ~16 MiB at
  192 kHz. `prepare()` resets ring to nullptr. Saves ~25–60% of More-Phi's
  baseline memory when the feature is off.
  `src/AI/SonicMasterAnalysisEngine.{h,cpp}`.

### ParameterBridge memory (MEM: ~64 KB saved)
- **[LOW] throttleStates_ reduction (PERF-MEM)** — Reduced from 8192 to 4096
  entries. All bounds checks use `.size()` — safe.
  `src/Host/ParameterBridge.cpp`.

### Profiling infrastructure (tooling)
- **[CRITICAL] Profiler initialization bug fixed** — `PerformanceProfiler` was
  silently broken: `registerSection()` was never called, so all
  `MORE_PHI_PROFILE` timing data was dropped. Now 13 sections are registered in
  `prepareToPlay()`: `processBlock_total`, `command_queue_drain`,
  `midi_processing`, `morph_computation`, `modulation_engine`,
  `parameter_application`, `hosted_plugin_process`, `sonicmaster_capture`,
  `audio_domain_total`, `spectral_engine`, `granular_engine`, `formant_engine`,
  `hybrid_blend`. `src/Plugin/PluginProcessor.cpp`.
- **[LOW] Profiling coverage gaps filled** — Added `MORE_PHI_PROFILE`
  instrumentation for `midi_processing`, `hosted_plugin_process`,
  `sonicmaster_capture`, and `modulation_engine`.
  `src/Plugin/PluginProcessor.cpp`.

### Audio-domain engines (CPU: ~40-60% reduction when enabled)
- **[MED] CPU Saver mode (PERF-CPU)** — New `cpuSaver` APVTS bool parameter
  (default OFF). When enabled: halves FFT size (min 512), caps oversampling
  at ×2. Applied in both `prepareToPlay` and `syncStateFromAPVTS`.
  `src/Plugin/PluginProcessor.{h,cpp}`.

### Documentation
- **New:** `docs/PERFORMANCE_AUDIT_REPORT.md` — Full CPU/memory audit (24 KB).
- **New:** `tests/Performance/ComprehensiveProfilingHarness.cpp` — Automated
  profiling harness covering all components across buffer sizes (43 KB).
- **Updated:** `AGENTS.md`, `CLAUDE.md` — New PERF conventions documented.

## 2026-07-15 — Multi-agent orchestration audit fixes (branch `chore/audit-fixes`)

A comprehensive technical audit of the multi-agent orchestration layer found
11 issues (2 critical/high, 4 high/medium, 5 low). All fixed. Full test suite
passes (700 tests).

### Audio pipeline / thread safety
- **[HIGH] Command drain try-lock drops parameter writes (C-3)** — the audio
  thread's `commandConsumerLock_` try-lock gated both the command queue drain
  AND the morph-to-parameter application. When the assistant flush path held
  the lock, morph output was silently dropped for entire blocks. **Fix:**
  separated `canDrainCommands` (gated by try-lock) from morph application
  (always proceeds). `liveEditHold_` reads moved inside `hasTouchLock` guard
  to prevent data race with concurrent assistant flush.
  `src/Plugin/PluginProcessor.{h,cpp}`.
- **[MED-HIGH] APVTS state restore silently skipped (H-3)** — DAWs that wrap
  state XML in an extra element caused APVTS parameters to not restore, with
  only a `DBG` message (invisible in Release). **Fix:** recursive search for
  the APVTS state element via `std::function`.
  `src/Plugin/PluginProcessor.cpp`.
- **[MED] getStateInformation blocks on audio thread (H-4)** —
  `beginExclusivePluginUse(500)` called unconditionally from
  `getStateInformation`, which some DAWs invoke from the audio thread during
  offline render. **Fix:** detect calling thread; only block on message thread;
  fall back to buffered `pendingHostedState_` on audio thread.
  `src/Plugin/PluginProcessor.cpp`.

### Agent layer
- **[HIGH] BlackboardBridge::publish() discards eventId (C-2)** — returned
  empty string instead of the generated event ID, breaking event tracing for
  all agent blackboard events. **Fix:** capture `ev.eventId` before
  `std::move`, return it. `src/AI/Agents/Blackboard/BlackboardBridge.cpp`.
- **[MED-HIGH] PriorityScheduler O(n log n) starvation bump + std::mutex
  (H-1, M-4)** — single `std::priority_queue` forced O(n log n) drain+rebuild
  for starvation promotion under mutex; starvation guard was 5000ms. **Fix:**
  replaced with 4 per-priority-level `std::queue` instances. Push, pop, and
  starvation promotion are all O(1). Guard reduced to 1000ms.
  `src/AI/Agents/Scheduler/PriorityScheduler.{h,cpp}`.
- **[LOW-MED] license refresh detached thread race (L-5)** — `juce::Thread::launch`
  lambda captured raw `LicenseManager*`; could dangle if processor destroyed
  during refresh. **Fix:** `licenseManager_` changed to `shared_ptr`; lambda
  captures shared_ptr copy to extend lifetime.
  `src/Plugin/PluginProcessor.{h,cpp}`.

### Snapshot bank
- **[MED] findParameterIndex blocking lock on audio path (M-7)** —
  `SnapshotBank::findParameterIndex` used blocking `ScopedLockType`; called
  during MIDI-triggered recall on audio thread. **Fix:** changed to
  `ScopedTryLockType`; returns -1 if lock contended (caller falls back to
  index-based recall). `src/Core/SnapshotBank.h`.

### Code quality
- **[LOW] Duplicate finalOutput_.resize() removed (L-3)**
- **[LOW] readRawBool >=0.5f threshold documented (L-1)**
- **[LOW-MED] pendingStateRestore_ consumer consolidated (H-5)** — processBlock
  now uses `requestMessageThreadMaintenance()` instead of directly starting timer.

## 2026-06-19 — AI/MCP re-audit + DSP verification (branch `chore/ponytail-dead-code-cleanup`)

A focused VST3 + AI-MCP-integration re-audit closed the residual isolation,
verification, and documentation gaps left by the 2026-06-18 audit. Full suite
now at **520 test cases / 87,445 assertions** (all green, up from 505/87394).

### Security / Multi-instance isolation
- **[HIGH] ToolResultCache cross-instance read-through** — the process-wide
  shared read-only tool-result cache now namespaces keys by `instanceId`, so
  one plugin instance can no longer read another's cached `get_plugin_info`
  (which embeds `instanceId`/`port`/`morphCode`). `src/AI/ToolResultCache.{h,cpp}`,
  `src/AI/MCPToolHandler.cpp`.
- **[HIGH] AsyncToolExecutor enumerable job IDs** — async job IDs were a bare
  global counter (`async_N`) any MCP client could enumerate to poll another
  instance's jobs. Job IDs are now prefixed with the submitting instance's
  `morphCode`. `src/AI/AsyncToolExecutor.{h,cpp}`, `src/AI/MCPToolHandler.cpp`.
- **[LOW] validateAuth timing** — documented the token-length pre-check leak
  as acceptable (fixed-size public token format) with a note for future
  variable-length tokens. `src/AI/MCPServer.cpp`.

### DSP correctness & verification
- **[HIGH] Latency / PDC** — verified that `setLatencySamples()` already sums
  all four components (hosted plugin + oversampling + FFT window + mastering-
  chain lookahead). Added 3 pinning tests. `tests/Unit/TestDSPQuality.cpp`.
- **LUFSMeter** — K-weighting coefficients verified to match ITU-R BS.1770-4
  Annex 1 Table 1 literally; end-to-end momentary matches analytic prediction
  at 7 frequencies to 0.001 dB; absolute + relative gating tested. New file
  `tests/Unit/TestLUFSMeter.cpp`. Per-instance channel weights added
  (BS.1770-4 Ls/Rs = 1.41; stereo-preserving).
- **AdaptiveEQ** — steady-state gain verified against RBJ cookbook `|H(f)|`
  at filter centres to 0.001 dB (peak/low-shelf/high-shelf/LP/HP). New file
  `tests/Unit/TestAdaptiveEQ.cpp`.
- **TruePeakEstimator** — characterized vs an independent Kaiser-windowed 4×
  reference; refuted the prior "±0.2 dBTP" header claim (12-tap prototype
  under-reads near-Nyquist by ~25 dB); header corrected; behaviour pinned as
  regression guards. `tests/Unit/TestTruePeakEstimator.cpp`,
  `src/Core/TruePeakEstimator.h`.
- **GranularMorphEngine** — H5 normalization re-derived from Hann² mean-square
  (`1/sqrt(0.375·N)`) with the average-vs-instantaneous limitation documented;
  peak-gain-vs-density tests added. `src/Core/GranularMorphEngine.cpp`,
  `tests/Unit/TestGranularEngine.cpp`.
- **EnvelopeFollower / ModulationMatrix** — promoted from inspection-verified
  to tested (block-size-independent time constants; order-independent
  accumulate-then-clamp). New file `tests/Unit/TestModulationAndEnvelope.cpp`.

### Documentation
- **[LOW] iZotope IPC header comment** — removed the stale "29-byte" spec
  reference; recorded the verified 33-byte `<IBddQI` layout with a cross-
  reference to the Python peer. `src/AI/VST3IPCBridge.h`.
- Updated `More-Phi_Technical_Review_Report.md` resolution banner with the
  2026-06-19 fixes.
- Updated `docs/validation/PRODUCTION_READINESS_AUDIT_2026-06-18.md` gate
  table; verdict promoted to **READY** (pending B3 acceptance gates).
- Added this CHANGELOG.

### Build
- Removed an accidental duplicate `TestLUFSMeter.cpp` entry in
  `tests/CMakeLists.txt` (would have broken the test build).

### Known limitations / deferred
- **B3** — multi-platform build verification (MSVC + macOS Universal + Linux
  ASan), `pluginval --strictness-level 5`, `vst3_validator`, manual DAW smoke
  (Ableton/FL/Logic/Reaper). Acceptance-gate work, not code.
- **N1** — `MCPToolHandler.cpp` remains ~6,100 lines (monolithic). A per-
  category split is tracked as a maintainability follow-up.
- True-peak estimator is accurate for DC/low-frequency ISP but under-reads
  near-Nyquist (documented; would need a wider prototype FIR to reach
  reference-grade accuracy).

### Architectural facts & corrections (factual record, no pricing)
Recorded so future readers don't re-derive them from stale framings. Pricing
rationale lives separately in `docs/PRODUCT_POSITIONING.md`.

- **JUCE 8 commercial cost is $0 at launch volumes.** Verified against the
  official EULA ([juce.com/legal/juce-8-licence](https://juce.com/legal/juce-8-licence/)).
  Tiers: **Personal** (≤ $20K/yr revenue *or funding*, free, commercial use
  allowed), **Starter** (≤ $300K/yr, $800 perpetual), **Indie** (no limit,
  $3,500 perpetual), **Pro** (no limit, higher). There is **no cost cliff**
  forcing a price floor up: at $149 list × ~200 copies/yr (~$30K gross) the
  product lands in Starter at $800 — a ~2.7% COGS line item. The earlier
  "$1,200–4,000/yr must be absorbed" framing was a Pro-tier number that does
  not apply at launch.
- **Runtime AI surface is ~22K LOC, not 40.5K.** `src/AI/` totals 40,582 LOC,
  but the breakdown is:
  - `src/AI/Dataset/` — **15,245 LOC** (offline synthetic-audio dataset
    generator; not in the shipped plugin's runtime path).
  - `src/AI/StandaloneMcp/` — **3,532 LOC** (standalone CLI MCP server; not
    the plugin's embedded server).
  - **Runtime AI (MCP server + tool handlers + mastering heuristics) —
    21,805 LOC** (~22% of the codebase).
  The "over half the codebase is AI infrastructure" framing overstates the
  *shipped* runtime AI surface; once the offline Dataset tool and Standalone
  CLI are stripped, the AI the binary executes is large-but-defensible for a
  40+-tool MCP surface, not sprawling.
- **`find` operator-precedence gotcha (do not re-introduce the 36K figure).**
  An unparenthesized count like
  `find src/AI -name "*.cpp" -o -name "*.h" -not -path "*/Dataset/*"`
  parses as `(all .cpp) OR (non-Dataset .h)` — it counts **all** `.cpp`
  including Dataset's, plus only non-Dataset `.h`, inflating the result by
  ~11K (this produced an erroneous "36,252" figure in an earlier analysis).
  Always parenthesize:
  `find src/AI \( -name "*.cpp" -o -name "*.h" \) -not -path "*/Dataset/*"`.
  The verified total/Dataset/StandaloneMcp/runtime figures above were
  produced with parenthesized commands and reproduce exactly.

---

## 2026-06-18 — Comprehensive production-readiness audit (branch `feat/store-backend`)

47 issues identified (16 Critical, 14 High, 14 Medium, 6 Low) across
threading, memory safety, DSP, serialization, AI/MCP security, and build
stability. All resolved. Full record: `docs/audits/CRITICAL_BUGS_FIXED.md`.

Highlights: ThreadPool RAII deadlock fix (C1), SnapshotBank seqlock (C2),
HostedPluginWindow use-after-free (C3), V1→V2 preset migration (C4),
discrete-parameter step counts (C5), Formant/Spectral/Granular engine
correctness (C6/C7/H5), physics symplectic-Euler stabilization (C8/C9),
real-time `std::pow` removal (C15), PerformanceProfiler audio-thread
allocation (C16), ModulationMatrix accumulate-then-clamp (H3), block-size-
independent sidechain coefficients (H4), MCP instance isolation +
constant-time auth + idle timeout + zombie eviction (C13/C14).
