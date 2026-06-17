# Sub-Agent 1 Report: Plugin Hosting & Memory Safety Audit
**Project:** More-Phi VST3 Plugin v3.3.0  
**Scope:** `src/Host/`, `src/Plugin/`, `src/Core/LockFreeQueue.h`, `src/Core/AudioBufferPool.{h,cpp}`  
**Date:** 2025-06-11  
**Auditor:** Sub-Agent 1 (Plugin Hosting & Memory Safety)

---

## Executive Summary

The hosting layer (`PluginHostManager`, `ParameterBridge`) is **generally well-engineered** with robust exception handling, lease-based lifetime management, and graceful degradation for misbehaving hosted plugins. However, several **contract mismatches**, **thread-safety gaps**, and **unbounded waits** introduce risks of DAW hangs, audio-thread violations, and incorrect state propagation. The `LockFreeQueue` implementation is correct for its SPSC+MPMC-producer design. `AudioBufferPool` is not real-time safe and must not be used on the audio thread.

---

## Critical Issues

### 1. `noexcept` Contract Mismatch: `processBlock` → `hostManager.processBlock`  
**File:** `PluginProcessor.cpp:1174`, `PluginHostManager.h:31`  
**Severity:** Crash (`std::terminate`)

`MorePhiProcessor::processBlock` is declared `noexcept` (line 1174), yet it calls `hostManager.processBlock()` which is **not** `noexcept`. While `PluginHostManager::processBlock` currently wraps all work in `try/catch`, the **interface contract does not guarantee this**. A future regression (e.g., removing a catch block) or a hosted plugin throwing from a destructor during stack unwinding would bypass the catches and invoke `std::terminate` on the audio thread, crashing the DAW instantly.

**Recommended Fix:** Mark `PluginHostManager::processBlock` as `noexcept` (it already handles all exceptions internally). Alternatively, add a `noexcept` wrapper in `PluginProcessor` that calls the non-noexcept `hostManager.processBlock` inside its own `try/catch`.

```cpp
// PluginHostManager.h
void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;  // change to noexcept

// PluginHostManager.cpp
void PluginHostManager::processBlock(juce::AudioBuffer<float>& buffer,
                                      juce::MidiBuffer& midi) noexcept  // add noexcept
{
    // existing body
}
```

---

### 2. Unbounded Wait in `unloadPlugin` Can Hang DAW Forever  
**File:** `PluginHostManager.cpp:258`  
**Severity:** DAW Freeze / Deadlock

