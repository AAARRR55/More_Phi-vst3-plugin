# Sub-Agent 1 Report: Plugin Hosting & Memory Safety

**Audit Date:** 2026-06-17  
**Auditor:** Sub-Agent 1 (Plugin Hosting & Memory Safety)  
**Scope:** `src/Host/*`, `src/Plugin/*` (13 files reviewed)  
**Focus:** VST3 compliance, memory safety, resource management, lifecycle, thread safety, parameter handling, legacy code.

---

## Critical Issues (CRASH / MEMORY LEAK / DATA LOSS / HANG)

### Issue 1: `audioDomainUsers_` Leak on Exception — Permanent Message-Thread Hang
- **Location:** `PluginProcessor.cpp` ~1420–1532 (`processBlock` audio-domain path)
- **Severity:** Critical
- **Description:** In the audio-domain morph branch, `audioDomainUsers_.fetch_add(1)` is executed, but `fetch_sub(1)` sits at the end of the block. If any code inside throws (e.g., `hostManagerB_.processBlock`, `oversampling_.upsample`, `spectralEngine_.processBlock`), the decrement is skipped. `reconfigureAudioDomainProcessing()` spins forever on `while (audioDomainUsers_ > 0)`, permanently hanging the message thread. Audio domain can never be reconfigured again.
- **Root Cause:** Lack of RAII guard around the user counter; `fetch_sub` is not in a destructor or `finally` block.
- **Recommended Fix:**
  ```cpp
  struct AudioDomainUserGuard {
      std::atomic<int>& users;
      explicit AudioDomainUserGuard(std::atomic<int>& u) : users(u) { users.fetch_add(1, std::memory_order_acq_rel); }
      ~AudioDomainUserGuard() { users.fetch_sub(1, std::memory_order_acq_rel); }
  };
  // Replace manual fetch_add/fetch_sub with this guard.
  ```

### Issue 2: `HostedPluginWindow` Dangling Pointer After Plugin Unload
- **Location:** `PluginEditor.cpp` ~380–393 (`openPluginWindow`)
- **Severity:** Critical
- **Description:** `openPluginWindow` captures `processor.getHostManager().getPlugin()` (a raw `juce::AudioPluginInstance*`) into `HostedPluginWindow`. If the user later loads a different plugin or the hosted plugin is auto-suspended/unloaded, `HostedPluginWindow` retains a dangling pointer. Any UI interaction (redraw, parameter change, mouse click) inside the hosted window will crash the DAW.
- **Root Cause:** `HostedPluginWindow` is given a raw snapshot of the plugin pointer with no invalidation mechanism when the hosted plugin is destroyed.
- **Recommended Fix:** Store the plugin pointer inside a weak-reference wrapper that calls `hostManager.acquirePluginForUse()` on every access, or close the window automatically inside `PluginHostManager::unloadPlugin()` via a callback. At minimum, add `closePluginWindow()` to `PluginHostManager::unloadPlugin()` or subscribe to a plugin-unloaded broadcaster.

### Issue 3: `getAIAssistant()` Unconditional Null Dereference
- **Location:** `PluginProcessor.h` ~118
- **Severity:** Critical
- **Description:** `getAIAssistant()` returns `*aiAssistant_` unconditionally. `aiAssistant_` is only instantiated inside `prepareToPlay()` (lazy init). If the editor or an MCP tool calls this accessor before the host has called `prepareToPlay()`, the result is an immediate null-pointer dereference and DAW crash.
- **Root Cause:** Missing null check on a lazily-initialized `std::unique_ptr`.
- **Recommended Fix:**
  ```cpp
  AIAssistant* getAIAssistant() noexcept { return aiAssistant_.get(); }
  // Or, if reference semantics are required, initialize aiAssistant_ in the constructor, not prepareToPlay.
  ```

