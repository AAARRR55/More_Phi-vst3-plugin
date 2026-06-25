# More-Phi VST3 Multi-Agent Orchestration — Technical Audit Report

**Date:** 2026-06-24  
**Subject:** `G:\More_Phi-vst3-plugin` — More-Phi v3.3.0 Synthesizer Edition  
**Scope:** VST3 API compliance, multi-agent architecture, audio pipeline, state management, error handling, performance, thread safety, host compatibility.

---

## Executive Summary

The More-Phi plugin is a JUCE 8-based VST3 parameter-morphing engine with an embedded MCP JSON-RPC server, a 7-agent orchestration runtime (Conductor + 6 specialists), neural mastering (ONNX + HTTP fallback), and audio-domain morphing (spectral, granular, formant). The codebase has clearly undergone several audit passes (evidenced by inline `AUDIT-FIX` comments and numbered fix markers). However, several architectural risks remain — particularly around `std::mutex` usage in the agent runtime on threads that can indirectly block the audio path, spinlock contention under load, and state-version forward-migration gaps. The findings below are organized by severity tier.

---

## CRITICAL ISSUES

### C-1: `std::mutex` in `PriorityScheduler` can cause priority inversion on agent worker threads

**Component:** `src/AI/Agents/Scheduler/PriorityScheduler.h:81`, `src/AI/Agents/AgentRuntime.cpp:257,296,332,352,373`

