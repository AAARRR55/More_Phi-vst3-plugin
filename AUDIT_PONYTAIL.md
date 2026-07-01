# MorePhi — ponytail-audit (repo-wide complexity & dead-code scan)

**Date:** 2026-06-30
**Scope:** whole tree. Hunt for over-engineering only — dead code, single-impl
abstractions, stdlib/platform rewrites, YAGNI, shrinkage. **Excludes**
correctness, security, performance (those are in `AUDIT_VST3_COMPLIANCE_2026-06-30.md`).
**Method:** call-site analysis (`grep -rln <symbol>` excluding the defining file),
CMake/`#ifdef` cross-reference, LOC tally. Read-only, one-shot.

Format: `<tag> <what to cut>. <replacement>. [path]` — ranked biggest cut first.

---

## Findings

1. **delete:** Orphaned `ABCompareEngine` class — 0 instantiations anywhere
   (`src/Core/ABCompareEngine.{h,cpp}`, 190 LOC). The live A/B-compare path is a
   *separate* hand-rolled inline implementation in `PluginProcessor`
   (`abCompareState_` / `captureABCompareRef` / `toggleABCompare`,
   `PluginProcessor.cpp:730-760`, called from `PluginEditor.cpp:101`). Two A/B
   implementations; one is dead. **Replacement:** nothing — the inline path is
   already complete and is the one the editor calls. Also drop the
   `#include "Core/ABCompareEngine.h"` at `PluginProcessor.cpp:14`. **Note:** the
   `kReservedSlot` comment at `PluginProcessor.cpp:4577` *references*
   `ABCompareEngine.h:30` — keep the slot reservation logic, just delete the
   class and update the comment to point at the inline impl. [src/Core/ABCompareEngine.{h,cpp}]

2. **delete:** `AgentOrchestrator.{h,cpp}` (281 LOC) — 0 production callers
   (only `tests/Unit/TestAgentOrchestrator.cpp`). The v2 VST3 audit already
   flagged it as "secondary wiring … can drift" from the canonical
   `MorePhiProcessor::startMCPServerIfNeeded()` path. **Replacement:** the
   canonical wiring in `PluginProcessor.cpp:3779-3788` (per AGENTS.md AUDIT A1).
   Delete the class + its test + the `tests/CMakeLists.txt:171` registration.
   [src/AI/Orchestrator/AgentOrchestrator.{h,cpp}, tests/Unit/TestAgentOrchestrator.cpp]

3. **delete:** `MORE_PHI_ENABLE_DATASET_V3` CMake define — a literal no-op.
   `#define`d to `1` in two places (`CMakeLists.txt:879,966`) but **zero
   `#ifdef` readers** anywhere in `src/`. `DatasetGeneratorV3` itself is live
   (called from `MCPToolsExtended.cpp:1166,1392`), so the *code* stays; only the
   dead flag + the AGENTS.md claim that it's a "deprecated compatibility flag"
   go. **Replacement:** nothing. [CMakeLists.txt:879,966; AGENTS.md "Dataset V3 is always compiled"]

4. **(WITHDRAWN 2026-06-30 during Phase 3 verification)** ~~`MORE_PHI_BUILD_HEADLESS_RENDER`
   and `MORE_PHI_ENABLE_CLANG_TIDY` dead CMake options~~. On closer inspection
   both are **legitimate opt-in gates, not dead code**: `MORE_PHI_BUILD_HEADLESS_RENDER`
   conditionally `add_subdirectory(tools/headless_mastering_render)` for the T2
   CMA-ES training teacher (target exists), and `MORE_PHI_ENABLE_CLANG_TIDY` is
   wired at `CMakeLists.txt:66-67` (`CMAKE_CXX_CLANG_TIDY`). **No action.** Keep both.

5. **yagni:** `UndoRedoManager` (123 LOC) — owned (`PluginProcessor.h:704
   undoRedoManager_`) and exposed via `getUndoRedoManager()` (`:339`), but the
   getter has **0 callers** and there are **0 UI controls** wired to it (v2 audit
   confirms "no UI references"). **Phase 3 correction:** the manager turned out to
   be a *write-only sink* — `pushSnapshotState()` was called at
   `PluginProcessor.cpp:828` on every snapshot capture, but `undo()`/`redo()` had
   zero callers (no UI button, no MCP tool, no test), so the recorded history was
   never consumed. Removed the class + member + getter + include + CMake entries
   + the `:828` push site. [src/Core/UndoRedoManager.{h,cpp}, src/Plugin/PluginProcessor.{h,cpp}]

6. **(WITHDRAWN 2026-06-30 during Phase 3)** ~~**native:** `more_phi::ThreadPool`
   → `juce::ThreadPool`~~. On inspection the migration is **not** the drop-in swap
   the `native:` tag implied: `OfflineBatchRenderer.cpp` relies on `enqueue()`
   returning `std::future<void>` (collected into `initFutures`/`renderFutures`
   vectors and `.get()`-ed for sync), but `juce::ThreadPool::addJob` does NOT
   return a `std::future` — it uses `juce::ThreadPoolJob`/`JobStatus`. Migrating
   would require rewriting the dataset renderer's completion-sync pattern
   (`juce::WaitableEvent` or atomic counters), changing concurrency semantics on
   a working (if OFF-by-default) path. **Decision: keep `more_phi::ThreadPool`.**
   The hand-rolled version is correct and the 86-LOC saving isn't worth the risk.

