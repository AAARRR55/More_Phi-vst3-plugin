# More-Phi v3.3.0 — Snapshot System & State Management Audit Report

**Sub-Agent:** Snapshot System & State Management Specialist  
**Scope:** `SnapshotBank`, `ParameterState`, `PresetSerializer` (V1/V2), `MetaPresetManager`, `PresetLibrary`, `SnapshotRing`  
**Date:** 2026-05-09  
**Lines of Code Audited:** ~2,800 across 14 source files  

---

## Executive Summary

The 12-slot snapshot system uses a **seqlock** for lock-free audio-thread reads, a `juce::SpinLock` (`writeLock_`) for serializing non-audio writers, and a separate `chunksLock_` for VST3 opaque state chunks. The core seqlock implementation is **correct** for weakly-ordered architectures (ARM/Apple Silicon), but there are **several thread-safety holes** around the `paramNames_` array and the mutable scratch buffers, plus **data-loss gaps** in preset serialization (V1 meta-presets do not restore state chunks; V2 presets do not capture current `SnapshotBank` state at all).

**Severity Distribution:**
- Critical: 4
- High: 8
- Medium: 5
- Low: 4

---

## Critical Issues

### C1 — `paramNames_` is unprotected on read paths (data race / crash potential)

`SnapshotBank` stores parameter names per slot in `std::array<juce::StringArray, NUM_SLOTS> paramNames_` (header line 345). Writers (`capture()`, `captureValuesWithNames()`, `fromXml()`, `clearSlot()`, `clearAll()`) hold `writeLock_` when modifying this array. However, **two public read paths access `paramNames_` without any lock:**

1. **`toXml()`** (`SnapshotBank.h:149–155`) reads `paramNames_[i]` inside the per-slot seqlock loop but does **not** acquire `writeLock_`. If `capture()` (from the MCP thread) runs concurrently, `juce::StringArray` may reallocate its internal heap buffer while `toXml()` is iterating, leading to **use-after-free or corrupted DAW state XML**.

2. **`findParameterIndex()`** (`SnapshotBank.h:93–99`) is a public inline method that reads `paramNames_[slot]` with **zero synchronization**. It is called from the test suite (`TestStatePersistence.cpp:39`) and could be called from UI or MCP threads. A concurrent `capture()` or `fromXml()` produces a classic reader–writer data race.

> **Rationale:** `juce::StringArray` is heap-allocating (not real-time safe). Its internal `Array<juce::String>` can reallocate on `clear()` or `add()`. A concurrent read during reallocation is undefined behavior in C++.

---

### C2 — Mutable scratch buffers are shared across threads (data corruption)

`SnapshotBank` declares two mutable scratch buffers (`SnapshotBank.h:349–353`):

```cpp
mutable std::array<float, MAX_PARAMETERS> recallScratch_{};
mutable std::array<float, MAX_PARAMETERS> captureScratch_{};
```

**`captureScratch_` is written to *before* the write lock is acquired.**

In `captureValuesWithNames()` (`SnapshotBank.cpp:78–82`):
```cpp
captureScratch_.fill(0.0f);
std::copy_n(values, static_cast<size_t>(safeCount), captureScratch_.begin());

WriteScope write(*this);   // <-- lock acquired *after* scratch is written
(*slots_)[slot].capture(captureScratch_.data(), safeCount);
```

If the UI thread (right-click on `SnapshotRing`) and the MCP thread (`MCPToolHandler.cpp:2926`, `3901`) call `captureSnapshotToSlot()` simultaneously, they both write to `captureScratch_` before either enters the critical section. One thread’s data can be partially overwritten by the other, resulting in **a slot containing a corrupted mix of two captures**.

**`recallScratch_` is similarly shared** between the audio-thread MIDI callback (`PluginProcessor.cpp:116`) and MCP self-test threads (`MCPToolHandler.cpp:2471`, `3587`). During self-test execution, a concurrent `recallFast()` from the audio thread can overwrite the buffer while the MCP thread is still reading it.

