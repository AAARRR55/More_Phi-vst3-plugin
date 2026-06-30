# Plugin Hosting Layer Completion — Design Spec

> **Date:** 2026-06-29
> **Status:** Approved (brainstorm), pending implementation plan
> **Supersedes:** `2025-07-21-plugin-hosting-layer-enhancement.md` (kept for history)
> **Scope:** Complete and repair the in-flight hosting layer so it compiles, links, and actually delivers the four required capabilities: native editor hosting, DAW transport forwarding, crash isolation with auto-recovery, and safe teardown.
> **Approach:** Complete the existing seams; remove the parallel/duplicate mechanisms; drive the existing-but-dead state machine. Minimal new surface area.

---

## 0. Background — the actual starting state

An earlier spec (`2025-07-21-plugin-hosting-layer-enhancement.md`) proposed three new components (`HostedPluginEditor`, `DAWTransportForwarder`, `PluginHealthMonitor`) atop `PluginHostManager`. Implementation was started but is currently **non-compiling and functionally inert**. This spec describes how to salvage and complete that work. The verified defects (file:line evidence captured during brainstorm):

1. **`PluginHostManager.cpp` references deleted members.** `applyExceptionGracePeriod()` (lines ~633–669) and the `processBlock` success branch (lines ~632–646) use `exceptionCount_`, `suspended_`, `recoveryGracePeriod_` — none of which are declared in `PluginHostManager.h` (they were replaced by `healthMonitor_`). **Hard compile error.**
2. **`PluginHealthMonitor.cpp` uses `juce::jmax`/`juce::Time` with no JUCE include.** Its header includes only `<atomic>/<cstdint>/<functional>/<chrono>`. **Hard compile error.**
3. **`DAWTransportForwarder.cpp` uses a removed JUCE 8 API.** It declares `AudioPlayHead::CurrentPositionInfo` and calls `playHead->getPosition(info)`. In JUCE 8.0.4 (this project), `getPosition()` is `Optional<PositionInfo> getPosition() const`; the out-param form is the differently-named `getCurrentPosition(CurrentPositionInfo&)`. **Hard compile error.**
4. **The three new `.cpp` files are not in `CMakeLists.txt`.** `MORE_PHI_HOST_SOURCES` (lines 434–440) is an explicit list with no glob; none of the three files appear. Even with 1–3 fixed, **link errors** for every non-inline method of the new types.
5. **`DAWTransportForwarder::applyToPlugin()` is a no-op.** It reads `PositionInfo`, mutates a local copy, discards it, and never calls any setter. It is also **never called** from anywhere. The transport relay is dead.
6. **The auto-recovery state machine is never driven.** `shouldRecover()`/`beginRecovery()`/`endRecovery()` have zero call sites. Once a plugin trips 20 failures → `Suspended`, nothing attempts recovery, so it stays bypassed forever.
7. **The UI bypasses `HostedPluginEditor` entirely.** `PluginBrowserPanel` uses a separate, working `HostedPluginWindow` (UI-layer) to own/parent the editor. The manager's `activeEditor_` (`HostedPluginEditor`) is created with a `nullptr` parent, never shown, queried by nobody — a second, parallel, inert editor lifecycle.
8. **No tests** for any of the new components. The only host test (`TestHostIntegration.cpp`) touches none of the new APIs.

**The unifying fix:** complete the seams that already work, remove the duplicates, and wire the one dead driver. Every requirement is satisfied by finishing existing machinery — not by introducing parallel mechanisms.

---

## 1. Architecture overview

`PluginHostManager`'s core is sound and is **not** rewritten: ref-counted leases (`acquirePluginForUse`/`releasePluginFromUse`/`beginExclusivePluginUse`), the SEH trampoline, the deferred-doom queue + bounded-wait teardown, the JUCE-8 audio-thread `setPlayHead` path, and the per-plugin `KnownPluginList` cache. The `IPluginHostManager` interface gains **no** new pure-virtuals — new capability is opt-in via concrete methods.

