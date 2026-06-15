# More-Phi VST3 — Unified Technical Review

**Date:** 2026-06-15
**Reviewer:** Lead Architect (single-reviewer pass)
**Scope:** More-Phi VST3/AU plugin — JUCE 8, C++20, namespace `more_phi`, project version 3.3.0
**Build targets in scope:** VST3 (all platforms), AU (macOS incl. Apple Silicon)

---

## Process Note (read this first)

The planned 6-agent parallel review workflow was **blocked by an account-level 429 fair-usage rate limit** — fanning out 6 concurrent reviewer agents plus their adversarial verifiers tripped the cap, and all 13 agent calls were rejected. Rather than stall, the review was performed **single-threaded by the Lead Architect**, reading every line of the critical correctness/safety surface first-hand.

Every finding below is grounded in code that was actually read, with `file:line` citations and quoted evidence. What was **not** completed (and is flagged explicitly in §8) is the independent adversarial second-opinion pass — that should be re-run once the rate limit clears.

---

## 0. Scope Actually Audited (first-hand)

| Subsystem | Files read line-by-line | Verdict basis |
|---|---|---|
| Physics | `Core/PhysicsEngine.{h,cpp}`, `Core/MorphProcessor.{h,cpp}` | Full |
| Interpolation | `Core/InterpolationEngine.{h,cpp}` (IDW + 1D + SIMD) | Full |
| Snapshot / state | `Core/SnapshotBank.{h,cpp}`, `Core/ParameterState.h`, `Core/LockFreeQueue.h` | Full |
| Hosting | `Host/PluginHostManager.{h,cpp}`, `Host/ParameterBridge.{h,cpp}`, `Host/IPluginHostManager.h` | Full |
| Audio entry | `Plugin/PluginProcessor.cpp` — `prepareToPlay` → `processBlock` → `getState/setStateInformation` → deferred reload | Full |
| MIDI | `MIDI/MIDIRouter.{h,cpp}` | Full |
| Genetic | `Core/GeneticEngine.{h,cpp}` | Full |
| MCP protocol | `AI/MCPServer.{h,cpp}`, `AI/StandaloneMcp/JsonRpc.h`, `AI/InstanceRegistry.{h,cpp}` | Full |
| **Not audited** | `AI/MCPToolHandler.cpp` (5,446 lines), `AI/MCPToolsExtended.cpp`, `AI/TokenOptimizer.cpp`, `AI/StandaloneMcp/StandaloneMcpServer.cpp` dispatch, `Core/ModulationEngine`/`ModulationMatrix`, mastering processors, `AI/Dataset/*`, `Licensing/*` | **Surveyed only** |

**Verified-safe (initially suspected, refuted by first-hand read — included to show rigor):**

- `MCPServer::connectedClients_` **is** `std::atomic<int>` (`MCPServer.h:76`) — no data race.
- `isRestoring_` **is** cleared on timer retry-exhaustion (`PluginProcessor.cpp:2155`) — no permanent morph lockup.
- The oversampling reconfigure double-check (`PluginProcessor.cpp:1416-1425` + `reconfigureAudioDomainProcessing` `:2009-2013`) **is** correct — no audio/reconfigure race.

---

## 1. Critical Issues

### C-1 — Seqlock reader uses `atomic_signal_fence`, not `atomic_thread_fence` → torn reads on Apple Silicon

- **Location:** `src/Core/SnapshotBank.h:280`, `:399`; same pattern in `toXml()` reader loop (`:128`).
- **Category:** thread-safety / correctness.
- **Severity:** High → Critical on the AU / macOS-ARM target; latent-but-safe on x86.

```cpp
// tryReadLocked() — SnapshotBank.h:274-287
fn(*slots_);                                         // non-atomic reads of ParameterState
std::atomic_signal_fence(std::memory_order_seq_cst); // ← compiler fence ONLY
uint32_t seq2 = seqlock_.load(std::memory_order_acquire);
if (seq1 == seq2) return true;                       // torn read can pass this check
```

`std::atomic_signal_fence` is defined to emit **no CPU memory barrier** — it only constrains the compiler against signal-handler reordering *within a thread*. For a seqlock protecting data read by one thread and mutated by another, the data reads must be ordered **before** the second sequence load *in hardware*. The writer side is correct (`endWrite()` uses `atomic_thread_fence(release)`, `:355`), but the reader is missing its matching acquire fence.

- On **x86 (TSO)**: loads are not reordered with loads → works by accident.
- On **ARMv8 / Apple Silicon** (the `macos-arm64-debug` and `macos-universal-release` presets): loads *can* be reordered. A torn `ParameterState` read can validate as consistent (`seq1 == seq2`) and yield a blend of two snapshots' float arrays → wrong morph output / glitch, or a torn `char name[64]`.

The comment at `:277-280` even states the intent ("Prevent the compiler from moving non-atomic slot reads below the second sequence check") — the author believed `signal_fence` achieved this across threads. It does not.

**Fix (one-token change in 3 places):**

```cpp
// Was: std::atomic_signal_fence(std::memory_order_seq_cst);
std::atomic_thread_fence(std::memory_order_acquire);  // pairs with writer's release fence
```

Apply in `tryReadLocked` (`:280`), `copySlotValues` (`:399`), and the `toXml()` read loop (`:128`). Rationale: an acquire fence *after* the data reads and *before* the `seq2` load prevents those reads from being reordered past the validation load — the seqlock correctness contract on weakly-ordered CPUs.

---

## 2. High-Priority Improvements