### Issue 4: `wideBuffer_` Sub-Buffer Can View Past Allocation on Host Block-Size Violation
- **Location:** `PluginHostManager.cpp` ~371–373 (`processBlock` wide-buffer path)
- **Severity:** Critical
- **Description:** `wideBuffer_` is pre-sized to `currentBlockSize` in `prepare()`. The temporary `subBuffer` is constructed with `buffer.getNumSamples()`. If a misbehaving host calls `processBlock` with more samples than the most recent `prepareToPlay` (or calls `processBlock` before `prepareToPlay`), `subBuffer` will reference memory beyond the `wideBuffer_` allocation — a linear buffer overflow that corrupts the heap or crashes.
- **Root Cause:** `subBuffer` trusts `buffer.getNumSamples()` without clamping to `wideBuffer_.getNumSamples()`.
- **Recommended Fix:**
  ```cpp
  const int safeSamples = juce::jmin(buffer.getNumSamples(), wideBuffer_.getNumSamples());
  juce::AudioBuffer<float> subBuffer(wideBuffer_.getArrayOfWritePointers(),
                                     requiredChannels, safeSamples);
  ```
  Also add a `jassert(buffer.getNumSamples() <= currentBlockSize)` in debug builds.

### Issue 5: `exceptionCount_` Signed Integer Overflow → Undefined Behavior
- **Location:** `PluginHostManager.cpp` ~128, 323–345, 389–418 (`processBlock` suspended & catch paths)
- **Severity:** Critical
- **Description:** `exceptionCount_` is `std::atomic<int>`. While a plugin is suspended, `exceptionCount_` is incremented every single audio block. In a long-running session or offline export, this can exceed `INT_MAX`. Signed integer overflow is undefined behavior in C++ and can be exploited by optimizers or lead to miscompiled code.
- **Root Cause:** Monotonically incrementing a signed counter without saturation or wrap-safe type.
- **Recommended Fix:** Change to `std::atomic<uint32_t>` and cap at `MAX_PLUGIN_EXCEPTIONS + 1`, or use `fetch_add(1)` with `std::atomic<unsigned int>`.

### Issue 6: `midiCopyB_.addEvents` Can Allocate on the Audio Thread
- **Location:** `PluginProcessor.cpp` ~1435 (`processBlock` audio-domain path)
- **Severity:** Critical
- **Description:** `midiCopyB_.addEvents(filteredMidiBuffer_, 0, -1, 0)` is called inside `processBlock`. `juce::MidiBuffer` stores data in a `HeapBlock<uint8_t>` that grows on demand. If a dense MIDI passage exceeds the current capacity, `addEvents` triggers a heap allocation on the audio thread, causing glitches or crashes in real-time hosts.
- **Root Cause:** `midiCopyB_` is never pre-reserved to a worst-case size; it is only default-constructed.
- **Recommended Fix:** Pre-size `midiCopyB_` in `prepareToPlay` to the worst-case MIDI data size expected (e.g., 128 events × 3 bytes + overhead). JUCE `MidiBuffer` does not expose `reserve`, so either maintain a custom pre-allocated `uint8_t` scratch block or use `MidiBuffer::ensureSize` if available, or re-use the same buffer and ensure it never shrinks.

### Issue 7: `ParameterBridge::withPlugin` Non-PluginHostManager Path Skips Reference Counting
- **Location:** `ParameterBridge.cpp` ~54–89 (`withPlugin` template)
- **Severity:** Critical
- **Description:** If `cachedConcreteHost_` is null (e.g., a mock `IPluginHostManager` or any future subclass), `withPlugin` falls back to `host.getPlugin()`, which returns the raw atomic pointer without incrementing `activePluginUsers_`. A concurrent `unloadPlugin()` can then destroy the plugin while the bridge is still using it, producing a use-after-free.
- **Root Cause:** `IPluginHostManager::getPlugin()` provides raw pointer access with no lifecycle contract.
- **Recommended Fix:** Add `acquirePluginForUse()` / `releasePluginFromUse()` to the `IPluginHostManager` interface so all implementations must provide reference-counted access. Remove the raw `getPlugin()` fallback from production audio-path code.

### Issue 8: `getLastDescription()` Pointer Used After Lock Release
- **Location:** `PluginHostManager.cpp` ~510–516 (`getLastDescription`) and `PluginProcessor.cpp` ~1573 (`getStateInformation`)
- **Severity:** Critical
- **Description:** `getLastDescription()` acquires `descLock_`, returns `&lastDescription`, and then releases the lock. The caller (`getStateInformation`) then invokes `desc->createXml()`, which reads `lastDescription` fields without any synchronization. If another thread is inside `loadPlugin()` writing `lastDescription` at the same time, `createXml()` reads torn or corrupted data, potentially crashing during XML serialization or producing corrupted state files.
- **Root Cause:** Returning a pointer to a protected member while the lock is held creates a TOCTOU race.
- **Recommended Fix:** Return a `juce::PluginDescription` **by value** (copy under lock) instead of a pointer. Or add a `getLastDescriptionCopy()` method and deprecate the pointer-returning variant.