| Subsystem | Decision (approved) | Net change |
|---|---|---|
| **Native editor** | Consolidate on `HostedPluginWindow`; **retire `HostedPluginEditor`** | Delete the file + `activeEditor_` member + its manual destructor dance. Keep the working UI path + `setWindowCloseCallback`. Document the lease-as-teardown-gate invariant. |
| **DAW transport** | `DAWTransportForwarder` = **audio→message snapshot bus only** | Rewrite to JUCE 8 API + seqlock. Delete the no-op `applyToPlugin()`. Plugin transport stays on audio-thread `setPlayHead` (unchanged). |
| **Crash isolation / recovery** | Keep `PluginHealthMonitor`; **drive it** | Fix the missing JUCE include. Add `tickHealth()` on the existing 50 ms timer → background-thread `releaseResources()`+`prepareToPlay()` reinit via `beginExclusivePluginUse`. |
| **Safe teardown** | Harden the existing deferred-doom + lease machinery | Add a `shuttingDown_` guard so recovery aborts cleanly on unload; debug-only doom-queue size assertion; documented invariants. No new teardown mechanism. |
| **Build + tests** | Add to CMake; add unit tests | Two `.cpp` into `MORE_PHI_HOST_SOURCES`; three new Catch2 test files. |

**Principle:** every requirement is met by *completing existing seams* rather than introducing parallel mechanisms. We remove a duplicate editor lifecycle, remove a no-op transport relay, and drive the existing-but-dead recovery state machine.

### Threading contract (must hold for all new code)

| Method / access | Caller thread | Mechanism |
|---|---|---|
| `healthMonitor_.reportSuccess/reportFailure/shouldProcess` | **Audio thread only** | `noexcept`, relaxed/acquire atomics (already so) |
| `healthMonitor_.shouldRecover/beginRecovery/endRecovery/getSnapshot` | **Message thread only** (`tickHealth`) or the recovery bg thread | Atomics; `beginRecovery` uses CAS |
| `tickHealth()` | **Message thread only** (the 50 ms timer) | Cheap poll; launches reinit on bg thread |
| `tryStartRecovery()` + the `juce::Thread::launch` lambda | Launched from message thread; body on **background thread** | `recoveryInFlight_`/`shuttingDown_` guards; reinit via `beginExclusivePluginUse` |
| `transportForwarder_.updateFromAudioThread` | **Audio thread only** | Seqlock write (release) |
| `transportForwarder_.getSnapshot` | **Message thread only** | Seqlock read (acquire + retry) |
| `unloadPlugin()` setting `shuttingDown_` | Message thread | Atomic store-release |

This preserves the project's strict invariant (AGENTS.md): **agents/heavy work never run on the audio thread; the audio thread performs no allocations or locks.** The recovery reinit happens on a background thread behind `beginExclusivePluginUse`, which snaps the audio thread off the plugin (it sees `nullptr` and fades to silence). The transport producer is a lock-free seqlock write.

---

## 2. Native plugin editor integration

The manager's job is **not** to own the editor — `HostedPluginWindow` already does that correctly. The manager's job is to **guarantee the editor is torn down before the plugin instance dies, and notify the UI when that is about to happen.**

### Editor ownership contract (existing working path, made explicit)

```
PluginBrowserPanel::openPluginEditor()          [src/UI/PluginBrowserPanel.cpp:219-258]
  → host_.acquirePluginForUse()                  // lease; holds the instance alive
  → HostedPluginWindow(plugin, onClose, releaseCb)   [src/UI/HostedPluginWindow.h]
       └─ setContentOwned(plugin->createEditor())    // window owns the editor
  → ~HostedPluginWindow()
       └─ clearContentComponent() FIRST              // editor dies
       └─ releasePluginCallback_() once              // THEN the lease drops
```

