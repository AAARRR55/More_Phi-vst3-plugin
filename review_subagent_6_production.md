# Sub-Agent 6 Report: Production Readiness & Code Quality

**Scope:** Build configuration, performance bottlenecks, error handling, host compatibility, code standards, test coverage, memory tracking, thread pool, latency reporting, and stress tests.  
**Date:** 2026-04-26  
**Files Reviewed:** 22 source/test files + 7 directory scans (≈ 170 files total)  

---

## Critical Issues (BUILD FAILURE / PERFORMANCE / CRASH)

### Issue 1: ThreadPool `activeTasks_` Leak on Exception — `waitForAll()` Deadlock
- **Location:** `src/Core/ThreadPool.cpp:50–54` and `src/Core/ThreadPool.h:114–117`
- **Severity:** Critical
- **Description:** When an enqueued task throws, `workerThread()` catches the exception and continues, but `activeTasks_` is decremented inside the task wrapper lambda (`ThreadPool.h:116`). If `task()` throws, `(*task)()` completes via the packaged_task mechanism (which stores the exception in the future), but the surrounding lambda in `enqueue()` still runs `activeTasks_.fetch_sub(1)`. **Wait** — actually the lambda wrapping the task will still run to completion because the `try/catch` is inside `workerThread`, not inside the lambda. Let me re-examine: `workerThread` does `task()` inside a `try/catch`. The lambda in `enqueue` is `[task, this] { (*task)(); activeTasks_.fetch_sub(1); }`. If `(*task)()` throws, the `workerThread` catches it, but the lambda has already thrown — `activeTasks_.fetch_sub(1)` is **never executed**.
- **Root Cause:** The `activeTasks_` decrement is placed after the user callable inside the lambda, but `workerThread` catches exceptions at the outer `task()` level, so the lambda body aborts mid-way.
- **Recommended Fix:** Move `activeTasks_` management into `workerThread()` itself, not the lambda wrapper:
```cpp
// In workerThread():
activeTasks_.fetch_sub(1);  // Decrement BEFORE executing task
// OR wrap in RAII guard:
struct ActiveTaskGuard {
    std::atomic<size_t>& counter;
    ActiveTaskGuard(std::atomic<size_t>& c) : counter(c) { counter.fetch_add(1); }
    ~ActiveTaskGuard() { counter.fetch_sub(1); }
};
```
Alternatively, decrement `activeTasks_` immediately after popping from the queue, before calling `task()`, so even a throw won't lose the count.

### Issue 2: `std::pow` on Audio Thread in `EnvelopeFollower::processBlock()`
- **Location:** `src/Core/EnvelopeFollower.cpp:104`
- **Severity:** Critical
- **Description:** `const float coeff = std::pow(baseCoeff, static_cast<float>(numSamples));` executes `std::pow` once per block on the audio thread. While the comment correctly identifies this as a fix for block-rate vs sample-rate mismatch, `std::pow` is a non-inlineable, branch-heavy libm call that can take hundreds of cycles and is not suitable for real-time audio paths.
- **Root Cause:** The coefficient needs to be raised to the `numSamples` power, but this is done inside `process()` which is called from the audio thread every block.
- **Recommended Fix:** Pre-compute `coeff` whenever `attackCoeff_`/`releaseCoeff_` or `numSamples` changes (in `prepare()` or a setter), and store it in an atomic. If the block size is dynamic, use a small LUT for common block sizes (64, 128, 256, 512, 1024, 2048) or approximate with `std::exp(numSamples * std::log(baseCoeff))` which may be faster on some platforms, but better yet: pre-compute per-block-size in `prepare()`.

### Issue 3: `Version.h` `__DATE__`/`__TIME__` Force Excessive Recompilation
- **Location:** `src/Version.h:20–21`
- **Severity:** Critical (Build System)
- **Description:** `constexpr const char* BUILD_DATE = __DATE__;` and `BUILD_TIME = __TIME__;` are compile-time macros that change on every compilation. Any translation unit that includes `Version.h` (or includes a header that includes it) will be recompiled every time `make`/`cmake --build` is invoked, even if no source code changed. This destroys incremental build performance and CI cache efficiency.
- **Root Cause:** `__DATE__` and `__TIME__` are not stable across compilation invocations.
- **Recommended Fix:** Move `BUILD_DATE` and `BUILD_TIME` to a *single* `.cpp` file (e.g., `src/Version.cpp`) that is the only translation unit touching these macros. Alternatively, pass them as `target_compile_definitions` on a single stub file, or use CMake's `configure_file` to generate a version string at configure time (not compile time). Keep `VERSION_MAJOR/MINOR/PATCH` as `constexpr` in the header since they are stable.