**Problem:** `PriorityScheduler` uses `std::mutex` + `std::condition_variable` for its work queue, and `AgentRuntime` uses `std::mutex` for `resultsMutex_` and `runsMutex_`. Agent workers call `MCPToolHandler::handle()` which calls back into `MorePhiProcessor` methods that acquire `commandConsumerLock_` (a `juce::SpinLock`) and `hostManager.beginExclusivePluginUse()`. If a worker holds `resultsMutex_` and then tries to acquire `commandConsumerLock_`, it blocks the MCP flush path. More critically, `DefaultToolInvoker` routes tool calls through `MCPToolHandler::handle()`, which can trigger `enqueueParameterSet()` → `commandQueue.push()` (acquires the queue's push spinlock). None of this runs on the audio thread directly, but `flushPendingParameterCommandsForAssistant()` on the MCP thread and the audio-thread drain both contend on `commandConsumerLock_` and `touchStateLock_`. The `std::mutex` in the scheduler is NOT priority-inversion safe — on Windows, a low-priority thread holding the scheduler mutex can be preempted while a higher-priority thread (e.g. MCP request handler) waits, causing unbounded latency for AI-driven parameter edits.

**Severity:** CRITICAL — can cause multi-second stalls in AI parameter editing under CPU load.

**Recommended Fix:**
1. Replace `std::mutex` in `PriorityScheduler` with `juce::SpinLock` (already used elsewhere in the codebase for the same reason). The critical sections are short (queue push/pop), making spinlocks appropriate.
2. For `resultsMutex_` and `runsMutex_` in `AgentRuntime`, replace with `juce::SpinLock` since the data structures they protect are small (bounded map + deque, bounded run states).
3. Alternatively, set scheduler worker thread priority to "high" via `juce::Thread::setPriority()` so they are never preempted by normal-priority threads.

---

### C-2: `getStateInformation` can be called from the audio thread in some DAWs — heap allocation path

**Component:** `src/Plugin/PluginProcessor.cpp:2312-2501`

**Problem:** The code correctly detects when `getStateInformation` is called from the audio thread (`isAudioThread` check) and avoids `beginExclusivePluginUse()` in that case. However, the remainder of the function still does extensive heap allocation: `apvts.copyState()`, `state.createXml()`, `XmlElement` manipulation, `toBase64Encoding()`, `copyXmlToBinary()`. In Pro Tools and certain FL Studio export paths, `getStateInformation` can be called from a thread that shares priority with the audio thread. Any heap allocation during this call can cause audio dropouts under memory pressure.

**Severity:** CRITICAL — audio dropouts during project save in Pro Tools and FL Studio export.

**Recommended Fix:**
1. Pre-allocate a cached state snapshot on the message thread (e.g., on dirty-flag or timer tick) and return the cached binary data when `getStateInformation` is called from a non-message thread.
2. Alternatively, use JUCE's `AudioProcessor::getCurrentProgramStateInformation()` pattern — maintain a `MemoryBlock` that is atomically swapped with a new version whenever state changes.

---

### C-3: `enqueueParameterBatch` early-returns `false` on first invalid index — silently drops valid commands

**Component:** `src/Plugin/PluginProcessor.cpp:327-344`

**Problem:** `enqueueParameterBatch` iterates over all commands to validate them and returns `false` immediately if ANY command has an invalid index. Unlike `enqueueParameterSet` which only rejects the single invalid command, the batch variant rejects the ENTIRE batch. An MCP tool that sends a batch including even one out-of-range parameter (e.g., after a hosted plugin change that reduced parameter count) will have ALL its edits dropped silently.

**Severity:** CRITICAL — AI/MCP edits silently lost, no error feedback.

**Recommended Fix:**
1. Skip/filter invalid indices instead of failing the whole batch:
```cpp
for (auto command : commands) {
    if (command.paramIndex < 0 || command.paramIndex >= MAX_PARAMETERS)
        continue;  // skip invalid, don't fail the batch
    command.value = juce::jlimit(0.0f, 1.0f, command.value);
    sanitized.push_back(command);
}
if (sanitized.empty()) return true;
return commandQueue.pushRange(sanitized);
```
2. Add a return value or counter that indicates how many commands were actually enqueued so the caller can detect filtering.

---

### C-4: `LinkBroadcaster` shared-memory morph sync has no authentication or integrity check

**Component:** `src/Plugin/PluginProcessor.h:346-348`, `src/AI/LinkBroadcaster.h/cpp`

**Problem:** Link Mode uses shared memory for cross-instance morph synchronization. Any process on the machine can write to the shared memory segment. A malicious or buggy process can write arbitrary morph coordinates, causing the plugin to rapidly jump morph position and produce extreme audio output (potentially speaker-damaging if gain staging is high).

**Severity:** CRITICAL — security risk (unauthenticated cross-process write → uncontrolled audio output).

**Recommended Fix:**
1. Add a simple integrity check (e.g., CRC32 or a process-ID watermark) to the shared memory segment so the plugin can detect and ignore corrupted or foreign writes.
2. Add bounds validation when reading from shared memory in the audio thread (current code does clamp via `morphX_.store()`, but the link-read path at `PluginProcessor.cpp:1770` passes `linkX`/`linkY` directly into `morphProcessor.process()` without clamping).

---

## HIGH-PRIORITY ISSUES

### H-1: `std::mutex` in `IntegrationEventBus`, `ActionLedger`, `PermissionKernel`, `WorkflowOrchestrator`, `MemoryStore` blocks blackboard pump thread

**Component:** `src/AI/AutomationControlPlane.h:314,330,343,366,396,428`

**Problem:** All five subsystems of `AutomationRuntime` use `std::mutex`. The blackboard pump thread (`AgentRuntime::blackboardPumpThread_`) calls `blackboard_.poll()` which calls `bus.listRecentSince()`, acquiring `IntegrationEventBus::mutex_`. The MCP server thread calls `dispatchTool()` which can acquire `ActionLedger::mutex_`, `PermissionKernel::mutex_`, `WorkflowOrchestrator::mutex_`, and `MemoryStore::mutex_`. Concurrent lock acquisition across these five subsystems creates a 5-lock dependency graph where any lock held while acquiring another creates a potential deadlock.

**Severity:** HIGH — deadlock potential under concurrent MCP + agent workloads.

**Recommended Fix:**
1. Establish a strict lock acquisition ordering and document it: `IntegrationEventBus::mutex_` → `ActionLedger::mutex_` → `PermissionKernel::mutex_` → `WorkflowOrchestrator::mutex_` → `MemoryStore::mutex_`.
2. Never acquire an earlier-ordered lock while holding a later-ordered one.
3. Replace `std::mutex` with `juce::SpinLock` in hot paths (event bus, action ledger) where critical sections are short.

---

### H-2: `createEditor()` uses raw `new` — exception in constructor leaks memory

**Component:** `src/Plugin/PluginProcessor.cpp:3372`

**Problem:** `return new MorePhiEditor(*this);` — if the `MorePhiEditor` constructor throws after partial construction, the raw `new` leaks. The catch block returns `nullptr`, but the partially-constructed object's memory is lost.

**Severity:** HIGH — memory leak on editor construction failure.

**Recommended Fix:**
```cpp
try {
    auto editor = std::make_unique<MorePhiEditor>(*this);
    return editor.release();
} catch (...) {
    return nullptr;
}
```

---

### H-3: State version is stored but never used for forward migration

**Component:** `src/Plugin/PluginProcessor.cpp:2515-2516`

**Problem:** `const juce::String stateVersion = xml->getStringAttribute("stateVersion", "0.0.0"); juce::ignoreUnused(stateVersion);` — the version attribute is read and immediately discarded. Old project files from v1.x or v2.x will have different state layouts (different snapshot bank format, missing agents section, etc.) that will silently corrupt or crash the plugin.

**Severity:** HIGH — project file corruption when loading old state in newer plugin versions.

**Recommended Fix:**
1. Implement a migration chain: detect version, apply incremental transforms to upgrade the XML to the current schema before restoring.
2. At minimum, validate that the loaded XML has the expected child elements (`SNAPSHOT_BANK`, `HOSTED_PLUGIN`, etc.) and reject with a user-visible warning if critical sections are missing.

---

### H-4: `ScopedAudioCallback allocGuard` at top of `processBlock` — unbounded RAII scope

**Component:** `src/Plugin/PluginProcessor.cpp:1601`

**Problem:** `ScopedAudioCallback allocGuard;` is declared at the very top of `processBlock`. In the JUCE VST3 wrapper, `ScopedAudioCallback` calls `IComponent::setActive(true)` in its constructor. This means activation happens on every `processBlock` call, which is redundant — the plugin should be activated once in `prepareToPlay`. Depending on the JUCE version, this may cause unnecessary virtual calls per block.

**Severity:** HIGH — per-block overhead from redundant activation call.

**Recommended Fix:**
1. Remove `ScopedAudioCallback allocGuard;` from `processBlock`. JUCE's VST3 wrapper already calls `setActive(true)` during `prepareToPlay`. If this was added for a specific DAW workaroud, document it and limit the scope to the specific code path.

---

### H-5: `flushPendingParameterCommandsForAssistant` sleeps on the caller thread with `juce::Thread::sleep()`

**Component:** `src/Plugin/PluginProcessor.cpp:875`

**Problem:** The retry loop in `flushPendingParameterCommandsForAssistant` calls `juce::Thread::sleep(5-25ms)` when exclusive access isn't immediately available. If called from the MCP server's connection thread, this blocks the TCP handler, preventing other MCP clients from being served. Under sustained AI edit floods, the MCP thread can sleep for up to 100ms (4 retries × 25ms max), making the MCP server appear unresponsive.

**Severity:** HIGH — MCP server unresponsiveness during high-load parameter editing.

**Recommended Fix:**
1. Move the flush operation to a dedicated background thread or use an async pattern (enqueue the flush request, deliver results via callback).
2. Alternatively, use `juce::SpinLock::tryEnter()` with a tight spin instead of `Thread::sleep()` — this keeps the thread responsive while waiting briefly.

---

### H-6: `reconfigureAudioDomainProcessing()` busy-waits with `Thread::sleep(1)` on the message thread

**Component:** `src/Plugin/PluginProcessor.cpp:3034-3044`

**Problem:** The function spins in a 1ms sleep loop on the message thread waiting for `audioDomainUsers_` to reach zero. With large FFT sizes (4096) at low buffer sizes (64 samples) and high oversampling (x8), audio domain processing can take significant time per block. The 100ms timeout is a good safety net, but the message thread is blocked during the entire wait, freezing the DAW's UI.

**Severity:** HIGH — DAW UI freeze for up to 100ms when toggling audio-domain features.

**Recommended Fix:**
1. Queue the reconfiguration as a deferred task and use a lock-free handoff (set a "reconfigure pending" flag, let the audio thread detect it at the start of the next safe block, do the reconfiguration between blocks, and signal completion).
2. This eliminates any message-thread blocking — the audio thread owns its own configuration change.

---

### H-7: No bounds check on `linkX`/`linkY` from `LinkBroadcaster::receive()` in audio thread

**Component:** `src/Plugin/PluginProcessor.cpp:1770-1780`

**Problem:** When Link Mode is active as a follower, the audio thread reads `linkX` and `linkY` from `linkBroadcaster_.receive()` and stores them into `morphX_`/`morphY_` without clamping. While `setMorphX()`/`setMorphY()` DO clamp, the direct store `morphX_.store(linkX, ...)` at line 1773 bypasses those setters. Corrupted shared memory could produce NaN or values outside [0,1].

**Severity:** HIGH — potential NaN propagation or out-of-range morph coordinate on audio thread.

**Recommended Fix:**
```cpp
if (linkBroadcaster_.receive(linkX, linkY)) {
    linkX = juce::jlimit(0.0f, 1.0f, std::isfinite(linkX) ? linkX : 0.5f);
    linkY = juce::jlimit(0.0f, 1.0f, std::isfinite(linkY) ? linkY : 0.5f);
    morphX_.store(linkX, std::memory_order_relaxed);
    morphY_.store(linkY, std::memory_order_relaxed);
}
```

---

### H-8: `AgentRuntime::sharedContext_` lifetime — raw pointer into stack-local context was fixed but agent `prepare()` timing remains fragile

**Component:** `src/Plugin/PluginProcessor.cpp:2986-3016`, `src/AI/Agents/AgentRuntime.cpp:45-52`

**Problem:** The code correctly fixes the dangling-context bug by making `sharedContext_` a member of `AgentRuntime`. However, `registry_.prepareAll(sharedContext_)` is called in `AgentRuntime::start()`, which happens AFTER `registerAgent()`. Agents' `prepare()` methods store `ctx->processor`, `ctx->tools`, etc. as raw pointers. If `startAgentRuntimeIfNeeded()` is called before `mcpServer.startServer()` completes (possible if the timer fires while the MCP server is still binding its port), the `AutomationRuntime*` pointer in `sharedContext_` may point to a partially-initialized automation runtime.

**Severity:** HIGH — agents may dereference a partially-initialized `AutomationRuntime` if MCP server startup is slow.

**Recommended Fix:**
1. Ensure `startMCPServerIfNeeded()` is fully complete (MCP server running, `AutomationRuntime` fully initialized) before `startAgentRuntimeIfNeeded()` is called. Add a checkpoint or sequenced guard.
2. In `AgentRuntime::start()`, validate that `sharedContext_.runtime != nullptr` before calling `registry_.prepareAll()`.

---

## MEDIUM-PRIORITY ISSUES

### M-1: `MemoryBlock::toBase64Encoding()` allocates in `getStateInformation`

**Component:** `src/Plugin/PluginProcessor.cpp:2400,2428`

**Problem:** `pluginState.toBase64Encoding()` and `pendingStateCopy.toBase64Encoding()` allocate a `juce::String` of potentially large size (hosted plugin state can be 100KB+). Called from the message thread, this is acceptable, but the allocation is proportional to state size and can take significant time.

**Severity:** MEDIUM — potential UI stall when saving projects with large hosted plugin states.

**Recommended Fix:** Use `MemoryBlock::append()` to copy the raw bytes directly into the `XmlElement` as a hex or raw binary attribute instead of base64, or pre-allocate the output string using `juce::String::preallocate()`.

---

### M-2: `enqueueParameterState` iterates all parameters even when most are unchanged

**Component:** `src/Plugin/PluginProcessor.cpp:346-370`

**Problem:** `enqueueParameterState` pushes ALL `MAX_PARAMETERS` values into the command queue, even when only a handful have changed since the last state. For a 2048-parameter plugin, this means 2048 individual `push()` calls (each acquiring the push spinlock) for a snapshot recall where perhaps 20 parameters actually differ.

**Severity:** MEDIUM — unnecessary queue contention and audio-thread drain work.

**Recommended Fix:**
1. Diff against `lastApplied_` or the current param snapshot and only enqueue changed parameters.
2. Add a `batchStart`/`batchEnd` marker pattern so the drain can apply the batch atomically.

---

### M-3: `BlackboardBridge::poll()` acquires `subscribersMutex_` (std::mutex) — can block the pump thread

**Component:** `src/AI/Agents/Blackboard/BlackboardBridge.h:61`, `BlackboardBridge.cpp`

**Problem:** The poll thread acquires `subscribersMutex_` while iterating subscribers. If an `onEvent` callback in a subscriber blocks (e.g., the Conductor re-delegates via `submitTask` which acquires the scheduler mutex), the pump thread stalls. The pump thread is a dedicated `std::thread` so this doesn't affect the audio thread, but it can delay event delivery.

**Severity:** MEDIUM — event delivery latency spikes under concurrent agent activity.

**Recommended Fix:** Use a read-write lock pattern: subscribers are registered infrequently, so a `std::shared_mutex` (C++17) or a copy-on-write pattern (snapshot the subscriber list under lock, iterate the snapshot without the lock) would eliminate contention.

---

### M-4: `AudioBufferPool` uses `std::mutex` — documented as NOT audio-thread safe

**Component:** `src/Core/AudioBufferPool.h:76`

**Problem:** The class correctly documents that it's not audio-thread safe, but nothing prevents accidental use from the audio thread. No `jassert` or runtime check guards against this.

**Severity:** MEDIUM — latent risk if `AudioBufferPool` is ever used from the audio thread during future development.

**Recommended Fix:** Add a `jassert(!juce::MessageManager::getInstanceWithoutCreating() || juce::MessageManager::getInstanceWithoutCreating()->isThisTheMessageThread())` in `acquireBuffer()` and `releaseBuffer()`, or check `juce::Thread::getCurrentThreadId()` against the audio thread ID.

---

### M-5: `AgentRuntime::blackboardPumpThread_` uses `std::thread` directly — not a `juce::Thread`

**Component:** `src/AI/Agents/AgentRuntime.cpp:77-89`

**Problem:** Using `std::thread` means the thread name doesn't appear in JUCE's thread list or the debugger's thread viewer. `juce::Thread::sleep()` is called on a raw `std::thread`, which works but doesn't respect JUCE's `Thread::threadShouldExit()` pattern. There's no clean shutdown timeout — if the poll loop hangs (e.g., deadlocked in `IntegrationEventBus::mutex_`), the `join()` in `stop()` blocks forever.

**Severity:** MEDIUM — degraded debuggability and potential hang on shutdown.

**Recommended Fix:**
1. Replace with a `juce::Thread` subclass for the pump. This gives proper thread naming, `threadShouldExit()` support, and JUCE debugger integration.
2. Add a timeout to the `join()`: if the pump thread doesn't stop within 2 seconds, detach it and log an error.

---

### M-6: `std::pow(10.0f, ...)` in `syncStateFromAPVTS()` — called every audio block

**Component:** `src/Plugin/PluginProcessor.cpp:1479`

**Problem:** `sidechainThresholdLinear_.store(std::pow(10.0f, scThresholdDb / 20.0f), ...)` — `std::pow` with floating-point arguments is expensive (20-50 cycles) and is called every block in the `syncStateFromAPVTS()` path from `processBlock`.

**Severity:** MEDIUM — unnecessary per-block CPU cost (small but measurable).

**Recommended Fix:** Use a fast dB-to-linear lookup table or approximation: for the range [-60, 0] dB with 0.5 dB resolution, a 121-entry lookup table eliminates the `pow` call entirely. Alternatively, use `juce::Decibels::decibelsToGain()` which JUCE may already optimize.

---

### M-7: `getProfilingReport()` sorts results on every call — allocation in `std::sort`

**Component:** `src/Plugin/PluginProcessor.cpp:961-966`

**Problem:** `getProfilingReport()` copies the stats map into a `std::vector` and sorts it. This is called from the UI thread for display purposes, so it's not audio-critical, but the `std::vector` copy allocates from the heap.

**Severity:** MEDIUM — unnecessary heap allocation when UI polls profiling data.

**Recommended Fix:** Pre-sort the stats at the point of insertion or maintain a sorted container. Alternatively, use a stack-allocated fixed-size array since the stats map is bounded by the number of registered profiling sections (currently ~12).

---

### M-8: `PriorityScheduler` starvation parameters are not configurable

**Component:** `src/AI/Agents/Scheduler/PriorityScheduler.h:88-92`

**Problem:** `starvationGuardMs_ = 1000` and `escalationTier2Ms_ = 5000` are hardcoded. Under heavy DAW load with many concurrent MCP requests, 1 second starvation guard may be too aggressive (promoting Background tasks too soon), while 5 seconds may be too long for time-sensitive analysis tasks.

**Severity:** MEDIUM — agent priority scheduling not tunable for different workload profiles.

**Recommended Fix:** Make these configurable via `EcosystemConfig` or a constructor parameter with sensible defaults.

---

### M-9: No VST3 unit information exposed for DAW bank/preset browsing

**Component:** `src/Plugin/PluginProcessor.h` (no `IUnitInfo` implementation)

**Problem:** The plugin exposes 12 programs via `getNumPrograms()`/`getCurrentProgram()` but doesn't implement `IUnitInfo` (VST3's way to expose preset hierarchy). Many DAWs (Cubase, Nuendo, Studio One) use unit info for their preset browser UI. Without it, the plugin's snapshots don't appear in the DAW's dedicated preset panel.

