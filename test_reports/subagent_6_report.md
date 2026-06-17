# Sub-Agent 6 Report: Production Readiness & Code Quality Review
## More-Phi VST3 Plugin v3.3.0

**Date:** 2026-06-15  
**Scope:** Build configuration, performance, test coverage, and code quality  
**Files Audited:** `CMakeLists.txt`, `tests/CMakeLists.txt`, `src/Version.h`, `src/Core/PerformanceProfiler.h/cpp`, `src/Core/AllocationTracker.h`, `src/Core/ThreadPool.h/cpp`, `src/Core/LockFreeQueue.h`, `.clang-tidy`, `src/Plugin/PluginProcessor.cpp`, `tests/TestMorphingEngine.cpp`, `tests/Unit/*`, `tests/Integration/*`, `tests/Performance/*`, `tests/Mocks/MockInterfaces.h`

---

## Critical Issues

### 1. `processBlock` is `noexcept` but delegates to hosted plugin without exception guard
**File:** `src/Plugin/PluginProcessor.cpp`  
**Lines:** 1415, 1436  
**Issue:** `processBlock` is declared `noexcept` (line 1174), yet it calls `hostManager.processBlock(buffer, filteredMidiBuffer_)` and `hostManagerB_.processBlock(bufferB_, midiCopyB_)` without any `try/catch` wrapper. If the hosted VST3/AU plugin throws (common with poorly written third-party plugins), `std::terminate()` is invoked, crashing the entire DAW.  
**Rationale:** The `noexcept` contract is appropriate for the audio thread to avoid exceptions, but the implementation must be defensive. The `readParameter`/`writeParameter` lambdas inside `drainParameterCommandQueue` do wrap `getParameters()` in `try/catch`, but the main `hostManager.processBlock` call at line 1415 is unprotected.  
**Suggested Fix:** Add a `try/catch(...)` block around `hostManager.processBlock()` and `hostManagerB_.processBlock()` inside `processBlock`, or remove `noexcept` from `processBlock` if the hosted plugin's exception safety cannot be guaranteed. The latter is the safer choice for a plugin host.

### 2. `ThreadPool` has a post-shutdown enqueue race that causes `waitForAll()` to spin forever
**File:** `src/Core/ThreadPool.h`, `src/Core/ThreadPool.cpp`  
**Lines:** `ThreadPool.h:99-122`, `ThreadPool.cpp:58-68`  
**Issue:** `enqueue()` does not check `stop_.load()` before pushing a task. After `shutdown()` sets `stop_ = true` and joins workers, a subsequent `enqueue()` will still push the task and increment `activeTasks_`. Because workers are already joined, the task will never execute. `waitForAll()` spins on `activeTasks_ == 0 && tasks_.empty()` with a 1ms sleep — if a late enqueue occurs, `activeTasks_` will never reach zero and `waitForAll()` will spin forever.  
**Suggested Fix:** Add a `stop_.load()` check at the top of `enqueue()` (before `activeTasks_.fetch_add(1)`) and throw `std::runtime_error` or return an invalid future if the pool is stopped. Also, `shutdown()` should clear `tasks_` under the lock to prevent unexecuted tasks from leaking.

### 3. Use of deprecated JUCE 8 APIs in `processBlock`
**File:** `src/Plugin/PluginProcessor.cpp`  
**Lines:** 1229, 1232  
**Issue:** `getBus(true, 1)` and `getBusBuffer(buffer, true, 1)` are deprecated in JUCE 7+ and may be removed in future JUCE versions. Relying on deprecated APIs is a forward-compatibility risk.  
**Suggested Fix:** Replace with `getBusesLayout().getChannelSet(true, 1)` and manually extract the `AudioBuffer` region, or use JUCE 8's `AudioProcessor::getBusBuffer()` if still available. Verify against JUCE 8.0.4 changelog for the exact replacement.