---

### C3 — `fromXml()` marks empty slots as occupied, breaking snapshot consistency

`SnapshotBank::fromXml()` (`SnapshotBank.h:207–210`) contains:

```cpp
else {
    (*tmpSlots)[slot].setName(name.toRawUTF8());
    (*tmpSlots)[slot].occupied = true;   // <-- BUG
}
```

If the XML element has no `values` base64 string or `paramCount <= 0`, the slot is still marked **occupied** with **zero parameters** and **no values**. This creates an inconsistent state:

- `isOccupied(slot)` returns `true`.
- `recall()` / `recallFast()` returns early because `parameterCount == 0` (`SnapshotBank.cpp:393`).
- The UI will draw the slot as occupied, but recalling it does nothing.

This can happen with malformed DAW state XML, a corrupted preset file, or a manual XML edit.

---

### C4 — V1 `PresetSerializer` never restores state chunks, breaking Full recall mode

`PresetSerializer::deserialize()` (`PresetSerializer.cpp:117–142`) restores snapshot values via `bank.captureValues()` but **never calls `bank.captureStateChunk()`** for any slot. Therefore:

- A user saves a meta-preset with `RecallMode::Full` (including opaque VST3 state chunks).
- On reload, the float parameters are restored, but the opaque state chunks are **lost**.
- Plugins like Kontakt or wavetable synths will not restore their internal state, leading to **audible state divergence** or lost sample mappings.

The same gap exists in `PresetSerializerV2::migrateFromV1()` (`PresetSerializerV2.cpp:299–391`), which parses the V1 `SNAPSHOT_BANK` XML but ignores any `stateChunk` attributes.

---

## High-Priority Improvements

### H1 — `toXml()` silently drops slots after 128 retries (silent data loss)

`SnapshotBank::toXml()` iterates each slot inside a seqlock retry loop. If a slot exhausts `MAX_READ_RETRIES` (128) without a consistent read, the loop simply moves to the next slot (`SnapshotBank.h:110–165`). There is **no log message, no error flag, and no way for the caller to know data was lost**.

Under heavy write contention (e.g., rapid MCP capture during a DAW state save), a slot could be omitted from the serialized XML. The DAW would save an incomplete snapshot bank, and the user would permanently lose that slot on project reload.

> **Suggested fix:** Add a `bool allSlotsSaved` flag. If any slot fails, log a warning (`juce::Logger::writeToLog`) and return a partially populated XML so the caller can at least detect the issue.

---

### H2 — `PresetSerializer::serialize()` ignores `tryReadLocked()` failure

`PresetSerializer::serialize()` (`PresetSerializer.cpp:29–48`) calls:

```cpp
bank.tryReadLocked([&](const std::array<ParameterState, SnapshotBank::NUM_SLOTS>& slots)
{
    // ... serializes slots directly into JSON arrays
});
```

The return value of `tryReadLocked()` is **discarded**. If all 128 retries fail, the lambda still executes with **potentially torn or partially updated data** from `slots_`. The resulting JSON preset is silently corrupted.

> **Suggested fix:** `if (!bank.tryReadLocked(...)) { juce::Logger::writeToLog(...); return {}; }`

---

### H3 — `PresetSerializerV2::toJson()` does not capture current `SnapshotBank` state

`PresetSerializerV2::toJson()` (`PresetSerializerV2.cpp:50–124`) builds a JSON skeleton from the `PresetEntry` metadata. For the `snapshots` section, it **only merges existing `jsonData` if present** (lines 76–92). If the user has created a new preset and captured snapshots, the `PresetEntry::jsonData` field is empty, so `toJson()` creates an **empty snapshot structure**:

```cpp
j["snapshots"] = {
    {"count", 12},
    {"occupied", nlohmann::json::array()},
    {"data", nlohmann::json::object()}
};
```

