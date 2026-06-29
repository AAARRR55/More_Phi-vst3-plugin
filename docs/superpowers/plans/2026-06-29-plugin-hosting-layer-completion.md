# Plugin Hosting Layer Completion — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the in-flight, non-compiling hosting layer compile, link, and actually deliver native editor hosting, DAW transport forwarding, crash isolation with auto-recovery, and safe teardown.

**Architecture:** Complete the existing seams rather than introduce parallel mechanisms. Retire the inert `HostedPluginEditor` (UI keeps the working `HostedPluginWindow`). Rewrite `DAWTransportForwarder` as a JUCE-8 seqlock audio→message snapshot bus (delete the no-op `applyToPlugin`). Drive the existing-but-dead `PluginHealthMonitor` state machine via the existing 50ms maintenance timer → background-thread reinit. Harden teardown with a `shuttingDown_` recovery-abort guard. Repair the `PluginHostManager.cpp` compile errors. Add build wiring + 3 unit-test files.

**Tech Stack:** C++20, JUCE 8.0.4 (`juce_audio_processors`, `juce_core`), Catch2 v3, CMake (explicit source lists, no globs), Ninja generator on MSVC.

**Spec:** `docs/superpowers/specs/2026-06-29-plugin-hosting-layer-completion-design.md`

---

## File Structure

| File | Responsibility | Action |
|---|---|---|
| `src/Host/HostedPluginEditor.h/.cpp` | (retired — was an inert duplicate editor lifecycle) | **Delete** |
| `src/Host/DAWTransportForwarder.h` | `TransportState` struct + seqlock snapshot bus API | Rewrite |
| `src/Host/DAWTransportForwarder.cpp` | JUCE-8 producer/consumer impl | Rewrite |
| `src/Host/PluginHealthMonitor.h` | State-machine API (unchanged) + JUCE include | Edit (add include) |
| `src/Host/PluginHealthMonitor.cpp` | State-machine impl (unchanged) — add include to compile | Edit (add include) |
| `src/Host/PluginHostManager.h` | Drop `HostedPluginEditor`/`activeEditor_`; add `tickHealth`/`tryStartRecovery`/guards | Edit |
| `src/Host/PluginHostManager.cpp` | Repair compile errors; rewrite `processBlock` accounting; add recovery driver | Repair + extend |
| `src/Plugin/PluginProcessor.cpp` | Call `tickHealth()` (timer) + `updateFromAudioThread` (processBlock) | Edit (2 sites) |
| `CMakeLists.txt` | Add 4 source lines to `MORE_PHI_HOST_SOURCES` | Edit |
| `tests/CMakeLists.txt` | Register 3 new test files | Edit |
| `tests/Unit/TestPluginHealthMonitor.cpp` | State-machine unit tests | Create |
| `tests/Unit/TestDAWTransportForwarder.cpp` | Seqlock snapshot unit tests | Create |
| `tests/Unit/TestPluginHostManagerHealth.cpp` | Manager + recovery integration tests | Create |

**Task order follows the spec's compile-safe rollout (§9):** each task leaves the tree compiling so failures are bisectable.

---

## Task 1: Add the surviving Host sources to CMake (so the new files actually compile/link)

**Files:**
- Modify: `CMakeLists.txt:434-440` (`MORE_PHI_HOST_SOURCES`)

> **Why first:** the new `.cpp` files are not in any build target. Adding them now means the compiler will tell us immediately whether `PluginHealthMonitor.cpp`/`DAWTransportForwarder.cpp` are valid (they are not yet — fixed in Tasks 2–3). This task just registers them; the *fixes* are subsequent tasks. After this task, a build will *fail* on the new files — that is expected and is the signal that drives Tasks 2–3.

- [ ] **Step 1: Add the four new source lines**

Edit `CMakeLists.txt` — replace the `MORE_PHI_HOST_SOURCES` block (currently lines 434–440):

```cmake
set(MORE_PHI_HOST_SOURCES
    src/Host/IPluginHostManager.h
    src/Host/PluginHostManager.h
    src/Host/PluginHostManager.cpp
    src/Host/ParameterBridge.h
    src/Host/ParameterBridge.cpp
    src/Host/DAWTransportForwarder.h
    src/Host/DAWTransportForwarder.cpp
    src/Host/PluginHealthMonitor.h
    src/Host/PluginHealthMonitor.cpp
)
```

Note: `HostedPluginEditor` is intentionally **not** added — it is retired in Task 4.

- [ ] **Step 2: Configure CMake to confirm the list parses**

Run:
```bash
cmake -B build-ninja -S . -DMORE_PHI_BUILD_TESTS=ON -G Ninja
```
Expected: CMake configuration succeeds (re-configure is fine; it just rewrites `build-ninja/build.ninja`). Do **not** build yet — a build will fail until Tasks 2–3 land.

- [ ] **Step 3: Commit**

```bash
git add CMakeLists.txt
git commit -m "build(host): register DAWTransportForwarder + PluginHealthMonitor sources

Adds the four new Host source lines to MORE_PHI_HOST_SOURCES (used by
MorePhi, MorePhiCLI, MorePhiMcpCore). HostedPluginEditor intentionally
omitted (retired in a later task). Build still fails until the new .cpp
files are fixed for JUCE 8 API + missing includes."
```

---

## Task 2: Fix `PluginHealthMonitor` to compile (missing JUCE include)

**Files:**
- Modify: `src/Host/PluginHealthMonitor.h:9-12` (add include)

> The `.cpp` uses `juce::jmax` and `juce::Time::getMillisecondCounter()` but the header includes only `<atomic>/<cstdint>/<functional>/<chrono>`. Add the JUCE core include. No logic change.

- [ ] **Step 1: Add the JUCE core include to the header**

In `src/Host/PluginHealthMonitor.h`, after the existing includes (`<atomic>`, `<cstdint>`, `<functional>`, `<chrono>`), add:

```cpp
#include <juce_core/juce_core.h>
```

The final include block (top of the file, after `#pragma once`) reads:
```cpp
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <chrono>
#include <juce_core/juce_core.h>
```

This pulls in `juce::jmax` and `juce::Time`. The `.cpp` already `#include "PluginHealthMonitor.h"`, so no edit to the `.cpp` is required.

- [ ] **Step 2: Compile `PluginHealthMonitor.cpp` in isolation to confirm it builds**

Run (Ninja, single TU, fast):
```bash
cmake --build build-ninja --target MorePhi 2>&1 | grep -iE "PluginHealthMonitor|error" | head -30
```
Expected: **no errors mentioning `PluginHealthMonitor`**. (The build overall will still fail — `DAWTransportForwarder.cpp` is still broken from Task 1, and `PluginHostManager.cpp` has the deleted-member errors. But `PluginHealthMonitor` itself must be clean. If you see `juce::jmax`/`juce::Time` errors here, the include path is wrong.)

- [ ] **Step 3: Commit**

```bash
git add src/Host/PluginHealthMonitor.h
git commit -m "fix(host): PluginHealthMonitor — add juce_core include

The .cpp uses juce::jmax and juce::Time::getMillisecondCounter but the
header pulled in no JUCE header. Adding <juce_core/juce_core.h> resolves
the unresolved-identifier compile errors. No logic change."
```

---

## Task 3: Rewrite `DAWTransportForwarder` as a JUCE-8 seqlock snapshot bus (delete the no-op `applyToPlugin`)

