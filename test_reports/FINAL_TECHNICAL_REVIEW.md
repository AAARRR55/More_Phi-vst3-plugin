# More-Phi v3.3.0 — Unified Technical Review & Production Readiness Report

**Lead Architect:** Orchestrator Synthesis  
**Date:** 2025-06-17  
**Scope:** Full source-tree audit across 6 specialized domains  
**Sources:** Sub-Agent Reports 1–6 (`test_reports/subagent_1_report.md` through `subagent_6_report.md`)

---

## Executive Summary

More-Phi v3.3.0 is a sophisticated, well-architected JUCE 8 VST3/AU plugin with strong foundational design (seqlock-based snapshot bank, lease-based plugin hosting, lock-free command queues, and SIMD-tuned interpolation). However, **the codebase is not production-ready without a focused v3.3.1 patch release**. We identified **23 critical issues** that can cause DAW crashes, user data loss, stuck notes, or silent parameter corruption. The majority of these issues are not deep architectural flaws but rather **contract mismatches, missing synchronization, and incomplete serialization paths** that can be fixed with targeted, low-risk patches.

**Key Verdict:**
- **Immediate blockers:** 7 (DAW crash, data loss, stuck notes, security side-channel, deadlock)
- **High-risk fixes needed:** 16 (thread races, NaN propagation, preset corruption, UI drift)
- **Production readiness:** **Conditional — fix blockers before release.**

---

## 1. Systemic Risk Assessment (Cross-Domain Issues)

These are issues that span multiple subsystems and share common root causes. Fixing them requires coordinated changes across files.

### S1 — Audio-Thread `noexcept` Contract Violation
**Root Cause:** The project declares `processBlock` and `MorphProcessor::process` as `noexcept`, but delegates to functions that are not guaranteed to be exception-free.

**Affected Systems:**
- `PluginProcessor::processBlock` (`noexcept`) → `hostManager.processBlock()` (not `noexcept`) — **Agent 1 #1, Agent 6 #1**
- `MorphProcessor::process` (`noexcept`) → `updatePhysics()` / `applySmoothing()` (not `noexcept`) — **Agent 3 #Medium-3**

**Impact:** A misbehaving hosted plugin or a thrown exception in the physics engine calls `std::terminate()` on the audio thread, crashing the DAW instantly.

**Coordinated Fix:**
1. Mark `PluginHostManager::processBlock` as `noexcept` (it already catches internally).
2. Mark `MorphProcessor::updatePhysics` and `applySmoothing` as `noexcept` (they perform no allocations).
3. Alternatively, wrap `hostManager.processBlock()` in `processBlock` with a `try/catch(...)` guard as a safety net.

---

### S2 — UI ↔ Audio Thread Data Races on Plain `float` Telemetry
**Root Cause:** Several audio-thread variables are read by the UI thread without `std::atomic`, violating the C++ memory model.

**Affected Systems:**
- `MorphProcessor::processedX_` / `processedY_` — **Agent 3 #3**
- `LFO::currentValue_` — **Agent 3 #6**
- `EnvelopeFollower::envelope_` — **Agent 3 #7**

**Impact:** Torn reads, stale values, or compiler reordering causing incorrect UI display. On x86, torn 32-bit floats are rare, but the compiler is free to optimize based on the assumption of no cross-thread access.

**Coordinated Fix:** Change all three to `std::atomic<float>` with `memory_order_relaxed` stores/loads. This is a one-way, visual-only telemetry channel; relaxed ordering is sufficient.

---

### S3 — Shared Mutable Scratch Buffers in `SnapshotBank`
**Root Cause:** `SnapshotBank` uses mutable `captureScratch_` and `recallScratch_` member arrays to avoid stack allocation, but these are shared across UI, MCP, and audio threads without synchronization.

**Affected Systems:**
- `SnapshotBank::captureValuesWithNames()` — UI and MCP threads write simultaneously — **Agent 2 #C2**
- `SnapshotBank::recallFast()` — Audio thread and MCP self-test threads — **Agent 2 #C2**

**Impact:** Corrupted snapshot captures, mixed parameter data, or torn reads during self-test.

**Coordinated Fix:** Replace member scratch buffers with `thread_local` arrays or stack-local arrays inside the methods. `thread_local` is safe on desktop VST3/AU targets and eliminates shared state entirely.

---

