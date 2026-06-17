# Sub-Agent 2 Report: Snapshot System & State Management

## Executive Summary

The More-Phi snapshot system and state management code shows mature design intent with seqlock-based lock-free reads, heap-allocated buffers for stack safety, and careful attention to real-time constraints. However, **several critical concurrency bugs** were found in the seqlock implementation, **data race conditions** on auxiliary snapshot metadata, a **fundamentally broken V1→V2 migration path**, and **defective discrete parameter classification** that collapses all multi-step discrete parameters to a single value. Additionally, the **test suite contains schema mismatches** that would cause failures when linked against production code.

---

## Critical Issues (CRASH / DATA CORRUPTION / STATE LOSS)

### Issue 1: SnapshotBank::toXml() Violates Seqlock Read Pattern — Data Read AFTER Validation
- **Location**: `SnapshotBank.h:136–162` (inside `toXml()`)
- **Severity**: Critical
- **Description**: The seqlock pattern requires that **all data reads happen between the two sequence-number loads** (`seq1` and `seq2`). In `toXml()`, the `seq2` validation occurs at line 133, but the actual reads of `name`, `values`, and `paramNames_` happen **after** the validation (lines 136–162). If a writer begins between `seq2` and these reads, the reader sees partially updated or corrupted data.
- **Root Cause**: The seqlock read loop was incorrectly structured so that XML creation (base64 encoding, attribute writes) was placed after the consistency check rather than before it. Only `occupied` and `parameterCount` are read before `seq2`; the actual payload reads happen after.
- **Impact**: DAW state saves (`getStateInformation`) can serialize corrupted snapshot data, leading to **preset data loss** or **restore of corrupted parameters** on next session load. Since `toXml()` runs on the message thread while the MCP thread may concurrently call `capture()`, this is a real race.
- **Recommended Fix**: Move all data reads into temporaries **before** the `seq2` check. Read `paramNames_` under `writeLock_` (see Issue 2). Example:

```cpp
for (int retry = 0; retry < MAX_READ_RETRIES; ++retry)
{
    uint32_t seq1 = seqlock_.load(std::memory_order_acquire);
    if ((seq1 & 1) != 0) continue;

    bool occupied = (*slots_)[i].occupied;
    int count = (*slots_)[i].parameterCount;
    char nameCopy[64];
    std::copy_n((*slots_)[i].name, 64, nameCopy);
    std::array<float, MAX_PARAMETERS> valuesCopy{};
    if (occupied && count > 0)
        std::copy_n((*slots_)[i].values.data(), count, valuesCopy.data());

    juce::StringArray namesCopy;
    {
        const juce::SpinLock::ScopedLockType lock(writeLock_);
        namesCopy = paramNames_[i];
    }
    juce::MemoryBlock chunkCopy;
    {
        const juce::SpinLock::ScopedLockType chunkLock(chunksLock_);
        if (occupied) chunkCopy = stateChunks_[i];
    }

    std::atomic_thread_fence(std::memory_order_acquire);
    uint32_t seq2 = seqlock_.load(std::memory_order_acquire);
    if (seq1 != seq2) continue;

    // Use nameCopy, valuesCopy, namesCopy, chunkCopy only
    // ... build XML from temporaries ...
    break;
}
```

---

### Issue 2: SnapshotBank::toXml() Reads paramNames_ Without writeLock_ — Data Race
- **Location**: `SnapshotBank.h:152–157` (inside `toXml()`)
- **Severity**: Critical
- **Description**: `paramNames_` is a `std::array<juce::StringArray, NUM_SLOTS>`. `juce::StringArray` is **not thread-safe** for concurrent read/write — it uses reference-counted heap blocks and dynamic reallocation. All write paths (`capture()`, `captureValuesWithNames()`, `clearSlot()`, `clearAll()`, `fromXml()`) acquire `writeLock_` before modifying `paramNames_`. However, `toXml()` reads `paramNames_[i]` **without holding any lock**, while the MCP thread or message thread may be writing to it. This is undefined behavior and can cause heap corruption or crashes.
- **Root Cause**: The `toXml()` author assumed the seqlock protects `paramNames_`, but `paramNames_` is **not** inside the seqlock-protected `slots_` array. The header comment at `findParameterIndex()` explicitly states: *"FIX C4: paramNames_ is written under writeLock_; read must serialize too."* — but `toXml()` was not updated to follow this rule.
- **Impact**: Crash (use-after-free, double-free, or corrupted `StringArray` internal heap) during DAW state save if a concurrent snapshot capture occurs.
- **Recommended Fix**: As shown in Issue 1, read `paramNames_[i]` under `writeLock_` and copy it into a local `juce::StringArray` before releasing the lock. The copy is fast (reference-counted pointer swap for `StringArray` internals).