The lease **is** the safety mechanism: while the window is open, `activePluginUsers_ ≥ 1`, so `unloadPlugin()`'s bounded 500 ms wait either drains naturally or defers the instance to the doom-queue. The editor never outlives its instance.

### Changes in the manager

1. **Delete `HostedPluginEditor`** — the file, the `#include`, the `activeEditor_` member, and the fragile `activeEditor_.~HostedPluginEditor()` + placement-new reconstruction at `PluginHostManager.cpp:399-400` (calling a destructor then placement-new on a member with a custom deleter is UB-adjacent and now holds no real editor).
2. **Keep `setWindowCloseCallback`** (the 2025 spec proposed deprecating it; we keep it — it is the working interop seam the UI already wires for out-of-band unload).
3. **Document the teardown-before-instance invariant** in `unloadPlugin()` (see §5). The existing lease count is the gate; no new mechanism.

**Why not migrate the UI to a manager-owned editor:** it would destabilize the one editor path that works, invent a Component-handoff contract across the manager/UI boundary, and the only safety property needed — editor dies before instance — is *already* guaranteed by the lease. Retiring `HostedPluginEditor` is the smaller, safer blast radius.

---

## 3. DAW transport forwarding

The hosted plugin already receives full transport (BPM, time signature, loop, position) **sample-accurately** via `plugin->setPlayHead(playHead)` on the audio thread (`PluginProcessor.cpp:2176`, cached to avoid per-block calls via the H9 fix). So `DAWTransportForwarder` does **not** feed the plugin.

### The structural problem it solves

`MorePhiProcessor::updateTransportContextSnapshot()` (`PluginProcessor.cpp:2117-2146`) reads `AudioPlayHead` on the audio thread into a field of *individual* relaxed atomics (`transportBpm_`, `transportPlaying_`, `transportPpqPosition_`, …). A consumer reading them one-by-one gets a **torn snapshot** (BPM from block *N*, position from block *N+1*). The forwarder provides a **versioned, atomic, all-fields-coherent snapshot** in one read.

### Design — seqlock versioned snapshot

```cpp
struct TransportState {        // trivially copyable
    double  bpm = 120.0;
    int     timeSigNumerator = 4;
    int     timeSigDenominator = 4;
    bool    isPlaying = false;
    bool    isLooping = false;
    double  ppqPosition = 0.0;       // musical position
    double  timeInSeconds = 0.0;
    uint32_t version = 0;            // bumped on every publish
};
```

- **Producer (audio thread):** `void updateFromAudioThread(const juce::AudioPlayHead*) noexcept;` — reads `playHead->getPosition()` (JUCE 8 `Optional<PositionInfo>` API, fixing the current compile error), fills a `TransportState`, publishes via a **seqlock**: increment an odd sequence counter before the write, even after (release); consumer reads the counter (acquire) before/after and retries if it changed mid-read. This is the project's established pattern (`SnapshotBank` uses the same seqlock technique, per AGENTS.md) — coherent multi-field read, no lock, no per-field tearing.
- **Consumer (message thread):** `std::optional<TransportState> getSnapshot() const noexcept;` — coherent read, retry on seqlock mismatch.
- **Producer call site:** the existing `processBlock` site, immediately after `hostManager.setPlayHead(playHead)` (`PluginProcessor.cpp:2176`).

### Deleted

The broken `applyToPlugin()` is removed entirely — it was a no-op (populated `PositionInfo` discarded, no setter called) and redundant with `setPlayHead`. The speculative "find a tempo/bpm parameter and `setValue`" loop (unsound param-name guessing with an arbitrary normalization scale) is removed.

### Relationship to the existing processor atomics

We do **not** rip out `updateTransportContextSnapshot()` — it feeds `context.get_transport` today and works. The forwarder is offered as the *preferred* coherent-snapshot source going forward; consumers needing a coherent read migrate to `getSnapshot()`. This avoids a risky rewrite of the working MCP path.