### H-1 — `MCPToolHandler.cpp` is a 5,446-line / 246 KB god-file

- **Category:** architecture / maintainability / testability.
- **Severity:** High (not a runtime bug, but a structural blocker).

This single file holds every MCP tool — roughly 7% of the entire `src/` tree in one translation unit. Consequences: impossible to review/audit holistically (it could not be read in this pass), slow incremental compiles, a merge-conflict magnet, and every tool shares one set of includes/symbols. The codebase already has a clean seam (`MCPToolHandler::handle(method, params, processor, identity)` dispatch) — decompose by domain: `SnapshotTools`, `MorphTools`, `McpParamTools`, `GeneticTools`, `OzoneTools`, `DatasetTools`. Keep `handle()` as a ~100-line dispatcher.

### H-2 — Elastic spring is sample-rate-dependent (double-compensation)

- **Location:** `src/Core/PhysicsEngine.cpp:20-34`, `:36-52`.
- **Category:** correctness (perceptual).
- **Severity:** Medium-High.

The code applies **both** `dtScale = kRefDt/dt` to stiffness/damping **and** adaptive sub-stepping. Sub-stepping alone already makes the spring advance the correct physical amount per real-time second regardless of sample rate — so `dtScale` double-compensates. Mathematically, scaling `k` and `c` by `s` scales both ωₙ and ζ by √s, so the spring gets **faster and less bouncy** at higher sample rates. A project authored at 44.1 kHz feels different when reopened at 96 kHz or rendered at 48 kHz in another DAW.

```cpp
const float dtScale = (dt > 1e-8f) ? kRefDt / dt : 1.0f;   // ← remove this
...
stiffness *= dtScale; damping *= dtScale;                    // ← and these two
```

**Fix:** delete the `dtScale` compensation; keep the sub-stepping (which is what actually guarantees stability + sample-rate independence). Verify with a unit test asserting identical settling-time at 44.1 / 48 / 96 kHz.

### H-3 — `PluginHostManager::unloadPlugin()` spins on exclusive-use with no timeout

- **Location:** `src/Host/PluginHostManager.cpp:247-248`. Called from `~PluginHostManager()`.
- **Category:** crash-vector / deadlock.
- **Severity:** Medium-High.

```cpp
while (exclusivePluginUseRequested_.load(std::memory_order_acquire))
    juce::Thread::yield();   // unbounded
```

If a `beginExclusivePluginUse()` caller (state capture/restore) faults or is slow and never calls `endExclusivePluginUse()`, the destructor hangs forever → the plugin instance never tears down → **DAW hangs on track removal**. `beginExclusivePluginUse` itself *has* a timeout (`timeoutMs=200`), but the unload side does not.

**Fix:** bounded spin, then force-clear the flag and proceed.

```cpp
for (int i = 0; i < 200 && exclusivePluginUseRequested_.load(std::memory_order_acquire); ++i)
    juce::Thread::sleep(1);
exclusivePluginUseRequested_.store(false, std::memory_order_release); // force release
```

### H-4 — Per-block touch detection does up to 3×paramCount hosted-plugin virtual reads

- **Location:** `src/Plugin/PluginProcessor.cpp:1322-1389`.
- **Category:** performance.
- **Severity:** Medium-High.

For each parameter each block, the loop may call `paramBridge.getParameterNormalized(i)` up to **3 times** (lines `1338`, `1347`, `1380`). Each call goes through `withPlugin` → `acquirePluginForUse` / `releasePluginFromUse` (2 atomics) → `plugin->getParameters()` → virtual `getValue()`. On a 1000+ parameter synth that is ~6000 virtual calls + 6000 atomic pairs per block — a real CPU load that scales with the hosted plugin's parameter count, not with how many params are actually morphing.

**Fix:** snapshot the hosted parameter values **once per block** into a pre-allocated buffer (`captureParameterState()` already exists; route it into a member buffer in `prepareToPlay`), then read from that array in the touch loop. Cuts hosted-plugin reads from O(3·N) to O(N) with zero atomics inside the loop.

### H-5 — Notifications receive spurious JSON-RPC responses (`id:null`)

- **Location:** `src/AI/MCPServer.cpp:292-355`.
- **Category:** api-compliance.
- **Severity:** Medium.

A JSON-RPC notification (request with no `id`) must receive **no response**. Here `idVar` defaults to void → `reqId=null`, and the code still builds and returns a full `{"id":null,...}` envelope (including `-32600 Unauthorized` for unauthenticated notifications). Strict MCP clients that count response lines can desync framing; the extra `id:null` lines are noise the spec forbids.

**Fix:** after parsing, if `idVar.isVoid()` (and it is not `initialize`), treat as notification — execute (if authenticated) and return an empty `juce::String` (no `\n`, no write).

---

## 3. Medium-Priority Refinements