`unloadPlugin` performs an **unbounded** `while (activePluginUsers_.load(...) > 0) yield();` after setting `hostedPluginPtr_ = nullptr`. If the audio thread crashes or is preempted while holding a lease (e.g., inside a hosted plugin's `processBlock` that deadlocks), the counter never reaches zero. The destructor calls `unloadPlugin`, so removing a track or closing the project will hang the DAW. The H-3 FIX comment (line 246) acknowledges this and explicitly states the wait is "intentionally still unbounded" because "proceeding past a live audio lease would be a use-after-free." However, an **indefinite hang is worse than a controlled crash** in a DAW context.

**Recommended Fix:** Add a bounded wait (e.g., 500 ms) with a diagnostic flag. If the timeout fires, log the fault and proceed, accepting the vanishingly small risk of a use-after-free over a guaranteed user-visible hang.

```cpp
void PluginHostManager::unloadPlugin()
{
    // ... existing bounded wait for exclusive use ...

    hostedPluginPtr_.store(nullptr, std::memory_order_release);

    const auto waitStart = juce::Time::getMillisecondCounter();
    while (activePluginUsers_.load(std::memory_order_acquire) > 0)
    {
        if (static_cast<int>(juce::Time::getMillisecondCounter() - waitStart) > 500)
        {
            // Audio thread is stuck. Force proceed to avoid hanging the DAW.
            DBG("PluginHostManager::unloadPlugin — timeout waiting for audio thread lease");
            break;
        }
        juce::Thread::yield();
    }
    // ... rest of unload ...
}
```

---

### 3. `recoveryGracePeriod_` Not Reset on New Plugin Load  
**File:** `PluginHostManager.cpp:232-234`  
**Severity:** Incorrect Plugin Suspension

`loadPlugin` resets `exceptionCount_` and `suspended_` but **does not reset `recoveryGracePeriod_`**. If the previous plugin was in a grace period (e.g., 10 blocks remaining) when it was unloaded, the new plugin inherits that value. If the new plugin throws on its first `processBlock`, the grace-period check (lines 389, 406, 461, 478) sees `recoveryGracePeriod_ > 0` and suspends immediately, even though the new plugin has only failed once. This is incorrect: a new plugin should get the full 20-exception budget before suspension.

**Recommended Fix:** Reset `recoveryGracePeriod_` to 0 in `loadPlugin`.

```cpp
// In PluginHostManager::loadPlugin, after successful swap:
exceptionCount_.store(0, std::memory_order_relaxed);
suspended_.store(false, std::memory_order_relaxed);
recoveryGracePeriod_.store(0, std::memory_order_relaxed);  // ADD THIS
```

---

### 4. `ParameterBridge::applyParameterState` Aborts Entire Batch on Single Parameter Throw  
**File:** `ParameterBridge.cpp:162-200`  
**Severity:** Silent Partial Failure / Parameter Drift

`applyParameterState` calls `withPlugin`, which wraps the entire lambda in a single `try/catch(...)`. If `params[i]->setValue(clamped)` throws for any single parameter (e.g., a buggy hosted plugin parameter), the **entire batch is aborted** and the remaining parameters are not updated. This leads to silent partial state application: a snapshot recall may set only the first 50 of 200 parameters, leaving the rest at their old values. This is a data-integrity failure.

**Recommended Fix:** Move the `try/catch` inside the per-parameter loop so that one bad parameter is skipped but the rest are applied.

```cpp
for (int i = 0; i < safeCount; ++i)
{
    const float clamped = juce::jlimit(0.0f, 1.0f, values[i]);
    bool throttled = false;
    if (hasThrottleLock)
    {
        const auto& state = throttleStates_[static_cast<size_t>(i)];
        throttled = state.lastValue >= 0.0f
                 && (now - state.lastUpdateTime) < 2
                 && std::abs(clamped - state.lastValue) <= 0.01f;
    }
    if (throttled) continue;

    try {
        params[i]->setValue(clamped);
    } catch (...) {
        // Skip this parameter; continue with the rest
        continue;
    }

    if (hasThrottleLock)
        throttleStates_[static_cast<size_t>(i)] = {clamped, now};
}
```

---

### 5. `PluginHostManager::processBlock` Suspended Path Returns Without Clearing Buffer (Stale Audio Risk)  
**File:** `PluginHostManager.cpp:319-346`  
**Severity:** Audio Glitch / Stale Data

When a plugin is `suspended_`, the function returns at line 346 without clearing `buffer`. The comment says "Output silence (buffer unchanged = pass-through)" but the **behavior is dry pass-through**, not silence. For an effect plugin host, the buffer may contain stale audio from the previous block. Although the exception path (lines 387, 404, 459, 476) clears the buffer before suspension, if the plugin reaches suspension for any other reason (e.g., future manual suspend API), the buffer would leak unprocessed audio.

**Recommended Fix:** Clear the buffer unconditionally in the suspended path.

```cpp
if (suspended_.load(std::memory_order_relaxed))
{
    buffer.clear();  // Ensure silence, not stale pass-through
    // ... existing recovery logic ...
    return;
}
```

---

## High-Priority Improvements

### 6. `PluginHostManager::processBlock` Should Be `noexcept`  
**File:** `PluginHostManager.h:31`  
The function is called exclusively from the real-time audio thread and is already internally exception-safe. Marking it `noexcept` formalizes the contract and prevents accidental regressions.

### 7. `PluginHostManager::prepare` Wide-Buffer Race Condition  
**File:** `PluginHostManager.cpp:111`  
`prepare()` resizes `wideBuffer_` on the message thread via `setSize(...)`. In the same function, `processBlock()` reads `wideBuffer_` on the audio thread. JUCE's `AudioBuffer::setSize` may reallocate heap memory if the size changes. A misbehaving host that calls `prepareToPlay` concurrently with `processBlock` could cause a use-after-free or race on the internal channel pointers. The code assumes JUCE's lifecycle guarantees, but a defensive `std::atomic<bool> preparing_` flag or a pre-allocated max-size buffer would eliminate the risk.

### 8. `ParameterBridge` Heap Allocations in Audio-Reachable Paths  
**File:** `ParameterBridge.cpp:202-213`, `408-425`  
`captureParameterState()` allocates a `std::vector<float>` of size `params.size()`. `getParameterDescriptors()` allocates a `std::vector<ParameterDescriptor>`. Both are public virtual methods reachable from any thread. If called from the audio thread (even accidentally), they violate the real-time "zero allocations after prepare" rule. **Recommendation:** Change these APIs to accept pre-allocated output buffers, or document them as **message-thread only** and add `jassert` guards.

### 9. `setStateInformation` Race on `pendingPluginDesc_`  
**File:** `PluginProcessor.cpp:1780-1808`  
`pendingPluginDesc_` is a plain `juce::PluginDescription` (not atomic) written in `setStateInformation` and read in `timerCallback`. `setStateInformation` may be called from any thread (JUCE does not guarantee the message thread). If two threads call it concurrently, or if the timer fires while a new state is being set, `pendingPluginDesc_` is read/written without synchronization, resulting in a data race. **Fix:** Protect `pendingPluginDesc_` with a `SpinLock` or `std::mutex`.

### 10. `getLastDescriptionRef` Is Thread-Unsafe  
**File:** `PluginHostManager.h:76`  
Returns a `const juce::PluginDescription&` to `lastDescription` without acquiring `descLock_`. Meanwhile `loadPlugin` writes `lastDescription` under `descLock_`. Any concurrent caller gets a data race on the `juce::String` members inside `PluginDescription`. This should be removed or documented as "caller must hold descLock_" (which is private).

### 11. `ParameterBridge::withPlugin` Fallback Path Is Unleased  
**File:** `ParameterBridge.cpp:54-89`  
If `cachedConcreteHost_` is null (theoretically impossible given current usage, but reachable via the interface), `withPlugin` falls back to `host.getPlugin()` and returns the raw pointer **without incrementing `activePluginUsers_`**. This bypasses the lease system and could lead to a use-after-free during concurrent unload. **Fix:** Remove the fallback; `cachedConcreteHost_` is guaranteed non-null in this codebase. Or, add a lease in the fallback path.

### 12. Duplicate Exception-Handling Code in `processBlock`  
**File:** `PluginHostManager.cpp:375-418`, `439-489`  
The m-5 FIX grace-period logic is copy-pasted in four places (wide-buffer path and normal path, each with `std::exception` and `catch(...)`). This is a maintenance hazard. A single inline helper function should handle exception bookkeeping.

---

## Medium-Priority Refinements

### 13. `PluginHostManager::setPlayHead` Virtual Call Per Block  
**File:** `PluginHostManager.cpp:316`  
`plugin->setPlayHead(...)` is called on every `processBlock` invocation. Some hosted plugins perform expensive work in `setPlayHead` (e.g., allocating transport context structures). Cache the last playhead pointer and only call `setPlayHead` when it changes.

### 14. `ParameterBridge::getParameterNormalized` Lease Per Parameter  
**File:** `ParameterBridge.cpp:103-112`  
Inside `PluginProcessor::processBlock` (line 1353), `paramBridge.getParameterNormalized(i)` is called inside a loop for every parameter. Each call does `acquirePluginForUse()` / `releasePluginFromUse()` (atomic inc/dec). For 1000+ parameters, this is ~2000 atomic operations per block. Consider adding a batched `getParameterNormalized` that acquires the lease once and reads all values into a pre-allocated buffer.

### 15. `PluginHostManager::exceptionCount_` Signed Overflow Risk  
**File:** `PluginHostManager.h:128`  
While suspended, `exceptionCount_` increments once per block. Signed integer overflow is technically undefined behavior in C++. At 512-sample blocks @ 48 kHz, overflow would take ~5.5 hours of continuous suspension. Extremely unlikely, but saturating arithmetic (`std::min(count, MAX_PLUGIN_EXCEPTIONS)`) would be safer.

### 16. `LockFreeQueue::pushRange` `std::distance` O(N) Assumption  
**File:** `Core/LockFreeQueue.h:66-88`  
`pushRange` uses `std::distance` on the input range. For most containers this is O(1), but for arbitrary ranges it may be O(N), potentially holding the producer `SpinLock` for a long time. Add a `static_assert` or documentation that the range must provide O(1) `std::distance`.

### 17. `PluginHostManager::scanPluginFolders` Thread Safety  
**File:** `PluginHostManager.cpp:492-508`  
`scanPluginFolders` modifies `knownPlugins` without any lock. If the UI thread reads `knownPlugins` while a background `PluginScanner` thread is scanning, there is a data race. `PluginScanner` is a `juce::Thread` that calls `scanPluginFolders`. The `KnownPluginList` is not thread-safe. **Fix:** Add a `SpinLock` around `knownPlugins` access, or ensure scanning only happens on the message thread.

### 18. `AudioBufferPool` Uses `std::mutex` (Not Real-Time Safe)  
**File:** `Core/AudioBufferPool.h:74`  
The pool uses `std::mutex` with `std::lock_guard`. If this class is ever used on the audio thread (e.g., in the audio-domain processing path), it will block on kernel mutexes, breaking real-time guarantees. The header comment says "thread-safe" but does not say "audio-thread safe." Add a prominent comment: **"DO NOT use on the audio thread — use only from the message thread or background worker threads."** Or replace `std::mutex` with `juce::SpinLock` if it must be audio-reachable.

### 19. `exceptionLog_` Dead Code + Dangling Pointer Risk  
**File:** `PluginHostManager.h:146-148`  
`exceptionLog_`, `exceptionLogCursor_`, and `lastExceptionCode_` are declared but **never written to** in the `.cpp` file. If future code writes raw `e.what()` pointers into `std::atomic<const char*>`, they will dangle when the `std::exception` object is destroyed. **Fix:** Either remove the dead code, or use `std::array<std::atomic<juce::String>, ...>` (but `juce::String` is not trivially copyable). A small ring buffer of `std::array<char, 128>` would be safer.

---

## Low-Priority Enhancements

### 20. Raw `new` in `createEditor` and `createPluginFilter`  
**File:** `PluginProcessor.cpp:2222`, `2268`  
JUCE convention requires returning raw pointers from these factory functions, but `createPluginFilter` lacks a `try/catch` guard. Add one for consistency with `createEditor`.

### 21. Comment/Behavior Mismatch in Suspended Path  
**File:** `PluginHostManager.cpp:319`  
Comment says "pass-through silence" but code does pass-through (dry). Align comment with code after applying Fix #5.

### 22. `assert` vs `jassert` Inconsistency  
**File:** `Core/DiscreteParameterHandler.cpp:65`, `Core/ModulationEngine.cpp:306-322`, `Core/OversamplingWrapper.h:125-127`  
Some files use `assert` (C standard library) instead of `jassert` (JUCE). In release builds with `NDEBUG`, `assert` disappears entirely. `jassert` is active in debug builds and can be configured to not vanish. Standardize on `jassert` for JUCE projects.

### 23. `LockFreeQueue` Lacks `static_assert` on `T` Triviality  
**File:** `Core/LockFreeQueue.h`  
The queue stores `T` in a `std::array` and copies via assignment. For non-trivial types, this could be non-atomic from the reader's perspective. Add: `static_assert(std::is_trivially_copyable_v<T>, "LockFreeQueue requires trivially copyable types");`.

### 24. `PluginHostManager::discoverPlugin` Verbose `std::cerr` Output  
**File:** `PluginHostManager.cpp:527-678`  
`discoverPlugin` writes diagnostic messages to `std::cerr` when `verbose=true`. In a plugin context, `std::cerr` may not be visible to the user and can pollute host logs. Consider routing through `juce::Logger` or `DBG`.

---

## Correctness Verdicts

### `LockFreeQueue` — **CORRECT with minor caveats**
- SPSC consumer (audio thread) is lock-free and correctly uses `memory_order_release`/`acquire`.
- MPMC producers are serialized via `juce::SpinLock`, which is acceptable for UI/MCP threads.
- Power-of-2 capacity is enforced via `static_assert`.
- Cache-line alignment (`alignas(64)`) on indices is present.
- **Caveat:** `std::distance` in `pushRange` may be O(N) for some iterators; add a `static_assert` for `std::random_access_iterator_tag` or document the requirement.

### `AudioBufferPool` — **CORRECT for its intended use, but not RT-safe**
- `std::mutex` is correct for message-thread / background-thread use.
- `std::unique_ptr` ownership is clear.
- **Caveat:** Must not be used on the audio thread. Add explicit documentation.

### `PluginScanner` — **CORRECT but thread-unsafe with UI**
- Simple `juce::Thread` wrapper. No raw `new`/`delete`.
- **Caveat:** `scanPluginFolders` modifies `knownPlugins` without locking (see Refinement #17).

### VST3 Interface Lifecycle — **MOSTLY CORRECT**
- `prepareToPlay` → `hostManager.prepare` → `hostedPlugin->prepareToPlay` is correct.
- `releaseResources` is called in `PluginHostManager::unloadPlugin` and `~MorePhiProcessor`.
- `getStateInformation` / `setStateInformation` implement the full state persistence with hosted plugin opaque chunk + snapshot bank + APVTS.
- **Caveat:** Timer-based deferred loading (`MAX_PLUGIN_LOAD_RETRIES = 10`, 50 ms interval) is well-implemented and handles FL Studio / offline rendering edge cases correctly.

### Timer-Based Deferred Loading — **CORRECT**
- `setStateInformation` uses `callFunctionOnMessageThread` (not `callAsync`) to guarantee timer startup.
- `timerCallback` retries up to `MAX_PLUGIN_LOAD_RETRIES` (10) and clears `isRestoring_` on final failure.
- `isRestoring_` correctly blocks morph processing during async load.
- **Caveat:** `pendingPluginDesc_` needs locking (see Issue #9).

---

## Appendix: File-by-File Quick Reference

| File | Critical | High | Medium | Low |
|------|----------|------|--------|-----|
| `PluginHostManager.h` | — | 6, 10 | 15, 19 | — |
| `PluginHostManager.cpp` | 2, 3, 5 | 7, 12 | 13, 17 | 21, 24 |
| `ParameterBridge.h` | — | 11 | — | — |
| `ParameterBridge.cpp` | 4 | 8, 11 | 14 | — |
| `PluginProcessor.h` | — | 9 | — | — |
| `PluginProcessor.cpp` | 1 | 9 | — | 20 |
| `LockFreeQueue.h` | — | — | 16 | 23 |
| `AudioBufferPool.{h,cpp}` | — | — | 18 | — |
| `PluginScanner.{h,cpp}` | — | — | 17 | — |
| `DiscreteParameterHandler.cpp` | — | — | — | 22 |
| `ModulationEngine.cpp` | — | — | — | 22 |
| `OversamplingWrapper.h` | — | — | — | 22 |

---

*End of Report*