### Threading

Producer = audio thread only (sequence-counter release after write). Consumer = message thread only (acquire before/after read with retry). No locks, no allocations. Cache-line padded (`padding_[64]`, sized correctly) to avoid false sharing. Matches the AGENTS.md memory-ordering convention for seqlock-style producer/consumer.

---

## 4. Crash isolation & auto-recovery

`PluginHealthMonitor`'s state machine is well-formed; today it is simply never driven. The design: the existing 50 ms maintenance timer polls recovery; a background thread does the reinit; the audio thread only ever calls `reportSuccess`/`reportFailure`/`shouldProcess`.

### State machine (as implemented, now wired)

```
Healthy ──fail──► Degraded ──20 fails──► Suspended ──5s + timer poll──► Recovering
   ▲                                              │                          │
   └───────────── success ──────────────  endRecovery(true) ◄────────┘ reinit on bg thread
                          reinit fails ──► back to Suspended (retry after another 5s)
              3 failed attempts ──► Terminated (bypassed permanently this session)
```

Config is unchanged and matches existing constants: **20** consecutive failures → suspend, **5 s** recovery delay, **3** attempts max. `shouldProcess()` returns true only for `Healthy`/`Degraded` (a degraded plugin still processes — single glitches must not mute audio); false for `Suspended`/`Recovering`/`Terminated` (bypass with fade, via the existing `currentGain_` ramp).

### Driver — `PluginHostManager::tickHealth()`, on the existing timer

The processor's 50 ms `timerCallback` already calls `hostManager.drainDeferredDoomedPlugins()` (`PluginProcessor.cpp:4242`). Add one line beside it:

```cpp
hostManager.tickHealth();   // message thread, ~microseconds when healthy
```

`tickHealth()` does only two cheap things on the message thread:
1. `if (healthMonitor_.shouldRecover()) tryStartRecovery();`
2. (optionally) refresh a cached health snapshot for UI/MCP getters.

It does **not** do the reinit itself — reinit is heavy and must not jank the message thread.

### Reinit — background thread, off audio + message threads

```cpp
void tryStartRecovery() {
    if (shuttingDown_.load(std::memory_order_acquire)) return;
    if (recoveryInFlight_.exchange(true)) return;                 // one recovery at a time
    if (!healthMonitor_.beginRecovery()) { recoveryInFlight_ = false; return; } // CAS Suspended→Recovering
    juce::Thread::launch([this] {
        bool ok = false;
        if (!shuttingDown_.load(std::memory_order_acquire)) {
            if (auto* p = beginExclusivePluginUse(/*timeoutMs*/ 500)) {   // snap off audio thread
                try { p->releaseResources(); p->prepareToPlay(currentSampleRate, currentBlockSize); ok = true; }
                catch (...) { ok = false; }
                endExclusivePluginUse();
            }
        }
        healthMonitor_.endRecovery(ok);                           // → Healthy (reset) or back to Suspended
        recoveryInFlight_ = false;
    });
}
```

This reuses the existing `beginExclusivePluginUse()`/`endExclusivePluginUse()` pair — the exact mechanism built for opaque state capture. While exclusive use is requested, `acquirePluginForUse()` returns nullptr on the audio thread, so `processBlock` skips the plugin and fades to silence; the reinit therefore cannot race audio processing. `endRecovery(true)` resets to `Healthy` and `currentGain_` ramps back in on the next processed block; `endRecovery(false)` returns to `Suspended` and the timer retries after another 5 s (up to 3 attempts, then `Terminated`).

### `PluginHostManager.cpp` repair (the compile fix)