**V2 presets therefore cannot save the current snapshot bank at all.** This is a major architectural gap: the `PresetLibrary` CRUD system is disconnected from the live `SnapshotBank`.

> **Suggested fix:** Add a `SnapshotBank` reference parameter to `PresetSerializerV2::toJson()` and serialize the live bank data into the `snapshots` object when `jsonData` is empty or stale.

---

### H4 — `SnapshotBank::captureStateChunk()` does not catch exceptions

`SnapshotBank::captureStateChunk()` (`SnapshotBank.cpp:117–129`) wraps `plugin->getStateInformation(chunk)` but **has no try/catch**:

```cpp
juce::MemoryBlock chunk;
plugin->getStateInformation(chunk);   // can throw
```

Some hosted plugins throw exceptions during state serialization. The caller (`PluginProcessor::captureSnapshotToSlot`, line 312) does have a try/catch around the whole block, but a direct call to `captureStateChunk()` (e.g., from future MCP tools) would crash.

Similarly, `recallStateChunk()` (`SnapshotBank.cpp:142–176`) does not catch exceptions from `plugin->setStateInformation()`.

> **Suggested fix:** Wrap both in `try/catch` blocks and return `false` on failure, matching the defensive style in `PluginProcessor::getStateInformation()`.

---

### H5 — `fromXml()` does not validate `paramCount` against negative/malicious values

`fromXml()` reads `paramCount` from XML (`SnapshotBank.h:186`):

```cpp
int count = child->getIntAttribute("paramCount", 0);
```

If `count` is negative, the `if (base64.isNotEmpty() && count > 0)` guard is false, so the `else` branch executes: `occupied = true` with no values. If `count` is extremely large (e.g., `999999999`), the `PARAM_NAMES` parsing loop (`SnapshotBank.h:215–223`) iterates that many times, doing nothing useful but burning CPU.

> **Suggested fix:** Clamp `count` to `[0, MAX_PARAMETERS]` immediately after reading, and skip the slot if out of range.

---

### H6 — `PresetSerializer::deserialize()` does not restore parameter names

`PresetSerializer::deserialize()` uses `bank.captureValues(s, values)` (line 140). This method does **not** set `paramNames_`. Consequently, after loading a V1 meta-preset, `findParameterIndex()` will always return `-1`, breaking the VST3-H1 forward-compatibility feature that remaps parameters when a hosted plugin’s parameter order changes.

---

### H7 — `SnapshotBank::toXml()` allocates inside the seqlock read window

Inside the seqlock read loop (`SnapshotBank.h:136–162`), `toXml()` calls:

- `juce::String((*slots_)[i].name)` — allocates a `juce::String`.
- `block.toBase64Encoding()` — allocates a `juce::String` for base64.
- `slotXml->createNewChildElement(...)` — allocates XML nodes.

These allocations happen **while the seqlock is open** (between `seq1` and `seq2`). Because `toXml()` is not on the audio thread, allocations are allowed, but they **extend the critical section**, increasing contention with writers. Under rapid MCP writes, the audio thread or other readers could starve.

> **Suggested fix:** Copy the slot data into a stack-local or pre-allocated buffer, then exit the seqlock before doing XML construction and base64 encoding.

---

### H8 — `SnapshotBank::toXml()` cannot distinguish "no snapshots" from "all reads failed"

`toXml()` returns `std::unique_ptr<juce::XmlElement>` which is always non-null because the root element is created before the loop (`SnapshotBank.h:105`). If every slot fails seqlock reads, the caller receives an empty `<SNAPSHOT_BANK/>` element, indistinguishable from a bank where all slots are genuinely unoccupied.

---

## Medium-Priority Refinements

### M1 — `PresetLibrary::rebuildIndex()` does not handle JSON parse failures gracefully

