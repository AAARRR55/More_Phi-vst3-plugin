# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

More-Phi is a JUCE 8-based C++20 VST3/AU audio plugin that hosts other plugins and morphs between parameter snapshots using parameter interpolation, physics modes, audio-domain engines, genetic breeding, and AI control through MCP. The project version in CMake is 3.3.0.

Everything is in the `more_phi` namespace unless a submodule states otherwise. The plugin entry point is `MorePhiProcessor` (`src/Plugin/PluginProcessor.*`), which owns subsystem instances directly; avoid adding singletons except for the existing cross-instance `InstanceRegistry`.

## Build Commands

```bash
# Generic configure/build with tests
cmake -B build -S . -DMORE_PHI_BUILD_TESTS=ON
cmake --build build --config Release --parallel 2

# Debug build
cmake -B build -S . -DMORE_PHI_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug --parallel 2

# Run all tests from a generic build directory
ctest --test-dir build --build-config Release --output-on-failure --parallel 4

# Run a single Catch2/CTest test by name or regex
ctest --test-dir build --build-config Release -R "TestName" --output-on-failure

# Build standalone tools only
cmake --build build --config Release --target MorePhiCLI MorePhiMcpServer

# Build benchmarks (opt-in)
cmake -B build-bench -S . -DMORE_PHI_BUILD_TESTS=ON -DMORE_PHI_BUILD_BENCHMARKS=ON
cmake --build build-bench --config Release --target MorePhiBenchmarks

# Sanitizer build (Clang/GCC only)
cmake -B build-asan -S . -DMORE_PHI_BUILD_TESTS=ON -DMORE_PHI_ENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan --config Debug --parallel 2
ctest --test-dir build-asan --build-config Debug --output-on-failure
```

Windows local build presets are configured for stability on mid-range machines:

```bash
# Safe local plugin build; tests are disabled in this preset
cmake --preset windows-msvc-safe
cmake --build --preset windows-safe --parallel 2

# Single-job fallback if the machine is unstable
cmake --build --preset windows-single --parallel 1

# Debug build with presets
cmake --preset windows-msvc-debug
cmake --build --preset windows-debug --parallel 2

# Full Windows release/test configure preset
cmake --preset windows-msvc-release
cmake --build build/windows-msvc-release --config Release --parallel 2
ctest --preset windows-tests
```

Useful CMake options:

- `MORE_PHI_BUILD_TESTS` (default `ON`) — adds `tests/`.
- `MORE_PHI_COPY_PLUGIN_AFTER_BUILD` (default `OFF`) — copies built plugin to the system plugin folder.
- `MORE_PHI_ENABLE_SANITIZERS` (default `OFF`) — ASAN/UBSAN for Clang/GCC Debug builds.
- `MORE_PHI_SAFE_BUILD_MODE` (default `ON`) — conservative MSVC linker/build settings.
- `MORE_PHI_MSVC_MP` (default `2`) — MSVC `/MP` worker count; set `0` to disable.
- `MORE_PHI_ENABLE_LTO` (default `OFF`) — Release LTO, intended for CI/release rather than local stability.
- `MORE_PHI_BUILD_COMPREHENSIVE_E2E` (default `OFF`) — adds generated comprehensive VST3 E2E suite; tracks experimental APIs.
- `MORE_PHI_ENABLE_DATASET_V3` is a deprecated compatibility flag; Dataset V3 sources are always compiled.

Dependencies are fetched by CMake: JUCE 8.0.4, nlohmann/json 3.11.3, and Catch2 v3.4.0.

## Build Targets and Executables

- `MorePhi` — JUCE plugin target; emits VST3 on all platforms and AU on macOS.
- `MorePhiCLI` — offline hosted-plugin dataset/render CLI from `src/CLI/main.cpp`.
- `MorePhiMcpCore` — static core for the standalone MCP server.
- `MorePhiMcpServer` — stdio JSON-RPC MCP executable from `src/AI/StandaloneMcp/StandaloneMcpMain.cpp`.
- `MorePhiTests` — active Catch2 unit/integration test executable.
- `MorePhiMcpServerTests` — standalone MCP server tests.
- `MorePhiBenchmarks` — optional benchmark target when `MORE_PHI_BUILD_BENCHMARKS=ON`.