### S4 — Preset Serialization Loses Opaque State Chunks (V1) and Live Snapshots (V2)
**Root Cause:** The V1 JSON serializer and V2 CRUD serializer were developed independently and do not use `SnapshotBank::toXml()` / `fromXml()` as the canonical persistence path.

**Affected Systems:**
- `PresetSerializer::deserialize()` — never restores state chunks — **Agent 2 #C4**
- `PresetSerializerV2::toJson()` — never captures live snapshot bank data — **Agent 2 #H3**
- `PresetSerializer::serialize()` — ignores `tryReadLocked()` failure — **Agent 2 #H2**

**Impact:** Full-recall meta-presets (Kontakt, wavetable synths) lose internal state. V2 presets saved from the browser contain empty snapshot structures.

**Coordinated Fix:**
1. Extend V1 JSON format to include `stateChunk` and `names` per slot.
2. Wire `PresetSerializerV2::toJson()` to accept a `SnapshotBank&` and serialize live bank data when `jsonData` is empty.
3. Check `tryReadLocked()` return value and fail loudly on retry exhaustion.

---

### S5 — Inconsistent Exception Handling & Defensive Coding
**Root Cause:** The codebase has a defensive philosophy (many `try/catch` blocks) but applies it inconsistently, leading to both unprotected `noexcept` boundaries and over-broad `catch(...)` swallowing fatal errors.

**Affected Systems:**
- `processBlock` unprotected from hosted plugin throws — **Agent 1 #1, Agent 6 #1**
- `ParameterBridge::applyParameterState` aborts entire batch on single throw — **Agent 1 #4**
- `SnapshotBank::captureStateChunk` / `recallStateChunk` lack `try/catch` — **Agent 2 #H4**
- `catch(...)` overused (10 instances) swallowing `std::bad_alloc` — **Agent 6 #6**
- `MCPServer::ConnectionThread::run` nested throw in `catch(...)` — **Agent 4 #M-3**

**Coordinated Fix:**
- At `noexcept` boundaries, ensure all callees are `noexcept` or wrap with `try/catch`.
- Inside loops, catch per-iteration so one failure does not abort the batch.
- Replace broad `catch(...)` with `catch(const std::exception&)` where possible; reserve `catch(...)` for third-party hosted plugin boundaries only.

---

### S6 — MCP Protocol Non-Compliance & Security Gaps
**Root Cause:** The MCP server implements a custom JSON-RPC layer without strict spec validation.

**Affected Systems:**
- Notifications return responses — **Agent 4 #C-2**
- No batch request support — **Agent 4 #H-1**
- Mixed `juce::JSON` / `nlohmann::json` parsers — **Agent 4 #M-1**
- Timing side-channel in auth — **Agent 4 #C-1**
- Port TOCTOU race — **Agent 4 #H-4**

**Coordinated Fix:** Standardize on `nlohmann::json` for all MCP parsing/serialization, add batch request dispatch, suppress responses for void `id`, and fix the auth comparison to use a fixed-length constant-time comparison.

---

## 2. Critical Issues (Immediate Blockers)

These issues can cause crashes, data loss, security breaches, or broken user workflows. They must be fixed before any production release.