**Severity:** MEDIUM — reduced DAW integration quality for preset browsing.

**Recommended Fix:** Implement `IUnitInfo` in the VST3 wrapper or use JUCE's `AudioProcessor::addGroup()` API, mapping each occupied snapshot slot to a named program within a "Snapshots" unit.

---

### M-10: `formantSourceCaptured_` is a plain `bool` — not atomic, accessed from audio thread only but set in two different code paths

**Component:** `src/Plugin/PluginProcessor.h:547`, `PluginProcessor.cpp:2163-2176,2218-2230`

**Problem:** `formantSourceCaptured_` is a non-atomic `bool` read and written only on the audio thread, which is technically safe. However, the variable is set in two different code paths (the non-oversampled and oversampled branches of `applyOutputGainAndMetering`). If a reconfiguration happens mid-block (changing the oversampling factor), the `formantSourceCaptured_` flag could be set in one branch but the formant engine's internal state could be from the other branch.

**Severity:** MEDIUM — theoretical inconsistency in formant capture state during reconfiguration.

**Recommended Fix:** Reset `formantSourceCaptured_ = false` in `reconfigureAudioDomainProcessing()` since the spectral/formant engines are re-prepared there anyway.

---

## LOW-PRIORITY ISSUES

### L-1: `friend class MCPToolHandler` / `friend class MCPToolsExtended` breaks encapsulation