### Issue 4: `PerformanceProfiler::updateStats()` Allocates on Audio Thread
- **Location:** `src/Core/PerformanceProfiler.cpp:80` (`stats_[name]`)
- **Severity:** Critical
- **Description:** `recordTime()` uses `juce::SpinLock::ScopedTryLockType`. If the lock is acquired (which it usually is on the audio thread if no reader holds it), `updateStats()` is called. Inside `updateStats`, `auto& stat = stats_[name];` performs a `std::unordered_map::operator[]` insertion if the key doesn't exist. This insertion **heap-allocates** memory for the new `ProfileStats` and potentially rehashes the bucket array. This is a direct violation of the "zero allocations after prepare()" rule for the audio thread.
- **Root Cause:** `std::unordered_map` is not real-time safe. `operator[]` allocates on first access for any given name string.
- **Recommended Fix:** Replace the `std::unordered_map` with a fixed-size array or pre-allocated linear probing hash table. Pre-register all profiler section names during `prepare()` so no new keys are ever inserted on the audio thread. If dynamic names are truly needed, use a lock-free ring buffer to pass timing records to the message thread for aggregation.

---

## High-Priority Issues (ARCHITECTURAL / MAINTAINABILITY)

### Issue 5: `AudioBufferPool` is NOT Real-Time Safe Despite Being in `src/Core/`
- **Location:** `src/Core/AudioBufferPool.cpp:14, 25, 31, 42, 52, 66`
- **Severity:** High
- **Description:** `AudioBufferPool` uses `std::mutex` (not `juce::SpinLock`), `std::stack`, and `std::unique_ptr<juce::AudioBuffer<float>>`. `std::mutex` can enter the kernel (priority inversion), and `std::unique_ptr` deletion/creation can allocate. The class is used in `OfflineBatchRenderer` (dataset generation, not audio thread), but its placement in `src/Core/` and naming suggest it is intended for audio-thread buffer pooling. If any developer uses it in `processBlock()`, it will break real-time safety.
- **Root Cause:** Misleading abstraction placement and missing documentation about thread-domain restrictions.
- **Recommended Fix:** Either (a) rename to `OfflineAudioBufferPool` and move to `src/AI/Dataset/`, or (b) replace `std::mutex` with `juce::SpinLock`, replace `std::unique_ptr` with raw pointers + pre-allocated fixed pool, and add `static_assert` comments enforcing "not for audio thread" or "audio thread safe".

### Issue 6: `LockFreeQueue::pushRange()` Reads `std::distance` on Input Range Without Validation
- **Location:** `src/Core/LockFreeQueue.h:69`
- **Severity:** High
- **Description:** `pushRange` computes `count = static_cast<size_t>(std::distance(std::begin(items), std::end(items)))`. If the input range is an infinite range or a non-random-access iterator (e.g., a custom input iterator), `std::distance` may be O(N) or undefined. For large ranges, this also iterates twice (once for distance, once for copy). More importantly, `std::distance` on some iterators can throw or have unexpected behavior with non-finite ranges.
- **Root Cause:** No `std::iterator_traits` static_assert or constraint on `Range`.
- **Recommended Fix:** Add `static_assert(std::is_base_of_v<std::random_access_iterator_tag, typename std::iterator_traits<decltype(std::begin(items))>::iterator_category>, "pushRange requires random-access range");` or use C++20 `requires`/`concept`.