| # | Issue | Files | Impact | Root System |
|---|-------|-------|--------|-------------|
| **C1** | `processBlock` is `noexcept` but calls non-`noexcept` hosted plugin | `PluginProcessor.cpp:1415`, `PluginHostManager.h:31` | DAW crash (`std::terminate`) | S1 |
| **C2** | Unbounded wait in `unloadPlugin` hangs DAW forever | `PluginHostManager.cpp:258` | DAW freeze / deadlock | Hosting |
| **C3** | `ParameterBridge` aborts entire batch on single parameter throw | `ParameterBridge.cpp:162-200` | Silent partial snapshot recall | S5 |
| **C4** | `SnapshotBank::paramNames_` read without lock | `SnapshotBank.h:93-99`, `149-155` | Use-after-free / crash | S3 |
| **C5** | Mutable scratch buffers shared across threads | `SnapshotBank.cpp:78-82` | Corrupted captures | S3 |
| **C6** | V1 `PresetSerializer` never restores state chunks | `PresetSerializer.cpp:117-142` | Data loss for Kontakt/wavetable synths | S4 |
| **C7** | NaN → undefined behavior in `compute1D` | `InterpolationEngine.cpp:273` | NaN / out-of-bounds | Physics |
| **C8** | NaN propagation in `compute2D` | `InterpolationEngine.cpp:315` | All output parameters become NaN | Physics |
| **C9** | UI thread data race on `processedX_` / `processedY_` | `MorphProcessor.h:99` | Torn reads / UB | S2 |
| **C10** | Elastic mode invisible in UI | `MorphPad.cpp:276` | UI shows raw position, not physics | Physics |
| **C11** | LFO S&H / Random miss multiple wraps | `LFO.cpp:128` | Wrong timing at large block sizes | Physics |
| **C12** | Non-atomic `currentValue_` in LFO | `LFO.h:88` | Data race / UB | S2 |
| **C13** | Non-atomic `envelope_` in EnvelopeFollower | `EnvelopeFollower.h:69` | Data race / UB | S2 |
| **C14** | MCP auth timing side-channel leaks token length | `MCPServer.cpp:449-483` | Security vulnerability | S6 |
| **C15** | JSON-RPC notifications return responses | `MCPServer.cpp:292-447` | Protocol non-compliance | S6 |
| **C16** | TokenOptimizer deadlock (lock-order inversion) | `TokenOptimizer.cpp:127-140`, `455-475` | Deadlock between UI and MCP threads | S6 |
| **C17** | `BreedingPanel` bypasses `GeneticEngine` and `SanityConfig` | `BreedingPanel.cpp:50-101` | Volume/bypass mutation unprotected | Genetics |
| **C18** | `ParameterClassifier` sets `stepCount=1`, breaking discrete params | `ParameterClassifier.cpp:67-68` | All discrete values quantize to 0 | Genetics |
| **C19** | MIDI note-offs swallowed in trigger range | `MIDIRouter.cpp:46-60` | Stuck notes in hosted synths | MIDI |
| **C20** | `ThreadPool` post-shutdown enqueue race | `ThreadPool.h:99-122`, `ThreadPool.cpp:58-68` | Infinite spin / resource leak | Threading |
| **C21** | `recoveryGracePeriod_` not reset on new plugin load | `PluginHostManager.cpp:232-234` | New plugin suspended prematurely | Hosting |
| **C22** | Suspended path returns without clearing buffer | `PluginHostManager.cpp:319-346` | Stale audio / pass-through glitch | Hosting |
| **C23** | `fromXml()` marks empty slots as occupied | `SnapshotBank.h:207-210` | Inconsistent snapshot state | Snapshots |

---

## 3. High-Priority Improvements

These are architectural or performance issues that degrade stability, correctness, or maintainability. They should be addressed in the next sprint.

| # | Issue | Files | Impact | Root System |
|---|-------|-------|--------|-------------|
| **H1** | Elastic damping not scaled with stiffness | `PhysicsEngine.cpp:28-33` | "Heavy" preset rings *more* than "Slow" | Physics |
| **H2** | Drift Orbit angle loses precision over time | `PhysicsEngine.cpp:177` | Orbit freezes after ~17 days | Physics |
| **H3** | `compute1D` leaves output tail uninitialized | `InterpolationEngine.cpp:282-289` | Stale data in tail parameters | Physics |
| **H4** | Macro seqlock can exhaust 64 retries with no fallback | `ModulationEngine.cpp:147-161` | Torn macro values under contention | Physics |
| **H5** | `smoothRate_` unclamped (>1.0 causes unstable amplification) | `MorphProcessor.cpp:124` | Filter becomes an unstable high-pass | Physics |
| **H6** | `PresetSerializerV2` cannot capture live snapshot data | `PresetSerializerV2.cpp:50-124` | V2 presets save empty snapshots | S4 |
| **H7** | `toXml()` silently drops slots after 128 retries | `SnapshotBank.h:110-165` | Silent data loss during DAW save | S4 |
| **H8** | `setStateInformation` race on `pendingPluginDesc_` | `PluginProcessor.cpp:1780-1808` | Data race on plugin description | Hosting |
| **H9** | `getLastDescriptionRef` returns unprotected reference | `PluginHostManager.h:76` | Thread-unsafe reference to `lastDescription` | Hosting |
| **H10** | `ParameterBridge` heap allocations in audio-reachable paths | `ParameterBridge.cpp:202-213`, `408-425` | RT allocation violation | Hosting |
| **H11** | `PluginHostManager::prepare` wide-buffer race | `PluginHostManager.cpp:111` | Use-after-free on `wideBuffer_` | Hosting |
| **H12** | Duplicate exception-handling code in `processBlock` | `PluginHostManager.cpp:375-418`, `439-489` | Maintenance hazard | S5 |
| **H13** | No JSON-RPC batch request support | `MCPServer.cpp:295` | Breaks spec-compliant clients | S6 |
| **H14** | `startThread()` failure ignored in MCP connection | `MCPServer.cpp:54-58` | Resource leak / crash vector | S6 |
| **H15** | `connectedClients_` leak on forced thread stop | `MCPServer.cpp:81-186` | State corruption | S6 |
| **H16** | Port availability TOCTOU race | `InstanceRegistry.cpp:110-118` | Port binding failure | S6 |