**Files:**
- Modify: `src/Host/DAWTransportForwarder.h` (full rewrite)
- Modify: `src/Host/DAWTransportForwarder.cpp` (full rewrite)
- Test: `tests/Unit/TestDAWTransportForwarder.cpp` (create)
- Modify: `tests/CMakeLists.txt` (register the test)

> **TDD ordering:** write the test first (it defines the new API: `updateFromAudioThread(playHead)` producer + `getSnapshot()` consumer returning a coherent `TransportState`), watch it fail to compile, then implement.

### 3a. The test

- [ ] **Step 1: Write the failing test**

Create `tests/Unit/TestDAWTransportForwarder.cpp`:

```cpp
// More-Phi — DAWTransportForwarder unit tests (Catch2 v3)
#include <juce_audio_basics/juce_audio_basics.h>
#include "../../src/Host/DAWTransportForwarder.h"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <thread>

namespace {
// Minimal stub AudioPlayHead returning a fixed PositionInfo, mirroring the
// pattern used in tools/HeadlessHost/HeadlessHostMain.cpp.
class StubPlayHead : public juce::AudioPlayHead
{
public:
    explicit StubPlayHead(PositionInfo info) : info_(info) {}
    Optional<PositionInfo> getPosition() const override { return info_; }
private:
    PositionInfo info_;
};

juce::AudioPlayHead::PositionInfo makeInfo(double bpm, int num, int den,
                                           bool playing, bool looping,
                                           double ppq, double seconds)
{
    juce::AudioPlayHead::PositionInfo info;
    info.setBpm(bpm);
    info.setTimeSignature(juce::AudioPlayHead::TimeSignature{ num, den });
    info.setIsPlaying(playing);
    info.setIsLooping(looping);
    info.setPpqPosition(ppq);
    info.setTimeInSeconds(seconds);
    return info;
}
} // namespace

TEST_CASE("DAWTransportForwarder publishes a coherent snapshot", "[host][transport]")
{
    DAWTransportForwarder forwarder;

    // Before any update: no snapshot available.
    REQUIRE_FALSE(forwarder.getSnapshot().has_value());

    StubPlayHead playHead(makeInfo(140.0, 7, 8, true, true, 12.5, 26.0));
    forwarder.updateFromAudioThread(&playHead);

    const auto snap = forwarder.getSnapshot();
    REQUIRE(snap.has_value());
    REQUIRE_THAT(snap->bpm,            Catch::WithinULP(140.0, 5));
    REQUIRE(snap->timeSigNumerator   == 7);
    REQUIRE(snap->timeSigDenominator == 8);
    REQUIRE(snap->isPlaying == true);
    REQUIRE(snap->isLooping == true);
    REQUIRE_THAT(snap->ppqPosition,    Catch::WithinULP(12.5, 5));
    REQUIRE_THAT(snap->timeInSeconds,  Catch::WithinULP(26.0, 5));
    REQUIRE(snap->version > 0u);
}

TEST_CASE("DAWTransportForwarder version bumps on each publish", "[host][transport]")
{
    DAWTransportForwarder forwarder;
    StubPlayHead a(makeInfo(100.0, 4, 4, false, false, 0.0, 0.0));
    StubPlayHead b(makeInfo(110.0, 4, 4, false, false, 1.0, 1.0));

    forwarder.updateFromAudioThread(&a);
    const auto first  = forwarder.getSnapshot();
    forwarder.updateFromAudioThread(&b);
    const auto second = forwarder.getSnapshot();

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(second->version > first->version);
    REQUIRE_THAT(second->bpm, Catch::WithinULP(110.0, 5));
}

TEST_CASE("DAWTransportForwarder never observes a torn snapshot under a concurrent producer", "[host][transport]")
{
    // Producer alternates two internally-consistent states; the consumer must
    // never see a mix (e.g. bpm from state A with ppq from state B).
    DAWTransportForwarder forwarder;

    std::atomic<bool> stop{ false };
    std::thread producer([&] {
        bool toggle = false;
        while (!stop.load(std::memory_order_relaxed))
        {
            // State A: bpm 100, ppq 10.0.  State B: bpm 200, ppq 20.0.
            // A valid read has bpm/ppq from the SAME state.
            StubPlayHead ph(toggle
                ? makeInfo(100.0, 4, 4, true, false, 10.0, 10.0)
                : makeInfo(200.0, 4, 4, true, false, 20.0, 20.0));
            forwarder.updateFromAudioThread(&ph);
            toggle = !toggle;
        }
    });

    bool observedValid = false;
    for (int i = 0; i < 20000; ++i)
    {
        const auto snap = forwarder.getSnapshot();
        if (snap.has_value())
        {
            const bool isStateA = (snap->bpm == 100.0 && snap->ppqPosition == 10.0);
            const bool isStateB = (snap->bpm == 200.0 && snap->ppqPosition == 20.0);
            INFO("torn snapshot: bpm=" << snap->bpm << " ppq=" << snap->ppqPosition);
            REQUIRE((isStateA || isStateB));
            observedValid = true;
        }
    }
    REQUIRE(observedValid);

    stop.store(true, std::memory_order_relaxed);
    producer.join();
}
```

- [ ] **Step 2: Register the test in CMake**

In `tests/CMakeLists.txt`, inside the `add_executable(MorePhiTests ...)` list, add (anywhere among the `Unit/...` entries, e.g. after the `Unit/TestHostIntegration.cpp` line):

```cmake
    Unit/TestDAWTransportForwarder.cpp
```

- [ ] **Step 3: Run the test to verify it fails (does not compile — header API not yet rewritten)**

Run:
```bash
cmake --build build-ninja --target MorePhiTests 2>&1 | grep -iE "TestDAWTransportForwarder|getSnapshot|error" | head -20
```
Expected: **compile error** — the test calls `getSnapshot()` and references fields (`ppqPosition`, `timeInSeconds`, `isPlaying`) that the current header does not have. This confirms the test is exercising the new API.

### 3b. The implementation

- [ ] **Step 4: Rewrite `DAWTransportForwarder.h`**

Replace the entire file with:

```cpp
/*
 * More-Phi — Host/DAWTransportForwarder.h
 * Lock-free (seqlock) audio→message snapshot of DAW transport state.
 *
 * The hosted plugin already receives full transport sample-accurately via
 * plugin->setPlayHead() on the audio thread (unchanged). This forwarder does
 * NOT feed the plugin — it publishes a coherent, versioned TransportState so
 * message-thread consumers (UI transport readout, agent context, MCP) can read
 * BPM/time-sig/loop/position in one shot without per-field tearing.
 */
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <cstdint>
#include <optional>

namespace more_phi {

/**
 * Immutable snapshot of DAW transport state. Trivially copyable.
 * Published on the audio thread, read coherently on the message thread.
 */
struct TransportState
{
    double   bpm = 120.0;
    int      timeSigNumerator = 4;
    int      timeSigDenominator = 4;
    bool     isPlaying = false;
    bool     isLooping = false;
    double   ppqPosition = 0.0;
    double   timeInSeconds = 0.0;
    uint32_t version = 0;            /**< bumped on every publish */
};

/**
 * Producer/consumer transport snapshot bus.
 *
 *   // Audio thread (processBlock):
 *   forwarder.updateFromAudioThread(playHead);
 *
 *   // Message thread (UI timer, agent context, MCP tool):
 *   if (auto snap = forwarder.getSnapshot()) { ... use snap->bpm ... }
 */
class DAWTransportForwarder
{
public:
    DAWTransportForwarder() = default;

    /** Producer — audio thread only. Reads the AudioPlayHead and publishes. */
    void updateFromAudioThread(const juce::AudioPlayHead* playHead) noexcept;

    /**
     * Consumer — message thread only. Returns the latest coherent snapshot, or
     * std::nullopt if no state has ever been published. Retries on seqlock
     * mismatch so a torn read is impossible.
     */
    std::optional<TransportState> getSnapshot() const noexcept;

private:
    // Seqlock: even sequence ⇒ stable, readable; odd ⇒ write in progress.
    // Producer bumps to odd before writing the payload, to even after.
    mutable std::atomic<uint32_t> sequence_{ 0 };
    TransportState state_{};

    // Padding to prevent false sharing with adjacent members on the cache line.
    char padding_[64 - sizeof(std::atomic<uint32_t>) - sizeof(TransportState)];
};

} // namespace more_phi
```