---

### Issue 3: PresetSerializerV2::migrateFromV1() Expects XML but V1 Format Is JSON
- **Location**: `PresetSerializerV2.cpp:299–391`
- **Severity**: Critical
- **Description**: The `migrateFromV1()` function takes a `juce::XmlElement&` and expects the V1 format to be XML (`<PRESET version="1" name="...">` with `<SNAPSHOT_BANK>` children). However, the actual V1 `PresetSerializer::serialize()` produces a **JSON object** (`juce::DynamicObject` with properties `"version"`, `"snapshots"`, `"apvts"`, etc.). The migration path is therefore **orphaned** — it can never succeed on real V1 presets.
- **Root Cause**: The V1 preset format was changed from XML to JSON during development, but the V2 migration was written against the old XML spec without updating to match the JSON format. There is no XML serialization path for V1 presets in the codebase.
- **Impact**: **Users upgrading from V1 to V2 will lose all presets.** The migration function is dead code that always returns `false` for real V1 data.
- **Recommended Fix**: Rewrite `migrateFromV1()` to accept a `juce::var` (or `nlohmann::json`) representing the V1 JSON structure:

```cpp
static bool migrateFromV1(const juce::var& v1Json, PresetEntry& outPreset)
{
    if (!v1Json.isObject()) return false;
    int version = v1Json.getProperty("version", 0);
    if (version != 1) return false;
    // ... parse "snapshots" array, "apvts" string, etc. ...
}
```

Alternatively, provide a dual-path migration that accepts both XML (legacy DAW state) and JSON (legacy meta-presets).

---

### Issue 4: ParameterClassifier::analyzeParameters() Sets stepCount=1 for All Discrete/Enumeration Parameters
- **Location**: `ParameterClassifier.cpp:67–68`
- **Severity**: Critical
- **Description**: For any parameter classified as `Discrete` or `Enumeration`, `analyzeParameters()` hardcodes `meta.stepCount = 1`. This value is propagated to `DiscreteParameterHandler::stepCount_` during `initialize()`. In `DiscreteParameterHandler::valueToStep()`:

```cpp
const float maxStep = static_cast<float>(steps - 1);  // = 0 when steps == 1
return std::clamp(static_cast<int>(value * maxStep), 0, steps - 1);  // always 0
```

With `stepCount = 1`, **every discrete parameter is quantized to a single step (value 0)** regardless of its actual range. This makes the `Stepwise` blend strategy completely non-functional. Even worse, `stepToValue()` returns 0 when `maxStep == 0`, so any parameter using stepwise morphing is permanently stuck at 0.
- **Root Cause**: The `IParameterBridge` interface does not expose the actual number of steps for discrete parameters, and the classifier does not attempt to query it. The fallback value of 1 is a placeholder that was never replaced with real detection.
- **Impact**: Any discrete parameter with more than 2 options (e.g., oscillator waveform selector, filter type, envelope mode) will be **destroyed during stepwise morphing** — the user loses all intermediate settings.
- **Recommended Fix**: Add a step-count query to `IParameterBridge` (e.g., `getNumSteps(int index)` or `getNumProgrammaticValues(int index)`). In `analyzeParameters()`, query the real step count and set `meta.stepCount` accordingly. For JUCE-hosted plugins, `AudioProcessorParameterWithID` / `AudioParameterChoice` expose the number of choices.