`rebuildIndex()` (`PresetLibrary.cpp:86–98`) iterates all `.json` files and deserializes them. If a file is corrupted, `PresetSerializerV2::deserialize` returns `false` and the file is silently skipped. There is **no log output** and **no mechanism to quarantine bad files**. A user with 100 presets and one corrupted file will never know which one is missing.

> **Suggested fix:** Log the filename when deserialization fails, or move corrupted files to a `.corrupted` subfolder.

---

### M2 — `MetaPresetManager` has no thread safety and no error handling

`MetaPresetManager` (`MetaPresetManager.h:8–37`) is documented as "message thread only," but there is no enforcement. `savePreset()` (`MetaPresetManager.cpp:12–17`) calls `file.replaceWithText()` which returns `void` — if the disk is full or the path is read-only, the failure is **silent**.

> **Suggested fix:** `replaceWithText()` writes to a temp file and renames on success; check the return value and propagate `false`.

---

### M3 — `ABCompareEngine` reserves slot 11 without runtime enforcement

`ABCompareEngine::kReservedSlot = 11` (`ABCompareEngine.h:30`) is the rollback slot. The `SnapshotBank` API does not mark slot 11 as reserved or prevent UI capture into it. A user could right-click slot 11 and overwrite the rollback checkpoint, breaking the AB-compare workflow.

> **Suggested fix:** Add a reserved-slot mask to `SnapshotBank`, or make `ABCompareEngine` allocate a dedicated hidden slot outside the 12 UI slots.

---

### M4 — `SnapshotBank::getOccupiedSlots` mixes `std::array` with C-array usage in UI code

`SnapshotBank::getOccupiedSlots()` returns into a `std::array<int, NUM_SLOTS>`. However, `MorphPad.cpp` (`lines 85, 297`) and `BreedingPanel.cpp` use raw C arrays:

```cpp
int occupiedSlots[SnapshotBank::NUM_SLOTS];
```

This is inconsistent and prevents the compiler from bounds-checking in debug builds.

---

### M5 — `SnapshotBank::toXml()` does not check `count` for sanity before using it

While `ParameterState::capture()` clamps `parameterCount`, `toXml()` trusts the value read under seqlock (`SnapshotBank.h:116`). If memory corruption ever produces a `count > MAX_PARAMETERS` or `count < 0`, `MemoryBlock` could be constructed with an invalid size. This is defensive-depth only, as `capture()` does clamp.

---

## Low-Priority Enhancements

### L1 — AGENTS.md documents stale `~384 KB` size for `SnapshotBank`

`AGENTS.md:143` says: "`SnapshotBank` heap-allocates its 12-slot array (~384 KB)". The inline comment in `SnapshotBank.h:332–334` corrects this: the actual size is **96.8 KB** (12 × 2048 × 4 bytes + overhead). The AGENTS.md should be updated to prevent misleading stack-safety reasoning.

---

### L2 — `PresetLibrary::getPresetFile()` sanitization is incomplete for non-UUID IDs

`getPresetFile()` (`PresetLibrary.cpp:43–51`) replaces `/`, `\`, and `:` with `_`. If the preset ID is user-supplied (not a UUID), characters like `?`, `*`, `<`, `>` could still produce invalid filenames on Windows. Since the current code uses UUID v4, this is theoretical, but the sanitization should be more robust.

---

### L3 — `SnapshotBank::recall()` and `recallFast()` are identical

Both methods have the exact same implementation (`SnapshotBank.cpp:88–115`). The only difference is the name. This is confusing for maintainers. `recallFast()` should either be a trivial inline forwarding call to `recall()`, or the comment should be updated to explain why they are intentionally identical (they are both "params-only" recall; the "Fast" name is historical).

---

### L4 — `PresetSerializerV2::fromJson()` does not validate inner snapshot structure

The `validate()` method (`PresetSerializerV2.cpp:204–295`) checks top-level metadata (`version`, `id`, `name`, `rating`, `tags`, `hostedPlugin`) but does **not** validate the `snapshots`, `morphState`, or `modulation` sections. A malformed `snapshots.data` object could cause crashes in downstream consumers.

---

## Recommended Fixes (with code sketches)

### Fix C1 — Synchronize `paramNames_` reads

**Option A:** Add `writeLock_` acquisition to `toXml()` and `findParameterIndex()`.

```cpp
// In toXml(), before reading paramNames_[i]:
const juce::SpinLock::ScopedLockType readLock(writeLock_);
const auto& names = paramNames_[i];