### Issue 7: `PatchJuceForMSVC.cmake` Mutates Fetched Source Tree In-Place
- **Location:** `cmake/PatchJuceForMSVC.cmake`
- **Severity:** High
- **Description:** The CMake script uses `file(READ)` + `string(REPLACE)` + `file(WRITE)` to modify files inside `${juce_SOURCE_DIR}`, which is the FetchContent-populated directory. This mutates the external dependency source tree on disk. If the user switches between Release and Debug, or re-runs CMake with a different generator, the patches may be applied twice or fail. The idempotency check (`_CONTENT MATCHES "noBadge"`) is fragile string matching.
- **Root Cause:** Patching fetched content is an anti-pattern; it breaks reproducibility and hermetic builds.
- **Recommended Fix:** Apply patches via a local fork of JUCE, or use `add_compile_definitions` + `target_compile_definitions` to define away the conflicting macros, or add a `target_compile_definitions(MorePhi PRIVATE ...)` that defines the conflicting enum names before JUCE headers are parsed. If patching is unavoidable, copy the JUCE module into the project's `third_party/` and patch the copy, not the fetched directory.

### Issue 8: `MORE_PHI_TRACK_ALLOCATIONS` is Not a CMake Option
- **Location:** `src/Core/AllocationTracker.h:17`, `CMakeLists.txt` (missing option)
- **Severity:** High
- **Description:** `AllocationTracker` is gated by `#if JUCE_DEBUG && MORE_PHI_TRACK_ALLOCATIONS`, but `MORE_PHI_TRACK_ALLOCATIONS` is never defined as a CMake `option()`. The `README.md` and `DEVELOPER_GUIDE.md` document it as a CMake flag, but users cannot actually toggle it without manually adding `-DMORE_PHI_TRACK_ALLOCATIONS=1` to the compiler flags. This is confusing and breaks the documented workflow.
- **Root Cause:** Missing `option(MORE_PHI_TRACK_ALLOCATIONS ...)` in `CMakeLists.txt`.
- **Recommended Fix:** Add `option(MORE_PHI_TRACK_ALLOCATIONS "Enable allocation tracking in debug builds" OFF)` and pass it as `target_compile_definitions(MorePhi PRIVATE MORE_PHI_TRACK_ALLOCATIONS=$<BOOL:${MORE_PHI_TRACK_ALLOCATIONS}>)`.

### Issue 9: `BenchmarkSuite.cpp` Uses `rand()` (Deprecated, Non-Thread-Safe, Poor Quality)
- **Location:** `tests/Performance/BenchmarkSuite.cpp:299, 308, 309, 504, 515`
- **Severity:** High
- **Description:** `rand()` is used to generate random cursor positions for the 2D interpolation benchmark. `rand()` is not thread-safe, has poor statistical quality (LCG with short period), and `RAND_MAX` is only guaranteed to be 32767, which limits precision for float generation. Since this is a benchmark, measurement noise from poor randomness can affect cache behavior unpredictably.
- **Root Cause:** Convenience use of C legacy random function.
- **Recommended Fix:** Replace with `std::mt19937` seeded from `std::random_device`. Store the generator as a `thread_local` or local static in the benchmark functions.

### Issue 10: `processBlock()` `catch (...)` Blocks Swallow Errors Silently
- **Location:** `src/Plugin/PluginProcessor.cpp:317, 458, 478, 598, 1601, 1893, 2061, 2110, 2224, 2241`
- **Severity:** High
- **Description:** There are 11 `catch (...)` blocks in `PluginProcessor.cpp`. Several of them are empty (`catch (...) {}`) or only log at debug level. For example, `flushPendingParameterCommandsForAssistant` catches exceptions from `plugin->getParameters().size()` and silently returns 0. This makes debugging hosted-plugin crashes extremely difficult because the exception stack trace is lost.
- **Root Cause:** Defensive programming taken too far — exceptions from hosted plugins are being suppressed rather than logged or handled.
- **Recommended Fix:** At minimum, log the exception type and location to the JUCE logger or a crash reporter buffer. Example:
```cpp
catch (const std::exception& e) {
    juce::Logger::writeToLog("PluginProcessor: exception in hosted plugin access: " + juce::String(e.what()));
} catch (...) {
    juce::Logger::writeToLog("PluginProcessor: unknown exception in hosted plugin access");
}
```

### Issue 11: `LatencyManager` Omits Modulation and Audio-Domain Engine Latency
- **Location:** `src/Core/LatencyManager.h:11–14, 85–93`
- **Severity:** High
- **Description:** The latency equation documented in the header is:
  `totalLatency = oversamplingLatency + fftWindowLatency + hostedPluginLatency + masteringChainLatency`.
  It is missing latency contributions from:
  - `OversamplingWrapper` (if used in audio-domain engines)
  - `SpectralMorphEngine` FFT window/lookahead
  - `GranularMorphEngine` grain buffer delay
  - `FormantMorphEngine` formant shift delay
  - `ModulationEngine` LFO/step-sequencer phase alignment (if any)
  - `VAEMorphEngine` inference latency (if run synchronously)
  If any of these engines are active, the DAW will report incorrect latency to the user, causing phase misalignment in parallel signal chains.