The dead `applyExceptionGracePeriod()` (which references `exceptionCount_`/`suspended_`/`recoveryGracePeriod_`) is removed. All failure accounting flows through `healthMonitor_.reportFailure()` (already called at `PluginHostManager.cpp:499/573/584`) and the success reset through `reportSuccess()` (already at `:589`). The broken success branch at `:632-646` is rewritten to just `healthMonitor_.reportSuccess()` + the `currentGain_` fade-in. `MAX_PLUGIN_EXCEPTIONS = 20` stays as the documented default and is mirrored via `healthMonitor_.setMaxConsecutiveFailures(20)`.

### Graceful degradation (the 20-failure threshold)

`reportFailure()` increments; on the 20th consecutive the monitor flips to `Suspended` and stamps `suspendedAtMs_`. From that block `shouldProcess()` is false → audio fades to silence/bypass (existing `currentGain_` ramp + `buffer.clear()`). Degradation is per-hosted-plugin; other plugins/host tracks are unaffected.

---

## 5. Safe plugin teardown

The deferred-destruction machinery is already robust (C-1/L-1 fixes). Three hardenings compose it with the recovery + editor paths:

### 5.1 Explicit drain before load (memory-bloat guard)

`loadPlugin()` calls `unloadPlugin()` then `drainDeferredDoomedPlugins()` (`PluginHostManager.cpp:316-320`). Ordering is kept (create-new-first so a failed creation leaves the old plugin running; drain after the old instance is moved to the doom queue and before the new one publishes). We document this as an invariant and add a **debug-only** assertion that the doom queue can't grow unbounded:

```cpp
jassert(deferredDoomedPlugins_.size() < 4);   // a handful at most during rapid cycling; more ⇒ stuck lease
```

### 5.2 Recovery + teardown composition

A plugin can be mid-recovery (background thread inside `beginExclusivePluginUse`) when `unloadPlugin()` is called. The existing unload already bounded-waits 200 ms for `exclusivePluginUseRequested_` then force-releases (`PluginHostManager.cpp:390-392`). Extended to the recovery path: `unloadPlugin()` sets `shuttingDown_`; the recovery lambda checks `shuttingDown_` before *and* after `beginExclusivePluginUse` and exits without touching the plugin if set; `tryStartRecovery()` refuses to launch when `shuttingDown_`. Teardown never races a reinit — recovery either completes its exclusive window before unload's 200 ms force-release, or observes shutdown and aborts cleanly. The doomed instance (if recovery held the last lease) flows through the standard doom queue.

### 5.3 Editor teardown-before-instance (cross-ref §2)

The lease-count gate already guarantees this: an open `HostedPluginWindow` holds `activePluginUsers_ ≥ 1`, so `unloadPlugin()`'s bounded wait either drains (window closed, editor gone) or defers. Documented invariant in `unloadPlugin()`:

> `hostedPlugin.reset()` / doom-queue enqueue is reached only after the editor window has been closed (via `windowCloseCallback_`) and its lease released, OR after the bounded wait — in which case the instance is deferred, not destroyed, so the editor (if somehow still alive) references a live instance until the next drain. `HostedPluginWindow`'s own dtor does `clearContentComponent()` before `releasePluginCallback_()` (`HostedPluginWindow.h:45-64`), so even the deferred path is safe.

### Deliberately not added (YAGNI)