### 4. `processBlock` and `setStateInformation` violate `.clang-tidy` function-size thresholds by 3×–6×
**File:** `.clang-tidy`, `src/Plugin/PluginProcessor.cpp`  
**Lines:** `.clang-tidy:96-103`, `PluginProcessor.cpp:1173-1561`, `1677-1831`  
**Issue:** `.clang-tidy` sets `readability-function-size.LineThreshold = 60` and `StatementThreshold = 40`. `processBlock` is **388 lines** and `setStateInformation` is **154 lines**. These functions handle multiple unrelated responsibilities (MIDI routing, sidechain, morph computation, parameter application, audio-domain blending, output gain, RMS metering, and profiling) in a single block, making them extremely difficult to unit-test, review, or debug.  
**Suggested Fix:** Decompose `processBlock` into private helpers: `drainCommandsAndApply()`, `processAudioDomain()`, `writeDriftOutput()`, `applyOutputGain()`, `updateRMS()`. Decompose `setStateInformation` into `restoreAPVTS()`, `restoreSnapshotBank()`, `restoreHostedPlugin()`, etc. This also makes the code match the `.clang-tidy` configuration that the project claims to enforce.

### 5. `MockInterfaces.h` depends on Google Mock, which is not linked in the test CMake
**File:** `tests/Mocks/MockInterfaces.h`, `tests/CMakeLists.txt`  
**Lines:** `MockInterfaces.h:9`, `tests/CMakeLists.txt:91-98`  
**Issue:** `MockInterfaces.h` includes `<gmock/gmock.h>`, but `tests/CMakeLists.txt` never fetches or links Google Mock. The file is referenced by `TestVST3ComprehensiveE2E.cpp` (line 30), but that test is opt-in and likely fails to compile if enabled. In practice, the active tests use hand-written fakes (`FakeParameterBridge`, `FakePluginHostManager`) instead.  
**Suggested Fix:** Either remove `MockInterfaces.h` and delete the gmock dependency, or add a `FetchContent_Declare(googletest)` block and link `gmock` to `MorePhiTests`. Since the project already uses hand-written fakes successfully, removing the unused gmock dependency is the simpler and lower-risk path.

---

## High-Priority Improvements

### 6. `catch (...)` is overused — swallows `std::bad_alloc` and unexpected exceptions
**File:** `src/Plugin/PluginProcessor.cpp`  
**Lines:** 317, 458, 478, 598, 1601, 1893, 2061, 2110, 2224, 2241 (10 instances)  
**Issue:** Blanket `catch (...)` is used around hosted plugin calls, XML serialization, and state restoration. While defensive for a plugin host, it also silently swallows `std::bad_alloc`, `std::system_error`, and logic errors that should be fatal or at least logged.  
**Suggested Fix:** Replace with `catch (const std::exception& e)` where possible and log `e.what()` via `DBG()`. Reserve `catch (...)` only for the outermost hosted-plugin boundary where a third-party throw is truly unpredictable.

### 7. `TestMorphingEngine.cpp` is legacy, excluded from CMake, and still in the repo
**File:** `tests/TestMorphingEngine.cpp`  
**Lines:** 1–310  
**Issue:** This file uses the old `morphy` namespace (line 14), `#include`s paths like `../src/morphing/MorphingEngine.h` that no longer exist in the active build, and is explicitly excluded from `tests/CMakeLists.txt` (line 20: "Legacy Morphy-era tests under tests/src are intentionally excluded"). Keeping it in the repo causes confusion and bit-rot.  
**Suggested Fix:** Move to `tests/legacy/` or delete it. If archived, add a `README.md` in `tests/legacy/` explaining why it was retired.

### 8. `Unit/TestDatasetModules.cpp` is commented out because it is "out of sync with current dataset APIs"
**File:** `tests/CMakeLists.txt`  
**Line:** 54  
**Issue:** 1,093 lines of test code are disabled in the build. This means the dataset generation modules (a major subsystem) have zero automated test coverage.  
**Suggested Fix:** Either fix the API drift and re-enable the tests, or remove the file if the subsystem is no longer supported. Leaving dead code in the CMake file is a maintenance hazard.