// In findParameterIndex():
int findParameterIndex(int slot, const juce::String& paramName) const
{
    if (slot < 0 || slot >= NUM_SLOTS) return -1;
    const juce::SpinLock::ScopedLockType readLock(writeLock_);
    const auto& names = paramNames_[slot];
    int idx = names.indexOf(paramName);
    return (idx >= 0) ? idx : -1;
}
```

> **Trade-off:** `writeLock_` is a spinlock; holding it during `toXml()` (which does base64 encoding) could block writers for a long time. **Option B** is preferred: move `paramNames_` into a `std::array<std::atomic<char*>, NUM_SLOTS>` or use a `std::shared_mutex` for reader-writer semantics. However, the simplest immediate fix is to **acquire `writeLock_` in `toXml()` for the entire loop** (not per-slot), since `toXml()` is infrequent and `writeLock_` is short-held by writers.

---

### Fix C2 — Thread-local or per-call scratch buffers

Replace the mutable shared scratch buffers with **stack-local arrays** inside the methods, or use **thread-local storage**:

```cpp
// Option: thread-local scratch (no allocation, no shared state)
thread_local std::array<float, MAX_PARAMETERS> captureScratch;
thread_local std::array<float, MAX_PARAMETERS> recallScratch;
```

> **Trade-off:** `thread_local` may not be available on all embedded targets, but for desktop VST3/AU it is safe. Alternatively, make the callers provide a buffer (API change), or allocate on the stack (the original motivation for scratch buffers was avoiding 8 KB stack frames, but modern hosts have larger stacks; `SnapshotBank` is already heap-allocated, so 8 KB on the caller's stack is acceptable).

---

### Fix C3 — Reject empty slots in `fromXml()`

```cpp
// In fromXml(), replace the else block:
else {
    // Slot has a name but no parameter data — do NOT mark occupied
    (*tmpSlots)[slot].setName(name.toRawUTF8());
    // (*tmpSlots)[slot].occupied = true;  // REMOVED
}
```

A slot should only be considered occupied if it has at least one parameter value or a state chunk.

---

### Fix C4 & H6 — Restore state chunks and parameter names in V1 deserialization

In `PresetSerializer::deserialize()`, after `bank.captureValues(s, values)` (line 140), add:

```cpp
// Restore parameter names for VST3-H1 remapping
if (auto* namesArr = slotVar.getProperty("names", {}).getArray())
{
    juce::StringArray names;
    for (int i = 0; i < namesArr->size(); ++i)
        names.add(namesArr->getReference(i).toString());
    // Need a new API: bank.captureValuesWithNames(s, values.data(), values.size(), names);
}

// Restore state chunk (Full recall mode)
if (auto* chunkVar = slotVar.getProperty("stateChunk", {}).getBinaryData())
{
    juce::MemoryBlock chunk(chunkVar->getData(), chunkVar->getSize());
    bank.captureStateChunk(s, chunk);
}
```

> **Note:** This requires extending the V1 JSON format to include `names` and `stateChunk` per slot, or switching the meta-preset system to use `SnapshotBank::toXml()` / `fromXml()` directly instead of a custom JSON format.

---

### Fix H2 — Check `tryReadLocked()` return value

```cpp
bool readOk = bank.tryReadLocked([&](const auto& slots) { ... });
if (!readOk)
{
    juce::Logger::writeToLog("PresetSerializer::serialize — SnapshotBank read failed");
    return {};
}
```

---

### Fix H5 — Clamp `paramCount` in `fromXml()`

```cpp
int count = child->getIntAttribute("paramCount", 0);
if (count < 0 || count > MAX_PARAMETERS)
    continue;  // Skip malformed slot