- **Root Cause:** LatencyManager was designed for the mastering chain only, but V2 audio-domain engines were added later without extending latency accounting.
- **Recommended Fix:** Add `setSpectralLatency()`, `setGranularLatency()`, `setFormantLatency()`, `setVAELatency()` methods, and ensure each engine calls `latencyManager.setXXX()` during `prepare()`. Verify the sum is reported in `getTotal()`.

### Issue 12: Test Gaps in Critical Paths
- **Location:** `tests/Unit/`, `tests/Integration/`
- **Severity:** High
- **Description:** Several critical subsystems have minimal or no dedicated tests:
  | Subsystem | Test File | Lines | Gap |
  |-----------|-----------|-------|-----|
  | ThreadPool | `ThreadPoolTests.cpp` | 23 | Only concurrent execution; missing shutdown, exception handling, `waitForAll()` deadlock, thread count limits |
  | AudioBufferPool | `AudioBufferPoolTests.cpp` | 23 | Only basic acquire/release; missing thread safety, preallocation stress, OOM edge cases |
  | PerformanceProfiler | `PerformanceProfilerTests.cpp` | 18 | Only single-thread timer; missing multi-thread contention, `tryEnter` fallback, `reset()` race |
  | Stress/Edge Cases | `TestStressEdgeCases.cpp` | 97 | Only 4 test cases; missing rapid parameter changes, memory pressure, long-running stability (24h simulation), extreme physics values |
  | Host Integration | `TestHostIntegration.cpp` | 680 bytes | Likely minimal (file is very small) |
  | State Persistence | `TestStatePersistence.cpp` | 1402 bytes | Minimal |
  | Physics Stability | `TestPhysicsAndGenetic.cpp` | — | Has tests but missing: NaN/Inf recovery from `std::clamp` bypass, sub-stepping overflow with `dt > 1s`, spring divergence at very high stiffness |
  | MCP Security | `TestMCPServerUnit.cpp` | — | Large file (73KB) but check for: token replay attacks, rate-limit bypass, malformed JSON crash resistance |
  | Audio I/O Edge Cases | `TestVST3AudioSignalAccuracy.cpp` | — | Missing: zero-channel layout, 7.1 surround, 192kHz sample rate, block size = 1, block size > 2048 |
- **Root Cause:** Test suite grew organically; some modules were added after the initial test infrastructure.
- **Recommended Fix:** Prioritize adding tests for ThreadPool exception handling, AudioBufferPool thread safety, and a 24-hour stability simulation test. Enable `MORE_PHI_BUILD_COMPREHENSIVE_E2E` in CI (it is currently OFF by default).

---

## Medium-Priority Issues (EDGE CASE / ROBUSTNESS)

### Issue 13: `WindowsCompat.h` Undefines `WINAPI` and `CALLBACK`
- **Location:** `src/Core/WindowsCompat.h:62–72`
- **Severity:** Medium
- **Description:** The header undefines `WINAPI` and `CALLBACK`, which are Windows calling-convention macros (`__stdcall`), not just convenience macros. If any translation unit includes `WindowsCompat.h` and then includes a Windows SDK header (or JUCE header that pulls in `windows.h`), the calling convention will be missing, leading to link errors or ABI mismatches. The comment says these "conflict with C++ keywords" but they don't — they are preprocessor macros that happen to be uppercase.
- **Root Cause:** Overly aggressive undefining of Windows macros.
- **Recommended Fix:** Remove `WINAPI` and `CALLBACK` from the undef list. They are not known to conflict with JUCE or C++ syntax. If a specific conflict exists, document it with a reference to the compiler error.