The CLI usage string is `morephi-dataset --plugin <path.vst3> --input <dry.wav> [options]`, with `--list-params`, `--dry-run`, `--config-dir`, `--duration`, `--variations`, and `--workers` options.

## Architecture

### Layer Responsibilities

| Layer | Key Classes / Targets | Role |
|-------|------------------------|------|
| `src/Plugin/` | `MorePhiProcessor`, `MorePhiEditor` | JUCE plugin entry point, APVTS parameter layout, audio lifecycle, state persistence, subsystem ownership |
| `src/Core/` | `MorphProcessor`, `InterpolationEngine`, `PhysicsEngine`, `GeneticEngine`, `SnapshotBank`, modulation/audio-domain/mastering classes | Real-time-safe morphing, DSP, analysis, modulation, spectral/granular/formant engines, mastering processors |
| `src/Host/` | `PluginHostManager`, `ParameterBridge`, `PluginScanner`, `IPluginHostManager` | Hosted VST3/AU lifecycle, parameter reads/writes, test seams |
| `src/AI/` | `MCPServer`, `MCPToolHandler`, `MCPToolsExtended`, `AIAssistant`, `TokenOptimizer`, Ozone/LLM/dataset classes | Embedded TCP MCP server, AI tools, LLM settings, Ozone Track Assistant integration, dataset generation |
| `src/AI/StandaloneMcp/` | `StandaloneMcpServer`, `OzonePluginBackend`, `IZotopeIPCDiscovery`, `JsonRpc` | Stdio MCP executable for standalone Ozone/iZotope workflows |
| `src/MIDI/` | `MIDIRouter` | Snapshot note triggers and CC morph routing |
| `src/Preset/` | `MetaPresetManager`, `PresetSerializer`, `PresetLibrary`, `CloudSyncClient` | Meta-preset and preset-library persistence |
| `src/UI/` | `MorphPad`, `SnapFader`, `SnapshotRing`, panels | JUCE editor components and message-thread interaction |

### Audio Processing Pipeline

`MorePhiProcessor::processBlock()` is the audio-thread entry point:

```text
sync APVTS atomics
→ drain LockFreeQueue<ParamCommand, 8192>
→ MIDIRouter note/CC handling
→ SnapshotBank recall/capture data paths
→ MorphProcessor: physics → interpolation → discrete snap → smoothing
→ ModulationEngine and optional audio-domain engines
→ ParameterBridge applies normalized values
→ PluginHostManager processes hosted plugin audio
```

Core real-time constraints: do not allocate, lock, block, perform I/O, or throw on the audio path. Pre-size buffers in `prepareToPlay()` or subsystem `prepare()` methods. Message/MCP/UI code should communicate to audio through atomics, lock-free queues, or existing handoff mechanisms.

### Thread Domains

- **Audio thread**: `processBlock()`, `MorphProcessor::process()`, interpolation/physics/modulation/audio-domain processing, command queue drain, hosted plugin parameter application.
- **Message thread**: UI components, JUCE timers, deferred hosted-plugin loading, MCP startup trigger, full-state recall maintenance, Ozone parameter-map refresh, audio-domain reconfiguration.
- **MCP/connection threads**: Embedded `MCPServer` TCP JSON-RPC handling, auth/rate limiting, tool dispatch; parameter changes must enqueue back to the processor rather than touching audio state directly.
- **Background workers**: `ThreadPool`, `ChainPlanExecutor`, dataset/offline rendering work.
- **Standalone MCP process**: `MorePhiMcpServer` runs stdio JSON-RPC independently of the plugin instance.

### Key Concurrency Primitives

- `SnapshotBank` uses a seqlock-style reader path for audio-thread reads; writers serialize with `juce::SpinLock`.
- `LockFreeQueue<ParamCommand, 8192>` transfers UI/MCP/assistant parameter edits to the audio thread.
- APVTS-backed and internal morph/physics flags use relaxed atomics for simple UI/MCP-to-audio state.
- `ModulationMatrix` publishes route buffers with a double-buffered atomic index.
- Touch/live-edit hold state prevents morph output from immediately overwriting manual hosted-plugin edits.
- Hosted plugin exclusive use is mediated through `PluginHostManager`; do not bypass it for cross-thread plugin access.

### State Persistence