### Issue 9: `unloadPlugin` Can Race with `loadPlugin` Window
- **Location:** `PluginHostManager.cpp` ~148–235 (`loadPlugin`) and ~237–273 (`unloadPlugin`)
- **Severity:** Critical
- **Description:** `loadPlugin` is protected by `isSwapping_`, but `unloadPlugin` does **not** check `isSwapping_`. A concurrent `unloadPlugin` call (e.g., from the destructor, or from a UI thread action) can execute between `hostedPlugin = std::move(newPlugin)` and `hostedPluginPtr_.store(hostedPlugin.get(), std::memory_order_release)` inside `loadPlugin`. After `unloadPlugin` resets the `unique_ptr`, the subsequent `hostedPluginPtr_.store(...)` publishes a dangling pointer. The next audio block will use a deleted object.
- **Root Cause:** `unloadPlugin` is not serialized with `loadPlugin` by `isSwapping_`.
- **Recommended Fix:** Guard `unloadPlugin` with the same `isSwapping_` CAS, or use a single mutex/seqlock to serialize both load and unload. Also set `hostedPluginPtr_` to `nullptr` before resetting `hostedPlugin`.

---

## High-Priority Issues (THREAD-SAFETY / ARCHITECTURAL / REAL-TIME)

### Issue 10: Unbounded Spin in `unloadPlugin` on `activePluginUsers_`
- **Location:** `PluginHostManager.cpp` ~258 (`unloadPlugin`)
- **Severity:** High
- **Description:** `unloadPlugin` spins forever with `while (activePluginUsers_.load(...) > 0) juce::Thread::yield();`. While the comment correctly notes this is intentional to prevent use-after-free, there is no upper bound or host-abort path. If the audio thread is terminated by the DAW (e.g., during export or track freeze), or if a prior exception corrupted the counter, the message thread hangs indefinitely. The DAW track removal or project close will deadlock.
- **Root Cause:** Trade-off chosen to avoid use-after-free, but no escape hatch.
- **Recommended Fix:** After a long timeout (e.g., 5 seconds), force the flag and log a fatal error. Consider using `juce::Thread::sleep(1)` instead of `yield()` to reduce CPU burn. Document the risk in the header.

### Issue 11: `std::pow` Called on Audio Thread Every Block
- **Location:** `PluginProcessor.cpp` ~1081 (`syncStateFromAPVTS`)
- **Severity:** High
- **Description:** `sidechainThresholdLinear_.store(std::pow(10.0f, scThresholdDb / 20.0f), ...)` executes inside `processBlock` (audio thread). While `powf` is usually fast, it is not guaranteed real-time safe on all CRT implementations (some use lookup tables, branchy paths, or even allocations). Called every single block, it adds unnecessary non-deterministic latency to the audio callback.
- **Root Cause:** Conversion from dB to linear is performed on the audio thread instead of the message thread when the parameter changes.
- **Recommended Fix:** Compute `sidechainThresholdLinear_` only when `sidechainThreshold_` changes (e.g., in a `parameterChanged` callback or in `syncStateFromAPVTS` guarded by a dirty flag), or replace with a small `std::array` lookup table for the dB range.

### Issue 12: `PluginHostManager::prepare` Swallows `prepareToPlay` Exception and Never Retries
- **Location:** `PluginHostManager.cpp` ~113–131 (`prepare`)
- **Severity:** High
- **Description:** If `hostedPlugin->prepareToPlay` throws, the catch block silently swallows it. `hasPreparedConfiguration_` is still set to `true`. The next time `prepare()` is called with the same sample rate / block size, `configurationChanged` is `false`, so `prepareToPlay` is **never retried**. The hosted plugin remains in a broken, unprepared state for the rest of the session.
- **Root Cause:** `hasPreparedConfiguration_` is set before the `prepareToPlay` call, and exceptions do not reset it.
- **Recommended Fix:** Set `hasPreparedConfiguration_ = true` **only** after `prepareToPlay` succeeds. If it throws, leave it `false` so the next `prepare()` will retry.