### 9. `AllocationTracker::recordAllocation` takes a `location` parameter that is never used
**File:** `src/Core/AllocationTracker.h`  
**Line:** 46  
**Issue:** `recordAllocation(size_t /*size*/, const char* /*location*/)` accepts `location` but the body ignores it. This triggers `-Wunused-parameter` on strict compilers and reduces the utility of the tracker. The global `new`/`delete` override is also not actually hooked — the file only provides a `MORE_PHI_NEW` macro that nobody uses.  
**Suggested Fix:** Either remove the unused parameter, or log the location string to a lock-free ring buffer for debug diagnostics. Also, `MORE_PHI_TRACK_ALLOCATIONS` is documented in `README.md` and `DEVELOPER_GUIDE.md` but is **not defined as a CMake option** in `CMakeLists.txt` — add `option(MORE_PHI_TRACK_ALLOCATIONS "Enable allocation tracking in debug builds" OFF)`.

### 10. `PerformanceProfiler` uses `std::chrono` syscalls on the audio thread in profiling builds
**File:** `src/Core/PerformanceProfiler.cpp`  
**Lines:** 15–27  
**Issue:** The `Timer` destructor calls `std::chrono::high_resolution_clock::now()`, which is a system call on some platforms (e.g., older Linux `clock_gettime`). When `MORE_PHI_ENABLE_PROFILING` is defined, multiple `MORE_PHI_PROFILE` macros inside `processBlock` create `Timer` objects and hit the audio thread. While `tryEnter` prevents priority inversion from the SpinLock, the chrono syscall itself can cause jitter.  
**Suggested Fix:** Use `juce::Time::getHighResolutionTicks()` (CPU timestamp counter, no syscall) or a `rdtsc`-based fallback on x86. Document the trade-off in the header.

### 11. `cacheRawParameterPointers()` is a manual, error-prone mapping with no compile-time validation
**File:** `src/Plugin/PluginProcessor.cpp`  
**Lines:** 154–190  
**Issue:** This function maps 36 APVTS parameter IDs to raw atomic pointers by hand. If a parameter is added to `createParameterLayout()` but not cached here, `readRawFloat()` will silently fall back to the default value, causing subtle bugs that are hard to detect.  
**Suggested Fix:** Generate the parameter layout and cache simultaneously using a macro or a small code-generator script, or add a `static_assert` that counts `params.size()` against the number of cached pointers. At minimum, add a debug-only validation in `prepareToPlay()` that checks every APVTS parameter has a non-null cached pointer.

### 12. `Version.h` embeds `__DATE__` and `__TIME__`, making builds non-reproducible
**File:** `src/Version.h`  
**Lines:** 20–21, 138–141  
**Issue:** `BUILD_DATE` and `BUILD_TIME` use the non-deterministic preprocessor macros. This means two builds from the same source commit produce different binaries, breaking reproducible builds and deterministic CI caching.  
**Suggested Fix:** Replace with a CMake-generated `BuildInfo.h` that uses the commit hash and a user-supplied build timestamp (or `SOURCE_DATE_EPOCH` if set). This is standard practice for reproducible builds.

### 13. `.clang-tidy` is not integrated into the CMake build
**File:** `.clang-tidy`, `CMakeLists.txt`  
**Lines:** `.clang-tidy:15`, `CMakeLists.txt:666-670` (stale reference)  
**Issue:** The `.clang-tidy` file references a stale `cmake/CompilerFlags.cmake` that is explicitly excluded from the build pipeline. No `CMAKE_CXX_CLANG_TIDY` is set in `CMakeLists.txt`. This means the carefully configured checks (including `WarningsAsErrors` for `bugprone-use-after-move`, `concurrency-mt-unsafe`, etc.) are never enforced in CI or local builds.  
**Suggested Fix:** Add a CI job that runs `clang-tidy` with `--config-file=.clang-tidy` on `src/Core/*.cpp` and `src/Plugin/*.cpp`. Alternatively, add an opt-in CMake option `MORE_PHI_ENABLE_CLANG_TIDY` that sets `CMAKE_CXX_CLANG_TIDY`.