`getStateInformation()` / `setStateInformation()` serialize APVTS XML, snapshot-bank values, hosted plugin description, hosted plugin opaque state chunks, and modulation routes. Hosted plugin reload after state restore is deferred through `juce::Timer` retries to satisfy DAW/JUCE threading constraints.

### AI and MCP

The embedded MCP server is tied to a `MorePhiProcessor` instance. It uses localhost TCP ports allocated by `InstanceRegistry` starting at 30001, per-instance identity and bearer-token authentication, and `TokenOptimizer` request limits. Tool handlers should return JSON-RPC 2.0 compatible responses and use `enqueueParameterSet()` / `enqueueParameterState()` for parameter writes.

Ozone Track Assistant support flows through `OzoneParameterMap`, `OzonePlanApplicator`, `AutoMasteringEngine`, and `ChainPlanExecutor`; Ozone-specific parameter changes are skipped safely when no Ozone 11 mapping is available.

The standalone MCP server is a separate stdio server under `src/AI/StandaloneMcp/`, sharing host/plugin support through `MorePhiMcpCore` instead of the plugin's embedded TCP server.

### Dataset Generation

Dataset code lives under `src/AI/Dataset/` and is compiled into the plugin/CLI. V2 is the sequential pipeline (`DatasetGeneratorV2`, sampler, audio library, plugin chain, render pipeline, feature extraction, metadata, validation, organizer). V3 is the modular async pipeline (`DatasetGeneratorV3`, scheduling, worker pool, monitoring, checkpointing, watchdog) and is always compiled despite the deprecated compatibility option.

### Audio-Domain and Mastering Engines

In addition to parameter morphing, `MorePhiProcessor` owns spectral, granular, formant, VAE-stub, oversampling, latency, and hybrid-blend components. Mastering processors include multiband dynamics, adaptive EQ, stereo imaging, excitation, true-peak/loudness analysis, normalization, limiter, realtime spectrum/stereo analyzers, and `AutoMasteringEngine`.

## Tests

Tests use Catch2 v3 and link against the JUCE shared-code target. Active tests are registered with `catch_discover_tests()` from `tests/CMakeLists.txt`:

- `MorePhiTests`: unit tests plus plugin lifecycle, MCP, and dataset integration tests.
- `MorePhiMcpServerTests`: standalone stdio MCP server tests.
- Optional validator tests are added when `vst3_validator` (Windows) or `auval` (macOS) is available; `pluginval` validation (strictness level 5) is also registered when the `pluginval` executable is on `PATH`.
- `MorePhiBenchmarks` is built only with `MORE_PHI_BUILD_BENCHMARKS=ON`.

Legacy Morphy-era tests under old paths are intentionally excluded from active CMake targets. Tests compile with `MORE_PHI_TEST_MODE=1`; plugin-linked tests use `JUCE_STANDALONE_APPLICATION=0`, while standalone MCP tests use `JUCE_STANDALONE_APPLICATION=1`.

## Platform Notes

CMakePresets.json defines cross-platform configure/build/test presets beyond the Windows ones listed above: `macos-universal-release`, `macos-arm64-debug`, `macos-x64-debug`, `linux-gcc-release`, `linux-clang-asan`, and CI variants `ci-windows` / `ci-macos` with corresponding build and test presets. The `windows-nmake-release` / `windows-nmake-single` presets use NMake Makefiles and require a VS Developer Command Prompt.

- Windows builds set a 4 MB stack for plugin-in-plugin host compatibility.
- `cmake/PatchJuceForMSVC.cmake` patches JUCE headers that conflict with Windows macros.
- Windows local builds default to conservative `/MP2`, safe linker threading, and no LTO; use `docs/BUILD_STABILITY_GUIDE.md` if builds freeze or exhaust resources.
- AU is built only on macOS; non-macOS builds emit VST3 only.
- `ParameterState` uses fixed arrays for up to 2048 parameters; `SnapshotBank` heap-allocates its 12-slot array to avoid host stack pressure.
- SIMD tuning is scoped to `src/Core/SIMDAudio.cpp` (`/arch:AVX2` on MSVC or `-mavx2 -msse4.1` on Clang/GCC when x86 is detected).

<!-- SPECKIT START -->
For additional context about technologies to be used, project structure,
shell commands, and other important information, read the current plan
<!-- SPECKIT END -->