### Issue 13: v3.3.0 Discrete Parameter Hysteresis Not Wired Into Audio Path
- **Location:** `PluginProcessor.cpp` ~1318–1399 (`processBlock` parameter application) and `PluginProcessor_v330.cpp` ~76–89 (commented integration)
- **Severity:** High
- **Description:** `PluginProcessor_v330.cpp` contains the intended integration for `discreteHandler_.processDiscreteParameters(...)`, but the actual `processBlock` in `PluginProcessor.cpp` never calls it. The `DiscreteParameterHandler` class is constructed and can be configured via `refreshParameterClassification()`, but its audio-thread processing is a no-op. The v3.3.0 feature for discrete-parameter hysteresis is effectively dead code.
- **Root Cause:** Migration comment was left in place but never implemented in the main `processBlock`.
- **Recommended Fix:** Integrate the discrete handler into the morph pipeline immediately after `morphProcessor.process()` and before `paramBridge.applyParameterState()`, as described in the v330 comment. Ensure `finalOutput_` is sized correctly.

### Issue 14: `applyPendingFullStateRecall` Marks Generation as Applied on Failure
- **Location:** `PluginProcessor.cpp` ~2107–2113 (`applyPendingFullStateRecall`)
- **Severity:** High
- **Description:** In both the `try` and `catch` branches, `appliedFullStateRecallGeneration_ = generation;` is executed. If `plugin->setStateInformation(chunk)` throws, the state recall is lost, but the generation counter is advanced as if it succeeded. The timer will never retry the failed recall, and the user will silently get the wrong plugin state.
- **Root Cause:** The generation counter is updated unconditionally.
- **Recommended Fix:** Only update `appliedFullStateRecallGeneration_` on success. On failure, leave the generation unchanged so the timer will retry on the next tick (with a retry limit to avoid infinite loops).

### Issue 15: `plugin->setPlayHead` Called Every Block from Audio Thread
- **Location:** `PluginHostManager.cpp` ~316 (`processBlock`)
- **Severity:** High
- **Description:** `plugin->setPlayHead(playHead_.load(...))` is executed at the top of every `processBlock`. While JUCE's `AudioPluginInstance` wrapper typically handles this, not all VST3 plugins expect `setPlayHead` from the real-time thread on every block. Some plugins acquire internal locks or post messages, causing priority inversion or missed deadlines. Additionally, it adds per-block virtual-call overhead.
- **Root Cause:** Playhead is forwarded unconditionally every block, even when it has not changed.
- **Recommended Fix:** Cache the last playhead pointer actually sent to the plugin and skip the call if it hasn't changed. Alternatively, send the playhead only when the transport state changes (e.g., play/stop/seek).

### Issue 16: `SnappySnapProcessor` Is Dead Legacy Code with Real-Time Unsafe Mutex
- **Location:** `Plugin/SnappySnapProcessor.h`
- **Severity:** High
- **Description:** This file defines an entire alternative `AudioProcessor` (`snap::SnappySnapProcessor`) that duplicates the architecture of `MorePhiProcessor`. It includes a `std::mutex commandQueueProducerMutex` in the audio path, is never instantiated in `createPluginFilter()`, and is not compiled into the main plugin as far as the entry point shows. Keeping it in the source tree risks accidental compilation, increases binary bloat, and misleads future maintainers.
- **Root Cause:** Incomplete migration from the SnappySnap codebase to More-Phi.
- **Recommended Fix:** Remove the file entirely. If historical reference is needed, archive it outside the active `src/` tree.

### Issue 17: `getLastDescriptionRef()` Returns Reference Without Lock
- **Location:** `PluginHostManager.h` ~76
- **Severity:** High
- **Description:** This public method returns a `const juce::PluginDescription&` directly, bypassing `descLock_`. Any caller reading `name`, `fileOrIdentifier`, etc. concurrently with `loadPlugin()` (which writes under `descLock_`) will experience a data race.
- **Root Cause:** Convenience accessor added without synchronization contract.
- **Recommended Fix:** Remove the method or document that the caller **must** hold `descLock_`. Prefer returning a copy or using the existing pointer-based `getLastDescription()`.

---

## Medium-Priority Issues (EDGE CASE / ROBUSTNESS)