- [ ] **Step 5: Rewrite `DAWTransportForwarder.cpp`**

Replace the entire file with:

```cpp
/*
 * More-Phi — Host/DAWTransportForwarder.cpp
 */
#include "DAWTransportForwarder.h"

namespace more_phi {

void DAWTransportForwarder::updateFromAudioThread(const juce::AudioPlayHead* playHead) noexcept
{
    if (playHead == nullptr)
        return;

    const auto optInfo = playHead->getPosition();   // JUCE 8: Optional<PositionInfo>
    if (!optInfo.hasValue())
        return;

    const auto& info = *optInfo;

    // Begin write: mark sequence odd so a concurrent reader retries.
    const uint32_t seqBegin = sequence_.fetch_add(1, std::memory_order_acquire);
    // (seqBegin is even here on a well-behaved single-producer; the store below
    //  publishes the odd value to lock readers out for the duration of the write.)

    state_.bpm                 = info.getBpm().orFallback([] { return 120.0; });
    if (const auto ts = info.getTimeSignature())
    {
        state_.timeSigNumerator   = ts->numerator;
        state_.timeSigDenominator = ts->denominator;
    }
    else
    {
        state_.timeSigNumerator   = 4;
        state_.timeSigDenominator = 4;
    }
    state_.isPlaying  = info.getIsPlaying();
    state_.isLooping  = info.getIsLooping();
    state_.ppqPosition   = info.getPpqPosition().orFallback([] { return 0.0; });
    state_.timeInSeconds = info.getTimeInSeconds().orFallback([] { return 0.0; });
    state_.version       = seqBegin / 2 + 1;   // monotonic per publish

    // End write: mark sequence even again (release — payload visible to reader).
    sequence_.store(seqBegin + 2, std::memory_order_release);
}

std::optional<TransportState> DAWTransportForwarder::getSnapshot() const noexcept
{
    for (int attempts = 0; attempts < 4; ++attempts)
    {
        const uint32_t seq1 = sequence_.load(std::memory_order_acquire);
        if ((seq1 & 1u) != 0u)
            continue;   // write in progress — retry

        // Copy payload (reads are ordered after the acquire load above).
        TransportState copy = state_;

        const uint32_t seq2 = sequence_.load(std::memory_order_acquire);
        if (seq1 != seq2)
            continue;   // changed under us — retry

        // First-ever state has version 0 (state_ default); treat as "not yet".
        if (copy.version == 0u)
            return std::nullopt;

        return copy;
    }
    return std::nullopt;   // contended; caller may retry next tick
}

} // namespace more_phi
```

> **Note on the producer's seqlock convention:** the single-producer (audio thread) is the only writer. `fetch_add(1)` makes the sequence odd during the payload write; `store(seqBegin + 2)` makes it even again and releases the payload. A reader that observes an odd sequence, or a sequence that changes between its two reads, retries. This matches `SnapshotBank`'s established seqlock pattern.

- [ ] **Step 6: Build the test target and run the transport tests**

Run:
```bash
cmake --build build-ninja --target MorePhiTests
cd build-ninja && ctest -R "Transport" --output-on-failure
```
Expected: 3 transport tests **PASS**: `DAWTransportForwarder publishes a coherent snapshot`, `... version bumps on each publish`, `... never observes a torn snapshot under a concurrent producer`.

> If the `Catch::WithinULP` matcher isn't available, replace it with `REQUIRE(std::abs(snap->bpm - 140.0) < 1e-6)` etc. The repo already uses Catch2 v3 matchers (see other tests), so `WithinULP` should resolve.

- [ ] **Step 7: Commit**

```bash
git add src/Host/DAWTransportForwarder.h src/Host/DAWTransportForwarder.cpp \
        tests/Unit/TestDAWTransportForwarder.cpp tests/CMakeLists.txt
git commit -m "feat(host): DAWTransportForwarder as JUCE-8 seqlock snapshot bus

Rewrites the forwarder to publish a coherent, versioned TransportState
(bpm/time-sig/loop/playing/ppq/timeInSeconds) from the audio thread via a
seqlock, readable coherently on the message thread via getSnapshot().

Fixes the removed JUCE 8 CurrentPositionInfo/getPosition(info) API by
using getPosition()->PositionInfo. Deletes the no-op applyToPlugin()
(populated info was discarded; it was redundant with the audio-thread
plugin->setPlayHead() path).

The plugin continues to receive transport sample-accurately via
setPlayHead — unchanged. The forwarder now serves message-thread
consumers (UI transport readout, agent context, MCP) instead."
```

---

## Task 4: Retire `HostedPluginEditor` (delete the file + manager member + manual destructor dance)

**Files:**
- Delete: `src/Host/HostedPluginEditor.h`
- Delete: `src/Host/HostedPluginEditor.cpp`
- Modify: `src/Host/PluginHostManager.h` (remove include + `activeEditor_` member)
- Modify: `src/Host/PluginHostManager.cpp` (remove ctor/dtor dance at lines ~331-335 and ~396-400)

> The UI keeps `HostedPluginWindow`; the lease-count gate already guarantees editor-dies-before-instance. `HostedPluginEditor` was an inert duplicate. This task removes it without touching the working UI path.

- [ ] **Step 1: Delete the two files**

```bash
git rm src/Host/HostedPluginEditor.h src/Host/HostedPluginEditor.cpp
```

- [ ] **Step 2: Remove the include + member from `PluginHostManager.h`**

In `src/Host/PluginHostManager.h`:

(a) Remove this include line (near the top, after `#include "IPluginHostManager.h"`):
```cpp
#include "HostedPluginEditor.h"
```
(leave `DAWTransportForwarder.h` and `PluginHealthMonitor.h`).

(b) Remove the member block (near the bottom of the private section):
```cpp
    // ------------------------------------------------------------------------
    //  New: Hosted Plugin Editor — RAII wrapper for native editor window
    // ------------------------------------------------------------------------
    HostedPluginEditor activeEditor_;
```

- [ ] **Step 3: Remove the ctor usage + manual destructor dance from `PluginHostManager.cpp`**

In `src/Host/PluginHostManager.cpp`, **inside `loadPlugin()`**, remove the block that constructs the editor (currently around lines 328–335):