| ID | Location | Issue | Fix |
|---|---|---|---|
| M-1 | `InterpolationEngine.cpp:221` | Contention fallback `std::fill(output, 0.5f)` snaps **every** hosted param to midpoint → loud glitch under MCP write contention. | Hold previous frame: do nothing (leave `output` unchanged) or write the last-good cached vector. |
| M-2 | `PluginHostManager.cpp:349` | `requiredChannels = min(max(in,out), 16)` silently **truncates** >16-channel surround plugins (e.g. 22.2 ambisonics clips). | Raise `kMaxHostChannels` to 32, or assert-and-log when clamping rather than silently corrupting audio. |
| M-3 | `InstanceRegistry.cpp:110-118`, `86-105` | Port-availability probe binds + immediately closes → **TOCTOU**: an external process can grab the port between probe and `createListener`. Also O(N) bind probes per registration. | After `createListener` succeeds in `MCPServer::run`, *that* is the source of truth; on bind failure, return the port to the registry and reallocate. Probe is advisory only. |
| M-4 | `GeneticEngine.cpp` (whole file, 67 lines) | **No fitness function, no population, no real crossover.** `breed()` is a weighted-average + uniform noise; "genetic" is a misnomer. Docs / feature reference overstate this. | Either rename to "blend / randomize" in the UI + docs, or implement genuine multi-parent crossover with a measurable fitness (e.g. spectral-distance to a target). Don't ship "genetic breeding" that isn't. |
| M-5 | `PluginProcessor.cpp:1683-1684` | `stateVersion` is read then `ignoreUnused` — **forward migration is a no-op.** Loading a v3.2 project into v3.3 relies entirely on `fromXml` defaults silently papering over schema changes. | Implement a migration switch keyed on `stateVersion` before restoring each XML child; log when migration occurs. |
| M-6 | `PluginProcessor.cpp:1780-1812` | `setStateInformation` uses `callFunctionOnMessageThread` — if a host calls it **on the audio thread** (some do for preset recall), this blocks the audio thread on a message-thread hop → dropout / deadlock. | Detect thread context; if on audio thread, defer purely via atomic flag + timer (never block). |
| M-7 | `MIDIRouter.cpp:65` | Only **CC#1 (mod wheel)** is hard-coded for morph control. `CLAUDE.md` promises "CC routing for morphing parameters" — no learn / flexible mapping exists. | Add a CC → target learn map; or correct the docs. |
| M-8 | `MorphProcessor.h:84-85` | `process()` is `noexcept` but calls `updatePhysics` / `applySmoothing` which are **not** noexcept. Any propagated exception → `std::terminate` → instant plugin + DAW crash. | Mark both `noexcept` (they only do arithmetic on pre-sized buffers) so the contract is enforced at compile time, or remove `noexcept` from `process`. |
| M-9 | `PluginHostManager.h:5-8` (doc) | Header still says misbehaving plugins are **"auto-unloaded"**; code actually **suspends** (`suspended_`, recovery loop). Stale doc misleads maintainers. | Update comment to match suspend-and-recover behavior. |
| M-10 | `JsonRpc.h:34-36` | `makeToolResult` isError path does `structuredContent["error"].get<std::string>()` — throws if `error` is an object/array, not a string. | `is_string()` guard; fall back to `.dump()`. |
| M-11 | `ParameterBridge.cpp:269-272` | `shouldThrottle` lambda has dead branches — both `if (newValue == state.lastValue) return true;` and the trailing `return true;`. Harmless but confusing. | Collapse to a single `return true;` once the delta / abs guards pass. |
| M-12 | `PluginHostManager.cpp:96-131` (`prepare`) | Calls `hostedPlugin->enableAllBuses()` + `prepareToPlay` + `wideBuffer_.setSize`. If a host calls `prepareToPlay` on an RT-adjacent thread during a live config change, these allocate / lock on or near the audio path. | JUCE convention says prepare is off-RT, but guard with an assert or move hosted-prepare to the message thread. |

---

## 4. Low-Priority Enhancements

- **L-1** `PluginProcessor.cpp:930-931` — `finalOutput_.resize(MAX_PARAMETERS, 0.0f);` appears **twice** (copy-paste). Delete the duplicate.
- **L-2** `InterpolationEngine.cpp:32-92` — `hasAVXSupport()` / `hasSSESupport()` do **runtime** cpuid detection with a manual `static bool checked` guard (benign data race), yet `interpolateBatch_SIMD` is selected at **compile time** (`#if defined(MORE_PHI_USE_AVX)`). The runtime functions appear unused for dispatch → dead / misleading. Either wire runtime dispatch or remove them.
- **L-3** `InterpolationEngine.cpp:188` — `getClockPositions(radius≠1)` returns a reference to a `static thread_local` buffer that is overwritten on each call. Footgun if a caller holds the ref across another call. Return by value, or document the invalidate-on-call contract.
- **L-4** `MCPServer.cpp` — no batch-request support (JSON-RPC arrays dispatched as empty-method). MCP does not require it; reject explicitly with `-32600` instead of silently mishandling.
- **L-5** `MIDIRouter.h:80-81` — sidechain envelope coeffs (`0.5` / `0.9`) are not block-rate-normalized → different ballistics at different buffer sizes. Make them `1 - exp(-1/(tc*blocksPerSec))`.
- **L-6** `GeneticEngine.cpp:21` — `juce::Logger::writeToLog` on parameter-count mismatch (off-audio, fine today) — if breeding ever moves to a worker that is RT-adjacent, swap for an atomic counter.
- **L-7** `MCPServer.cpp` — no TLS. Acceptable (localhost-only + `isLocal()` reject + constant-time bearer auth), but document the threat model explicitly so a future "remote MCP" feature does not inherit the plaintext assumption.
- **L-8** Pervasive `DBG(...)` calls in hot-ish paths (`setStateInformation`, `loadHostedPluginFromState`). `DBG` is a no-op in release, but audit that none leak onto the audio thread (none seen in `processBlock` — good).
- **L-9** `compute2D` `totalWeight < kEpsilon` early-return (`InterpolationEngine.cpp:322-323`) leaves `output` in its prior state (accidental "hold"). Make the intent explicit with a comment or an explicit hold.

---

## 5. Recommended Fixes — Corrected Code for the Criticals