### 14. Test coverage for `ThreadPool` is minimal and does not exercise shutdown safety
**File:** `tests/Unit/ThreadPoolTests.cpp`  
**Lines:** 1–23  
**Issue:** Only one test exists: "ThreadPool executes tasks concurrently". There is no test for `shutdown()` idempotency, `waitForAll()` behavior with pending tasks, exception handling in tasks, or the post-shutdown enqueue race described in Critical Issue #2.  
**Suggested Fix:** Add tests for: (a) `shutdown()` can be called twice, (b) `enqueue()` after `shutdown()` throws or returns invalid future, (c) `waitForAll()` returns promptly when tasks are complete, (d) a task exception does not crash the worker thread.

### 15. `TestHostIntegration.cpp` and `TestStatePersistence.cpp` are far too small for P0 modules
**File:** `tests/Unit/TestHostIntegration.cpp` (22 lines), `tests/Unit/TestStatePersistence.cpp` (47 lines)  
**Issue:** `tests/CMakeLists.txt` labels Host Integration as "P0" and State Persistence as "P0", yet the test files are trivial. `TestHostIntegration.cpp` only tests `beginExclusivePluginUse()` with no plugin loaded. `TestStatePersistence.cpp` only tests snapshot chunk copy/round-trip. Neither tests plugin loading, parameter bridge state, APVTS migration, or the `pendingStateMutex_` / `pendingHostedState_` paths.  
**Suggested Fix:** Expand `TestHostIntegration.cpp` to cover `loadPlugin`/`unloadPlugin`, `processBlock` with a fake plugin, and `getLastDescription`. Expand `TestStatePersistence.cpp` to cover `getStateInformation`/`setStateInformation` round-trip with morph positions, APVTS values, and the `pendingHostedState_` fallback path.

---

## Medium-Priority Refinements

### 16. SIMD compilation flags are only applied to two files, missing other math-heavy engines
**File:** `CMakeLists.txt`  
**Lines:** 649–664  
**Issue:** `SIMDAudio.cpp` and `InterpolationEngine.cpp` receive `/arch:AVX2` (MSVC) or `-mavx2 -msse4.1` (Clang/GCC). Other files with heavy DSP — `SpectralMorphEngine.cpp`, `GranularMorphEngine.cpp`, `FormantMorphEngine.cpp`, `EnvelopeFollower.cpp`, `OversamplingWrapper` — do not get these flags and may fall back to scalar math.  
**Suggested Fix:** Audit which files contain hot loops and expand the `set_source_files_properties` list, or apply a global baseline (`/arch:AVX2` for MSVC, `-mavx2` for GCC/Clang) with an opt-out for non-DSP files. Ensure non-AVX fallback paths are tested on CI (e.g., an SSE2-only runner).

### 17. `MetadataWriter.cpp` and `MetadataSchema.cpp` are compiled with `/Od` on MSVC
**File:** `CMakeLists.txt`  
**Lines:** 468–475  
**Issue:** Two large dataset translation units are compiled with `/bigobj;/Od` because "MSVC occasionally runs out of frontend heap". `/Od` disables **all** optimization on these files. While they are not on the audio thread, they are part of the CLI and dataset pipeline where performance matters for batch processing.  
**Suggested Fix:** Use `/bigobj` alone and split the translation units into smaller files if frontend memory is the issue. `/Od` should be a last resort, not a permanent fix. If truly needed, add a comment explaining why `/O1` or `/O2` is impossible.

### 18. `LockFreeQueue::pushRange` uses `std::distance` which is not guaranteed O(1)
**File:** `src/Core/LockFreeQueue.h`  
**Line:** 69  
**Issue:** `std::distance` is O(n) for non-random-access iterators. The callers pass `std::vector`, so it's currently O(1), but the API contract is not self-documenting. A caller with a `std::list` or `std::set` would cause an O(n) loop on the audio thread.  
**Suggested Fix:** Add a `static_assert` that the range iterator category is `std::random_access_iterator_tag`, or add a `std::size_t count` overload so callers must pre-compute the size.