### Issue 18: `PluginScanner` Modifies `knownPlugins` from Background Thread Without Lock
- **Location:** `PluginScanner.h` ~24, `PluginHostManager.cpp` ~492–508 (`scanPluginFolders`)
- **Severity:** Medium
- **Description:** `PluginScanner` runs `scanPluginFolders()` on a `juce::Thread`. `knownPlugins` is a `juce::KnownPluginList` (not thread-safe). If the UI reads `getKnownPlugins()` while scanning is in progress, the list may be in an inconsistent state.
- **Root Cause:** `KnownPluginList` is not protected by a lock during background scanning.
- **Recommended Fix:** Serialize access to `knownPlugins` with the same `descLock_` or a dedicated `scanLock_`, or perform scanning on the message thread.

### Issue 19: `setStateInformation` `callFunctionOnMessageThread` with Raw `this` Pointer
- **Location:** `PluginProcessor.cpp` ~1813–1817 (`setStateInformation`)
- **Severity:** Medium
- **Description:** `callFunctionOnMessageThread` is given a lambda that captures `this`. If the processor is destroyed on another thread while the message thread is blocked inside the call, the callback can access a destroyed object. While the call is synchronous, the timer started by the callback may fire after destruction.
- **Root Cause:** `startTimer(50)` is issued from the callback, creating a delayed callback on `this`.
- **Recommended Fix:** Use `JUCE_DECLARE_WEAK_REFERENCEABLE` and pass a `WeakReference` into the timer callback, or ensure the timer is stopped before destruction (already done in destructor, but the window exists).

### Issue 20: `PluginHostManager::discoverPlugin` Skips Stage 1 if Message Lock Fails
- **Location:** `PluginHostManager.cpp` ~532–591 (`discoverPlugin`)
- **Severity:** Medium
- **Description:** `withMessageLock` returns `false` if the `MessageManagerLock` cannot be gained. Stage 1 then fails, forcing a slower directory scan. For well-behaved plugins that don't need the message lock, this is unnecessary work.
- **Root Cause:** Conservative fallback treats lock failure as stage failure.
- **Recommended Fix:** If `lockWasGained()` is false, still attempt `findAllTypesForFile` without the lock, and only fall back to directory scan on actual plugin failure.

### Issue 21: `PluginHostManager::loadPlugin` Continues Without Message Lock
- **Location:** `PluginHostManager.cpp` ~167–178 (`loadPlugin`)
- **Severity:** Medium
- **Description:** If `MessageManagerLock` cannot be gained, `loadPlugin` resets the lock and continues anyway. The comment says "many plugins don't need it," but some VST3/AU plugins (especially on Windows) will crash or deadlock during instantiation without the message pump.
- **Root Cause:** Graceful continuation after a known failure mode.
- **Recommended Fix:** Return `false` if the lock is required but not gained. Provide a `forceNoLock` flag for unit tests only.

### Issue 22: `ParameterBridge::captureParameterState` Allocates `std::vector` on Any Thread
- **Location:** `ParameterBridge.cpp` ~202–213 (`captureParameterState`)
- **Severity:** Medium
- **Description:** The method returns `std::vector<float>` by value, which allocates on the heap. It is not called from the audio thread in the current code, but it is a `const` method and could be misused. A future developer might call it from `processBlock`.
- **Root Cause:** No compile-time or runtime guard against audio-thread allocation.
- **Recommended Fix:** Add a `jassert(!juce::Thread::getCurrentThread()->isRealtimeThread())` or change the signature to write into a pre-allocated `std::array<float, MAX_PARAMETERS>` passed by reference.

### Issue 23: `PluginHostManager::processBlock` Exception Recovery Path Skips `currentGain_` Ramp
- **Location:** `PluginHostManager.cpp` ~375–430 (`processBlock` wide-buffer path)
- **Severity:** Medium
- **Description:** If `plugin->processBlock` throws in the wide-buffer branch, `buffer.clear()` is called, but the fade-in `currentGain_` ramp is skipped. `currentGain_` remains at `0.0f`. On the next successful block, the audio will snap from silence to full volume instead of ramping, causing a click.
- **Root Cause:** `currentGain_` update is after the `try/catch` block.
- **Recommended Fix:** In the catch block, set `currentGain_ = 0.0f` explicitly so the next successful block ramps correctly.