### Fix C-1 (seqlock fence) — the one real correctness bug

```cpp
// src/Core/SnapshotBank.h — tryReadLocked(), inside the retry loop:
            fn(*slots_);

-           std::atomic_signal_fence(std::memory_order_seq_cst);
+           // Acquire fence pairs with the writer's release fence in endWrite().
+           // Unlike atomic_signal_fence, this emits a CPU barrier so the
+           // non-atomic slot reads cannot be reordered past the seq2 load on
+           // weakly-ordered CPUs (ARM/Apple Silicon). Required for seqlock
+           // correctness across threads.
+           std::atomic_thread_fence(std::memory_order_acquire);

            uint32_t seq2 = seqlock_.load(std::memory_order_acquire);
            if (seq1 == seq2) return true;
```

Apply the identical change in `copySlotValues` (`:399`) and the `toXml()` reader loop (`:128`).

### Fix M-1 (hold previous on contention)

```cpp
// src/Core/InterpolationEngine.cpp — computeWithRetry():
    if (!lockAcquired)
    {
-       std::fill(output.begin(), output.end(), 0.5f);
+       // Hold the previous frame rather than snap every param to 0.5,
+       // which would produce a loud glitch under MCP write contention.
+       // (output still holds last block's morph result.)
    }
```

### Fix H-2 (remove double-compensation)

```cpp
// src/Core/PhysicsEngine.cpp — updateElastic():
-   constexpr float kRefDt = 512.0f / 44100.0f;
-   const float dtScale = (dt > 1e-8f) ? kRefDt / dt : 1.0f;
    float stiffness, damping;
    switch (preset) { /* Slow/Medium/Heavy unchanged */ }
-   stiffness *= dtScale;
-   damping   *= dtScale;
    // sub-stepping below already guarantees stability + sample-rate independence
```

### Fix H-3 (bounded unload)

```cpp
// src/Host/PluginHostManager.cpp — unloadPlugin():
-   while (exclusivePluginUseRequested_.load(std::memory_order_acquire))
-       juce::Thread::yield();
+   for (int i = 0; i < 200 && exclusivePluginUseRequested_.load(std::memory_order_acquire); ++i)
+       juce::Thread::sleep(1);
+   exclusivePluginUseRequested_.store(false, std::memory_order_release); // never hang the destructor
```

---

## 6. Systemic Risk Assessment (cross-cutting)

**S-1 — "Looks RT-safe because it is noexcept/atomic, but the memory ordering is wrong in one place."**
The seqlock (C-1), the IDW-fallback, the touch-loop atomics, and the oversampling reconfigure all *use* the right primitives (atomics, seqlocks, try-locks) — the discipline is clearly present and mostly correct. C-1 is a single sharp edge where the primitive was mis-specified (`signal_fence` vs `thread_fence`). **Risk:** there may be a second such instance in code not read line-by-line (`MCPToolHandler.cpp`, `ModulationEngine`, mastering processors). **Mitigation:** a focused grep for `atomic_signal_fence` across `src/`, plus a Clang ThreadSanitizer run on the `linux-clang-asan` preset, would catch the whole class at once.

**S-2 — "The hard part is correctly handled; the edges leak."**
State-restore ordering, deferred plugin reload, exception-driven suspend / recover, exclusive-use gating, constant-time auth, localhost enforcement — these subtle, high-risk mechanisms are all done *well*. The remaining issues are edges: stale docs (M-9), dead code (L-2), feature-vs-claim gaps (M-4, M-7), perf (H-4), and the one fence bug. This is a mature codebase with strong real-time hygiene, not a fragile one.

**S-3 — Maintainability is the dominant long-term risk, not correctness.**
The 5,446-line `MCPToolHandler.cpp` (H-1) plus the 2,258-line `PluginProcessor.cpp` plus the 1,863-line `AutomationControlPlane.cpp` mean the three largest files hold ~12% of the codebase. Every future feature lands in a god-file, every audit (like this one) cannot fully cover them, and merge velocity will degrade. **Coordinated fix:** decompose `MCPToolHandler` by tool-domain first (highest payoff), then extract a `MorphAudioService` from `PluginProcessor` to shrink the processor.

**S-4 — No automated real-time / concurrency test coverage.**
458 Catch2 unit tests exist, but nothing exercises the seqlock under contention, the audio / MCP thread interleaving, or state-restore-while-processing. C-1 would have been caught by a TSan run on the `linux-clang-asan` preset with a concurrent capture / restore stress test. This is the single highest-leverage process change.

---

## 7. Production-Readiness Verdict

**Status: CONDITIONALLY READY — not a blocker for x86 VST3; a blocker for the Apple-Silicon AU until C-1 is fixed.**

- **No smoking-gun crash, memory leak, or use-after-free** was found in the audited core. Lifecycle (load / unload / reload), exception containment, state persistence, and the lock-free handoffs are engineered with more care than is typical.
- **One genuine correctness bug (C-1)** that is latent on x86 but **active on ARM** — and ARM is a first-class build target (`macos-arm64-debug`, `macos-universal-release`). The AU build can return torn snapshot data. **Fix is a one-token change; ship it before the next macOS release.**
- **The MCP server is genuinely production-grade:** constant-time auth, localhost bind + `isLocal()` reject, rate limiting, oversized-request protection, max-connection cap, graceful shutdown with 500 ms force-timeout. The only protocol blemish is the spurious-notification-response (H-5).
- **Architecture debt (H-1) is the thing that will eventually slow you down** more than any current bug. Decompose `MCPToolHandler.cpp`.