Cross-block all-or-nothing plan buffering (unrelated — that is `OzonePlanApplicator` plan-atomicity, out of scope); a separate "editor lease" type (the ref-counted lease already covers it); unbounded waits (bounded 500 ms + doom-queue is the project's established safe pattern).

---

## 6. Build wiring

`MORE_PHI_HOST_SOURCES` (`CMakeLists.txt:434-440`) is an explicit list with no glob and is reused by `MorePhi`, `MorePhiCLI`, and `MorePhiMcpCore`. Since `HostedPluginEditor` is retired, only two new files are added (one edit covers all three targets):

```cmake
set(MORE_PHI_HOST_SOURCES
    src/Host/IPluginHostManager.h
    src/Host/PluginHostManager.h
    src/Host/PluginHostManager.cpp
    src/Host/ParameterBridge.h
    src/Host/ParameterBridge.cpp
    src/Host/DAWTransportForwarder.h        # NEW
    src/Host/DAWTransportForwarder.cpp       # NEW
    src/Host/PluginHealthMonitor.h           # NEW
    src/Host/PluginHealthMonitor.cpp         # NEW
)
```

`HostedPluginEditor.{h,cpp}` are deleted from disk.

---

## 7. Testing strategy

New Catch2 v3 unit tests in `tests/Unit/`, matching the existing convention:

1. **`TestPluginHealthMonitor.cpp`** — pure state machine, no JUCE audio graph:
   - `reportFailure × 20 → Suspended`; `reportSuccess` from `Degraded → Healthy`; `shouldRecover()` false until the delay elapses, true after (use the configurable delay at its 100 ms minimum to keep tests fast); `beginRecovery` CAS only from `Suspended`; `endRecovery(false) → Suspended`; 3 failed attempts → `Terminated`; `Terminated` sticky.
   - `reportSuccess` from `Recovering → Healthy` with counts reset.
2. **`TestDAWTransportForwarder.cpp`** — seqlock coherence, no plugin:
   - Producer writes a known `TransportState` via a stub `AudioPlayHead` returning a known `PositionInfo` (mirror the stub pattern in `tests/Unit/` and `HeadlessHostMain.cpp`); consumer `getSnapshot()` returns the coherent struct; version bumps on each publish; no-op when unchanged.
   - Seqlock retry path: assert the consumer never observes a torn `TransportState` under a concurrent producer.
3. **`TestPluginHostManagerHealth.cpp`** — manager + monitor integration via the existing `IPluginHostManager` seam and a dummy plugin/stub:
   - `tickHealth()` is a no-op when `Healthy`; `tryStartRecovery()` launches only once (`recoveryInFlight_` guard); a plugin that throws in `processBlock` advances the monitor; after threshold + recovery the manager returns to processing.
   - Teardown: `unloadPlugin()` during in-flight recovery sets `shuttingDown_`; the recovery aborts without UAF (assert no crash, doom queue handled).

Registered alongside `TestHostIntegration.cpp`; run via `build-ninja.bat tests` / `ctest`.

---

## 8. File-change manifest

| File | Action | What |
|---|---|---|
| `src/Host/HostedPluginEditor.h` | **Delete** | Retired (§2) |
| `src/Host/HostedPluginEditor.cpp` | **Delete** | Retired (§2) |
| `src/Host/DAWTransportForwarder.h` | **Rewrite** | `TransportState` (+ ppq/timeInSeconds), seqlock, `getSnapshot()`; drop `applyToPlugin` |
| `src/Host/DAWTransportForwarder.cpp` | **Rewrite** | JUCE 8 `getPosition()` API; seqlock producer/consumer |
| `src/Host/PluginHealthMonitor.h` | **Edit** | Add `#include <juce_core/juce_core.h>` (for `juce::jmax`/`juce::Time`); otherwise unchanged |
| `src/Host/PluginHealthMonitor.cpp` | **Edit** | Same include (compile fix); logic unchanged |
| `src/Host/PluginHostManager.h` | **Edit** | Remove `HostedPluginEditor` include + `activeEditor_` member; add `tickHealth()`, `tryStartRecovery()`, `recoveryInFlight_`, `shuttingDown_`; keep `getHealthState`/`getHealthSnapshot` getters (now the first real health consumers for UI/MCP) |
| `src/Host/PluginHostManager.cpp` | **Repair + extend** | Remove `applyExceptionGracePeriod` + dead-member refs (compile fix); rewrite `processBlock` success/failure accounting onto `healthMonitor_`; remove `activeEditor_` ctor/dtor dance; add `tickHealth`/`tryStartRecovery` + shutdown guards |
| `src/Plugin/PluginProcessor.cpp` | **Edit** | Add `hostManager.tickHealth()` in `timerCallback` (next to `:4242`); add `transportForwarder_.updateFromAudioThread(playHead)` in `processBlock` (next to `:2176`) |
| `CMakeLists.txt` | **Edit** | Add 4 new source lines to `MORE_PHI_HOST_SOURCES` |
| `tests/Unit/TestPluginHealthMonitor.cpp` | **Create** | State-machine tests |
| `tests/Unit/TestDAWTransportForwarder.cpp` | **Create** | Snapshot/seqlock tests |
| `tests/Unit/TestPluginHostManagerHealth.cpp` | **Create** | Manager + recovery integration tests |
| tests CMake entry | **Edit** | Register the 3 new test files |

---

## 9. Rollout order (compile-safe at every step)

1. **CMake + includes + JUCE 8 API** — add the 2 `.cpp` to `MORE_PHI_HOST_SOURCES`; fix `PluginHealthMonitor`'s include; rewrite `DAWTransportForwarder` to JUCE 8. → *those two files compile standalone.*
2. **Delete `HostedPluginEditor`** + remove `activeEditor_` + its ctor/dtor dance. → *removes one source of breakage.*
3. **Repair `PluginHostManager.cpp`** — remove `applyExceptionGracePeriod` + dead-member refs; rewrite the `processBlock` accounting onto `healthMonitor_`. → *the whole Host layer compiles + links.*
4. **Wire recovery** — add `tickHealth`/`tryStartRecovery`/guards + the `PluginProcessor` timer call site.
5. **Wire transport** — add the `processBlock` producer call site.
6. **Tests** — add the 3 test files, register, run via `build-ninja.bat tests`.
7. **Verify** — full build + `ctest` green; spot-check audio-thread invariants (no allocations/locks on the audio path — recovery reinit is on a background thread via exclusive-use; transport producer is a seqlock write).

Each step leaves the tree compiling, so a failure at any step is bisectable.

---

## 10. Success criteria (mapped to requirements)

| Requirement | How satisfied | Evidence |
|---|---|---|
| Native editor integration | `HostedPluginWindow` owns/parents the editor; lease guarantees editor-dies-before-instance; `setWindowCloseCallback` handles out-of-band unload | §2 |
| DAW transport forwarding | Plugin gets sample-accurate transport via audio-thread `setPlayHead` (unchanged); message-thread consumers get a coherent versioned snapshot via the forwarder | §3 |
| Plugin discovery & management | Already implemented (`scanPluginFolders`, `discoverPlugin` two-stage, `KnownPluginList` cache with invalidation) — **out of scope, already working**; this spec does not touch it | (unchanged) |
| Crash isolation (20 failures → bypass) | `PluginHealthMonitor` `reportFailure` × 20 → `Suspended`; `shouldProcess()` gates `processBlock`; fade-to-silence bypass | §4 |
| Auto-recovery | 50 ms timer `tickHealth()` → background-thread reinit via `beginExclusivePluginUse`; `endRecovery` transitions; 3 attempts then `Terminated` | §4 |
| Safe teardown | Deferred-doom + bounded wait (existing) + `shuttingDown_` recovery-abort guard + debug doom-queue assertion; documented invariants | §5 |
| Graceful degradation | Per-plugin isolation; a failed plugin bypasses without crashing the host or blocking other plugins | §4–5 |
| Error logging | `DBG` only (AGENTS.md); diagnostic messages on recovery attempts, suspension, termination | §4 |
| Cross-platform paths | Already handled by JUCE format manager + `addDefaultFormats` (VST3/AU); **out of scope** | (unchanged) |

**Note on scope:** "Plugin discovery & management" (requirement 3) is already fully implemented and working (`scanPluginFolders`, `discoverPlugin`, the `KnownPluginList` XML cache with stale-cache fallback, the browsable `PluginBrowserPanel`). This spec explicitly does **not** re-litigate it. The work here is the four *other* capabilities that are broken or inert.