```

---

### Fix H3 — Connect V2 preset serialization to live `SnapshotBank`

Add a new overload or modify `PresetSerializerV2::toJson()` to accept a `SnapshotBank&` and serialize the current bank state when `jsonData` is empty:

```cpp
static nlohmann::json toJson(const PresetEntry& preset,
                             const SnapshotBank* bank = nullptr);
```

Inside `toJson()`, if `bank != nullptr` and `preset.jsonData.empty()`, call `bank->toXml()` and convert the XML to the V2 JSON snapshot format.

---

## Thread-Safety Matrix

| Component | Audio Thread | Message Thread | MCP Thread | Notes |
|-----------|-------------|----------------|------------|-------|
| `SnapshotBank::tryReadLocked()` | ✅ Safe (seqlock) | ✅ Safe | ✅ Safe | Correct on ARM/x86 |
| `SnapshotBank::recallFast()` | ⚠️ Race on `recallScratch_` | Not used | ⚠️ Race on `recallScratch_` (self-test) | See C2 |
| `SnapshotBank::capture()` | Not used | ⚠️ Race on `captureScratch_` | ⚠️ Race on `captureScratch_` | See C2 |
| `SnapshotBank::toXml()` | Not used | ⚠️ Race on `paramNames_` | Not used | See C1 |
| `SnapshotBank::fromXml()` | Not used | ⚠️ Race on `paramNames_` (via writeLock_) | ⚠️ Race on `paramNames_` (via writeLock_) | Writers are safe, readers are not |
| `SnapshotBank::findParameterIndex()` | Not used | ❌ Unsynchronized | ❌ Unsynchronized | See C1 |
| `PresetSerializer::serialize()` | Not used | ✅ Safe | Not used | Ignores `tryReadLocked` failure (H2) |
| `PresetSerializer::deserialize()` | Not used | ✅ Safe | Not used | Loses state chunks (C4) |
| `PresetLibrary` CRUD | Not used | ✅ Safe (single-threaded) | Not used | No error handling (M2) |

---

## Test Coverage Gaps

| Gap | Risk |
|-----|------|
| No test for concurrent `capture()` + `toXml()` | C1 race is not exercised |
| No test for `fromXml()` with empty/missing values | C3 not caught |
| No test for `PresetSerializer` round-trip with state chunks | C4 not caught |
| No test for `PresetSerializerV2` capturing live bank | H3 not caught |
| No test for `findParameterIndex()` under concurrent write | C1 not caught |
| No test for negative `paramCount` in XML | H5 not caught |

---

## Conclusion

The core seqlock and heap-allocation design of `SnapshotBank` is **sound and correct**. The most dangerous issues are **outside the seqlock itself**:

1. **`paramNames_` lacks reader synchronization** — a straightforward fix with `writeLock_` acquisition.
2. **Mutable scratch buffers are shared** — a `thread_local` or per-call buffer replacement eliminates the race.
3. **Preset serialization drops state chunks** — requires extending V1/V2 formats or using `toXml()`/`fromXml()` as the canonical serialization path.
4. **V2 presets do not capture live snapshot data** — an architectural gap requiring wiring `SnapshotBank::toXml()` into the V2 preset pipeline.

Fixing **C1, C2, and C4** should be the highest priority for the v3.3.1 maintenance release, as they directly affect user data integrity and crash stability.

---

*Report generated by Sub-Agent 2 (Snapshot System & State Management Specialist).*  
*All line numbers refer to commit `v3.3.0` source tree.*