**Component:** `src/Plugin/PluginProcessor.h:75-76`

**Problem:** Two `friend` declarations give MCP tool handlers access to all private members of `MorePhiProcessor`. This creates a wide coupling surface that makes refactoring risky.

**Severity:** LOW — maintainability concern.

**Recommended Fix:** Extract the internal helpers that MCP tools need into a public `InternalAccess` interface or a dedicated `ProcessorBridge` class, removing the `friend` declarations.

---

### L-2: `DBG()` calls inside `setStateInformation` — unconditional in debug builds

**Component:** `src/Plugin/PluginProcessor.cpp:2508,2512,2593,2606,2644,2679,2685`

**Problem:** Multiple `DBG()` calls are present in `setStateInformation`. In debug builds, `DBG` outputs to the debugger console, which can be slow. While this is acceptable during development, it impacts startup time in debug-test workflows (e.g., opening many project files for validation).

**Severity:** LOW — debug-build performance impact only.

**Recommended Fix:** Use a `MORE_PHI_LOG` macro that can be compiled out entirely, or gate on a `verboseLogging` flag.

---

### L-3: `toBase64Encoding()` for hosted plugin state is inefficient for large states

**Component:** `src/Plugin/PluginProcessor.cpp:2400,2428`

**Problem:** `juce::MemoryBlock::toBase64Encoding()` expands data by ~33% and requires a full copy. For large hosted plugin states (e.g., Ozone 11 with 100KB+ state), this is wasteful.