### Issue 24: `PluginHostManager::processBlock` `wideBuffer_` Not Fully Cleared Before Use
- **Location:** `PluginHostManager.cpp` ~362–373 (`processBlock`)
- **Severity:** Medium
- **Description:** The code clears channels `0..requiredChannels-1`, then copies `buffer` channels `0..numChannels-1`. For channels between `buffer.getNumChannels()` and `requiredChannels`, the data is zero. However, if `requiredChannels` > `buffer.getNumChannels()` and the plugin writes to the extra channels, those extra channels are then ignored during copy-back. This is okay, but if the plugin reads from the extra channels (e.g., for sidechain), the zeroed input is correct.

### Issue 25: `PluginHostManager::processBlock` Suspended Path Comment Mismatch
- **Location:** `PluginHostManager.cpp` ~346 (`processBlock`)
- **Severity:** Medium
- **Description:** The comment says "Output silence (buffer unchanged = pass-through)", but the buffer is **not** cleared when `suspended_` is true (unless `currentGain_` is used). The audio passes through dry. This is a functional mismatch, not a safety issue, but could confuse maintainers.
- **Root Cause:** Outdated comment from a previous iteration where the buffer was cleared.
- **Recommended Fix:** Update the comment to "Pass through dry signal while suspended" or explicitly clear the buffer if silence is desired.

### Issue 26: `PluginHostManager::exceptionLog_` and `lastExceptionCode_` Are Dead Code
- **Location:** `PluginHostManager.h` ~145–148
- **Severity:** Medium
- **Description:** These members are declared but never read or written in the reviewed code. They waste memory and mislead readers into thinking there is an exception-logging system.
- **Root Cause:** Leftover from a planned feature.
- **Recommended Fix:** Remove or implement the logging.

### Issue 27: `MorePhiProcessor::getTailLengthSeconds` Const-Cast Workaround
- **Location:** `PluginProcessor.cpp` ~2231–2248
- **Severity:** Medium
- **Description:** The method casts away `const` to call `acquirePluginForUse()`. This breaks const-correctness and indicates the interface lacks a `const` accessor for safe plugin queries.
- **Root Cause:** `acquirePluginForUse` is non-const even though it only reads the atomic counter.
- **Recommended Fix:** Add `acquirePluginForUse() const` overload or make the existing method `const` (it only modifies atomics, which are mutable).

---

## Low-Priority Issues (STYLE / DOCUMENTATION / CLEANUP)

### Issue 28: `PluginScanner.cpp` Is Empty
- **Location:** `PluginScanner.cpp`
- **Severity:** Low
- **Description:** The entire implementation is inline in the header. The `.cpp` file is unnecessary and adds compilation overhead.
- **Recommended Fix:** Move implementation to `.cpp` or delete the `.cpp` file and remove it from `CMakeLists.txt`.

### Issue 29: `PluginProcessor_v330.cpp` Contains Stale Commented Code
- **Location:** `PluginProcessor_v330.cpp` ~76–104
- **Severity:** Low
- **Description:** Large blocks of commented pseudocode for `processBlock` integration and token optimizer setup are left in the file. They are not compiled and serve only as documentation that is already stale.
- **Recommended Fix:** Remove the comments or convert them into a `TODO` ticket. Keep the source code clean.

### Issue 30: `PluginHostManager::scanPluginFolders` Unused `pluginName` Variable
- **Location:** `PluginHostManager.cpp` ~502–506
- **Severity:** Low
- **Description:** `pluginName` is passed to `scanNextFile` but never read.
- **Recommended Fix:** Replace with `juce::String pluginName; juce::ignoreUnused(pluginName);` or use it for logging.

### Issue 31: `MorePhiProcessor::createEditor` Uses Raw `new`
- **Location:** `PluginProcessor.cpp` ~2218–2228
- **Severity:** Low
- **Description:** While standard for JUCE `createEditor()`, returning `new MorePhiEditor(*this)` is the old style. Modern JUCE code often uses `std::make_unique` and returns `.release()` to make ownership intent explicit.
- **Recommended Fix:** `return std::make_unique<MorePhiEditor>(*this).release();` (JUCE 8 style).

### Issue 32: Missing `noexcept` on Hot Audio-Path Methods
- **Location:** `ParameterBridge.h`, `PluginHostManager.h`
- **Severity:** Low
- **Description:** Methods like `getParameterCount()`, `getParameterNormalized()`, `setParameterNormalized()` are called from the audio thread and should be marked `noexcept` to prevent exception unwinding in real-time contexts.
- **Recommended Fix:** Mark all audio-path methods `noexcept`. The `withPlugin` helper already catches exceptions, so the public API can safely promise no-throw.

