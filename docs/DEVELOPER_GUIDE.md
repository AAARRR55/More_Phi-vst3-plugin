# MorphSnap Developer Guide

Guide for developers who want to build, modify, or contribute to MorphSnap.

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

### Quick Build

```bash
# Clone repository
git clone https://github.com/your-repo/morphsnap.git
cd morphsnap

# Configure and build
cmake -B build -S .
cmake --build build --config Release
```

### Build Configurations

```bash
# Debug (with symbols, no optimization)
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
| `MORPHSNAP_TRACK_ALLOCATIONS` | OFF | Enable allocation tracking in debug |
| `MORPHSNAP_BUILD_TESTS` | OFF | Build test executables |
| `MORPHSNAP_BUILD_BENCHMARKS` | OFF | Build benchmark suite |

```bash
cmake -B build -S . -DMORPHSNAP_BUILD_TESTS=ON -DMORPHSNAP_TRACK_ALLOCATIONS=ON
```

### Build Artifacts

After building, find outputs at:

| Platform | Path |
|----------|------|
| Windows VST3 | `build/MorphSnap_artefacts/Release/VST3/MorphSnap.vst3` |
| macOS VST3 | `build/MorphSnap_artefacts/Release/VST3/MorphSnap.vst3` |
| macOS AU | `build/MorphSnap_artefacts/Release/AU/MorphSnap.component` |

---

## Project Structure

```
morphsnap/
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
│   ├── Core/               # Audio engine components
│   │   ├── ParameterState.h
│   │   ├── SnapshotBank.h/cpp
│   │   ├── InterpolationEngine.h/cpp
│   │   ├── PhysicsEngine.h/cpp
│   │   ├── GeneticEngine.h/cpp
│   │   ├── MorphProcessor.h/cpp
│   │   ├── LockFreeQueue.h
│   │   └── AllocationTracker.h
│   ├── Host/               # Plugin hosting
│   │   ├── PluginHostManager.h/cpp
│   │   ├── ParameterBridge.h/cpp
│   │   └── PluginScanner.h/cpp
│   ├── AI/                 # MCP server
│   │   ├── MCPServer.h/cpp
│   │   └── MCPToolHandler.h/cpp
│   ├── MIDI/               # MIDI processing
│   │   └── MIDIRouter.h/cpp
│   ├── Preset/             # State persistence
│   │   ├── MetaPresetManager.h/cpp
│   │   └── PresetSerializer.h/cpp
│   └── UI/                 # User interface
│       ├── MorphSnapLookAndFeel.h/cpp
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

## Architecture Overview

### Audio Thread Safety

MorphSnap follows strict real-time audio guidelines:

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
 │ PluginHostManager│ │  MorphProcessor │ │   MCPServer     │
 │                 │ │                 │ │                 │
 │ - Hosts VST3/AU │ │ - Interpolation │ │ - JSON-RPC 2.0  │
 │ - ParameterMap  │ │ - Physics       │ │ - Tool dispatch │
 └────────┬────────┘ │ - Smoothing     │ └────────┬────────┘
          │          └────────┬────────┘          │
          │                   │                   │
          ▼                   ▼                   ▼
┌─────────────────────────────────────────────────────────────┐
│                      SnapshotBank                           │
│                   (12-slot state storage)                   │
└─────────────────────────────────────────────────────────────┘
```

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
- **Namespaces:** All code in `namespace morphsnap {}`
- **Naming:**
  - `PascalCase` for types
  - `camelCase_` for member variables (trailing underscore)
  - `UPPER_CASE` for constants
  - `camelCase` for functions

### Header Style

```cpp
/*
 * MorphSnap — Module/FileName.h
 * Brief description of the file.
 */
#pragma once

#include <standard>
#include <libraries>

#include "ProjectHeaders.h"

namespace morphsnap {

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

} // namespace morphsnap
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
    DBG("MorphSnap: " << value);
#endif
```

### Allocation Tracking

Enable in build:
```bash
cmake -B build -S . -DMORPHSNAP_TRACK_ALLOCATIONS=ON
```

Use in code:
```cpp
#include "Core/AllocationTracker.h"

void processBlock(...)
{
    ScopedAudioCallback guard;  // Tracks allocations
    // ... audio processing
}
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

### IDE Setup

**Visual Studio:**
1. Open `build/MorphSnap.sln`
2. Set MorphSnap as startup project
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

```bash
# Build tests
cmake -B build -S . -DMORPHSNAP_BUILD_TESTS=ON
cmake --build build --config Debug

# Run unit tests
./build/tests/Unit/MorphSnap_UnitTests

# Run integration tests
./build/tests/Integration/MorphSnap_IntegrationTests

# Run benchmarks
./build/tests/Performance/MorphSnap_Benchmarks
```

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

## Resources

- [JUCE Documentation](https://docs.juce.com/)
- [Audio Plugin Developer Guide](https://steinbergmedia.github.io/vst3_dev_portal/pages/)
- [Real-Time Audio Programming](https://www.rossbencina.com/code/real-time-audio-programming-101-time-waits-for-nothing)
- [MCP Specification](https://modelcontextprotocol.io/)