**Severity:** LOW — unnecessary memory usage during state save.

**Recommended Fix:** Consider storing the state as a raw binary child element or using a compressed encoding.

---

### L-4: Agent capability function duplicates tool name strings

**Component:** `src/Plugin/PluginProcessor.cpp:2959-2970`

**Problem:** The `CapabilityFn` lambda in `startAgentRuntimeIfNeeded()` hardcodes tool name strings that must match `MCPToolHandler`'s dispatch table. Any mismatch is invisible at compile time.

**Severity:** LOW — maintenance risk from duplicated string constants.

**Recommended Fix:** Define tool names as `constexpr` string literals in a shared header included by both `MCPToolHandler` and the capability function.

---

### L-5: `currentSampleRate` and `currentBlockSize` are non-atomic plain types

**Component:** `src/Plugin/PluginProcessor.h:638-639`

**Problem:** `double currentSampleRate = 44100.0` and `int currentBlockSize = 512` are set in `prepareToPlay` and read in `processBlock`. Since JUCE guarantees `prepareToPlay` and `processBlock` are serialized (prepare always completes before process starts), this is technically safe. However, `reconfigureAudioDomainProcessing()` also reads these on the message thread, creating a cross-thread read that is technically a data race (though benign in practice since the values are always written before reads begin).