---

## 4. Medium-Priority Refinements

Edge cases, API inconsistencies, and performance micro-optimizations.

| # | Issue | Files | Impact |
|---|-------|-------|--------|
| **M1** | `SnapFader` division by zero if height ≤ 20 | `SnapFader.cpp:100-103` | NaN fader position |
| **M2** | Drift speed/distance not clamped at physics entry | `PhysicsEngine.cpp:152-155` | Unvalidated Perlin input |
| **M3** | Elastic velocity clamped after sub-step loop | `PhysicsEngine.cpp:41-58` | Transient overshoot possible |
| **M4** | `MIDIRouter` iterates MIDI buffer by value | `MIDIRouter.cpp:26` | Unnecessary copy overhead |
| **M5** | `DiscreteParameterHandler` silently fails if undersized | `DiscreteParameterHandler.cpp:65` | Uninitialized output in release builds |
| **M6** | `GeneticEngine` not real-time safe (logging, string allocation) | `GeneticEngine.cpp:18-25` | RT allocation violation |
| **M7** | Macro knobs hardcoded to parameters 0–7 | `MacroKnobStrip.cpp:15-29` | No user-configurable mapping |
| **M8** | `isBusesLayoutSupported` forces symmetric I/O | `PluginProcessor.cpp:1012-1035` | Mono plugin hosting blocked |
| **M9** | `ParameterClassifier` heuristic fragility | `ParameterClassifier.cpp:106-189` | Misclassification of parameters |
| **M10** | Time signature read but never used | `PluginProcessor.cpp:1168-1170` | Dead code per-block overhead |
| **M11** | `ABCompareEngine` slot 11 not runtime-enforced | `ABCompareEngine.h:30` | User can overwrite rollback slot |
| **M12** | `LockFreeQueue` `pushRange` `std::distance` O(N) risk | `LockFreeQueue.h:66-88` | Spinlock held too long |
| **M13** | `AudioBufferPool` uses `std::mutex` (not RT-safe) | `AudioBufferPool.h:74` | Kernel mutex if used on audio thread |
| **M14** | `PluginScanner` thread-unsafe `knownPlugins` access | `PluginHostManager.cpp:492-508` | Data race during scan |
| **M15** | `PerformanceProfiler` uses `std::chrono` syscalls on audio thread | `PerformanceProfiler.cpp:15-27` | Jitter in profiling builds |
| **M16** | Deprecated JUCE 8 APIs (`getBus`, `getBusBuffer`) | `PluginProcessor.cpp:1229-1232` | Forward-compatibility risk |
| **M17** | `processBlock` and `setStateInformation` violate `.clang-tidy` size limits | `PluginProcessor.cpp:1173-1561`, `1677-1831` | Unmaintainable, untestable |
| **M18** | `MockInterfaces.h` depends on unlinked Google Mock | `tests/Mocks/MockInterfaces.h` | Broken test dependency |
| **M19** | SIMD flags missing on spectral/granular engines | `CMakeLists.txt:649-664` | Scalar fallback on hot DSP |
| **M20** | `MetadataWriter.cpp` / `MetadataSchema.cpp` compiled with `/Od` | `CMakeLists.txt:468-475` | Unoptimized dataset pipeline |

---

## 5. Low-Priority Enhancements

Documentation, style, and minor optimizations.