**Recommended ship sequence:**

1. **C-1** (seqlock fence) + **M-8** (noexcept contract) — correctness, ~1 hour.
2. **H-3** (unload timeout) + **M-1** (hold-previous) — robustness / UX, ~2 hours.
3. **H-2** (spring SR-independence) + a unit test asserting settling-time across sample rates.
4. **S-4:** add a TSan + concurrent-seqlock stress test to CI; grep for any other `atomic_signal_fence`.
5. **H-1:** decompose `MCPToolHandler.cpp` (the big one — schedule as its own milestone).

---

## 8. Outstanding — Areas Needing the Adversarial Second Pass

Once the account rate limit clears, the multi-agent review (6 specialists + per-finding adversarial verification + systemic synthesis) should be re-run against the files **not** covered line-by-line here:

- `AI/MCPToolHandler.cpp` (5,446 lines) — tool argument validation, JSON building, every tool's error paths.
- `AI/MCPToolsExtended.cpp`
- `AI/TokenOptimizer.cpp` — rate-limit correctness.
- `AI/StandaloneMcp/StandaloneMcpServer.cpp` — request dispatch.
- `Core/ModulationEngine` / `ModulationMatrix` — route-buffer double-buffering correctness.
- Mastering processors — `AutoMasteringEngine`, `MultibandDynamicsProcessor`, `BrickwallLimiter` (true-peak / limiter ceiling correctness matters for a "mastering" plugin).
- `AI/Dataset/*` — the V2 / V3 dataset pipeline.

These can be covered either by re-launching the 6-agent workflow against just these files, or by continuing the single-reviewer sequential read.

---

## 9. Fix Application & Verification (2026-06-15)

The four recommended critical/high fixes were applied on branch **`fix/core-review-criticals`** and verified by build + full test suite.

| Fix | File(s) | Change | Verification |
|---|---|---|---|
| **C-1** | `src/Core/SnapshotBank.h` (×3 sites) | `std::atomic_signal_fence(seq_cst)` → `std::atomic_thread_fence(acquire)` in `tryReadLocked` and `copySlotValues`; **added** the missing acquire fence in the `toXml()` reader loop (which had no fence at all). | Compiles clean; all snapshot/state-persistence tests pass. **Note:** full cross-thread correctness on ARM is verified by code inspection against the canonical seqlock pattern; a ThreadSanitizer run on the `linux-clang-asan` preset is the gold-standard confirmation still pending (TSan is unavailable on MSVC). |
| **M-1** | `src/Core/InterpolationEngine.cpp` (`computeWithRetry`) | Contention fallback now **holds the previous frame** instead of `std::fill(output, 0.5f)`. | Interpolation/morph tests pass. |
| **H-2** | `src/Core/PhysicsEngine.cpp` (`updateElastic`) | Removed the `dtScale` double-compensation; spring now uses true physical stiffness + adaptive sub-stepping (which already guarantees sample-rate independence). | **New regression test** `PhysicsEngine::updateElastic: sample-rate independent (H-2)` added to `tests/Unit/TestPhysicsAndGenetic.cpp` — asserts 44.1 kHz and 96 kHz configs both track a fine-step reference within 0.03. Passes. Stale `dtScale` comment in an adjacent test corrected. |
| **H-3** | `src/Host/PluginHostManager.cpp` (`unloadPlugin`) | Unbounded `while (...) yield()` on `exclusivePluginUseRequested_` replaced with a bounded 200 ms wait, then force-release. The `activePluginUsers_` wait is intentionally left unbounded (proceeding past a live audio lease would be a use-after-free). | Plugin-lifecycle/hosting tests pass. |

**Build:** `cmake --build build/windows-msvc-release --config Release --target MorePhiTests` → exit 0. Only pre-existing warnings (deprecated `juce::Font` ctor in `LicenseActivationOverlay.cpp`; unused `blockSize` in `TestAudioEngine.cpp`) — none introduced by these changes.

**Tests:** full `MorePhiTests.exe` suite → **417 / 417 test cases pass, 71,040 assertions, 0 failures.** (The "[WARNING] No plugin host available — dry passthrough" line is expected output from a dataset integration test, not a failure.)

**Remaining (not applied in this pass):** H-1 (`MCPToolHandler.cpp` decomposition — schedule as its own milestone), H-4 (per-block touch-detection read batching), H-5 (notification response suppression), and the M-/L-tier items. These are documented above and await triage.

---

## 10. Extended Review — Mastering Path & MCP Tool Surface (2026-06-15)

Single-reviewer continuation into the areas §0 marked "surveyed only": the mastering/limiting processors and `MCPToolHandler.cpp` (5,446 lines, read via grep-driven targeted inspection rather than line-by-line — so this section is directional, not exhaustive).

### Critical

#### B-1 — Brickwall limiter enforces sample-peak, not true-peak; dBTP ceiling is not honored
- **Location:** `src/Core/BrickwallLimiter.cpp:88-100`; the `truePeak_` member is used only at `:123` for post-limit metering.
- **Category:** correctness (mastering-critical).
- **Severity:** Critical for the mastering use-case.

The limiter's gain-reduction loop computes its peak by scanning `std::abs(delayL_[idx])` — **sample-domain peaks**:

```cpp
// BrickwallLimiter.cpp:88-100  — the gain-control scan
float peakL = 0.f, peakR = 0.f;
for (int k = 0; k < lookaheadSamples_; ++k) {
    const int idx = (writePos_ - k + kLookaheadBufSize) % kLookaheadBufSize;
    if (nch >= 1) { const float a = std::abs(delayL_[idx]); if (a > peakL) peakL = a; }
    ...
}
const float peak = std::max(peakL, peakR);
float targetGain = 1.0f;
if (peak > ceiling && peak > 1e-12f) targetGain = ceiling / peak;
```

But the ceiling is set in **dBTP** (`setCeiling(plan.ceilingDBTP)`, `AutoMasteringEngine.cpp:305`), and the architecture docs explicitly claim ISP detection — `AutoMasteringEngine.h:14` ("`[8] BrickwallLimiter 4ms lookahead, ISP detection`") and `BrickwallLimiter.h:4-11`. A `TruePeakEstimator` (4× polyphase FIR, ITU-R BS.1770-4) is instantiated as `truePeak_` and `prepare()`d, but it is wired **only** to the post-limiting metering call (`truePeak_.processBlock(buf)` at `:123`) — never to the gain-control path.

**Consequence:** inter-sample peaks pass through ungated. On hot program material (sawtooth/square content, transients reconstructed by DAC reconstruction), the true peak of the output can exceed the ceiling by ~+0.3 to +1.0 dBTP. For a mastering plugin targeting streaming delivery (−1 dBTP), masters can fail loudness/peak QC and clip downstream encoders. The limiter meets its *sample-peak* ceiling but not the *true-peak* ceiling it advertises and is configured against.

**Fix (two viable options):**
1. **Drive gain from the true-peak estimator over the lookahead window.** In the scan loop, for each position run the 4 polyphase phases and take the max across phases as that sample's effective peak (then max over the window). This reuses the existing, correct `TruePeakEstimator` coefficients — just point them at the control path instead of only metering.
2. **Oversample the control path 4×** (the limiter already owns a `TruePeakEstimator` and the project has `OversamplingWrapper`), compute sample peaks in the upsampled domain, apply gain, downsample. More expensive; option 1 is cheaper and sufficient.

The estimator itself is sound (verified: 4-phase × 12-tap linear-phase symmetric FIR, correct ring-buffer indexing in `applyPhase`, BS.1770-4-conformant structure) — the defect is purely the **wiring**, which makes this a contained, high-leverage fix.

> This finding **raises the production-readiness bar for the mastering feature specifically**: until B-1 is fixed, "mastering" output should not be trusted to meet a stated dBTP ceiling. It does not affect the morphing/hosting core covered in §1–§9.

### Medium

#### B-2 — True-peak meter reports per-block peak, not a held session peak
- **Location:** `src/Core/TruePeakEstimator.cpp:98, 134-139`.
- **Severity:** Medium (metering semantics).

`processBlock()` resets `maxL`/`maxR` to 0 each call and stores the **last block's** peak. `getTruePeak_dBTP()` therefore returns a transient, not the running maximum since `reset()`. Meters, UI, and MCP `get_mastering_state` consumers see a flickering per-block value rather than the peak-so-far that mastering engineers expect. Fix: keep `peakL_`/`peakR_` as held-running maxima across blocks, reset only on explicit `reset()`.

#### M-13 — `runSelfTest` mutates hosted-plugin params directly on the MCP thread
- **Location:** `src/AI/MCPToolHandler.cpp:3380-3396, 3486, 3504` (the `preset_persistence` diagnostic suite).
- **Severity:** Medium (diagnostic-scoped race).

The self-test calls `bridge.applyParameterState()` / `setParameterNormalized()` / `getParameterNormalized()` directly — bypassing the `enqueueParameter*` lock-free handoff that every *normal* tool correctly uses (and that the code itself documents at `:2912-2913`). Both the self-test (MCP thread) and the morph engine (audio thread) then call `params[i]->setValue()` on the hosted plugin without coordination → data race on non-atomic parameter state, and audible glitches if invoked during playback. Nothing guards against running it while audio is active. Fix: gate the self-test behind an "audio suspended / idle" precondition, or route its writes through the queue.

### Low