```cpp
// In IParameterBridge (or IParameterBridge extension)
virtual int getNumSteps(int index) const { return 0; } // 0 = continuous

// In ParameterClassifier::analyzeParameters()
if (meta.type == ParameterType::Discrete || meta.type == ParameterType::Enumeration)
{
    int steps = host.getNumSteps(static_cast<int>(i));
    meta.stepCount = (steps > 0) ? static_cast<uint16_t>(steps) : 1;
}
```

---

### Issue 5: DiscreteParameterHandler::initialize() Does Not Clear strategyOverrides_
- **Location**: `DiscreteParameterHandler.cpp:22–50`
- **Severity**: High
- **Description**: When a new hosted plugin is loaded, `initialize()` is called with the new parameter classifier. It resizes `paramStates_`, `paramStrategies_`, and `stepCount_`, but **`strategyOverrides_` is never cleared**. Overrides from a previously loaded plugin (e.g., "use Crossfade for oscillator mix") remain active for indices that may now refer to completely different parameters in the new plugin.
- **Root Cause**: `strategyOverrides_` was omitted from the reset logic in `initialize()`.
- **Impact**: Incorrect discrete parameter blending strategies applied to wrong parameters after plugin swap, causing unexpected clicks, pops, or silent parameters.
- **Recommended Fix**: Add `strategyOverrides_.clear();` at the top of `initialize()`.

---

## High-Priority Issues (CONCURRENCY / ARCHITECTURAL)

### Issue 6: SnapshotBank.cpp Seqlock Spin-Wait Missing ARM Yield Instruction
- **Location**: `SnapshotBank.cpp:188–190`, `214–216`, `248–250`
- **Severity**: High
- **Description**: `isOccupied()`, `hasAnyOccupied()`, and `getOccupiedSlots()` use `#if defined(__x86_64__) ... _mm_pause(); #endif` but have **no ARM equivalent** (`__yield`). On Apple Silicon and other ARM64 hosts, the CPU spins busy-waiting instead of yielding, wasting power and potentially starving other threads.
- **Root Cause**: The ARM yield path (`__asm__ __volatile__("yield" ::: "memory");`) was added to `SnapshotBank.h` (`tryReadLocked`, `copySlotValues`) but not copied to the `.cpp` implementations.
- **Recommended Fix**: Add a shared `SPIN_PAUSE()` macro or inline helper in the header and use it consistently in both `.h` and `.cpp`:

```cpp
inline void spinPause() noexcept
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    _mm_pause();
#elif defined(__arm__) || defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}
```

---

### Issue 7: SnapshotBank::fromXml() Creates "Ghost" Occupied Slots
- **Location**: `SnapshotBank.h:209–212`
- **Severity**: High
- **Description**: In `fromXml()`, if a slot element has no `values` base64 data (or `count <= 0`), the `else` branch still sets `occupied = true`:

```cpp
else {
    (*tmpSlots)[slot].setName(name.toRawUTF8());
    (*tmpSlots)[slot].occupied = true;   // <-- should be false
}
```

A slot with no parameter values but `occupied = true` will pass `isOccupied()`, return `parameterCount = 0`, and cause `recall()` to be a no-op — but the UI will show it as occupied.
- **Recommended Fix**: Set `occupied = false` in the `else` branch, or only set `occupied = true` when `count > 0 && base64.isNotEmpty()`.

---

### Issue 8: PresetSerializer::deserialize() Ignores Base64 Decode Failure for Hosted Plugin State
- **Location**: `PresetSerializer.cpp:162`
- **Severity**: High
- **Description**: `hostedState.fromBase64Encoding(hostedStateVar.toString())` returns a `bool` indicating success/failure, but the return value is **not checked**. If the base64 string is malformed, `hostedState` may contain partial or garbage data. The code then checks `hostedState.getSize() > 0` and passes the garbage to `plugin->setStateInformation()`, which could crash the hosted plugin.
- **Recommended Fix**: Check the return value:

```cpp
if (hostedStateVar.isString() &&
    hostedState.fromBase64Encoding(hostedStateVar.toString()))
{
    // safe to use
}
```

---

### Issue 9: ParameterClassifier::deserialize() Leaves Stale Metadata Beyond New Count
- **Location**: `ParameterClassifier.cpp:631–638`
- **Severity**: High
- **Description**: After deserializing `count` metadata entries, the remaining `MAX_PARAMS - count` entries in `metadata_` retain their old values. If the previous plugin had 500 parameters and the new one has 10, parameters 11–499 still have stale `isExposed`, `isSanityProtected`, `importanceScore`, etc. These can leak into the new classification.
- **Recommended Fix**: Zero out all entries beyond the new count:

```cpp
for (uint32_t i = count; i < MAX_PARAMS; ++i)
    metadata_[i] = ParameterMetadata{};
```

---

### Issue 10: DiscreteParameterHandler::processDiscreteParameters() Passes All Parameters Through if parameterCount_ == 0
- **Location**: `DiscreteParameterHandler.cpp:83–84`
- **Severity**: High
- **Description**: If `initialize()` was never called (or the classifier reported 0 parameters), `parameterCount_` is 0. The loop over discrete parameters is skipped, but the fallback passthrough loop copies **all** `interpolatedValues` to `outputValues` without any discrete handling. This means discrete parameters are treated as continuous, causing clicks and pops.
- **Recommended Fix**: Add an assertion or early return with a warning. Better yet, require `initialize()` to be called before `processDiscreteParameters()` and return a sentinel or skip processing if not initialized.

---

## Medium-Priority Issues (EDGE CASE / ROBUSTNESS)

### Issue 11: SnapshotBank::capture() Double-Loads preparedParamCount_ with Mixed Memory Orders
- **Location**: `SnapshotBank.cpp:32–34`
- **Severity**: Medium
- **Description**: `capture()` loads `preparedParamCount_` with `acquire`, then if > 0, loads again with `relaxed`. The second load may observe a different value if `prepare()` was called between the two loads. In practice this is harmless (only changes from 0 to positive or vice versa), but it is a subtle atomic consistency issue.
- **Recommended Fix**: Load once into a local variable:

```cpp
const int prepared = preparedParamCount_.load(std::memory_order_acquire);
const int limit = (prepared > 0) ? prepared : MAX_PARAMETERS;
```

---

### Issue 12: SnapshotBank::fromXml() paramNames Loop Uses Raw `count` Instead of `safeCount`
- **Location**: `SnapshotBank.h:217`
- **Description**: The parameter-name restoration loop iterates `for (int p = 0; p < count; ++p)` where `count` is the raw XML attribute. Although `safeCount` is computed earlier (`juce::jmin(count, MAX_PARAMETERS)`), the names loop uses the unclamped `count`. If `count > MAX_PARAMETERS`, the loop adds `count` empty strings to `tmpNames[slot]`, which is wasteful and could cause `StringArray` to grow unnecessarily large.
- **Recommended Fix**: Use `safeCount` in the names loop.

---

### Issue 13: TestPresetLibrary.cpp Mock Schema Field Mismatches Production
- **Location**: `tests/Unit/TestPresetLibrary.cpp:60–61`, `87–88`
- **Severity**: Medium
- **Description**: The test mock uses `"schema": "2.0"` as the version field, but the production `PresetSerializerV2` uses `"version": "2.0"`. When the tests are eventually linked against production, all schema-validation tests will fail. Additionally, the test `PresetSearchQuery` uses `tags` while the production `PresetEntry.h` uses `requiredTags`.
- **Recommended Fix**: Update the test mock to match production field names exactly, or better yet, remove the mock and link tests directly against `PresetSerializerV2` and `PresetLibrary` once they are fully implemented.

---