**Severity:** LOW — technically a TBAA data race, but benign in practice.

**Recommended Fix:** Make them `std::atomic<double>` and `std::atomic<int>`, or simply document the JUCE-provided ordering guarantee.

---

### L-6: `backupHostManager` / `backupParamBridge` (`hostManagerB_`, `paramBridgeB_`) naming is unclear

**Component:** `src/Plugin/PluginProcessor.h:519-520`

**Problem:** `hostManagerB_` and `paramBridgeB_` are referenced in the audio-domain path but their naming suggests "backup" rather than "second instance." This is confusing for new developers.

**Severity:** LOW — code readability.

**Recommended Fix:** Rename to `hostManagerAudioDomain_` / `paramBridgeAudioDomain_` or `secondaryHostManager_`.

---

### L-7: No `noexcept` on `createEditor()`

**Component:** `src/Plugin/PluginProcessor.cpp:3368`

**Problem:** `createEditor()` is declared `override` but not `noexcept`. While JUCE's base class doesn't mark it `noexcept`, the VST3 spec expects `IEditController::createView()` to not throw.

**Severity:** LOW — potential UB if exception escapes (already caught, but the catch itself could throw).

**Recommended Fix:** Wrap more tightly — catch at the `MorePhiEditor` constructor level, or mark the function `noexcept` after ensuring all called code is noexcept-safe.