### Issue 14: `SnapshotBank` Seqlock Read Can Fail Silently After 128 Retries
- **Location:** `src/Core/SnapshotBank.h:51, 112–134`
- **Severity:** Medium
- **Description:** `MAX_READ_RETRIES = 128`. If the audio thread is starved and the UI thread holds the write lock for more than 128 retry iterations (which is possible under heavy MCP load or DAW UI thread contention), the seqlock read loop in `toXml()` and `getSlotValuesCopy()` simply gives up and returns potentially torn data. The caller has no way to know the read failed.
- **Root Cause:** Bounded retry with no failure signaling.
- **Recommended Fix:** Return `bool` from `getSlotValuesCopy` and `toXml` (already does for `getSlotValuesCopy` — check if `toXml` handles it). Actually `toXml` uses the retry loop but doesn't return failure. Consider returning `nullptr` or adding a `bool* outSuccess` parameter. In practice, 128 retries is generous, but silent failure is still poor engineering.

### Issue 15: `TestVST3ComprehensiveE2E.cpp` is Disabled by Default
- **Location:** `tests/CMakeLists.txt:21–23, 85–87`
- **Severity:** Medium
- **Description:** The most comprehensive E2E test (1379 lines, 25+ test cases covering GUI, MIDI, MCP, VST3 compliance, state persistence, discrete parameters, link mode, token optimizer) is gated behind `MORE_PHI_BUILD_COMPREHENSIVE_E2E=OFF`. The reason given is "it tracks experimental APIs." In a production-ready plugin, the most thorough test should not be opt-in.
- **Root Cause:** Fear of API churn breaking the test. But if the test breaks, that signals a real regression.
- **Recommended Fix:** Remove the option gate and always build the comprehensive E2E test. If it fails due to experimental API changes, fix the test or stabilize the API. Add the test to CI.

### Issue 16: `OfflineBatchRenderer` Creates `AudioBufferPool` on Worker Threads Without Pre-allocation
- **Location:** `src/AI/Dataset/OfflineBatchRenderer.cpp:620, 691`
- **Severity:** Medium
- **Description:** `scratchPool = std::make_unique<AudioBufferPool>(...)` is constructed inside a worker thread lambda. The `AudioBufferPool` constructor does not preallocate buffers. The first `acquireBuffer()` call will call `createBuffer()` which calls `std::make_unique<juce::AudioBuffer<float>>`, which allocates. In a multi-threaded offline rendering context, this causes heap contention and unpredictable render times.
- **Root Cause:** Missing `preallocate()` call after construction.
- **Recommended Fix:** After constructing the pool, call `scratchPool->preallocate(4)` (or estimate peak concurrent usage) to ensure all buffers are allocated before the render loop begins.

### Issue 17: `PluginProcessor.cpp` is 2269 Lines — Monolithic and Hard to Maintain
- **Location:** `src/Plugin/PluginProcessor.cpp`
- **Severity:** Medium
- **Description:** The processor file contains: parameter creation, `processBlock`, state serialization, APVTS sync, plugin hosting management, MIDI routing, sidechain processing, mastering analysis, MCP integration, UI editor factory, and license management. This violates the Single Responsibility Principle and makes code review, testing, and navigation difficult.
- **Root Cause:** Organic growth of a central God object.
- **Recommended Fix:** Extract subsystems into internal helper classes or free functions within an anonymous namespace:
  - `ParameterCommandDrainer` (lines ~540–560)
  - `StateSerializer` (lines ~1600–1900)
  - `APVTSSynchronizer` (lines ~1000–1100)
  - `PluginHostCoordinator` (lines ~1200–1500)

### Issue 18: `MCPToolHandler.cpp` is 250KB (≈ 6,000+ lines) — Unmaintainable
- **Location:** `src/AI/MCPToolHandler.cpp`
- **Severity:** Medium
- **Description:** A single `.cpp` file weighing 250KB is extremely difficult to review, navigate, and compile. It likely contains all MCP tool implementations in one translation unit. This causes long compile times, high memory usage during compilation, and makes merge conflicts likely.
- **Root Cause:** All tool handlers in one file.
- **Recommended Fix:** Split into `MCPToolHandler/` directory with one file per tool category (e.g., `PluginTools.cpp`, `SnapshotTools.cpp`, `MasteringTools.cpp`, `AnalysisTools.cpp`). Keep the `MCPToolHandler` class as a thin dispatcher.