| # | Issue | Files | Impact |
|---|-------|-------|--------|
| **L1** | `assert` vs `jassert` inconsistency | `DiscreteParameterHandler.cpp`, `ModulationEngine.cpp`, `OversamplingWrapper.h` | Standardization |
| **L2** | `LockFreeQueue` lacks `static_assert` on `T` triviality | `LockFreeQueue.h` | Safety for future types |
| **L3** | `Version.h` uses `__DATE__`/`__TIME__` (non-reproducible builds) | `src/Version.h` | CI caching / determinism |
| **L4** | `.clang-tidy` not integrated into CMake or CI | `.clang-tidy`, `CMakeLists.txt` | Unenforced quality rules |
| **L5** | `TokenOptimizer` token heuristic is simplistic | `TokenOptimizer.cpp:570-574` | Inaccurate budget warnings |
| **L6** | `MCPServer` uses wrong JSON-RPC error code for auth | `MCPServer.cpp:350-355` | Protocol semantics |
| **L7** | `MCPToolHandler.cpp` is 5507 lines (monolith) | `MCPToolHandler.cpp` | Compilation / navigation cost |
| **L8** | `enqueueParameterBatch` copies by value in loop | `PluginProcessor.cpp:216` | Warning / micro-overhead |
| **L9** | `processBlock` uses `std::fill` instead of SIMD clear | `PluginProcessor.cpp:1267` | Missed SIMD optimization |
| **L10** | `MORE_PHI_ENABLE_DATASET_V3` is a confusing no-op | `CMakeLists.txt:12` | Clarity |
| **L11** | `getEdition()` returns `Full` unconditionally | `src/Version.h:153` | Licensing stub |
| **L12** | `refreshDiscreteMap` defined outside namespace | `PluginProcessor.cpp:2256-2263` | Style consistency |

---

## 6. Recommended Fixes (Critical Issues with Code)

### Fix C1/C21 — `noexcept` Contract & `recoveryGracePeriod_` Reset

```cpp
// PluginHostManager.h
void processBlock(juce::AudioBuffer<float>& buffer,
                  juce::MidiBuffer& midi) override noexcept;

// PluginHostManager.cpp
void PluginHostManager::processBlock(juce::AudioBuffer<float>& buffer,
                                      juce::MidiBuffer& midi) noexcept
{
    // existing body (already internally exception-safe)
}

// In PluginHostManager::loadPlugin(), after successful swap:
exceptionCount_.store(0, std::memory_order_relaxed);
suspended_.store(false, std::memory_order_relaxed);
recoveryGracePeriod_.store(0, std::memory_order_relaxed);  // FIX C21
```

---

### Fix C2 — Bounded Wait in `unloadPlugin`

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
            DBG("PluginHostManager::unloadPlugin — timeout waiting for audio thread lease");
            break;  // Accept vanishingly small UAF risk over guaranteed DAW hang
        }
        juce::Thread::yield();
    }
    // ... rest of unload ...
}
```

---

### Fix C3 — Per-Parameter `try/catch` in `applyParameterState`

```cpp
for (int i = 0; i < safeCount; ++i)
{
    const float clamped = juce::jlimit(0.0f, 1.0f, values[i]);
    // ... throttling logic ...
    try {
        params[i]->setValue(clamped);
    } catch (...) {
        continue;  // Skip bad parameter; apply the rest
    }
    // ... throttle state update ...
}
```

---

### Fix C4/C5 — Thread-Safe `SnapshotBank` Reads & Scratch Buffers

```cpp
// Fix C4: acquire writeLock_ for paramNames_ reads
int SnapshotBank::findParameterIndex(int slot, const juce::String& paramName) const
{
    if (slot < 0 || slot >= NUM_SLOTS) return -1;
    const juce::SpinLock::ScopedLockType lock(writeLock_);  // FIX
    return paramNames_[slot].indexOf(paramName);
}

// In toXml(), acquire writeLock_ before the loop that reads paramNames_

// Fix C5: thread-local scratch buffers
thread_local std::array<float, MAX_PARAMETERS> captureScratch;
thread_local std::array<float, MAX_PARAMETERS> recallScratch;
// Replace captureScratch_ / recallScratch_ with these inside the methods
```

---

### Fix C6 — Restore State Chunks in V1 Deserialization

```cpp
// In PresetSerializer::deserialize(), after bank.captureValues(s, values):
if (auto* chunkVar = slotVar.getProperty("stateChunk", {}).getBinaryData())
{
    juce::MemoryBlock chunk(chunkVar->getData(), chunkVar->getSize());
    bank.captureStateChunk(s, chunk);
}
// Also restore parameter names for VST3-H1 remapping
```

---

### Fix C7/C8 — NaN Guards in InterpolationEngine

```cpp
void InterpolationEngine::compute1D(float faderPos, ...)
{
    if (!std::isfinite(faderPos)) {
        std::fill(output.begin(), output.end(), 0.5f);
        return;
    }
    // ... existing logic ...
}