---

## THREAD SAFETY SUMMARY

| Lock/Mutex | Thread(s) Acquired On | Contention Risk |
|---|---|---|
| `commandQueue.pushMutex_` (SpinLock) | UI, MCP, Agent workers | LOW — fast push |
| `commandConsumerLock_` (SpinLock) | Audio (try), MCP flush | MEDIUM — audio try-locks, MCP blocks |
| `touchStateLock_` (SpinLock) | Audio (try), MCP flush, command drain | MEDIUM — audio try-locks |
| `sanityConfigLock_` (SpinLock) | UI writes, breed/randomize reads | LOW — rare writes |
| `pendingStateMutex_` (SpinLock) | Message thread state save/restore | LOW — same-thread |
| `pendingPluginDescLock_` (SpinLock) | Timer callback + setState path | LOW |
| `resultsMutex_` (std::mutex) | Agent workers, MCP thread, any peekResult caller | **HIGH** — cross-thread, no priority safeguard |
| `runsMutex_` (std::mutex) | Agent workers | **HIGH** — same as above |
| `Scheduler::mutex_` (std::mutex) | Agent workers, submitGoal callers | **HIGH** — same as above |
| `IntegrationEventBus::mutex_` (std::mutex) | Blackboard pump, MCP, agents | **MEDIUM** — hot path for event delivery |
| `ActionLedger::mutex_` (std::mutex) | MCP tool dispatch | LOW — per-transaction |
| `PermissionKernel::mutex_` (std::mutex) | MCP tool dispatch | LOW — per-evaluation |
| `subscribersMutex_` (std::mutex) | Blackboard pump | MEDIUM — held during callback dispatch |
| `connectionsLock_` (CriticalSection) | MCP server thread | LOW — connection lifecycle only |

**Lock ordering (current, undocumented):**
1. `commandConsumerLock_` → `touchStateLock_` (audio thread path)
2. `commandConsumerLock_` → `touchStateLock_` (MCP flush path, blocking)
3. `Scheduler::mutex_` → `resultsMutex_` / `runsMutex_` (agent worker path)
4. `IntegrationEventBus::mutex_` → `subscribersMutex_` → `Scheduler::mutex_` (pump thread path, potential cycle!)