### Issue 14: SnapshotBank::getSlotValuesCopy() Is Documented as "Not Audio Thread" but No Enforcement
- **Location**: `SnapshotBank.h:271–287`, `SnapshotBank.cpp:271–287`
- **Severity**: Medium
- **Description**: `getSlotValuesCopy()` takes `writeLock_` (a spinlock) and allocates into `std::vector`. If accidentally called from the audio thread, it can cause priority inversion (spinning) and heap allocation. The comment warns against it, but there is no compile-time or runtime guard.
- **Recommended Fix**: Add a `JUCE_ASSERT_MESSAGE_THREAD` or `jassert(!juce::Thread::getCurrentThreadId() == audioThreadId_)` assertion. Store the audio thread ID during `prepare()` for comparison.

---

### Issue 15: PresetSerializerV2::toJson() Range-Based For Over String-Literal Initializer List
- **Location**: `PresetSerializerV2.cpp:82–86`
- **Severity**: Medium
- **Description**: `for (auto& section : {"snapshots", "morphState", "modulation"})` relies on `std::initializer_list<const char*>`. On some older nlohmann::json versions or strict compiler settings, `existing.contains(section)` where `section` is `const char*` may not match the expected overload (some versions only accept `std::string` or `json_pointer`). This could cause a compile error or unexpected behavior.
- **Recommended Fix**: Use explicit `std::string` keys:

```cpp
for (const auto* section : {"snapshots", "morphState", "modulation"})
{
    if (existing.contains(section))
        j[section] = existing[section];
}
```

---

### Issue 16: MorePhiProcessor::setStateInformation() May Block Audio Thread via callFunctionOnMessageThread
- **Location**: `PluginProcessor.cpp:1805–1817`
- **Severity**: Medium
- **Description**: If a misbehaving host calls `setStateInformation()` from the audio thread (violating JUCE spec, but some hosts do), `callFunctionOnMessageThread()` will **block the audio thread** until the message thread executes the callback. This can cause an audio dropout or deadlock.
- **Recommended Fix**: Check `juce::Thread::getCurrentThreadId()` against the message thread ID. If already on the message thread, call `startTimer(50)` directly. If on any other thread, use a thread-safe queue or flag instead of `callFunctionOnMessageThread`.

---

## Low-Priority Issues (STYLE / DOCUMENTATION)

### Issue 17: SnapshotBank::toXml() and fromXml() Use std::move on ParameterState (Unnecessary)
- **Location**: `SnapshotBank.h:241`, `SnapshotBank.h:251`
- **Severity**: Low
- **Description**: `ParameterState` is a POD-like struct with `std::array<float, 2048>`. It is not efficiently movable; `std::move` falls back to copy. The `std::move` in `fromXml()` is misleading and suggests optimization where there is none.
- **Recommended Fix**: Remove `std::move` for `ParameterState` assignments; keep it for `juce::StringArray` and `juce::MemoryBlock` which are movable.

---

### Issue 18: SnapshotBank::capture() Fills Entire 2048-Element Scratch Buffer
- **Location**: `SnapshotBank.cpp:36`, `58`, `78`
- **Severity**: Low
- **Description**: `captureScratch_.fill(0.0f)` zeros all 2048 elements even when `safeCount` is much smaller (e.g., 50). This wastes CPU cycles.
- **Recommended Fix**: Only fill up to `safeCount`:

```cpp
std::fill(captureScratch_.begin(), captureScratch_.begin() + safeCount, 0.0f);
```

---

### Issue 19: ParameterClassifier::calculateImportance() Uses Magic Number for Recency Half-Life
- **Location**: `ParameterClassifier.cpp:507`
- **Severity**: Low
- **Description**: `3600000000000.0f` (1 hour in nanoseconds) is a magic number with no named constant. The comment says "1 hour half-life" but the code is hard to verify.
- **Recommended Fix**: Add a named constant:

```cpp
static constexpr float RECENCY_HALF_LIFE_NS = 3.6e12f; // 1 hour
```

---