7. **delete:** 12 scratch build-script `.bat` files at repo root, all superseded
   by the canonical `build-ninja.bat` (configurable configure/build/tests/clean).
   Git-tracked, so this is a real cleanup: `_build_debug.bat`, `_build_lowmp.bat`,
   `_build_vcvars.bat`, `_buildlib.bat`, `_buildonly.bat`, `_ninja_build.bat`,
   `_ninja_plugin_only.bat`, `_ninja_tests.bat`, `_ninja_vst3.bat`, `_rebuild.bat`,
   `_validate.bat`, `_zcode_build.bat` (+ `_zcode_diag*.bat`, `_zcode_tests.bat`).
   **Replacement:** `build-ninja.bat` (and `cmake/Patch*.cmake` for vcvars). [repo root]

8. **delete:** ~20 stray log/output artifacts at repo root — build transcripts
   and debug dumps that don't belong in version control:
   `build_error.log`, `build_error2-5.log`, `build_result.txt`, `build_result2-4.txt`,
   `build_clean.log`, `build_log.txt`, `build_test_log.txt`, `build_vst3_log.txt`,
   `full_test.log`, `fulltest_out.log`, `serial_test.log`, `subset_test.log`,
   `mcptool_build.log`, `_build_lib.log`, `.tmp_disasm.log`, `.tmp_invoke_dry.log`,
   `.tmp_next.log`, `.tmp_panel.log`, `.tmp_panel2-6.log`, and a literal `nul`
   file (Windows null-device artifact created by a `> nul` redirect gone wrong).
   **Replacement:** add `*.log`, `build_result*.txt`, `.tmp_*.log`, `nul` to
   `.gitignore` and `git rm` them. [repo root]

9. **delete:** Committed Python bytecode `tools/__pycache__/ozone_static_recon.cpython-311.pyc`
   — build artifact, never belongs in VCS. **Replacement:** `.gitignore`
   `__pycache__/` + `*.pyc`. [tools/__pycache__/]

10. **shrink:** Dual A/B-compare surface (see finding 1) — even after deleting
    `ABCompareEngine`, the remaining inline `abCompareState_` + `abCompareActive_` +
    `abCompareHasRef_` + 4 methods (`PluginProcessor.h:285-291,826-828`) could
    collapse to a single small helper struct. Minor; only if touching this area
    for another reason. [src/Plugin/PluginProcessor.{h,cpp}]

---

## Explicitly NOT flagged (verified live / legitimate)

- **ONNX runner "proliferation"** (`NeuralMasteringController`,
  `OnnxNeuralMasteringRunner`, `SonicMasterDecisionRunner`,
  `INeuralMasteringModelRunner` + `Null`/`DeterministicBaseline` impls) — this is
  a **deliberate dual-path architecture**, not dead code. Old 63→72 path
  (`NeuralMasteringController` + `OnnxNeuralMasteringRunner`) and new 44-float
  path (`SonicMasterAnalysisEngine` + `SonicMasterDecisionRunner`) both feed
  `AutoMasteringEngine`; both are owned and wired in `PluginProcessor.h:638-657`
  / `.cpp:220,228,314`. The interface has 3 real impls (testability seam). Leave it.
- **`IPluginHostManager` / `IParameterBridge`** — single production impl each, but
  explicitly the documented testability seam (AGENTS.md "Interfaces for
  Testability"), and `IParameterBridge` has a 2nd impl (`SnapshotSelfTestBridge`).
  Not YAGNI.
- **`WaypointEngine`, `PluginHealthMonitor`, `DAWTransportForwarder`,
  `PresetSerializer` (v1) + `PresetSerializerV2`** — all have live callers in
  `src/UI`/`src/Host`/`src/Preset`. Both preset serializers are used by different
  UI panels. Leave them.
- **`MORE_PHI_BUILD_AAX`** — referenced in `Version.h:68,85` but only as changelog
  *text*, not a code `#ifdef`. The option is a real future-feature gate. Keep.

---

## Tally

```
delete:  ~520 LOC source (ABCompareEngine 190 + AgentOrchestrator 281 + UndoRedoManager 123 if chosen)
         +  12 scratch .bat + ~20 stray logs + 1 .pyc + 2 dead CMake options + 1 dead #define
native:   86 LOC (more_phi::ThreadPool → juce::ThreadPool)
yagni/shrink: as above

net: -~600 lines, -1 stdlib reimplementation (juce::ThreadPool), -2 dead CMake options, -1 dead #define, -~33 repo-root artifacts possible.
```

11. **(WITHDRAWN 2026-07-01)** ~~**native:** `wetGainScratch_`/`dryGainScratch_` `std::vector` → `std::array`~~. On inspection the `std::vector` is the correct choice — the vectors are pre-allocated once in `prepareToPlay()` and never resized on the audio thread (RT-SAFETY fix, 2026-07-01). A `std::array` would require a compile-time max block size or a large fixed stack allocation. The current design is correct: heap-allocated once at prepare time, zero allocations on the audio thread. **No action.**

---

*Scope per `ponytail-audit` skill: complexity & dead-code only. Correctness / security / performance findings are in the companion VST3 compliance report.*