** DEADLOCK RISK **: Path 4 creates a potential lock ordering inversion with path 3. If the pump thread holds `subscribersMutex_` while an `onEvent` callback submits a task (acquiring `Scheduler::mutex_`), and simultaneously a worker holds `Scheduler::mutex_` while publishing a result (acquiring `IntegrationEventBus::mutex_`), this is a classic ABBA deadlock.

---

## AUDIO-THREAD SAFETY VERDICT

| Operation in `processBlock` | Real-Time Safe? | Notes |
|---|---|---|
| `syncStateFromAPVTS()` | ⚠️ Mostly | `std::pow()` on sidechain threshold (M-6) |
| `drainParameterCommandQueue` | ✅ Yes | SpinLock try-enter, pre-allocated scratch |
| `processMidiAndSidechain` | ✅ Yes | Pre-allocated MIDI buffers |
| `applyMorphAndParameters` | ✅ Yes | No allocations, spinlock try-enter |
| `hostManager.processBlock` | ⚠️ Depends on hosted plugin | try/catch guards present |
| `hostedPlugin::setValue()` | ⚠️ Try/catch per param | Virtual call into unknown code |
| Audio-domain processing | ✅ Yes | Pre-allocated scratch buffers |
| `sonicMasterEngine_.capture()` | ✅ Yes | Lock-free ring buffer write |
| RMS metering | ✅ Yes | Throttled, simple computation |
| `acquirePluginForUse()` | ✅ Yes | Atomic increment, no lock |
| `releasePluginFromUse()` | ✅ Yes | Atomic decrement |

---

## HOST COMPATIBILITY MATRIX

| DAW | Known Issues | Status |
|---|---|---|
| **FL Studio** | Timer-based deferred loading handles FL's scan-time load issues; `callAsync` workaround in place; `prepared` flag prevents early processBlock | ✅ Good |
| **Ableton Live** | APVTS state nesting handled via recursive `findApvtsElement` (H-3 fix); program browser works via `getNumPrograms` | ✅ Good |
| **Pro Tools** | AAX wrapper uses JUCE; `getStateInformation` from audio-thread path detected and handled; potential state-size issues with large hosted plugin states | ⚠️ Needs testing |
| **Reaper** | Sidechain bus handling tested; block sizes as low as 64 supported; touch cooldown dynamic | ✅ Good |
| **Studio One** | No known issues; VST3 sidechain standard | ✅ Good |
| **Cubase/Nuendo** | Missing `IUnitInfo` means preset browser won't show snapshots (M-9) | ⚠️ Partial |
| **Logic Pro (AU)** | AU build not audited (VST3-only scope); JUCE AU wrapper should work if built | ❓ Unknown |
| **Offline Render/Export** | `forceSynchronousLoad_` flag exists; `pendingStateRestore_` for deferred loads; bounded retry with `MAX_PLUGIN_LOAD_RETRIES` | ✅ Good |

---

## RECOMMENDATIONS (PRIORITIZED)

1. **[CRITICAL]** Replace `std::mutex` in agent/scheduler subsystems with `juce::SpinLock` or implement a documented lock ordering protocol. (C-1, H-1)
2. **[CRITICAL]** Pre-cache state for `getStateInformation` audio-thread callers. (C-2)
3. **[CRITICAL]** Fix `enqueueParameterBatch` to skip invalid indices instead of rejecting the entire batch. (C-3)
4. **[CRITICAL]** Add integrity validation to `LinkBroadcaster` shared memory reads. (C-4)
5. **[HIGH]** Add bounds/NaN checks to link mode morph position on the audio thread. (H-7)
6. **[HIGH]** Implement state version migration chain. (H-3)
7. **[HIGH]** Move `reconfigureAudioDomainProcessing()` to a lock-free audio-thread handoff pattern. (H-6)
8. **[HIGH]** Make `flushPendingParameterCommandsForAssistant` non-blocking for the MCP thread. (H-5)
9. **[HIGH]** Fix `createEditor` memory leak on exception. (H-2)
10. **[HIGH]** Ensure `startAgentRuntimeIfNeeded()` is sequenced after MCP server startup. (H-8)
11. **[MEDIUM]** Diff-apply optimization for `enqueueParameterState`. (M-2)
12. **[MEDIUM]** Replace `std::pow` in `syncStateFromAPVTS` with table lookup. (M-6)

---

*End of audit report.*
