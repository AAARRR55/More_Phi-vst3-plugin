# More-Phi Developer Guide

Guide for developers who want to build, modify, or contribute to More-Phi.

---

## Table of Contents

1. [Prerequisites](#prerequisites)
2. [Building](#building)
3. [Project Structure](#project-structure)
4. [Architecture Overview](#architecture-overview)
5. [Coding Guidelines](#coding-guidelines)
6. [Debugging](#debugging)
7. [Testing](#testing)
8. [Contributing](#contributing)

---

## Prerequisites

### Required

| Tool | Version | Purpose |
|------|---------|---------|
| CMake | 3.24+ | Build system |
| C++ Compiler | C++20 | MSVC 2022, Clang 14+, GCC 11+ |
| Git | 2.x | Version control |
| JUCE | 8.0.4 | Audio framework (auto-downloaded) |

### Platform-Specific

**Windows:**
- Visual Studio 2022 or Build Tools
- Windows SDK 10+

**macOS:**
- Xcode 14+ (for AU support)
- Command Line Tools: `xcode-select --install`

**Linux:**
- GCC 11+ or Clang 14+
- Development libraries: `libfreetype6-dev`, `libx11-dev`, etc.

---

## Building

### Quick Build — Windows / MSVC (Ninja, recommended)

The Ninja generator is faster and avoids the Visual Studio generator's `.tlog`
file-lock thrash. A `build-ninja.bat` wrapper sets up the MSVC environment
(`vcvars64.bat`) and uses the VS-bundled `ninja.exe` — no separate install.
Artefacts land in `build-ninja/`.

```cmd
git clone https://github.com/your-repo/morephi.git
cd morephi

build-ninja.bat configure   :: one-time (re-fetches JUCE, ~2 min)
build-ninja.bat build       :: build the VST3 plugin (default target)
build-ninja.bat tests       :: build + run the full test suite
```

DAW-loadable VST3: `build-ninja\MorePhi_artefacts\Release\VST3\MorePhi.vst3\Contents\x86_64-win\MorePhi.vst3`

Other actions: `build-ninja.bat testonly -R "TestName" --output-on-failure`,
`build-ninja.bat target <TargetName>`, `build-ninja.bat clean`. See `AGENTS.md`
for the full list.

### Quick Build — other platforms / VS generator (fallback)

```bash
# Clone repository
git clone https://github.com/your-repo/morephi.git
cd morephi

# Configure and build
cmake -B build -S .
cmake --build build --config Release
```

### Build Configurations

Single-config build types apply directly to Ninja / single-config generators.
For the VS multi-config generator, pass `--config <Type>` to the build step.

```bash
# Debug (with symbols, no optimization) — Ninja: reconfigure with -DCMAKE_BUILD_TYPE=Debug
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug

# Release (optimized)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# RelWithDebInfo (optimized with symbols)
cmake -B build -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --config RelWithDebInfo
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `MORE_PHI_BUILD_TESTS` | ON | Build Catch2 test executable |
| `MORE_PHI_BUILD_BENCHMARKS` | OFF | Build benchmark suite |
| `MORE_PHI_ENABLE_DATASET_V3` | ON (always enabled) | Compatibility flag (no-op); Dataset V3 pipeline sources are always compiled |
| `MORE_PHI_ENABLE_SANITIZERS` | OFF | Enables ASAN + UBSAN (Clang/GCC only) |
| `MORE_PHI_MSVC_MP` | Host core count | MSVC `/MP` process count; `0` disables; ignored under Ninja |
| `MORE_PHI_COPY_PLUGIN_AFTER_BUILD` | OFF | Copies plugin to system plugin folder after build |
| `MORE_PHI_ENABLE_ONNX` | OFF | Enables ONNX Runtime for neural mastering inference (default OFF; requires download) |
| `MORE_PHI_ENABLE_LTO` | OFF | Enables release LTO for CI/release use |
| `MORE_PHI_SAFE_BUILD_MODE` | ON | Conservative build settings for local stability |
| `MORE_PHI_ENABLE_MCP_SERVER` | ON (Debug), OFF (Release) | Embedded MCP server and VST3 IPC bridge |
| `MORE_PHI_ENABLE_PROFILING` | OFF | Performance profiler instrumentation + diagnostics |
| `MORE_PHI_ENABLE_SONICMASTER_HTTP_FALLBACK` | OFF | Allow SonicMaster HTTP inference fallback |
| `MORE_PHI_BUILD_AAX` | OFF | Build AAX (Pro Tools) plugin format |
| `MORE_PHI_BUILD_HEADLESS_RENDER` | OFF | Build ctypes-callable headless mastering render harness |
| `MORE_PHI_PROD_ED25519_KEY_HEX` | (empty) | Production Ed25519 public key (CI-injected, 64 hex chars) |

```bash
cmake -B build -S . -DMORE_PHI_BUILD_TESTS=ON -DMORE_PHI_TRACK_ALLOCATIONS=ON
```

### Build Artifacts

After building, find outputs at:

| Platform | Path |
|----------|------|
| Windows VST3 | `build/MorePhi_artefacts/Release/VST3/MorePhi.vst3` |
| macOS VST3 | `build/MorePhi_artefacts/Release/VST3/MorePhi.vst3` |
| macOS AU | `build/MorePhi_artefacts/Release/AU/More-Phi.component` |

---

## Project Structure

```
morephi/
├── CMakeLists.txt          # Build configuration
├── README.md               # User documentation
├── LICENSE                 # MIT License
├── docs/                   # Documentation
│   ├── API_REFERENCE.md
│   ├── USER_GUIDE.md
│   ├── DEVELOPER_GUIDE.md
│   ├── ARCHITECTURE.md
│   └── ...
├── src/
│   ├── Plugin/             # Audio processor and editor
│   │   ├── PluginProcessor.h/cpp
│   │   └── PluginEditor.h/cpp
│   ├── Version.cpp         # Build-time version strings
    │   ├── Core/               # Audio engine components
    │   │   ├── ParameterState.h
    │   │   ├── SnapshotBank.h/cpp
    │   │   ├── InterpolationEngine.h/cpp
    │   │   ├── PhysicsEngine.h/cpp
    │   │   ├── GeneticEngine.h/cpp
    │   │   ├── MorphProcessor.h/cpp
    │   │   ├── LockFreeQueue.h
    │   │   ├── SpectralMorphEngine.h/cpp     # FFT overlap-add vocoder
    │   │   ├── GranularMorphEngine.h/cpp     # Grain pool morphing
    │   │   ├── FormantMorphEngine.h/cpp       # Formant-preserving spectral morph
    │   │   ├── VoronoiMorphEngine.h/cpp       # Voronoi-based morph
    │   │   ├── HybridBlend.h                  # Multi-engine blend coordinator
    │   │   ├── SpectralTypes.h                # Shared spectral types
    │   │   ├── ModulationEngine.h/cpp         # LFO/envelope/sequencer driver
    │   │   ├── ModulationMatrix.h/cpp         # Double-buffered route matrix (128 routes)
    │   │   ├── ModulationTypes.h              # Modulation route types
    │   │   ├── LFO.h/cpp                      # LFO with prebuilt sine LUT
    │   │   ├── StepSequencer.h/cpp            # Step sequencer mod source
    │   │   ├── EnvelopeFollower.h/cpp         # Envelope follower mod source
    │   │   ├── WaypointEngine.h               # Waypoint interpolation
    │   │   ├── GrainPool.h                    # Grain allocator
    │   │   ├── AutoMasteringEngine.h/cpp      # 12-stage mastering chain
    │   │   ├── MultibandSplitter.h/cpp        # 4-band LR splitter
    │   │   ├── MultibandDynamicsProcessor.h/cpp  # Per-band dynamics
    │   │   ├── AdaptiveEQ.h/cpp               # 32-band adaptive EQ
    │   │   ├── StereoImager.h/cpp             # Stereo image processor
    │   │   ├── HarmonicExciter.h/cpp          # Harmonic exciter
    │   │   ├── LoudnessNormalizer.h/cpp       # LUFS-targeted normalizer
    │   │   ├── BrickwallLimiter.h/cpp         # True-peak brickwall limiter
    │   │   ├── TransientShaper.h/cpp          # Transient/sustain shaper
    │   │   ├── TruePeakEstimator.h/cpp        # 4×64-tap polyphase FIR
    │   │   ├── LUFSMeter.h/cpp                # BS.1770-4 LUFS meter
    │   │   ├── MeterWindowAccumulator.h/cpp   # Sliding window for meters
    │   │   ├── MSMatrix.h                     # Mid/Side encode/decode
    │   │   ├── StereoFieldAnalyzer.h/cpp      # Stereo field analysis
    │   │   ├── TonalBalanceExtractor.h        # 8-band EQ balance extraction
    │   │   ├── RealtimeSpectrumAnalyzer.h/cpp # Real-time FFT analysis
    │   │   ├── AudioCaptureRing.h             # Rate-proportional capture ring
    │   │   ├── LatencyManager.h               # Latency compensation
    │   │   ├── OversamplingWrapper.h          # Polyphase oversampling
    │   │   ├── SIMDAudio.h/cpp                # SIMD operations
    │   │   ├── ThreadPool.h/cpp               # General thread pool
    │   │   ├── PerformanceProfiler.h/cpp      # RAII section profiler
    │   │   ├── MorePhiDiagnostics.h/cpp      # 250ms watchdog timer
    │   │   ├── UndoRedoManager.h/cpp          # Undo/redo stack
    │   │   ├── AudioBufferPool.h/cpp          # Pre-allocated buffer pool
    │   │   ├── ABCompareEngine.h/cpp          # A/B compare
    │   │   ├── MonoCompatibilityChecker.h     # Mono compatibility check
    │   │   ├── ParameterDistributionStore.h   # Param distribution tracking
    │   │   ├── ParameterClassifier.h/cpp     # Param type classification
    │   │   ├── DiscreteParameterHandler.h/cpp # Discrete param snapping
    │   │   ├── TransientDetector.h            # Transient detection
    │   │   └── NeuralMastering*.{h,cpp}       # Neural mastering types/safety
    │   ├── Host/               # Plugin hosting
    │   │   ├── PluginHostManager.h/cpp
    │   │   └── ParameterBridge.h/cpp
    │   ├── AI/                 # Embedded MCP Server & Core AI Integrations
    │   │   ├── MCPServer.h/cpp
    │   │   ├── MCPToolHandler.h/cpp
    │   │   ├── StandaloneMcp/    # Isolated STDIO workflows bridging headless contexts
    │   │   ├── Agents/           # Multi-agent orchestration layer
    │   │   │   ├── AgentRuntime.h/cpp, AgentRegistry.h/cpp, PriorityScheduler.h/cpp
    │   │   │   ├── Conductor/     # ConductorAgent
    │   │   │   ├── Agents/       # 6 specialist agents (Analysis, Optimization, Creative, RealtimeControl, QualitySafety, Memory)
    │   │   │   ├── Blackboard/   # BlackboardBridge (typed pub/sub)
    │   │   │   ├── Tooling/      # DefaultToolInvoker
    │   │   │   ├── Logging/      # StructuredAgentLogger, NullAgentLogger
    │   │   │   └── Llm/          # ILlmClient, RestLlmClient, DeterministicFallbackLlmClient
    │   │   ├── Dataset/          # Machine learning pipeline tools
    │   │   ├── SonicMaster/     # Neural mastering analysis/decode/runner
    │   │   └── Orchestrator/     # Agent orchestration facade
    │   │       ├── AgentOrchestrator.h/cpp
    │   │       ├── EcosystemConfig.h/cpp
    │   │       ├── SecurityValidator.h/cpp
    │   │       └── McpProtocol.h/cpp
    │   ├── MIDI/               # MIDI processing (wait-free CC routing)
    │   │   └── MIDIRouter.h/cpp
    │   ├── Preset/             # State persistence
    │   │   ├── PresetSerializer.h/cpp
    │   │   ├── PresetSerializerV2.h/cpp
    │   │   ├── PresetLibrary.h/cpp
    │   │   └── PresetEntry.h
    │   ├── Licensing/          # License key management & verification
    │   │   ├── LicenseManager.h/cpp
    │   │   ├── LicenseVerifier.h/cpp
    │   │   ├── LicenseKey.h/cpp
    │   │   ├── LicenseTypes.h/cpp
    │   │   ├── LicenseEnvelopeCrypto.h/cpp
    │   │   ├── Ed25519Verifier.h/cpp
    │   │   ├── ActivationClient.h/cpp
    │   │   ├── MachineFingerprint.h/cpp
    │   │   ├── SecureLicenseStore.h/cpp
    │   │   └── SigningKeys.h/cpp
    │   ├── Tools/              # Headless host & utilities
    │   │   └── HeadlessHost/HeadlessHostMain.cpp
    │   └── UI/                 # User interface
│       ├── MorePhiLookAndFeel.h/cpp
│       ├── MorphPad.h/cpp
│       ├── SnapFader.h/cpp
│       └── ...
└── tests/
    ├── DAW/                # DAW compatibility test plans
    ├── Integration/        # Integration tests
    ├── Performance/        # Benchmark suite
    └── Unit/               # Unit tests
```

---

## Architecture Detailed Overview

### Sub-System Domain Responsibility

To sustain real-time capabilities alongside advanced AI components, More-Phi separates concerns into rigid domains:

- **AI & CLI Integration**: Integrates a highly constrained embedding of MCP bridging LLMs to VST frameworks. Operates via the dedicated `MCPServer` relaying calls sequentially via `LockFreeQueue` bridges to protect audio thread operations. It additionally incorporates Dataset tooling for bulk AI synthetic generation completely unlinked from UI blocks.
- **Core DSP & MIDI**: Functions as the pure mathematically isolated root logic path spanning the `PhysicsEngine` bounding logic and Interpolations mapping states to discrete normalized values, completely divorced from `std` vector re-allocations or heap boundaries.
- **Host Abstractions & Presets**: Responsible exclusively for integrating and bounding third-party opaque states (VST3 components), ensuring they are predictably managed, suspended when unstable, and perfectly cached into structured JSON `PresetSerializer` mappings for disk retrieval without blocking operations in real-time execution.
- **Plugin Runtime & Tools**: Maintains active integration and translation of graphical input arrays via decoupled visualization tools such as the `MorphPad` executing render cycles optimized for `juce::Graphics`.

### Audio Thread Safety

More-Phi follows strict real-time audio guidelines:

1. **Zero allocations in processBlock** - All memory pre-allocated in prepareToPlay
2. **Lock-free communication** - SPSC queues for thread messaging
3. **Atomic operations** - For simple state sharing between threads
4. **No blocking calls** - Never block in audio thread

```
┌──────────────┐     Lock-Free      ┌──────────────┐
│   UI Thread  │ ◄───────────────► │ Audio Thread │
│              │      Queue         │              │
│  MorphPad    │                    │ processBlock │
│  Sliders     │    Atomics         │              │
│  MCP Server  │ ◄───────────────► │ MorphProcessor│
└──────────────┘                    └──────────────┘
       │                                    │
  Timer callbacks                      LockFreeQueue
       │                                    │
┌──────────────┐                    ┌──────────────┐
│BlackboardPump│                    │ ParameterBr. │
│  (50ms poll) │                    │ (hosted plug)│
└──────┬───────┘                    └──────────────┘
       │
┌──────────────┐
│Agent Workers │  2 scheduler threads (PriorityScheduler)
│  (agents)    │  Never on audio thread
└──────────────┘
```

### Component Interaction

```
                    ┌─────────────────┐
                    │ PluginProcessor │
                    │   (Central)     │
                    └────────┬────────┘
                             │
         ┌───────────────────┼───────────────────┐
         │                   │                   │
         ▼                   ▼                   ▼
┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐
 │ PluginHostManager│ │  MorphProcessor │ │ AgentOrchestrator│
 │                 │ │                 │ │                 │
 │ - Hosts VST3/AU │ │ - Interpolation │ │ - Wires runtime │
 │ - ParameterMap  │ │ - Physics       │ │ - EcosystemCfg  │
 └────────┬────────┘ │ - Smoothing     │ │ - SecurityVal   │
          │          └────────┬────────┘ └────────┬────────┘
          │                   │                   │
          ▼                   ▼                   ▼
┌─────────────────────────────────────────────────────────────┐
│                      SnapshotBank                           │
│                   (12-slot state storage)                   │
└─────────────────────────────────────────────────────────────┘
                             │
                             ▼
                    ┌─────────────────┐
                    │   MCPServer     │
                    │ - JSON-RPC 2.0  │
                    │ - Tool dispatch │
                    └─────────────────┘
```

> **AgentOrchestrator** (v3.4.0+): A single-initialization facade in `src/AI/Orchestrator/` that coordinates the wiring between `PluginProcessor`, the agent runtime, and `MCPServer`. It loads `EcosystemConfig` (unified JSON configuration), sets up `SecurityValidator` (message sanitization, auth, rate limiting), and registers `McpProtocol` explicit JSON-RPC 2.0 schemas (`McpRequest`, `McpResponse`, `McpNotification`, `McpError`). All orchestrator components are compiled into the `MorePhi` shared-code target.

### Data Flow

```
1. User Input (UI/MIDI/MCP)
         │
         ▼
2. Store to Atomics/Queue
         │
         ▼
3. processBlock reads atomics
         │
         ▼
4. MorphProcessor computes interpolation
         │
         ▼
5. ParameterBridge applies to hosted plugin
         │
         ▼
6. Hosted plugin processes audio
         │
         ▼
7. Audio output
```

---

## Coding Guidelines

### C++ Style

- **C++20** features allowed (concepts, ranges, etc.)
- **Namespaces:** All code in `namespace more_phi {}`
- **Naming:**
  - `PascalCase` for types
  - `camelCase_` for member variables (trailing underscore)
  - `UPPER_CASE` for constants
  - `camelCase` for functions

### Header Style

```cpp
/*
 * More-Phi — Module/FileName.h
 * Brief description of the file.
 */
#pragma once

#include <standard>
#include <libraries>

#include "ProjectHeaders.h"

namespace more_phi {

class ClassName
{
public:
    explicit ClassName(Dependency& dep);
    ~ClassName();

    // Public interface
    void publicMethod();

private:
    // Implementation details
    void privateMethod_();

    Dependency& dependency_;
    int memberVariable_ = 0;
};

} // namespace more_phi
```

### Audio Thread Rules

```cpp
// ✅ CORRECT: Pre-allocate in prepareToPlay
void prepareToPlay(double sr, int blockSize)
{
    buffer_.resize(2048);  // Allocate once
}

void processBlock(AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    // Use pre-allocated buffer - no allocations here
}

// ❌ WRONG: Allocate in processBlock
void processBlock(AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    buffer_.resize(buffer.getNumSamples());  // DON'T DO THIS
}
```

**Additional rules:**
- Mark audio-thread functions `noexcept` wherever exceptions are impossible.
- Never use `jassert` in the audio thread; `jassertfalse` is acceptable only in debug builds.
- Avoid non-real-time-safe functions like `std::pow()` in the audio thread. Pre-compute coefficients on the message/setting thread where possible, or use log-base exponentiation fallbacks (like `std::exp(numSamples * logBase)`).

### Thread Communication


```cpp
// ✅ CORRECT: Use atomics for simple values
std::atomic<float> morphX_{0.5f};
morphX_.store(newX, std::memory_order_relaxed);

// ✅ CORRECT: Use lock-free queue for commands
LockFreeQueue<ParamCommand, 512> commandQueue_;
commandQueue_.push({index, value});

// ❌ WRONG: Use mutex in audio thread
std::mutex mtx;  // DON'T lock this in processBlock
```

---

## Debugging

### Debug Output

Enable debug logging:

```cpp
#if JUCE_DEBUG
    DBG("More-Phi: " << value);
#endif
```

### Allocation Tracking

Per-process-block allocations are prevented at compile time by the zero-alloc
contract. To verify no heap operations occur during audio processing, enable
profiling and check the `processBlock_total` section:

```bash
cmake -B build -S . -DMORE_PHI_ENABLE_PROFILING=ON
```

### Common Issues

**Plugin crashes on load:**
- Check stack size (FL Studio needs 4MB)
- Verify no exceptions in constructor

**Audio dropouts:**
- Check for allocations in processBlock
- Verify SIMD alignment

**MCP connection refused:**
- Check if port 30001 is in use: `netstat -an | grep 30001`
- Verify firewall settings
- Verify the instance has not been evicted (idle timeout / zombie cleanup)

### IDE Setup

**Visual Studio:**
1. Open `build/More-Phi.sln`
2. Set More-Phi as startup project
3. Configure debugging to launch DAW

**CLion:**
1. Open CMakeLists.txt
2. Configure toolchain
3. Set run target

**Xcode:**
1. Open generated Xcode project
2. Configure scheme for debugging

---

## Testing

### Running Tests

Windows / MSVC (Ninja, recommended): `build-ninja.bat tests` builds and runs the
full suite in `build-ninja/`. For a subset: `build-ninja.bat testonly -R "TestName" --output-on-failure`.

Generic cmake (other platforms / VS generator fallback):

```bash
# Build tests
cmake -B build -S . -DMORE_PHI_BUILD_TESTS=ON
cmake --build build --parallel

# Run all wired tests
ctest --test-dir build --output-on-failure

# Run benchmarks
./build/tests/More-PhiBenchmarks
```

> **Note:** `TestGranularEngine.cpp` and `TestSpectralEngine.cpp` now exercise real production code rather than mocks. `TestComprehensiveE2E.cpp` is enabled in the default test suite.

### Unit Test Pattern

```cpp
#include <catch2/catch_test_macros.hpp>
#include "Core/InterpolationEngine.h"

TEST_CASE("InterpolationEngine::compute1D", "[core]")
{
    SnapshotBank bank;
    std::vector<float> output(256);

    SECTION("returns early for empty bank")
    {
        InterpolationEngine::compute1D(0.5f, bank, output);
        // output should be unchanged
    }

    SECTION("interpolates between two snapshots")
    {
        // Setup bank with two snapshots
        // Test interpolation at various positions
    }
}
```

### Performance Benchmarks

Located in `tests/Performance/BenchmarkSuite.cpp`:

- Scalar vs SIMD interpolation
- Physics update timing
- Full realtime simulation
- Deterministic RNG: uses `std::mt19937` instead of `rand()` for reproducible random data

---

## Contributing

### Pull Request Process

1. Fork the repository
2. Create a feature branch: `git checkout -b feature/my-feature`
3. Make changes following coding guidelines
4. Add tests for new functionality
5. Run all tests: `ctest --test-dir build`
6. Submit PR with description

### Commit Message Format

```
<Component>: Brief description

- Detailed change 1
- Detailed change 2

Refs: #issue-number
```

### Code Review Checklist

- [ ] No allocations in audio thread
- [ ] Thread-safe communication used correctly
- [ ] Memory barriers placed appropriately
- [ ] Tests added for new functionality
- [ ] Documentation updated
- [ ] Build succeeds on all platforms

---

## Architecture Decisions

### ADR-001: JUCE 8.x

**Decision:** Use JUCE 8.x for the audio framework.

**Rationale:**
- Industry standard for audio plugins
- Cross-platform (Windows, macOS, Linux)
- Built-in plugin hosting support
- Active maintenance

### ADR-002: Lock-Free Communication

**Decision:** Use lock-free queues instead of mutexes for thread communication.

**Rationale:**
- Mutexes can cause priority inversion
- Lock-free guarantees real-time safety
- Well-understood SPSC patterns

### ADR-003: MCP over OSC

**Decision:** Use MCP (Model Context Protocol) instead of OSC for external control.

**Rationale:**
- Better AI assistant integration
- JSON-RPC 2.0 is well-specified
- Growing ecosystem of MCP tools

---

## What's New in v3.4.1

### Multi-Agent Orchestration Layer

The v3.4.1 release builds on the Agent Orchestration Layer introduced in v3.4.0, located in `src/AI/Agents/` and `src/AI/Orchestrator/`. These provide structured initialization, configuration, and security mediation between the audio processor and the MCP server, along with a full multi-agent runtime.

**New components:**

- **`AgentOrchestrator`** — Single-initialization facade that wires `MorePhiProcessor` → `AgentRuntime` → `MCPServer`. Centralizes startup sequencing and runtime coordination.
- **`EcosystemConfig`** — Unified JSON configuration for plugin settings, agent behavior, MCP server options, and security policies. Replaces ad-hoc configuration scattered across subsystems.
- **`SecurityValidator`** — MCP message sanitization, authentication validation, and rate limiting. Protects the audio thread from malformed or excessive JSON-RPC traffic.
- **`McpProtocol`** — Explicit JSON-RPC 2.0 message schemas (`McpRequest`, `McpResponse`, `McpNotification`, `McpError`). Provides type-safe parsing and validation for all MCP traffic.

**Project structure changes:**
- New directory: `src/AI/Orchestrator/` containing the four component pairs listed above.
- New directory: `src/AI/Agents/` containing the multi-agent orchestration runtime, 7 specialist agents (Conductor + 6 specialists), blackboard bridge, tooling, logging, and LLM client seam.
- Experimental Ozone/iZotope research artifacts relocated to `research/` to keep the main source tree focused on production code.

**Neural mastering improvements (SonicMaster):**
- ONNX model embedded in binary via JUCE `BinaryData` — no runtime file I/O
- Capture ring now rate-proportional (`8.0 * sampleRate`, clamped `[2×44100, 32×192000]`) and **eagerly allocated** in `prepare()` (CAPTURE-DECOUPLE fix)
- Resampling uses `resamplePolyphase` (not linear interpolation)
- Mono capture support (`channelCount == 1`)
- Plan application uses pending-plan atomic-flag pattern (no `callAsync`)
- Neural path verification via `OzonePlanApplicator` read-back + plan boundary markers
- Analysis transition guard (`notifyHostedParameterChanged()` + ring flush)
- EQ gain normalization capped at ±12 dB
- Action ledger cap raised to 4096 (`kLedgerMaxTransactions`)
- `mastering.render_batch` dry-run populates real `lufs_error` per candidate
- `flushCaptureRing()` on hosted plugin load
- Genre-conditioned priors: target LUFS + tonal-balance residual via `GenreMasteringProfile` / `TonalBalanceExtractor`
- Pluggable ONNX genre model: `GenreClassifier::loadModel()` with heuristic fallback

**Agent layer improvements:**
- 7 MCP tools: `agents.list`, `agents.run_goal`, `agents.run_task`, `agents.run_status`, `agents.run_cancel`, `agents.blackboard.recent`, `agents.set_autonomy`
- `RestLlmClient` as production LLM transport for ConductorAgent when API key configured
- PriorityScheduler 4-level multi-queue with O(1) operations and starvation guard

**Build impact:**
- Orchestrator sources are compiled into the `MorePhi` shared-code target by default (no new CMake option required).
- The layer operates on the message and MCP threads only; it does not introduce any new audio-thread obligations.

---

## Resources

- [JUCE Documentation](https://docs.juce.com/)
- [Audio Plugin Developer Guide](https://steinbergmedia.github.io/vst3_dev_portal/pages/)
- [Real-Time Audio Programming](https://www.rossbencina.com/code/real-time-audio-programming-101-time-waits-for-nothing)
- [MCP Specification](https://modelcontextprotocol.io/)

---

*Updated 2026-06-27.*