### Issue 19: `CompilerFlags.cmake.stale` and Backup Files in Source Tree
- **Location:** `cmake/CompilerFlags.cmake.stale`, `src/Plugin/PluginProcessor.h.bak`, `src/Plugin/PluginProcessor_v330.cpp`, `src/Plugin/SnappySnapProcessor.h`
- **Severity:** Medium
- **Description:** Stale/backup files are checked into the repository. `CompilerFlags.cmake.stale` is explicitly named stale but still present. `PluginProcessor.h.bak` is a backup file. `SnappySnapProcessor.h` appears to be an old name. These files clutter the source tree, increase repo size, and may confuse developers or IDEs.
- **Root Cause:** Accidental inclusion of temporary files in version control.
- **Recommended Fix:** Delete all `.bak`, `.stale`, `_v330`, and legacy-name files. Add `*.bak`, `*.stale`, `*Processor.h.bak` to `.gitignore`.

### Issue 20: `ai_insight.json` in `src/AI/`
- **Location:** `src/AI/ai_insight.json` (181KB)
- **Severity:** Medium
- **Description:** A 181KB JSON data file is stored in the source code directory. It should be in a `data/`, `assets/`, or `resources/` directory. Having it in `src/AI/` means it may be picked up by IDEs as a source file, and it bloats the `src/` directory listing.
- **Root Cause:** Convenience placement during development.
- **Recommended Fix:** Move to `data/ai_insight.json` or `assets/ai/ai_insight.json`. Update any `#include` or file-path references.

### Issue 21: `more_phi_apply_build_tuning` LTO Check Has Variable Scope Issue
- **Location:** `CMakeLists.txt:79–86`
- **Severity:** Medium
- **Description:** In the `elseif(MORE_PHI_ENABLE_LTO)` branch, `check_ipo_supported(RESULT more_phi_ipo_supported ...)` is called, and then `if(more_phi_ipo_supported)` is used. However, `more_phi_ipo_supported` is a variable set by the CMake module. If `CheckIPOSupported` is not included before this function is called, `check_ipo_supported` may not be available. The `include(CheckIPOSupported)` is inside the `elseif`, which is correct, but if the function is called from a scope where `CheckIPOSupported` is already loaded, it may be fine. The real issue is that `more_phi_ipo_output` is also used inside the `if` block but could be unset if the check fails in a specific way.
- **Root Cause:** CMake variable scope is generally fine here, but the logic is fragile.
- **Recommended Fix:** Ensure `include(CheckIPOSupported)` is at the top-level of `CMakeLists.txt`, not inside the `elseif`. Add explicit `message(FATAL_ERROR ...)` if LTO is requested but IPO is unsupported, rather than silently warning.

---

## Low-Priority Issues (STYLE / DOCUMENTATION)

### Issue 22: `AudioBufferPool::acquireBuffer()` Unconditionally Clears Buffer
- **Location:** `src/Core/AudioBufferPool.cpp:29–32`
- **Severity:** Low
- **Description:** Every `acquireBuffer()` calls `buffer->clear()`, which fills the buffer with zeros. This is a CPU cost that may be unnecessary if the caller is going to overwrite the entire buffer anyway. In offline rendering, this adds up.
- **Root Cause:** Defensive "clean state" guarantee.
- **Recommended Fix:** Add an optional `bool clearBuffer = true` parameter, or provide `acquireBuffer(bool clear)` overload. Callers that overwrite the buffer can pass `false`.

### Issue 23: `BenchmarkSuite.cpp` Memory Measurement is Coarse and OS-Dependent
- **Location:** `tests/Performance/BenchmarkSuite.cpp:412–431`
- **Severity:** Low
- **Description:** `getMemoryDeltaBytes()` does a 1MB scratch allocation and measures working set delta. This is noisy and doesn't actually measure the plugin's memory footprint. The `sizeof`-based estimate is more accurate but labeled as "not a runtime measurement."
- **Root Cause:** Accurate per-allocation tracking is hard without instrumenting `malloc`.
- **Recommended Fix:** Use `mallinfo`/`mallinfo2` on Linux, `_CrtMemState` on Windows, or link with tcmalloc/jemalloc stats for accurate heap reporting. Document the limitation.

### Issue 24: `MORE_PHI_ENABLE_DATASET_V3` is a Deprecated No-Op but Still in Options
- **Location:** `CMakeLists.txt:12`
- **Severity:** Low
- **Description:** The option is documented as "Deprecated compatibility flag (no-op): Dataset V3 modular pipeline sources are always compiled." Keeping deprecated no-ops in the build system creates confusion.
- **Root Cause:** Backward compatibility after removing the conditional.
- **Recommended Fix:** Remove the option entirely. If users pass it, CMake will warn about an unused variable, which is acceptable.