---

## Positive Findings (What Is Done Well)

1. **SPSC LockFreeQueue for Parameter Commands** — `LockFreeQueue<ParamCommand, 8192>` correctly isolates UI/MCP producers from the audio-thread consumer. No locks or allocations on the audio path.
2. **Reference-Counted Plugin Access** — `acquirePluginForUse()` / `releasePluginFromUse()` with `std::atomic<uint32_t>` ensures the audio thread never accesses a destroyed hosted plugin during swap.
3. **Exclusive Plugin Use with Timeout** — `beginExclusivePluginUse(200)` safely blocks the audio thread for state capture/restore, with a bounded timeout to prevent DAW deadlock.
4. **`isSwapping_` Atomic Guard** — Prevents concurrent `loadPlugin()` calls from racing, which is a common pitfall in plugin hosting.
5. **`isRestoring_` Barrier in `processBlock`** — Blocks morph processing while the hosted plugin is being asynchronously reloaded, preventing torn state and parameter count mismatches.
6. **Wide Buffer Pre-Allocation** — `wideBuffer_` is pre-sized in `prepare()` to avoid any audio-thread heap allocation when expanding channels for multi-bus plugins (e.g., FabFilter Pro-Q 4 sidechain).
7. **Exception Safety in `processBlock`** — Hosted plugin exceptions are caught, the output buffer is cleared, and the plugin is suspended (not unloaded) to allow automatic recovery. This prevents a single bad plugin from crashing the entire DAW.
8. **Grace Period & Suspension Instead of Unload** — The `recoveryGracePeriod_` and `suspended_` mechanism prevents flapping (immediate re-suspend) and allows recovery without destroying the plugin instance.
9. **WeakReference in `setMorphPositionExternal`** — The `callAsync` callback uses `juce::WeakReference<MorePhiProcessor>` to avoid accessing the processor after destruction.
10. **SpinLock with `tryEnter` for Touch Detection** — The audio thread uses `touchStateLock_.tryEnter()` and skips touch detection if contested, a correct real-time-safe fallback.
11. **Raw APVTS Pointer Caching** — `cacheRawParameterPointers()` stores `std::atomic<float>*` for every parameter, avoiding expensive `apvts.getParameter()` lookups inside `processBlock`.
12. **Pre-Sized Vectors in `prepareToPlay`** — `finalOutput_`, `lastApplied_`, `touchCooldown_`, `liveEditHold_`, etc. are all sized once, eliminating per-block allocations.
13. **Pending Hosted State Preservation** — `pendingHostedState_` is a `juce::MemoryBlock` protected by `pendingStateMutex_`, ensuring the hosted plugin's opaque data survives across unload/load cycles and is available for state serialization even when the plugin is not loaded.
14. **Timer-Based Deferred Loading** — `setStateInformation` uses `startTimer(50)` instead of `callAsync` for deferred loading, avoiding silent callback drops in FL Studio and other hosts that discard async calls when the editor is closed.
15. **RAII `ScopedPluginUse`** — Inside `processBlock`, a local struct ensures `releasePluginFromUse()` is called even if the hosted plugin throws an exception, preventing counter leaks.
16. **JUCE `JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR`** — All major classes use this macro, preventing accidental copies and enabling leak detection in debug builds.
17. **VST3 Program/Preset Interface** — `getNumPrograms`, `setCurrentProgram`, `getProgramName` map to snapshot bank slots, providing DAW preset browser integration.
18. **Transport Context Snapshot Atomics** — All playhead-derived fields are stored in `std::atomic` variables with correct acquire/release semantics, allowing safe UI reads without locking.
19. **Sidechain Threshold Pre-Computation** — `sidechainThresholdLinear_` is updated once per block in `syncStateFromAPVTS`, avoiding repeated `std::pow` calls per sample.
20. **Comprehensive Exception Handling in `loadPlugin` and `unloadPlugin`** — Both methods wrap hosted-plugin calls in `try/catch` blocks, ensuring that a misbehaving plugin does not propagate exceptions into JUCE's plugin wrapper or the DAW.

---

*End of Report — Sub-Agent 1*