### 19. `MORE_PHI_MSVC_MP` defaults to 2, which is conservative for modern CI runners
**File:** `CMakeLists.txt`  
**Line:** 10  
**Issue:** The default `/MP2` limits MSVC to 2 parallel compilation processes. On a GitHub Actions runner (4 vCPUs) or a developer workstation (8–16 cores), this is a bottleneck. The comment says "local machines can tune safely", but the default should favor CI speed.  
**Suggested Fix:** Default to `0` (auto-detect via `ProcessorCount` module) or a value derived from `cmake_host_system_information(RESULT N QUERY NUMBER_OF_PHYSICAL_CORES)`.

### 20. `TestVST3ComprehensiveE2E.cpp` is 1,379 lines but opt-in only
**File:** `tests/Integration/TestVST3ComprehensiveE2E.cpp`, `tests/CMakeLists.txt`  
**Lines:** `tests/CMakeLists.txt:21-23`, `TestVST3ComprehensiveE2E.cpp:1-1379`  
**Issue:** A large E2E test suite is disabled by default because it "tracks experimental APIs". If it is unstable, it should be stabilized or removed. If it is stable, it should be enabled by default. Disabled tests rot quickly.  
**Suggested Fix:** Audit the test for experimental API usage, fix the API calls, and enable it by default. If the APIs are genuinely unstable, add `#ifdef` guards so the test compiles against the current API surface.

### 21. `enqueueParameterBatch` copies commands by value in the loop
**File:** `src/Plugin/PluginProcessor.cpp`  
**Line:** 216  
**Issue:** `for (auto command : commands)` copies each `ParamCommand` by value. `ParamCommand` is small, but this is an unnecessary copy and a warning on `-Wrange-loop-analysis`.  
**Suggested Fix:** Change to `for (const auto& command : commands)`.

### 22. `processBlock` uses `std::fill` instead of SIMD-aware JUCE clearing
**File:** `src/Plugin/PluginProcessor.cpp`  **Line:** 1267  
**Issue:** `std::fill(finalOutput_.begin(), finalOutput_.begin() + usedCount, 0.0f)` is used to clear the morph output buffer. JUCE provides `juce::FloatVectorOperations::clear()` which uses SIMD intrinsics on most platforms.  
**Suggested Fix:** Replace `std::fill` with `juce::FloatVectorOperations::clear(finalOutput_.data(), paramCount)` for consistent SIMD optimization.

### 23. `getProfilingReport()` is 126 lines and builds a large string on the message thread
**File:** `src/Plugin/PluginProcessor.cpp`  **Lines:** 637–763  
**Issue:** While this is message-thread-safe, the function is long and mixes string formatting with business logic (CPU spike diagnosis). It also sorts a `std::vector` of stats, which is fine but adds to the function length.  
**Suggested Fix:** Extract the diagnosis logic into a private `diagnoseCpuSpikes()` helper, and extract the string formatting into a `formatProfileStats()` helper. This brings `getProfilingReport()` under the 60-line threshold.

---

## Low-Priority Enhancements

### 24. `Version.h` `getEdition()` is a hardcoded stub
**File:** `src/Version.h`  **Line:** 153  
**Issue:** Returns `Edition::Full` unconditionally. This is fine for a placeholder but should have a `TODO` or be wired to the actual `LicenseManager`.  **Suggested Fix:** Add a `// TODO: wire to LicenseManager::isActivated()` comment.

### 25. `MORE_PHI_ENABLE_DATASET_V3` is a confusing no-op option
**File:** `CMakeLists.txt`  **Line:** 12  
**Issue:** The option defaults to OFF and the description says "deprecated compatibility flag (no-op)". Yet the compile definition `MORE_PHI_ENABLE_DATASET_V3=1` is hardcoded for `MorePhi`, `MorePhiCLI`, and `MorePhiMcpCore`. The option does nothing.  **Suggested Fix:** Remove the CMake option and the compile definition entirely, or keep the definition and remove the option. The current state is confusing.