void InterpolationEngine::compute2D(float cursorX, float cursorY, ...)
{
    if (!std::isfinite(cursorX) || !std::isfinite(cursorY)) {
        std::fill(output.begin(), output.end(), 0.5f);
        return;
    }
    // ... existing logic ...
}
```

---

### Fix C9/C12/C13 — Atomic UI Telemetry

```cpp
// MorphProcessor.h
std::atomic<float> processedX_{ 0.5f };
std::atomic<float> processedY_{ 0.5f };
float getProcessedX() const { return processedX_.load(std::memory_order_relaxed); }

// LFO.h
std::atomic<float> currentValue_{ 0.0f };
float getCurrentValue() const noexcept { return currentValue_.load(std::memory_order_relaxed); }

// EnvelopeFollower.h
std::atomic<float> envelope_{ 0.0f };
float getCurrentValue() const noexcept { return envelope_.load(std::memory_order_relaxed); }
```

---

### Fix C10 — Elastic Mode UI Visibility

```cpp
// MorphPad.cpp:276
if (physMode >= 1)   // Elastic (1) and Drift (2)
{
    float procX = morph.getProcessedX();
    float procY = morph.getProcessedY();
    cx = centre.x + procX * radius;
    cy = centre.y + procY * radius;
}
```

---

### Fix C11 — LFO Multiple Wrap Fix

```cpp
// LFO.cpp
phase_ += increment;
const int wrapCount = static_cast<int>(std::floor(phase_));
phase_ = std::fmod(phase_, 1.0f);
if (phase_ < 0.0f) phase_ += 1.0f;

// In S&H / Random branches:
if (wrapCount > 0) {
    shValue_ = nextRandom() * 2.0f - 1.0f;
}
```

---

### Fix C14 — Constant-Time Auth Comparison

```cpp
static constexpr size_t kTokenLen = 32; // bytes (256-bit hex = 64 chars)

bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i)
        diff |= (a[i] ^ b[i]);
    return diff == 0;
}

// In validateAuth(): compare against fixed kTokenLen, not max(candidate, expected)
```

---

### Fix C15 — Suppress JSON-RPC Notification Responses

```cpp
// MCPServer.cpp
if (idVar.isVoid()) {
    // Notification — process but do not return any response
    return {};
}
```

---

### Fix C16 — TokenOptimizer Lock Order

```cpp
// Define strict global order: statsMutex_ -> budgetMutex_ -> usageMutex_ -> ...
// Enforce in all functions, or merge budget+stats into a single coarse mutex
// since they are always accessed together.
```

---

### Fix C17 — Wire `BreedingPanel` to `GeneticEngine`

```cpp
void BreedingPanel::breedSnapshots()
{
    // ... obtain parent vectors ...
    ParameterState parentA, parentB;
    parentA.parameterCount = count;
    parentB.parameterCount = count;
    // ... fill values ...
    auto sanity = proc_.getSanityConfig();
    auto offspring = GeneticEngine::breed(parentA, parentB,
                                            0.2f + random_.nextFloat() * 0.6f,
                                            0.0f, random_, sanity);
    // ... apply offspring.values ...
}
```

---

### Fix C18 — Correct `stepCount` for Discrete/Enumeration

```cpp
// ParameterClassifier.cpp
else if (meta.type == ParameterType::Discrete || meta.type == ParameterType::Enumeration)
{
    const int hostSteps = host.getParameterNumSteps(static_cast<int>(i));
    meta.stepCount = (hostSteps > 1) ? static_cast<uint16_t>(hostSteps) : 10;
}
```

---

### Fix C19 — Pass Note-Offs Through in `MIDIRouter`

```cpp
if (note >= trigBase && note < trigBase + SnapshotBank::NUM_SLOTS)
{
    if (msg.isNoteOn() && msg.getVelocity() > 0)
    {
        const int slot = note - trigBase;
        auto cb = snapshotCb_.load(std::memory_order_acquire);
        if (cb) cb(slot, snapshotCtx_.load(std::memory_order_acquire));
        consumed = true;  // Only consume the triggering note-on
    }
    // Note-offs are NOT consumed — they must reach the hosted synth
}
```

---

### Fix C20 — ThreadPool Shutdown Guard

```cpp
// ThreadPool.h
auto enqueue(Task task) -> std::future<decltype(task())>
{
    if (stop_.load(std::memory_order_acquire))
        throw std::runtime_error("Cannot enqueue on stopped ThreadPool");
    // ... rest of enqueue ...
}