### Issue 25: `PhysicsEngine::updateDrift()` Uses `static const int perm[512]` — Duplicated Permutation Table
- **Location:** `src/Core/PhysicsEngine.cpp:74–100`
- **Severity:** Low
- **Description:** The standard Perlin permutation table is duplicated (256 entries + 256 repeat). This is 512 `int`s in `.rodata`. Not a big issue, but if Perlin is also used elsewhere (e.g., `GranularMorphEngine`), the table should be shared.
- **Root Cause:** Self-contained implementation.
- **Recommended Fix:** Extract to a shared `PerlinNoise.h` with `inline constexpr std::array<uint8_t, 256> kPerlinPermutation`.

### Issue 26: `TestDatasetModules.cpp` is Commented Out in `tests/CMakeLists.txt`
- **Location:** `tests/CMakeLists.txt:54`
- **Severity:** Low
- **Description:** `# Unit/TestDatasetModules.cpp  # Existing suite is out of sync with current dataset APIs`. This is dead code that should be either fixed or deleted.
- **Root Cause:** Test drift during API evolution.
- **Recommended Fix:** Fix the test or delete the file and the comment. Having commented-out code in CMakeLists is technical debt.

### Issue 27: `PluginProcessor.h` has `PluginProcessor.h.bak` in Same Directory
- **Location:** `src/Plugin/PluginProcessor.h.bak` (11101 bytes)
- **Severity:** Low
- **Description:** Backup file in source tree. Same as Issue 19 but specifically worth noting as it's in the most critical header directory.
- **Root Cause:** Editor backup or manual save.
- **Recommended Fix:** Delete and add `*.bak` to `.gitignore`.

### Issue 28: `Tests/CMakeLists.txt` Missing `catch_discover_tests` for `MorePhiBenchmarks`
- **Location:** `tests/CMakeLists.txt:171–199`
- **Severity:** Low
- **Description:** The benchmark executable is built but not registered with CTest. It cannot be run via `ctest` or CI.
- **Root Cause:** Benchmarks are considered standalone executables, not tests.
- **Recommended Fix:** Add `add_test(NAME BenchmarkSuite COMMAND MorePhiBenchmarks)` and `set_tests_properties(BenchmarkSuite PROPERTIES TIMEOUT 120 LABELS benchmark)`.

### Issue 29: `MIDIRouter.h` Does Not Document Thread Safety
- **Location:** `src/MIDI/MIDIRouter.h`
- **Severity:** Low
- **Description:** `MIDIRouter` has `setSnapshotCallback` and `setMorphCallback` that store raw function pointers + `void* context`. These are called from `processMidi()` (audio thread) but set from the UI thread. There is no atomic or lock protecting the callback pointer swap. A race between setting a new callback and the audio thread calling the old one could crash.
- **Root Cause:** Missing thread-safety documentation or atomic wrapper.
- **Recommended Fix:** Store callbacks in `std::atomic<void*>` or use a `juce::SpinLock` around callback assignment. Document that callbacks must be set before `prepareToPlay()` and not changed during audio processing.

### Issue 30: `PerformanceTests.cpp` (Integration) is a Placeholder
- **Location:** `tests/Integration/PerformanceTests.cpp`
- **Severity:** Low
- **Description:** The file is only 24 lines and only tests that `renderer.getParallelWorkerCount() == 4`. It does not measure actual performance. The file name is misleading.
- **Root Cause:** Incomplete integration test.
- **Recommended Fix:** Rename to `OfflineBatchRendererConfigTests.cpp` or add actual performance assertions (e.g., render time < threshold for a reference audio file).

---

## Positive Findings (What is Done Well)

1. **Audio Thread Safety:** `processBlock()` is marked `noexcept`, uses `juce::ScopedNoDenormals`, and avoids heap allocations after `prepare()`. The `ScopedAudioCallback` allocation guard is zero-overhead in release builds.

2. **Lock-Free Command Queue:** `LockFreeQueue` uses `juce::SpinLock` (not `std::mutex`) for multi-producer serialization, and `pop()` is truly lock-free for the single audio-thread consumer. Cache-line-aligned indices (`alignas(64)`) are used correctly.