### 26. `PluginProcessor.cpp` mixes `memory_order_relaxed` and `seq_cst` inconsistently
**File:** `src/Plugin/PluginProcessor.cpp`  **Line:** 910  
**Issue:** `prepared.store(false, std::memory_order_seq_cst)` at line 910 uses `seq_cst`, while virtually every other atomic operation uses `relaxed`. The comment says this prevents a race with a "concurrent prepareToPlay() call from misbehaving host", but `seq_cst` alone is not sufficient to guarantee ordering of all the surrounding state (buffer sizes, sample rate, etc.) without additional barriers.  **Suggested Fix:** Either use `release` semantics for the `prepared` flag and document the pairing `acquire` load, or add a full comment explaining why `seq_cst` is specifically required here. If the goal is to prevent concurrent `prepareToPlay`, a `std::atomic<bool>` flag with `exchange` is a clearer pattern.

### 27. `juce::ignoreUnused` is used to suppress warnings for incomplete features
**File:** `src/Plugin/PluginProcessor.cpp`  **Lines:** 1690, 2168, 2208  
**Issue:** `juce::ignoreUnused(stateVersion)` (future migration logic), `juce::ignoreUnused(totalAttempts)` (debug builds), and `juce::ignoreUnused(numOccupied)` (best-effort slot name reading) suggest the code has parameters or variables that are declared but not yet fully used. This is a code smell.  **Suggested Fix:** Either implement the feature (e.g., state version migration) or remove the variable. For `totalAttempts`, use `[[maybe_unused]]` or `JUCE_UNREFERENCED` instead of runtime suppression.

### 28. `TestMIDIRouting.cpp` is only 91 lines but labeled P1
**File:** `tests/Unit/TestMIDIRouting.cpp` (read partially during audit)  **Issue:** The MIDI routing module is a critical part of the processing pipeline (notes C3-B3 trigger snapshots, CC1 drives fader). The test file is very small.  **Suggested Fix:** Add tests for MIDI note-on triggering snapshot recall, CC1 fader mapping, sidechain threshold behavior, and MIDI pass-through correctness.

### 29. `tests/CMakeLists.txt` defines `JUCE_MODAL_LOOPS_PERMITTED=1` for all tests
**File:** `tests/CMakeLists.txt`  **Line:** 109  
**Issue:** Modal loops are permitted in the test binary, which can cause tests to hang on CI if a dialog is accidentally shown. This is a JUCE-level risk.  **Suggested Fix:** Ensure no UI code is executed in unit tests. If `MorePhiEditor` is instantiated in any test, verify it uses headless mocking.

### 30. The `tests/` directory contains `DAW/` and `scripts/` subdirectories with unclear purpose
**File:** `tests/DAW/`, `tests/scripts/`  **Issue:** `tests/scripts/run_vst3_validator.py` is used for VST3 validation testing, but `tests/DAW/` appears empty or contains test fixtures. The directory structure is not documented.  **Suggested Fix:** Add a `README.md` in `tests/` explaining the directory layout and what `DAW/` contains (e.g., Reaper/FL Studio project files for manual validation).

---

## Recommended Fixes (Summary Table)