// In shutdown(), clear tasks_ under the lock:
std::lock_guard<std::mutex> lock(tasksMutex_);
tasks_.clear();
```

---

### Fix C22 — Clear Buffer on Suspended Path

```cpp
if (suspended_.load(std::memory_order_relaxed))
{
    buffer.clear();  // Ensure silence, not stale pass-through
    // ... existing recovery logic ...
    return;
}
```

---

### Fix C23 — Reject Empty Slots in `fromXml()`

```cpp
// In SnapshotBank::fromXml()
else {
    (*tmpSlots)[slot].setName(name.toRawUTF8());
    // Do NOT set occupied = true here
}
```

---

## 7. Production-Readiness Verdict

### Blockers (Must Fix Before Release)

1. **DAW Crash (`std::terminate`)** — `processBlock` `noexcept` contract. Fix: wrap or propagate `noexcept`.
2. **DAW Hang** — `unloadPlugin` unbounded wait. Fix: add 500 ms timeout.
3. **Data Loss** — V1 preset serialization drops state chunks. Fix: restore chunks in `deserialize()`.
4. **Stuck Notes** — MIDI note-offs swallowed. Fix: only consume note-ons in trigger range.
5. **Discrete Parameter Corruption** — `stepCount=1` breaks snapping. Fix: query host step count.
6. **Security** — MCP auth timing side-channel. Fix: fixed-length constant-time comparison.
7. **Deadlock** — TokenOptimizer lock-order inversion. Fix: enforce strict lock order or merge mutexes.

### Conditional Readiness (Fix in v3.3.1 Patch)

- NaN propagation in interpolation engines.
- UI/audio thread data races on `processedX_`, `currentValue_`, `envelope_`.
- `SnapshotBank` scratch buffer and `paramNames_` synchronization.
- `BreedingPanel` bypassing `GeneticEngine`.
- MCP JSON-RPC notification compliance and batch support.
- `ThreadPool` shutdown race.
- `recoveryGracePeriod_` inheritance bug.
- Suspended path stale audio.
- `fromXml()` empty slot occupancy bug.

### Overall Assessment

| Criterion | Grade | Notes |
|-----------|-------|-------|
| **Crash Stability** | C | `noexcept` contract violations, unbounded waits, and NaN propagation are real crash vectors. |
| **Data Integrity** | C | Preset serialization drops opaque chunks; V2 presets disconnect from live bank; shared scratch buffers corrupt captures. |
| **Thread Safety** | B | Core seqlock and lock-free queue are correct, but peripheral data races (paramNames, scratch buffers, UI telemetry) are numerous. |
| **Security** | B+ | Localhost-only binding and CSPRNG token generation are strong, but auth timing side-channel and standalone server lack of auth are gaps. |
| **Protocol Compliance** | C | JSON-RPC notifications, batching, and version validation are missing. |
| **Build/Test Hygiene** | B | CMake is well-structured, but `.clang-tidy` is unenforced, critical tests are undersized, and dead code remains. |
| **Code Maintainability** | B- | Large monolithic functions (`processBlock` 388 lines, `MCPToolHandler` 5507 lines) impede review and testing. |
| **User Experience** | B- | Elastic UI invisible, Heavy preset rings more than Slow, MIDI note-offs swallowed, breeding unsafe. |

**Final Recommendation:** 
> **Delay production release until the 7 blockers are resolved.** The codebase is fundamentally sound but requires a disciplined v3.3.1 patch focusing on `noexcept` contracts, snapshot bank synchronization, preset serialization completeness, and MIDI routing correctness. After blockers are fixed, the remaining high/medium issues can be addressed in a v3.3.2 maintenance cycle. The architecture is strong enough to support rapid patching; none of the fixes require redesign.

---

*End of Unified Technical Review*