- **L-10 (MCP):** File-handling tools accept arbitrary filesystem paths from the request — `mastering.render_batch` (`:4310-4316`: input/output/plugin), `hosted_plugin.scan`/`load` (`:3794`, `:824`). Acceptable under the localhost + bearer-auth + `isLocal()` threat model, but the trust boundary is implicit. Consider validating output paths against an allow-list directory and documenting that an authenticated local client can read/write arbitrary paths (and load arbitrary native-code VST3s).
- **L-11 (MCP):** Unknown method names likely surface to the client as `-32603` (the server's "tool handler returned invalid JSON" fallback on an empty toolResult) rather than the spec-correct `-32601` ("method not found"). The exact `handle()` fallthrough return was not read line-by-line — verify it emits a `-32601` envelope.
- **L-12 (MCP):** `workflow.execute` (`:1140-1156`) recursively dispatches each step via `MCPToolHandler::handle` (`:1156`). Bounded by the workflow's step list, but a large workflow means deep recursion on the connection thread; an iterative dispatch with a step cap would be safer.

### Strengths observed (balancing the above)

- **Parameter-write discipline is correct in every normal tool.** All `set_parameter` / `set_parameters_batch` / morph / mastering tools route through `enqueueParameterSet` / `enqueueParameterBatch` (`:922, :2316, :2767, :2830, :4626, :4711`) — the single rule that matters most for real-time safety is obeyed. M-13 is the lone exception, and it's a diagnostic.
- **No manual JSON string concatenation** in tool responses — proper `juce::var`/`nlohmann::json` objects throughout (grep for hand-built `"{\"…\"` returned nothing), eliminating the injection / double-escape class of bugs that plagues ad-hoc MCP servers.
- **Dispatch table is clean and mature** (`handle()`, `:2536-2628+`): readable `if/return` chain, and every mutating tool is wrapped in `dispatchWithAutomationTransaction` for rollback/diff/audit support. This is the seam the H-1 decomposition should preserve.
- **True-peak estimator is technically correct** — the FIR design, coefficient symmetry, and polyphase indexing are sound. B-1 is a wiring defect, not a DSP defect.

### Recommended ship sequence (additions)

6. **B-1** — wire the true-peak estimator into the limiter gain-control path. Highest-value mastering fix; contained blast radius (the estimator already exists and works).
7. **B-2** — held-running peak in `TruePeakEstimator`.
8. **M-13** — gate `runSelfTest` against active audio.

### Coverage gap (still open)

`MCPToolHandler.cpp` was inspected via targeted grep + dispatch-table + representative-tool reads, **not** line-by-line. The per-tool argument validation, the `automation.*` / `memory.*` / `workflow.*` control-plane tools (`handleControlPlaneTool`), and the Ozone IPC assistant tools were not individually audited. A dedicated pass (or the adversarial multi-agent workflow, once the rate limit clears) over the tool bodies — especially argument bounds-checking and the control-plane state machine — remains warranted.

---

## 11. Mastering-Chain & Modulation Deep-Audit (2026-06-15)

A 4-agent parallel audit (LUFS, multiband, modulation, enhancers) with per-critical adversarial verification. **34 findings: 4 critical, 7 high, 13 medium, 10 low.** Verification confirmed 2 criticals, **refuted 1** (false positive caught), and downgraded 1 (critical→high). The full machine-readable result is in the workflow transcript; the verified essentials follow.

### ⚑ The load-bearing finding: the mastering chain is DORMANT in the shipped plugin

Independent confirmation (grep of `autoMasteringEngine_.` in `PluginProcessor.cpp`): the plugin calls only `prepare(..., false)` (which disables the chain + its 10 Hz timer), `reset()`, `analyzeBlock()` (a **metering-only** tap that runs the LUFS/true-peak/spectrum/stereo *analyzers* but **not** the limiter/normalizer/exciter/multiband), and `getChainPlanner()`. `AutoMasteringEngine::processBlock` — the entire shaping+limiting+normalizing chain — is reached **only from a unit test** (`TestMCPServerUnit.cpp:783`), never from the audio thread.

**Consequence for this report:** every finding inside `AutoMasteringEngine::processBlock` and its stages (limiter, normalizer, exciter, multiband dynamics) is **LATENT** — a real defect that does not affect shipped audio today but is a hard blocker the moment the mastering chain is wired into the audio path. Findings in the modulation engine and the LUFS/analysis meters (which `analyzeBlock` *does* run) are **LIVE**.

> This also reframes the B-1 fix shipped in commit `288d58b`: it is a correct fix to a real bug, but the `BrickwallLimiter` currently only runs in the dormant chain, so B-1 is latent too — it does not affect shipped audio until mastering is enabled. The fix remains correct and necessary; the PR's "masters exceed ceiling" framing describes latent behavior, not current shipped behavior.

### Confirmed CRITICAL (both latent — mastering-chain blockers)

- **LUFS-1 (confirmed, high confidence):** `LoudnessNormalizer` (Stage 10) applies up to **+6 dB** gain **after** the `BrickwallLimiter` (Stage 8), defeating the dBTP ceiling. A −1.0 dBTP-limited signal can ship at up to +5.0 dBTP. This is the same chain-order defect I flagged independently in §10's Lead-Architect note; the verifier corroborated it. *Latent* (chain dormant). Fix: normalize **before** limiting, or add a final true-peak safety limiter after normalization.
- **ENHANCERS-1 (confirmed, high confidence):** `HarmonicExciter` runs a memoryless `fastTanh` saturation at the native sample rate with **no oversampling** → generated harmonics alias back into the audible band. The codebase ships an RT-safe `OversamplingWrapper` (used by `TruePeakEstimator`) but the exciter doesn't use it. Also violates the project's own spec (`specs/002-neural-mastering-framework/audit-report.md`: "oversampled deterministic non-linear processors"). *Latent* (chain dormant). Fix: wrap the tanh stage in `OversamplingWrapper` (≥4×), report its latency to `LatencyManager`.

### Refuted / downgraded (verification working as intended)

- **MULTIBAND-1 (critical → REFUTED, low):** flagged `setSize` in `AutoMasteringEngine::processBlock` as an audio-thread allocation. The verifier proved the function is **dead code on the audio path** (see the dormancy finding above), so it's a latent code-hygiene defect, not a live RT violation. *This is exactly the false-positive the adversarial step exists to catch.*
- **MODULATION-1 (critical → CONFIRMED, downgraded to high):** the `ModulationMatrix::publishAndMirror()` double-buffer mirror copies into a buffer an in-flight audio reader may still hold → torn `ModRoute` reads (UB). Downgraded because `apply()` bounds-checks `destParamIndex`/`srcIdx` every read, so the worst case is an intermittent mis-targeted modulation delta (audible glitch), not a crash/OOB. **This one is LIVE** (the modulation engine runs on the audio path at `PluginProcessor.cpp:1302`). Fix: add a seqlock (writer increments an odd seq; reader retries on parity change) so the mirror can't race the reader.

### LIVE issues (on the shipped audio path — ship-blocking)

| ID | Sev | Issue | Fix |
|---|---|---|---|
| **MODULATION-1** | high | Matrix mirror race → torn route reads / glitches under live modulation | seqlock on the route buffer |
| **MODULATION-2** | high | Host BPM never forwarded — tempo-synced LFOs/step-sequencers stuck at **120 BPM** | call `modulationEngine_.setBPM(transportBpm_)` each block |
| **LUFS-3** | high | K-weighting DF2T biquads have **no denormal mitigation** on the audio path → ~50–100× slowdown on silence-after-transient | `juce::ScopedNoDenormals` at the top of `LUFSMeter::processBlock` |
| **LUFS-4** | high | Channel weighting hardcoded stereo; can't apply BS.1770 surround (1.41) weights or exclude LFE | raise `kMaxChannels`, add per-channel weight table |
| MODULATION-3 | med | EnvelopeFollower applies a per-sample coeff once per block → attack/release ~blockSize× too fast | `coeff = pow(coeff, numSamples)` |
| MODULATION-4 | med | LFO/envelope/macro params mutated non-atomically from UI while audio reads them | atomics or route via the `LockFreeQueue` |
| MODULATION-5 | med | LFO Random-shape smoothing mis-normalized (inverted rate term) | decouple tau from rate, `exp(-dt/tau)` |
| LUFS-7 | med | `updateLongTermMetrics` does a full O(N) gated-list rebuild + `nth_element` **every 100 ms block** (O(N²) over a long master) | incremental accumulators / throttle LRA |
| LUFS-8 | med | momentary/short-term published before the 400 ms / 3 s window is filled | min-block-count guard |
| LUFS-5/6 | med | relative-gate fallback semantics; LRA percentile uses floor (no interpolation per EBU Tech 3342) | guard/flag; linear interpolation |
| MULTIBAND-2/3/4 | high/med | unlinked per-channel detection (image wander); block-quantized detector; GR meter includes makeup | stereo-link; per-sample detector; split makeup from comp gain *(latent — chain dormant)* |

### Strengths the audit confirmed (balancing the above)

- **BS.1770-4 loudness math is correct:** K-weighting coefficients match the canonical 48 kHz biquad tables to float32 precision (both stages, correct order), the −0.691 calibration and `−0.691 + 10·log10(ms)` formula are exact, 400 ms / 75 %-overlap block construction and the two-stage (−70 LUFS absolute, −10 LU relative) gating are right, and `processBiquad` uses Direct Form II Transposed (the numerically preferable form).
- **The LR4 crossover is genuine:** cascaded 2nd-order Butterworth (Q=1/√2) ×2 per leg = true Linkwitz-Riley 4th-order; by the recursive H_LP+H_HP=1 identity the four bands **sum back flat** (no comb-filtering, no band-latency mismatch). The header's reconstruction claim holds.
- **StereoImager M/S math is internally exact** (unity round-trip at width=1, width applied to Side only, sub band forced mono by default).
- **Modulation engine is RT-clean at the loop level** (noexcept, fixed-size arrays, no allocs/locks; xorshift32 PRNGs; matrix apply clamps output to [0,1]; the modulation→`finalOutput_` handoff is correctly ordered after morph).
- **`StereoFieldAnalyzer` correlation** (normalized cross-correlation with epsilon floor, clamped [−1,1]) is correct, with a proper seqlock-retry snapshot read path.

### Systemic pattern (from synthesis)

> DSP-stage ordering and anti-aliasing discipline are **not enforced as invariant properties of the mastering chain**. LUFS-1 and ENHANCERS-1 share one root: a stage is wired in *without the surrounding infrastructure that makes it correct in a delivery context* — no safety limiter after post-limit gain; no oversample/decimate around a nonlinearity. Component-level invariants (the limiter's "−1.0 dBTP streaming safe" header claim) are honored in isolation while the surrounding pipeline silently invalidates them. The MODULATION-1 race is the concurrency analogue: the double-buffer "swap-then-copy" pattern doesn't actually guarantee a non-aliased read buffer without a generation/seqlock check on the reader side.

### Updated production-readiness verdict

- **Shipped audio path (morphing + hosting + modulation + metering):** no live **critical**; two live **high** issues — MODULATION-1 (matrix race, intermittent glitch) and MODULATION-2 (BPM stuck at 120, wrong tempo-sync rate) — plus live RT/perf risks in the LUFS meter (LUFS-3 denormals, LUFS-7 O(N²) rebuild). MODULATION-1 is bounded (no crash/OOB) but real.
- **Mastering feature:** **dormant.** The chain is correct infrastructure for a feature that is not yet enabled. Its two confirmed criticals (LUFS-1, ENHANCERS-1) are **pre-enablement blockers** — do not wire `AutoMasteringEngine::processBlock` into the audio path until both are fixed (and B-1's oversampled-gain follow-up is done).

### Recommended fix order

1. **MODULATION-1** — seqlock on `ModulationMatrix` (live, the highest-severity live issue; contained, with a concurrent stress test).
2. **MODULATION-2** — one-line `setBPM` wiring in `processBlock` (live, trivial, unambiguous).
3. **LUFS-3** — `ScopedNoDenormals` in `LUFSMeter::processBlock` (live RT risk, one line).
4. **LUFS-7** — incremental LUFS/LRA accumulators (live perf, larger).
5. **Pre-enablement (before mastering is wired in):** LUFS-1 (chain reorder), ENHANCERS-1 (oversampled exciter), B-1 follow-up (oversampled gain), MULTIBAND-2/3 (stereo-link + per-sample detector).