| # | Fix | Rationale | Effort |
|---|-----|-----------|--------|
| 1 | Add `try/catch` around `hostManager.processBlock()` in `processBlock`, or remove `noexcept` | Prevents DAW crash from misbehaving hosted plugin | Low |
| 2 | Add `stop_` guard in `ThreadPool::enqueue()` and clear `tasks_` in `shutdown()` | Fixes post-shutdown enqueue race and infinite spin | Low |
| 3 | Replace deprecated `getBus`/`getBusBuffer` with JUCE 8 equivalents | Forward compatibility | Medium |
| 4 | Decompose `processBlock` and `setStateInformation` into helpers | Matches `.clang-tidy`, improves testability | Medium |
| 5 | Remove `gmock` from `MockInterfaces.h` or add Google Mock to CMake | Dead code / broken dependency | Low |
| 6 | Delete or archive `TestMorphingEngine.cpp` and `TestDatasetModules.cpp` (or fix and re-enable) | Eliminates bit-rot and confusion | Low |
| 7 | Fix `AllocationTracker::recordAllocation` unused parameter, add `MORE_PHI_TRACK_ALLOCATIONS` CMake option | Clean up debug-only tracking | Low |
| 8 | Replace `std::chrono` in `PerformanceProfiler` with `juce::Time::getHighResolutionTicks()` or `rdtsc` | Reduces audio-thread syscall overhead | Low |
| 9 | Add compile-time or debug-time validation in `cacheRawParameterPointers()` | Prevents silent parameter cache misses | Low |
| 10 | Remove `__DATE__`/`__TIME__` from `Version.h` for reproducible builds | CI caching and deterministic builds | Low |
| 11 | Integrate `.clang-tidy` into CI or CMake (`MORE_PHI_ENABLE_CLANG_TIDY`) | Enforces the project's own quality rules | Low |
| 12 | Expand `ThreadPool`, `HostIntegration`, and `StatePersistence` tests | Critical path coverage gaps | Medium |
| 13 | Expand SIMD flags to `SpectralMorphEngine.cpp`, `GranularMorphEngine.cpp`, etc. | Performance improvement | Low |
| 14 | Remove `/Od` from `MetadataWriter.cpp` / `MetadataSchema.cpp` | Restore optimization in dataset pipeline | Low |
| 15 | Change `MORE_PHI_MSVC_MP` default to auto-detect | Faster CI builds | Low |
| 16 | Fix `enqueueParameterBatch` range-loop copy | Warning cleanup | Trivial |
| 17 | Replace `std::fill` with `juce::FloatVectorOperations::clear` in `processBlock` | Consistent SIMD usage | Trivial |
| 18 | Remove no-op `MORE_PHI_ENABLE_DATASET_V3` CMake option | Clarity | Trivial |

---

## Build/Test Hygiene Scorecard

| Area | Grade | Notes |
|------|-------|-------|
| CMake Configuration | B+ | Well-structured, FetchContent for deps, good separation of plugin/CLI/test targets. Lacks `clang-tidy` integration and a few missing options. |
| Compiler Warning Hygiene | C+ | `catch (...)` overuse, deprecated JUCE APIs, some `ignoreUnused` suppressions. No custom warning flags beyond JUCE defaults. |
| Thread Safety | B+ | Good use of atomics, SpinLock, and lock-free queue. Critical `ThreadPool` shutdown race and `processBlock noexcept` issue are gaps. |
| Test Coverage Depth | B- | Large test suite (21k+ lines) but many critical tests are tiny (`ThreadPool` 23 lines, `HostIntegration` 22 lines, `StatePersistence` 47 lines). Legacy and disabled tests present. |
| Test Coverage Breadth | B+ | Unit, Integration, Performance, and Validation (VST3/pluginval/AUval) tests exist. E2E is opt-in. Dataset tests disabled. |
| Mock / Fake Usage | B+ | Hand-written fakes are used well (`FakeParameterBridge`, `FakePluginHostManager`). `gmock` header is dead code. |
| SIMD / Performance | B | Good per-file SIMD flagging, but missing on several DSP engines. `PerformanceProfiler` is well-designed with opt-in macro. |
| Production Readiness | B | Code is generally defensive (try/catch around hosted plugin calls), but `noexcept` on `processBlock` is a risk. State persistence is robust with retry logic. |
| Reproducible Builds | C | `__DATE__`/`__TIME__` in `Version.h` break reproducibility. No `SOURCE_DATE_EPOCH` handling. |
| Documentation | B | `AGENTS.md` is excellent. `.clang-tidy` is detailed but not enforced. `testing-strategy.md` exists but not verified for currency. |

---

*End of Sub-Agent 6 Report*