### Issue 20: TestStatePersistence.cpp Has Minimal Coverage
- **Location**: `tests/Unit/TestStatePersistence.cpp`
- **Severity**: Low
- **Description**: The only test case covers the happy path (capture → chunk → XML → restore → clear). It does **not** test:
  - Concurrent read/write under seqlock contention
  - Seqlock retry exhaustion (`MAX_READ_RETRIES`)
  - Empty slot recall (should return false / no-op)
  - Parameter name remapping (`findParameterIndex()`)
  - Malformed XML restoration (partial data, missing attributes)
  - Rapid capture/recall cycles
- **Recommended Fix**: Add test cases for:
  - `tryReadLocked()` under concurrent write load (use a second thread)
  - `isOccupied()` returning `false` for cleared slots
  - `findParameterIndex()` with name remapping and missing names
  - `fromXml()` with malformed / partial XML elements (should preserve original state)
  - `getSlotValuesCopy()` returning correct data after concurrent capture

---

## Positive Findings (What Is Done Well)

1. **Heap-Allocated Slots Array**: `SnapshotBank` uses `std::unique_ptr<std::array<ParameterState, NUM_SLOTS>>` to avoid ~96 KB on the stack, preventing stack overflow in hosts like FL Studio that use small thread stacks. (`SnapshotBank.h:54–55`)

2. **Pre-Allocated Scratch Buffers**: Both `recallScratch_` and `captureScratch_` are pre-allocated `std::array<float, MAX_PARAMETERS>` members, ensuring zero heap allocation on the audio thread during capture/recall. (`SnapshotBank.h:351–355`)

3. **Seqlock with Proper Memory Fences**: The seqlock implementation includes `std::atomic_thread_fence(std::memory_order_acquire)` before `seq2` validation, which is essential for correctness on ARM/Apple Silicon weakly-ordered CPUs. (`SnapshotBank.h:291`, `SnapshotBank.h:419`)

4. **Separate Mutex for State Chunks**: `chunksLock_` is a separate `juce::SpinLock` for `stateChunks_`, preventing heap-allocating `MemoryBlock` copies from violating the seqlock's side-effect-free read contract. (`SnapshotBank.h:330`)

5. **Two-Phase Parsing in fromXml()**: `fromXml()` parses into temporary storage first, then swaps atomically under the write lock. If any XML element is malformed, the original snapshot data is preserved. (`SnapshotBank.h:176–253`)

6. **Graceful Degradation on Seqlock Exhaustion**: All public read methods (`isOccupied`, `hasAnyOccupied`, `getOccupiedSlots`, `tryReadLocked`) return safe fallbacks (`false`, 0, empty) if `MAX_READ_RETRIES` is exhausted, rather than crashing or hanging. (`SnapshotBank.h:303–305`)

7. **Exception Safety in PresetSerializer**: Both `serialize()` and `deserialize()` wrap `getStateInformation()` / `setStateInformation()` calls in `try/catch` blocks, preventing a misbehaving hosted plugin from crashing More-Phi during preset save/load. (`PresetSerializer.cpp:64–77`, `168–182`)

8. **Deferred Plugin Loading with Retry Logic**: `setStateInformation()` uses a `Timer`-based retry mechanism (up to 10 attempts) instead of `MessageManager::callAsync()`, which is known to drop callbacks in some hosts. This makes state restoration more reliable. (`PluginProcessor.cpp:1770–1831`)

9. **Touch Detection with Cooldown**: The per-parameter touch detection (`lastApplied_`, `touchCooldown_`, `touchMorphX_`/`touchMorphY_`) prevents the morph engine from overwriting manual knob changes, with a dynamic cooldown computed from sample rate and block size to always approximate ~200 ms. (`PluginProcessor.cpp:917`, `PluginProcessor.h:512`)

10. **Discrete Parameter Strategies**: `DiscreteParameterHandler` provides multiple blend strategies (`HardSwitch`, `Crossfade`, `Stepwise`, `HoldSource`, `HoldTarget`) and per-parameter overrides, showing thoughtful design for handling non-continuous parameters during morphing. (`DiscreteParameterHandler.h:57–77`)

---

*Report generated by Sub-Agent 2 (Snapshot System & State Management Specialist)*
*All findings verified by direct source code inspection of the listed files.*