3. **Seqlock in SnapshotBank:** `SnapshotBank` uses a seqlock pattern for audio-thread reads, with explicit `atomic_thread_fence(std::memory_order_acquire)` for correctness on weakly-ordered architectures. The state chunk read is separated from the seqlock to avoid heap allocation under the lock.

4. **Fixed-Size ParameterState:** `ParameterState` uses `std::array<float, 2048>` (no heap), and `SnapshotBank` heap-allocates the 12-slot array in the constructor to avoid stack overflow in hosts with small thread stacks (e.g., FL Studio). This is explicitly documented and correct.

5. **SIMD Optimization:** `MorphProcessor::applySmoothing()` has AVX2 and SSE paths with `MORE_PHI_USE_AVX`/`MORE_PHI_USE_SSE`. `InterpolationEngine.cpp` and `SIMDAudio.cpp` also have SIMD paths. The CMake build applies `/arch:AVX2` or `-mavx2 -msse4.1` to the relevant source files only.

6. **Windows Stack Size:** `CMakeLists.txt` correctly sets `/STACK:4194304` (4 MB) for MSVC and `--stack,4194304` for MinGW to handle plugin-in-plugin hosting in FL Studio.

7. **JUCE Macro Conflicts:** `WIN32_LEAN_AND_MEAN` and `NOMINMAX` are set globally via `add_compile_definitions()` before any JUCE headers are included. `WindowsCompat.h` provides additional safety for common macro conflicts.

8. **LatencyManager is Atomic:** All latency setters use `std::memory_order_relaxed` and `std::atomic<int>`. The getter is lock-free and safe from any thread. This is the correct pattern for latency reporting.

9. **Build Tuning Function:** `more_phi_apply_build_tuning()` is a clean, reusable CMake function that applies UTF-8, PCH tuning, `/MP`, and LTO consistently across all targets.

10. **Touch Detection with Live Edit Hold:** `processBlock()` has a sophisticated per-parameter touch detection system with cooldown counters and live-edit hold, preventing the morph from overwriting manual knob changes. The `tryEnter` on `touchStateLock_` ensures the audio thread never blocks.

11. **Test Infrastructure:** The project uses Catch2 v3, has unit tests, integration tests, and an opt-in benchmark suite. VST3 validation via `vst3_validator` and `pluginval` is integrated into CTest when the tools are found. AU validation is included on macOS.

12. **Sidechain Threshold Pre-computation:** `sidechainThresholdLinear_` is computed in `syncStateFromAPVTS()` (message thread) and stored as an atomic, so the audio thread only does a `load()` — no `std::pow` on the audio thread for sidechain processing.

13. **GranularMorphEngine Pitch LUT:** `std::pow` is used only during LUT initialization (`GranularMorphEngine.cpp:78`), not during audio processing. The comment explicitly states: "H-2 FIX: Use pre-computed LUT instead of std::pow() on audio thread."

14. **Comprehensive VST3 E2E Test:** `TestVST3ComprehensiveE2E.cpp` is an impressive 1379-line cross-reference of every documented feature against implementation, covering parameter automation, MIDI, audio signal accuracy, state persistence, GUI component existence, MCP tool availability, discrete parameter handling, and VST3 compliance. This is industry-grade validation when enabled.

---

## Summary Matrix

| Category | Count | Most Urgent |
|----------|-------|-------------|
| Critical | 4 | ThreadPool deadlock, `std::pow` on audio thread, Version.h build churn, Profiler allocation |
| High | 8 | AudioBufferPool RT mismatch, PatchJuce mutability, `catch(...)` swallowing, test gaps |
| Medium | 9 | WindowsCompat undef, SnapshotBank silent failure, monolithic files, stale backups |
| Low | 10 | Buffer clearing, dead CMake options, commented tests, naming nits |
| Positive | 14 | — |

**Recommendation:** Fix the 4 Critical issues before any release candidate. The ThreadPool `activeTasks_` leak is a genuine deadlock risk if any worker task throws. The `Version.h` macro issue will cause CI and developer build times to balloon. The `std::pow` in `EnvelopeFollower` and the `PerformanceProfiler` allocation are direct audio-thread performance regressions.