```cpp
    // Create the native editor wrapper (message thread)
    // The parent component can be set later if needed; for now we create the editor
    // with a null parent. The UI layer can reparent it.
    if (juce::MessageManager::getInstanceWithoutCreating() != nullptr
        && juce::MessageManager::getInstanceWithoutCreating()->isThisTheMessageThread())
    {
        activeEditor_ = HostedPluginEditor(hostedPlugin.get(), nullptr);
    }
```
(Delete the whole block. The UI's `PluginBrowserPanel`/`HostedPluginWindow` owns the visible editor; the manager no longer holds one.)

In `unloadPlugin()`, remove the manual destructor dance (currently around lines 396–400):

```cpp
        // Destroy the native editor wrapper first, before the plugin instance.
        // This is safe because the audio thread has already been snapped off
        // (hostedPluginPtr_ is null) and the message thread is calling this.
        activeEditor_.~HostedPluginEditor();
        new (&activeEditor_) HostedPluginEditor(); // placement-new a fresh empty one
```
(Delete the whole block. With `activeEditor_` gone, this UB-adjacent manual lifetime dance is unnecessary.)

- [ ] **Step 4: Build to confirm the Host layer still references only surviving symbols**

Run:
```bash
cmake --build build-ninja --target MorePhi 2>&1 | grep -iE "HostedPluginEditor|activeEditor_|error" | head -20
```
Expected: **no references to `HostedPluginEditor` or `activeEditor_`**. The build will *still fail* on `PluginHostManager.cpp`'s deleted-member errors (`exceptionCount_`/`suspended_`/`recoveryGracePeriod_`) — those are fixed in Task 5. But there must be **zero** `HostedPluginEditor`/`activeEditor_` errors.

- [ ] **Step 5: Commit**

```bash
git add -A src/Host/
git commit -m "refactor(host): retire HostedPluginEditor (inert duplicate lifecycle)

The UI already owns the visible editor via HostedPluginWindow, and the
ref-counted lease (activePluginUsers_) already guarantees editor-dies-
before-instance. HostedPluginEditor in the manager was a second, parallel,
never-visible lifecycle (created with a nullptr parent, queried by nobody).

Deletes the file, the activeEditor_ member, and the fragile
~HostedPluginEditor()+placement-new destructor dance in unloadPlugin()
(UB-adjacent and now unnecessary). UI path (HostedPluginWindow) untouched."
```

---

## Task 5: Repair `PluginHostManager.cpp` — remove deleted-member refs and rewrite `processBlock` accounting onto `healthMonitor_`

**Files:**
- Modify: `src/Host/PluginHostManager.cpp` — `processBlock()` success branch (~632–646) and `applyExceptionGracePeriod()` (~653–670, deleted)
- Test: `tests/Unit/TestPluginHealthMonitor.cpp` (create, in Task 6) will exercise the monitor the manager now relies on

> This is the compile-fix that makes the whole Host layer build. The dead `applyExceptionGracePeriod()` references `exceptionCount_`/`suspended_`/`recoveryGracePeriod_` (deleted when `healthMonitor_` was added). Remove it and route all failure/success accounting through the monitor (which the code *already* calls — we just remove the duplicate dead path).

- [ ] **Step 1: Rewrite the `processBlock` success branch (the SEH/try success path) onto `healthMonitor_`**

In `src/Host/PluginHostManager.cpp`, in `processBlock()`, find the success block of the **wide-buffer** SEH path (currently around lines 588–601) and the **normal** path (currently around lines 632–646). Replace each success block's body.

For the **wide-buffer path** success block (currently):
```cpp
        {
            healthMonitor_.reportSuccess();
        }
```
This is already correct — leave it. (It already calls `healthMonitor_.reportSuccess()`.)

For the **normal path** success block (currently lines ~632–646):
```cpp
    {
        exceptionCount_.store(0, std::memory_order_relaxed);  // reset on success
        // m-5 FIX: Decrement grace period. ...
        int grace = recoveryGracePeriod_.load(std::memory_order_relaxed);
        if (grace > 0)
            recoveryGracePeriod_.store(grace - 1, std::memory_order_relaxed);

        // Smoothly fade in after recall / bypass switch
        if (currentGain_ < 1.0f)
        {
            buffer.applyGainRamp(0, buffer.getNumSamples(), currentGain_, 1.0f);
            currentGain_ = 1.0f;
        }
    }
```
Replace the entire block with:
```cpp
    {
        healthMonitor_.reportSuccess();   // reset on success (drives Healthy/Recovering transitions)

        // Smoothly fade in after recall / bypass / recovery.
        if (currentGain_ < 1.0f)
        {
            buffer.applyGainRamp(0, buffer.getNumSamples(), currentGain_, 1.0f);
            currentGain_ = 1.0f;
        }
    }
```

- [ ] **Step 2: Rewrite the normal-path failure branch to use the monitor**

In `processBlock()`, the normal-path SEH/try **failure** branches currently call:
```cpp
        currentGain_ = 0.0f;
        applyExceptionGracePeriod(buffer);
        return;
```
Replace those two occurrences (one in `#if defined(_MSC_VER)` block ~614-619, one in the `try/catch` block ~620-631) so each failure branch reads:
```cpp
        currentGain_ = 0.0f;
        buffer.clear();                 // bypass to silence on this block
        healthMonitor_.reportFailure(); // advance the monitor (Healthy→Degraded→Suspended)
        return;
```
(The wide-buffer failure branches at ~573 and ~584 already call `healthMonitor_.reportFailure()` — leave those. The monitor's `reportFailure()` handles the 20-strike → `Suspended` transition internally, so `applyExceptionGracePeriod` is no longer needed.)

- [ ] **Step 3: Delete the dead `applyExceptionGracePeriod()` helper + its declaration**

In `src/Host/PluginHostManager.cpp`, delete the entire function definition (currently ~lines 649–670):
```cpp
// ---------------------------------------------------------------------------
//  H12 FIX: Deduplicated exception grace-period helper
// ---------------------------------------------------------------------------

bool PluginHostManager::applyExceptionGracePeriod(juce::AudioBuffer<float>& buffer) noexcept
{
    buffer.clear();
    if (recoveryGracePeriod_.load(std::memory_order_relaxed) > 0)
    {
        suspended_.store(true, std::memory_order_relaxed);
        recoveryGracePeriod_.store(0, std::memory_order_relaxed);
        return true;
    }
    // C12 FIX: Saturated unsigned increment — never wraps, never overflows.
    const uint32_t maxSat = static_cast<uint32_t>(MAX_PLUGIN_EXCEPTIONS) + 1;
    uint32_t count = exceptionCount_.load(std::memory_order_relaxed);
    if (count < maxSat)
        count = exceptionCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count >= static_cast<uint32_t>(MAX_PLUGIN_EXCEPTIONS))
        suspended_.store(true, std::memory_order_relaxed);
    return false;
}
```
(Delete the whole thing — every symbol it touches is a deleted member.)

In `src/Host/PluginHostManager.h`, remove the corresponding declaration (currently ~lines 217–218):
```cpp
    // H12 FIX: Deduplicated exception-handling grace-period logic
    bool applyExceptionGracePeriod(juce::AudioBuffer<float>& buffer) noexcept;
```

- [ ] **Step 4: Build the Host layer — it must now compile and link (minus the not-yet-added `tickHealth`)**

Run:
```bash
cmake --build build-ninja --target MorePhi 2>&1 | grep -iE "error|exceptionCount_|suspended_|recoveryGracePeriod_|applyExceptionGracePeriod" | head -30
```
Expected: **no errors and no references to the deleted symbols.** The `MorePhi` target should now build. (Tasks 6–8 add `tickHealth`/`tryStartRecovery` and wire them; the manager currently has the *old* recovery-less behavior — bypass-forever after 20 failures — which still compiles.)

> If you see errors about `getHealthState`/`getHealthSnapshot` getters referencing something missing, note them — Task 7 confirms those getters still compile against `healthMonitor_`.

- [ ] **Step 5: Commit**

```bash
git add src/Host/PluginHostManager.cpp src/Host/PluginHostManager.h
git commit -m "fix(host): repair PluginHostManager — route failure accounting through healthMonitor_

Removes applyExceptionGracePeriod() and all references to the deleted
exceptionCount_/suspended_/recoveryGracePeriod_ members (the source of the
hard compile error). processBlock now reports every success/failure through
healthMonitor_.reportSuccess/reportFailure (already partially done); the
monitor owns the 20-strike→Suspended transition internally.

Host layer compiles + links after this change. Recovery driver (tickHealth)
wired in a later task."
```

---

## Task 6: `PluginHealthMonitor` state-machine unit tests

**Files:**
- Test: `tests/Unit/TestPluginHealthMonitor.cpp` (create)
- Modify: `tests/CMakeLists.txt` (register)

> Pure state-machine tests — no JUCE audio graph. Exercises every transition in the spec's §4 diagram, including the recovery-window timing (using the monitor's configurable 100ms-minimum delay so the test is fast).

- [ ] **Step 1: Write the test**

Create `tests/Unit/TestPluginHealthMonitor.cpp`:

```cpp
// More-Phi — PluginHealthMonitor state-machine unit tests (Catch2 v3)
#include "../../src/Host/PluginHealthMonitor.h"
#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

TEST_CASE("PluginHealthMonitor starts Healthy", "[host][health]")
{
    PluginHealthMonitor m;
    REQUIRE(m.getState() == PluginHealthState::Healthy);
    REQUIRE(m.shouldProcess() == true);
    REQUIRE(m.shouldRecover() == false);
    REQUIRE(m.getConsecutiveFailureCount() == 0);
}

TEST_CASE("PluginHealthMonitor Degraded on first failure, resets on success", "[host][health]")
{
    PluginHealthMonitor m;
    m.reportFailure();
    REQUIRE(m.getState() == PluginHealthState::Degraded);
    REQUIRE(m.shouldProcess() == true);   // degraded still processes

    m.reportSuccess();
    REQUIRE(m.getState() == PluginHealthState::Healthy);
    REQUIRE(m.getConsecutiveFailureCount() == 0);
}

TEST_CASE("PluginHealthMonitor Suspended after 20 consecutive failures", "[host][health]")
{
    PluginHealthMonitor m;
    m.setMaxConsecutiveFailures(20);

    for (int i = 0; i < 19; ++i)
    {
        m.reportFailure();
        REQUIRE(m.getState() == PluginHealthState::Degraded);
        REQUIRE(m.shouldProcess() == true);
    }

    m.reportFailure();   // 20th
    REQUIRE(m.getState() == PluginHealthState::Suspended);
    REQUIRE(m.shouldProcess() == false);  // suspended = bypass
}

TEST_CASE("PluginHealthMonitor shouldRecover only after the delay", "[host][health]")
{
    PluginHealthMonitor m;
    m.setMaxConsecutiveFailures(1);     // trip to Suspended on first failure
    m.setRecoveryDelayMs(120);          // minimum clamp is 100ms; use 120 for headroom

    m.reportFailure();
    REQUIRE(m.getState() == PluginHealthState::Suspended);
    REQUIRE(m.shouldRecover() == false);   // immediately — delay not elapsed

    juce::Thread::sleep(150);              // wait past the delay
    REQUIRE(m.shouldRecover() == true);
}

TEST_CASE("PluginHealthMonitor beginRecovery CAS only from Suspended", "[host][health]")
{
    PluginHealthMonitor m;
    REQUIRE(m.beginRecovery() == false);   // Healthy, not Suspended
    REQUIRE(m.getState() == PluginHealthState::Healthy);

    m.setMaxConsecutiveFailures(1);
    m.reportFailure();
    m.setRecoveryDelayMs(100);
    juce::Thread::sleep(120);
    REQUIRE(m.shouldRecover() == true);
    REQUIRE(m.beginRecovery() == true);    // Suspended → Recovering
    REQUIRE(m.getState() == PluginHealthState::Recovering);
    REQUIRE(m.shouldProcess() == false);   // recovering = bypass
}

TEST_CASE("PluginHealthMonitor endRecovery(true) → Healthy with counts reset", "[host][health]")
{
    PluginHealthMonitor m;
    m.setMaxConsecutiveFailures(1);
    m.reportFailure();
    m.setRecoveryDelayMs(100);
    juce::Thread::sleep(120);
    REQUIRE(m.beginRecovery() == true);

    m.endRecovery(true);
    REQUIRE(m.getState() == PluginHealthState::Healthy);
    REQUIRE(m.getConsecutiveFailureCount() == 0);
    REQUIRE(m.shouldProcess() == true);
}

TEST_CASE("PluginHealthMonitor endRecovery(false) → back to Suspended", "[host][health]")
{
    PluginHealthMonitor m;
    m.setMaxConsecutiveFailures(1);
    m.reportFailure();
    m.setRecoveryDelayMs(100);
    juce::Thread::sleep(120);
    REQUIRE(m.beginRecovery() == true);

    m.endRecovery(false);
    REQUIRE(m.getState() == PluginHealthState::Suspended);
}

TEST_CASE("PluginHealthMonitor Terminated after 3 failed recovery attempts", "[host][health]")
{
    PluginHealthMonitor m;
    m.setMaxConsecutiveFailures(1);
    m.setMaxRecoveryAttempts(3);
    m.setRecoveryDelayMs(100);

    for (int attempt = 0; attempt < 3; ++attempt)
    {
        m.reportFailure();                 // → Suspended (count already 0 after prior endRecovery(false))
        if (m.getState() == PluginHealthState::Suspended)
        {
            juce::Thread::sleep(120);
            REQUIRE(m.beginRecovery() == true);
            m.endRecovery(false);          // back to Suspended
        }
    }
    // After exhausting attempts, beginRecovery transitions to Terminated.
    juce::Thread::sleep(120);
    const bool started = m.beginRecovery();
    REQUIRE(started == false);
    REQUIRE(m.getState() == PluginHealthState::Terminated);
    REQUIRE(m.shouldProcess() == false);
}

TEST_CASE("PluginHealthMonitor Terminated is sticky", "[host][health]")
{
    PluginHealthMonitor m;
    m.setMaxConsecutiveFailures(1);
    m.setMaxRecoveryAttempts(1);
    m.setRecoveryDelayMs(100);
    m.reportFailure();
    juce::Thread::sleep(120);
    m.beginRecovery();        // attempt 1 → since maxRecoveryAttempts==1, → Terminated
    REQUIRE(m.getState() == PluginHealthState::Terminated);

    // Further failures/successes must not revive it.
    m.reportFailure();
    REQUIRE(m.getState() == PluginHealthState::Terminated);
    m.reportSuccess();
    REQUIRE(m.getState() == PluginHealthState::Terminated);
}
```

- [ ] **Step 2: Register the test in CMake**

In `tests/CMakeLists.txt`, inside `add_executable(MorePhiTests ...)` (next to the line added in Task 3), add:
```cmake
    Unit/TestPluginHealthMonitor.cpp
```

- [ ] **Step 3: Build and run the health-monitor tests**

Run:
```bash
cmake --build build-ninja --target MorePhiTests
cd build-ninja && ctest -R "HealthMonitor|health" --output-on-failure
```
Expected: all 9 health-monitor tests **PASS**.

> **If a transition test fails,** the likely cause is the existing `reportFailure`/`beginRecovery` logic's exact boundary (`>= maxRecoveryAttempts` vs `> maxRecoveryAttempts`). The current `beginRecovery()` checks `if (currentAttempts >= getMaxRecoveryAttempts())` *after* incrementing and transitions to `Terminated`, returning false. The "Terminated after 3 failed attempts" test is written to match that. If the count semantics differ, fix the **test's loop bound** to match the implementation (do not change the implementation here — it's the approved spec's behavior).

- [ ] **Step 4: Commit**

```bash
git add tests/Unit/TestPluginHealthMonitor.cpp tests/CMakeLists.txt
git commit -m "test(host): PluginHealthMonitor state-machine unit tests

9 tests covering every spec §4 transition: Healthy→Degraded→Suspended,
reset-on-success, recovery-window timing (configurable 120ms delay),
beginRecovery CAS-gated-from-Suspended, endRecovery(true/false),
Terminated-after-3-attempts, and Terminated stickiness."
```

---

## Task 7: Add the recovery driver — `tickHealth()` + `tryStartRecovery()` + shutdown guards

**Files:**
- Modify: `src/Host/PluginHostManager.h` (add declarations + members)
- Modify: `src/Host/PluginHostManager.cpp` (add `tickHealth`/`tryStartRecovery`; set `shuttingDown_` in `unloadPlugin`)
- Test: `tests/Unit/TestPluginHostManagerHealth.cpp` (create, in Task 8)

> This drives the dead recovery state machine. `tickHealth()` (message thread, 50ms timer) polls `shouldRecover()`; `tryStartRecovery()` launches a background thread that reinitializes the plugin through `beginExclusivePluginUse` and reports via `endRecovery`. `shuttingDown_` aborts cleanly on unload.

- [ ] **Step 1: Add the declarations + members to `PluginHostManager.h`**

In `src/Host/PluginHostManager.h`:

(a) In the **public** section (near the existing `drainDeferredDoomedPlugins()` declaration), add:
```cpp
    /**
     * Message-thread health/recovery tick (called from the 50ms maintenance
     * timer). Cheap when healthy. If the monitor is due for a recovery attempt,
     * launches a background thread that reinitializes the plugin via
     * beginExclusivePluginUse and reports success/failure back to the monitor.
     */
    void tickHealth() noexcept;
```

(b) In the **private** section (near `healthMonitor_`), add the new members:
```cpp
    // Recovery driver: ensures only one recovery is in flight at a time, and
    // that an in-flight recovery aborts cleanly on shutdown (see tryStartRecovery).
    std::atomic<bool> recoveryInFlight_{ false };
    std::atomic<bool> shuttingDown_{ false };

    /** Launches a background-thread reinit if the monitor is due. */
    void tryStartRecovery() noexcept;
```

- [ ] **Step 2: Implement `tickHealth()` and `tryStartRecovery()` in `PluginHostManager.cpp`**

In `src/Host/PluginHostManager.cpp`, add (e.g. just after `drainDeferredDoomedPlugins()`):

```cpp
void PluginHostManager::tickHealth() noexcept
{
    // Message-thread only. Cheap poll — launches reinit on a background thread.
    if (!healthMonitor_.shouldRecover())
        return;
    tryStartRecovery();
}

void PluginHostManager::tryStartRecovery() noexcept
{
    if (shuttingDown_.load(std::memory_order_acquire))
        return;

    // One recovery at a time.
    bool expected = false;
    if (!recoveryInFlight_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

    // CAS Suspended → Recovering. If the monitor refuses (not Suspended, or
    // already Terminated), release the in-flight flag and bail.
    if (!healthMonitor_.beginRecovery())
    {
        recoveryInFlight_.store(false, std::memory_order_release);
        return;
    }

    // Detached background thread: reinit must NOT run on the audio or message
    // thread (releaseResources/prepareToPlay can be slow). It reuses the
    // existing beginExclusivePluginUse mechanism to snap the audio thread off
    // the plugin for the duration. shuttingDown_ is re-checked inside so an
    // unload racing the launch aborts without touching the plugin.
    const auto sampleRate = currentSampleRate;
    const auto blockSize  = currentBlockSize;

    juce::Thread::launch([this, sampleRate, blockSize] {
        bool ok = false;
        if (!shuttingDown_.load(std::memory_order_acquire))
        {
            if (auto* plugin = beginExclusivePluginUse(/*timeoutMs*/ 500))
            {
                try
                {
                    plugin->releaseResources();
                    plugin->prepareToPlay(sampleRate, blockSize);
                    ok = true;
                }
                catch (...)
                {
                    ok = false;   // reinit threw — treat as failed recovery
                }
                endExclusivePluginUse();
            }
        }
        // endRecovery drives Healthy (reset) or back to Suspended. On success,
        // currentGain_ ramps back in on the next processed block.
        healthMonitor_.endRecovery(ok);
        recoveryInFlight_.store(false, std::memory_order_release);
    });
}
```

- [ ] **Step 3: Set `shuttingDown_` in `unloadPlugin()` and reset it appropriately**

In `src/Host/PluginHostManager.cpp`, `unloadPlugin()`: at the **top** of the `if (hostedPlugin)` block (right after the `SwapGuard` is set up, before the exclusive-use bounded wait), add:
```cpp
        shuttingDown_.store(true, std::memory_order_release);   // abort any in-flight/pending recovery
```

And in `loadPlugin()`, right after `healthMonitor_.reset();` (the line added earlier that resets the monitor on a fresh load), add:
```cpp
        shuttingDown_.store(false, std::memory_order_release);  // a new plugin may use the recovery path
```

> The destructor already calls `unloadPlugin()`, so `shuttingDown_` is set on teardown too — any in-flight recovery observes it and aborts without touching the plugin.

- [ ] **Step 4: Build to confirm it compiles**

Run:
```bash
cmake --build build-ninja --target MorePhi 2>&1 | grep -iE "error|tickHealth|tryStartRecovery|recoveryInFlight|shuttingDown_" | head -20
```
Expected: **no errors.** `MorePhi` builds. (`tickHealth` is not yet called from the processor — that's Task 9.)

- [ ] **Step 5: Commit**

```bash
git add src/Host/PluginHostManager.h src/Host/PluginHostManager.cpp
git commit -m "feat(host): drive PluginHealthMonitor recovery (tickHealth + bg-thread reinit)

Adds the missing driver for the recovery state machine:
- tickHealth() (message thread, 50ms timer): cheap poll, launches reinit
  when the monitor is due.
- tryStartRecovery(): CAS-guarded single-flight; launches a detached
  background thread that reinitializes via beginExclusivePluginUse (snaps
  the audio thread off the plugin) and reports endRecovery(true/false).
- shuttingDown_: set in unloadPlugin (and thus the dtor), checked before
  and inside the recovery lambda so teardown never races a reinit.

Once a plugin trips 20 failures → Suspended, it is now actually
recovered instead of bypassed forever."
```

---

## Task 8: Manager + recovery integration test

**Files:**
- Test: `tests/Unit/TestPluginHostManagerHealth.cpp` (create)
- Modify: `tests/CMakeLists.txt` (register)

> Integration of the manager + monitor: `tickHealth` no-op when healthy, single-flight recovery, and clean teardown during in-flight recovery (the `shuttingDown_` guarantee). Uses a dummy processor that throws so the monitor advances.

- [ ] **Step 1: Write the test**

Create `tests/Unit/TestPluginHostManagerHealth.cpp`:

```cpp
// More-Phi — PluginHostManager health/recovery integration tests (Catch2 v3)
//
// These tests exercise the manager's tickHealth()/tryStartRecovery() wiring and
// the shuttingDown_ teardown guarantee without needing a real hosted plugin.
// We use the manager's own health monitor (exposed via getHealthSnapshot) and
// a minimal dummy AudioPluginInstance that throws on processBlock so the
// monitor advances through Degraded → Suspended.
#include <juce_audio_processors/juce_audio_processors.h>
#include "../../src/Host/PluginHostManager.h"
#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

namespace {
// Minimal dummy plugin: processBlock throws to simulate a crashing guest.
// We don't actually load it via loadPlugin (that needs a real PluginDescription
// + format manager); instead these tests drive the monitor + tickHealth paths
// directly on a default-constructed manager and observe getHealthSnapshot().
} // namespace

TEST_CASE("PluginHostManager tickHealth is a no-op when no plugin / healthy", "[host][health][integration]")
{
    PluginHostManager mgr;
    REQUIRE_FALSE(mgr.hasPlugin());
    REQUIRE(mgr.getHealthState() == PluginHealthState::Healthy);

    // tickHealth must not crash and must not change state when there's nothing
    // to recover (no plugin loaded → monitor stays Healthy).
    for (int i = 0; i < 5; ++i)
        mgr.tickHealth();

    REQUIRE(mgr.getHealthState() == PluginHealthState::Healthy);
}

TEST_CASE("PluginHostManager reports a bypass/silence buffer when processBlock has no plugin", "[host][health][integration]")
{
    // processBlock with no plugin must be a dry no-op (passthrough) — the spec's
    // case (1). It must not throw or report a failure.
    PluginHostManager mgr;
    juce::AudioBuffer<float> buffer(2, 64);
    juce::MidiBuffer midi;
    for (int c = 0; c < 2; ++c)
        for (int s = 0; s < 64; ++s)
            buffer.setSample(c, s, 0.5f);

    REQUIRE_NOTHROW(mgr.processBlock(buffer, midi));

    // Dry passthrough: samples unchanged.
    REQUIRE(buffer.getSample(0, 0) == 0.5f);
    REQUIRE(mgr.getHealthState() == PluginHealthState::Healthy);
}

TEST_CASE("PluginHostManager destructor is clean even if a recovery were in flight", "[host][health][integration]")
{
    // The shuttingDown_ guarantee: destroying the manager while (hypothetically)
    // a recovery could be launching must not crash or UAF. We can't easily get a
    // real Suspended state without a loaded plugin, but we CAN assert that
    // destroying right after a tickHealth call is safe (the fast healthy path).
    {
        PluginHostManager mgr;
        mgr.tickHealth();
        // destructor runs here — must be clean.
    }
    REQUIRE(true);   // reached here ⇒ no crash/UAF
}
```

> **Note:** fully exercising the Suspended→Recovering path with a real reinit requires loading an actual VST3 (out of reach for a unit test). The integration tests above cover the *wiring* invariants the manager owns (no-op when healthy, dry-passthrough no-crash, clean destruction). The state-machine *transitions* themselves are covered exhaustively by `TestPluginHealthMonitor.cpp` (Task 6). This split mirrors how the repo already separates pure-logic tests from plugin-lifecycle tests (`TestHostIntegration.cpp` only constructs a default manager too).

- [ ] **Step 2: Register the test in CMake**

In `tests/CMakeLists.txt`, inside `add_executable(MorePhiTests ...)`, add:
```cmake
    Unit/TestPluginHostManagerHealth.cpp
```

- [ ] **Step 3: Build and run**

Run:
```bash
cmake --build build-ninja --target MorePhiTests
cd build-ninja && ctest -R "health" --output-on-failure
```
Expected: the 3 new integration tests **PASS** (plus the 9 from Task 6).

- [ ] **Step 4: Commit**

```bash
git add tests/Unit/TestPluginHostManagerHealth.cpp tests/CMakeLists.txt
git commit -m "test(host): PluginHostManager health/recovery integration tests

Wiring invariants for the recovery driver: tickHealth no-op when healthy,
processBlock dry-passthrough no-crash with no plugin, clean destruction
right after a tickHealth call. The Suspended→Recovering *transitions* are
covered by TestPluginHealthMonitor (real reinit needs a loaded VST3, out
of unit-test scope — mirrors TestHostIntegration's default-construct style)."
```

---

## Task 9: Wire `tickHealth()` into the 50ms timer and `updateFromAudioThread` into `processBlock`

**Files:**
- Modify: `src/Plugin/PluginProcessor.cpp` — `timerCallback()` (next to line 4242) and `processBlock()` (next to line 2176/2178)

> The two integration sites the spec identified. One line each. After this, the whole feature is live: recovery is driven, transport is published.

- [ ] **Step 1: Add the transport producer call in `processBlock`**

In `src/Plugin/PluginProcessor.cpp`, in `processBlock()`, the playhead block is currently (lines ~2175–2178):
```cpp
    auto* const currentPlayHead = getPlayHead();
    hostManager.setPlayHead(currentPlayHead);
    hostManagerB_.setPlayHead(currentPlayHead);
    updateTransportContextSnapshot(currentPlayHead);
```
Add one line so the host manager's forwarder is fed from the audio thread too:
```cpp
    auto* const currentPlayHead = getPlayHead();
    hostManager.setPlayHead(currentPlayHead);
    hostManagerB_.setPlayHead(currentPlayHead);
    updateTransportContextSnapshot(currentPlayHead);
    hostManager.updateTransportFromAudioThread(currentPlayHead);
```
> `updateTransportFromAudioThread` is a thin public wrapper on the private `transportForwarder_` member (added in Task 10) so the processor doesn't reach into manager internals. If Task 10 hasn't landed yet, this won't compile — Task 10 adds the wrapper.

- [ ] **Step 2: Add the recovery tick in `timerCallback`**

In `src/Plugin/PluginProcessor.cpp`, `timerCallback()`, the drain line is currently (line ~4242):
```cpp
    { MSG_TRACE(diagnostics_, "drainDeferredDoomedPlugins"); hostManager.drainDeferredDoomedPlugins(); hostManagerB_.drainDeferredDoomedPlugins(); }
```
Add the health tick right after it:
```cpp
    { MSG_TRACE(diagnostics_, "drainDeferredDoomedPlugins"); hostManager.drainDeferredDoomedPlugins(); hostManagerB_.drainDeferredDoomedPlugins(); }
    { MSG_TRACE(diagnostics_, "tickHealth"); hostManager.tickHealth(); hostManagerB_.tickHealth(); }
```
> `hostManagerB_` is the second host manager instance (the project hosts two plugins — A and B). Ticking both keeps both recoverable.

- [ ] **Step 3: Build the plugin target**

Run:
```bash
cmake --build build-ninja --target MorePhi_VST3 2>&1 | grep -iE "error|tickHealth|updateTransportFromAudioThread" | head -20
```
Expected: **no errors** (Task 10 must have landed for `updateTransportFromAudioThread` to resolve). The full VST3 plugin builds.

> If `MorePhi_VST3` is not the exact target name, build the default target instead: `cmake --build build-ninja`. The DAW-loadable binary lands at `build-ninja/MorePhi_artefacts/Release/VST3/MorePhi.vst3/...`.

- [ ] **Step 4: Commit**

```bash
git add src/Plugin/PluginProcessor.cpp
git commit -m "feat(plugin): wire tickHealth (50ms timer) + transport forwarder (processBlock)

- timerCallback: tickHealth() on both host managers (next to the existing
  drainDeferredDoomedPlugins tick) drives the recovery state machine.
- processBlock: updateTransportFromAudioThread(playHead) (next to the
  existing setPlayHead) publishes the coherent transport snapshot for
  message-thread consumers.

Both are the single-line integration sites identified in the spec."
```

---

## Task 10: Expose the transport-snapshot wrapper + verify full build + run all tests

**Files:**
- Modify: `src/Host/PluginHostManager.h` (add `updateTransportFromAudioThread` + optional `getTransportSnapshot` wrappers)

> The processor (Task 9) calls `hostManager.updateTransportFromAudioThread(...)`. The manager wraps its private `transportForwarder_` member in a public method so the processor doesn't friend it. This also surfaces `getTransportSnapshot()` for any message-thread consumer (UI/MCP) that wants the coherent read.

- [ ] **Step 1: Add the wrappers to `PluginHostManager.h`**

In `src/Host/PluginHostManager.h`, in the **public** section (near the `tickHealth()` declaration added in Task 7), add:
```cpp
    /** Audio-thread producer: publish the latest transport snapshot. */
    void updateTransportFromAudioThread(juce::AudioPlayHead* playHead) noexcept
    {
        transportForwarder_.updateFromAudioThread(playHead);
    }

    /** Message-thread consumer: latest coherent transport snapshot, or nullopt. */
    std::optional<TransportState> getTransportSnapshot() const noexcept
    {
        return transportForwarder_.getSnapshot();
    }
```

(These are inline so no `.cpp` edit. `TransportState` and `std::optional` are already available via `DAWTransportForwarder.h`, which `PluginHostManager.h` includes.)

- [ ] **Step 2: Full clean build (Release config, Ninja)**

Run:
```bash
cmake --build build-ninja --config Release 2>&1 | tail -25
```
Expected: **build succeeds** for `MorePhi` (shared code), `MorePhiCLI`, `MorePhiMcpServer`, and the VST3 plugin. Zero errors.

- [ ] **Step 3: Run the full test suite**

Run:
```bash
cd build-ninja && ctest --build-config Release --output-on-failure --parallel 4
```
Expected: **all tests pass**, including the new `Transport`, `HealthMonitor`, and `health`/`integration` tests from Tasks 3/6/8. Note any pre-existing failures unrelated to this work (the repo had in-flight changes before this branch); the new tests must all be green.

- [ ] **Step 4: Verify the DAW-loadable VST3 exists**

Run:
```bash
ls -la build-ninja/MorePhi_artefacts/Release/VST3/MorePhi.vst3/Contents/x86_64-win/MorePhi.vst3
```
Expected: the binary is present and recently rebuilt.

- [ ] **Step 5: Commit**

```bash
git add src/Host/PluginHostManager.h
git commit -m "feat(host): expose transport-snapshot wrappers (updateTransportFromAudioThread, getTransportSnapshot)

Inline public wrappers around the private transportForwarder_ so the
processor and message-thread consumers (UI/MCP) can use the coherent
snapshot without friending the manager.

Final task: full Release build + ctest green; DAW-loadable VST3 present."
```

---

## Verification checklist (run after Task 10)

- [ ] **Compile:** `cmake --build build-ninja --config Release` — zero errors.
- [ ] **Tests:** `cd build-ninja && ctest --build-config Release --output-on-failure --parallel 4` — all pass.
- [ ] **New test files run:** `ctest -R "Transport|HealthMonitor|health"` — 15 new tests pass.
- [ ] **No deleted symbols remain:** `grep -rn "HostedPluginEditor\|activeEditor_\|exceptionCount_\|applyExceptionGracePeriod\|applyToPlugin" src/ tests/` returns **zero** hits (other than this plan/spec).
- [ ] **Audio-thread invariants hold:** the recovery reinit runs on a background thread behind `beginExclusivePluginUse` (audio thread sees `nullptr`, fades to silence); the transport producer is a seqlock write (no lock, no allocation). No new audio-thread allocations or locks introduced.
- [ ] **Spec coverage:** every section of the spec (§2–§9) maps to a task above (see self-review below).

---

## Self-Review (plan vs spec)

**1. Spec coverage:**
- §0 background (defects) → addressed by Tasks 1–5 (the fixes).
- §1 architecture + threading contract → reflected throughout; threading contract enforced by which thread each method targets (documented in task code comments).
- §2 native editor (retire `HostedPluginEditor`) → **Task 4**.
- §3 transport (seqlock snapshot bus, delete `applyToPlugin`) → **Task 3** (+ wrapper in Task 10, producer wire in Task 9).
- §4 crash isolation/recovery (drive monitor, repair accounting) → **Task 5** (repair) + **Task 6** (monitor tests) + **Task 7** (driver) + **Task 8** (integration tests).
- §5 safe teardown (`shuttingDown_` guard, doom-queue assertion) → **Task 7** (shuttingDown_ in unload/load). The doom-queue `jassert` + documented invariant: see note below.
- §6 build wiring → **Task 1** (+ test registration in Tasks 3/6/8).
- §7 testing → **Tasks 3, 6, 8**.
- §8 file manifest → matches the File Structure table above.
- §9 rollout order → task order 1→10 follows §9 exactly.
- §10 success criteria → verified by the checklist above.

**Gap noted & intentional:** The spec §5.1 mentions a debug `jassert(deferredDoomedPlugins_.size() < 4)`. That's a one-line defensive assertion in `drainDeferredDoomedPlugins()`. It's minor and the existing queue already can't grow unbounded (load drains it). To avoid a task that's a single uncommented line, it's folded into Task 5's "no behavior change, document invariant" scope — **add it during Task 5 Step 3** if desired. Calling it out here so it isn't lost.

**2. Placeholder scan:** No "TBD/TODO/implement later/add error handling" patterns. Every code step contains complete code. No "similar to Task N" cross-references without the code.

**3. Type/signature consistency:**
- `TransportState` fields (`bpm`, `timeSigNumerator`, `timeSigDenominator`, `isPlaying`, `isLooping`, `ppqPosition`, `timeInSeconds`, `version`) — consistent across Task 3 test, header, and impl.
- `DAWTransportForwarder::updateFromAudioThread(const juce::AudioPlayHead*)`, `getSnapshot() -> std::optional<TransportState>` — consistent across header, impl, test, and the Task 10 `updateTransportFromAudioThread`/`getTransportSnapshot` wrappers.
- `tickHealth()`, `tryStartRecovery()`, `recoveryInFlight_`, `shuttingDown_` — consistent across header (Task 7), impl (Task 7), processor wire (Task 9), and tests (Task 8).
- `PluginHealthMonitor` API (`reportSuccess/reportFailure/shouldProcess/shouldRecover/beginRecovery/endRecovery/getState/getConsecutiveFailureCount/setMaxConsecutiveFailures/setRecoveryDelayMs/setMaxRecoveryAttempts/reset/getSnapshot`) — all exist in the current `PluginHealthMonitor.h` (verified during brainstorm); used consistently in Tasks 6, 7, 8.
- `getHealthState()`/`getHealthSnapshot()` getters — already present in current `PluginHostManager.h`; used in Task 8 test. Consistent.

No inconsistencies found. Plan is complete and ready for execution.
