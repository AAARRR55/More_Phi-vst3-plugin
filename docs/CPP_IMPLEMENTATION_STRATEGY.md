# Morphy - C++ Implementation Strategy
## High-Performance Audio Plugin Development Guide

Version: 1.0
Author: Morphy Audio Team
Date: 2025-02-18

---

## Table of Contents

1. [C++ Standards and Patterns](#1-c-standards-and-patterns)
2. [Build System](#2-build-system)
3. [Code Organization](#3-code-organization)
4. [Performance Optimization](#4-performance-optimization)
5. [Real-Time Safety](#5-real-time-safety)
6. [Testing Strategy](#6-testing-strategy)
7. [Code Templates](#7-code-templates)

---

## 1. C++ Standards and Patterns

### 1.1 C++20 Feature Usage

#### Concepts
Use concepts for template constraints to improve compile-time error messages:

```cpp
// DSP processing concept
template<typename T>
concept AudioSampleType = std::same_as<T, float> || std::same_as<T, double>;

// Parameter concept
template<typename T>
concept ParameterType = std::floating_point<T> || std::integral<T>;

// Usage with concepts
template<AudioSampleType T>
class DSPProcessor {
    void process(juce::AudioBuffer<T>& buffer);
};

// Constrained template function
template<AudioSampleType T>
void applyGain(juce::AudioBuffer<T>& buffer, T gain) requires std::is_arithmetic_v<T>;
```

#### Coroutines
Use coroutines for asynchronous operations (non-audio thread):

```cpp
// For MCP client communication
std::future<ConnectionResult> connectToServer(std::string_view uri) {
    auto result = co_await websocket_connector_.connect(uri);
    co_return result;
}

// For state serialization
task<void> saveStateAsync(const juce::XmlElement& state) {
    co_await io_scheduler_.schedule();
    // Save operation here
}
```

**WARNING**: Never use coroutines in the audio thread.

#### Modules
When compiler support is mature, migrate to modules:

```cpp
// Current: Traditional headers
#include "MorphingEngine.h"

// Future: Modules (when JUCE supports)
import Morphy.DSP;
import Morphy.Parameters;
```

### 1.2 RAII and Smart Pointers

#### Smart Pointer Guidelines

```cpp
// Use std::unique_ptr for exclusive ownership
class MorphyAudioProcessor {
private:
    std::unique_ptr<ParameterManager> m_parameterManager;
    std::unique_ptr<MCPClient> m_mcpClient;
public:
    MorphyAudioProcessor()
        : m_parameterManager(std::make_unique<ParameterManager>(*this, m_APVTS))
        , m_mcpClient(std::make_unique<MCPClient>()) {}
};

// Use std::shared_ptr for shared ownership
class LockFreeQueue {
private:
    std::shared_ptr<std::atomic<int>> m_counter;
};

// Use std::weak_ptr to break cycles
class AIIntegration {
private:
    std::weak_ptr<MCPClient> m_client; // Prevent circular reference
};
```

#### RAII for Resource Management

```cpp
// RAII wrapper for audio thread suspension
class AudioThreadSuspender {
public:
    AudioThreadSuspender(juce::AudioProcessor& processor)
        : m_processor(processor), m_suspended(processor.suspendProcessing(true)) {}

    ~AudioThreadSuspender() {
        m_processor.suspendProcessing(m_suspended);
    }

private:
    juce::AudioProcessor& m_processor;
    bool m_suspended;
};

// Usage
{
    AudioThreadSuspender suspender(*this);
    // Perform non-real-time safe operations
    // Automatically resumes when scope exits
}
```

### 1.3 Move Semantics Optimization

```cpp
// Move constructor for parameter snapshots
class ParameterSnapshot {
public:
    ParameterSnapshot(ParameterSnapshot&& other) noexcept
        : m_parameters(std::move(other.m_parameters))
        , m_timestamp(std::exchange(other.m_timestamp, 0))
        , m_name(std::move(other.m_name)) {}

    ParameterSnapshot& operator=(ParameterSnapshot&& other) noexcept {
        if (this != &other) {
            m_parameters = std::move(other.m_parameters);
            m_timestamp = std::exchange(other.m_timestamp, 0);
            m_name = std::move(other.m_name);
        }
        return *this;
    }

    // Delete copy operations for move-only types
    ParameterSnapshot(const ParameterSnapshot&) = delete;
    ParameterSnapshot& operator=(const ParameterSnapshot&) = delete;
};

// Perfect forwarding for factory functions
template<typename... Args>
auto createDSPProcessor(Args&&... args) {
    return std::make_unique<DSPProcessor>(std::forward<Args>(args)...);
}
```

### 1.4 Template Metaprogramming for DSP

```cpp
// Compile-time interpolation curve selection
template<InterpolationMode Mode>
struct Interpolator {
    template<AudioSampleType T>
    static T interpolate(T a, T b, T t);
};

template<>
struct Interpolator<InterpolationMode::Linear> {
    template<AudioSampleType T>
    static T interpolate(T a, T b, T t) {
        return a + t * (b - a);
    }
};

template<>
struct Interpolator<InterpolationMode::Cosine> {
    template<AudioSampleType T>
    static T interpolate(T a, T b, T t) {
        const T t2 = (T(1) - std::cos(t * T(3.14159265358979323846))) / T(2);
        return a + t2 * (b - a);
    }
};

// Usage with compile-time dispatch
template<InterpolationMode Mode, AudioSampleType T>
void processWithInterpolation(juce::AudioBuffer<T>& buffer, T t) {
    Interpolator<Mode>::interpolation_fn(buffer.getReadPointer(0),
                                        buffer.getWritePointer(0),
                                        buffer.getNumSamples(), t);
}
```

### 1.5 Constexpr for Compile-Time Optimization

```cpp
// Compile-time constants
namespace Constants {
    constexpr int MAX_SNAPSHOTS = 4;
    constexpr int FFT_ORDER = 10;
    constexpr int FFT_SIZE = 1 << FFT_ORDER; // 1024
    constexpr int SPECTRUM_SIZE = FFT_SIZE / 2;
    constexpr float PI = 3.14159265358979323846f;
    constexpr float TWO_PI = 2.0f * PI;
}

// Constexpr functions for lookup table generation
constexpr std::array<float, 512> generateSineTable() {
    std::array<float, 512> table{};
    for (size_t i = 0; i < table.size(); ++i) {
        table[i] = std::sin(2.0f * Constants::PI * i / table.size());
    }
    return table;
}

// Compile-time initialized table
static constexpr auto SINE_TABLE = generateSineTable();
```

---

## 2. Build System

### 2.1 CMake Configuration Structure

```
D:/morphy/
├── CMakeLists.txt                 # Root configuration
├── cmake/
│   ├── CompilerFlags.cmake        # Compiler-specific flags
│   ├── Dependencies.cmake         # External dependencies
│   ├── PlatformSettings.cmake     # Platform-specific settings
│   └── Testing.cmake              # Test configuration
├── src/
│   ├── CMakeLists.txt             # Source organization
│   ├── core/
│   ├── dsp/
│   ├── morphing/
│   ├── ui/
│   └── util/
└── tests/
    └── CMakeLists.txt
```

### 2.2 Multi-Target Build Configuration

```cmake
# cmake/BuildTargets.cmake
include(CMakeDependentOption)

# Format-specific options
option(MORPHY_BUILD_VST3 "Build VST3 plugin" ON)
option(MORPHY_BUILD_AU "Build Audio Unit plugin (macOS only)" ON)
option(MORPHY_BUILD_AAX "Build AAX plugin (Pro Tools)" OFF)
option(MORPHY_BUILD_STANDALONE "Build standalone application" ON)
option(MORPHY_BUILD_ALL "Build all supported formats" OFF)

# Platform-dependent options
if(APPLE)
    CMAKE_DEPENDENT_OPTION(MORPHY_BUILD_AU "Build AU" ON "NOT MORPHY_BUILD_ALL" OFF)
elseif(WIN32)
    CMAKE_DEPENDENT_OPTION(MORPHY_BUILD_AAX "Build AAX" OFF "MORPHY_BUILD_ALL" OFF)
endif()

# Multi-format aggregation
if(MORPHY_BUILD_ALL)
    set(MORPHY_BUILD_VST3 ON CACHE BOOL "Build VST3" FORCE)
    if(APPLE)
        set(MORPHY_BUILD_AU ON CACHE BOOL "Build AU" FORCE)
    endif()
endif()

# Format map for JUCE
set(MORPHY_PLUGIN_FORMATS "")
if(MORPHY_BUILD_VST3)
    list(APPEND MORPHY_PLUGIN_FORMATS "VST3")
endif()
if(MORPHY_BUILD_AU AND APPLE)
    list(APPEND MORPHY_PLUGIN_FORMATS "AU")
endif()
if(MORPHY_BUILD_STANDALONE)
    list(APPEND MORPHY_PLUGIN_FORMATS "Standalone")
endif()

# Build type configurations
set(CMAKE_CONFIGURATION_TYPES "Debug;Release;ReleaseWithDebInfo;RelWithDebInfo" CACHE STRING "" FORCE)
set(CMAKE_RELEASE_POSTFIX "" CACHE STRING "Release library suffix")

# Export compile commands for IDEs
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
```

### 2.3 Cross-Compilation Setup

```cmake
# cmake/CrossCompilation.cmake

# Toolchain files for cross-compilation
set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/cmake/toolchains/${MORPHY_TARGET_PLATFORM}.cmake" CACHE FILEPATH "Toolchain file")

# Available toolchains
# - windows-x86_64.cmake
# - windows-arm64.cmake
# - macos-universal.cmake
# - macos-arm64.cmake
# - linux-x86_64.cmake

# Universal binary configuration (macOS)
if(APPLE AND MORPHY_BUILD_UNIVERSAL)
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Architecture" FORCE)

    # Universal build helper
    function(create_universal_binary target)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND lipo -create -output
                ${CMAKE_BINARY_DIR}/bin/${target}_universal
                ${CMAKE_BINARY_DIR}/bin/${target}_x86_64
                ${CMAKE_BINARY_DIR}/bin/${target}_arm64
            COMMENT "Creating universal binary for ${target}"
        )
    endfunction()
endif()
```

### 2.4 Dependency Management

#### vcpkg Integration

```cmake
# cmake/vcpkg.cmake
find_package(vcpkg REQUIRED)

# Dependencies via vcpkg
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

# Boost for networking (MCP)
find_package(Boost REQUIRED COMPONENTS system)

# Audio-specific libraries
find_package(FFTW3 CONFIG REQUIRED)
```

#### vcpkg.json

```json
{
  "name": "morphy",
  "version": "1.0.0",
  "description": "Advanced parameter morphing engine",
  "dependencies": [
    "nlohmann-json",
    "spdlog",
    "fmt",
    "boost-system",
    "fftw3",
    {
      "name": "catch2",
      "features": ["bazel-build"]
    }
  ],
  "builtin-baseline": "2025-02-18"
}
```

### 2.5 CI/CD Pipeline Definition

#### GitHub Actions (.github/workflows/build.yml)

```yaml
name: Morphy Build Pipeline

on:
  push:
    branches: [main, develop]
  pull_request:
    branches: [main, develop]
  workflow_dispatch:

jobs:
  build-windows:
    runs-on: [windows-latest]
    strategy:
      matrix:
        build_type: [Debug, Release]
        include:
          - build_type: Release
            artifact_suffix: rel

    steps:
      - uses: actions/checkout@v3
        with:
          submodules: recursive

      - name: Setup vcpkg
        uses: lukka/run-vcpkg@v10
        with:
          vcpkgGitCommitId: 'master'
          vcpkgJsonGlob: 'vcpkg.json'

      - name: Configure CMake
        run: |
          cmake -B build -G "Visual Studio 17 2022" `
            -DCMAKE_BUILD_TYPE=${{ matrix.build_type }} `
            -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
            -DMORPHY_BUILD_VST3=ON `
            -DMORPHY_BUILD_TESTS=ON

      - name: Build
        run: cmake --build build --config ${{ matrix.build_type }} --parallel

      - name: Test
        run: ctest --test-dir build --config ${{ matrix.build_type }} --output-on-failure

      - name: Package Plugin
        run: |
          cmake --build build --config ${{ matrix.build_type }} --target package

      - name: Upload Artifacts
        uses: actions/upload-artifact@v3
        with:
          name: morphy-windows-${{ matrix.artifact_suffix }}
          path: build/Morphy_artefacts/

  build-macos:
    runs-on: [macos-latest]
    strategy:
      matrix:
        arch: [x86_64, arm64]
        build_type: [Debug, Release]

    steps:
      - uses: actions/checkout@v3

      - name: Setup vcpkg
        run: |
          brew install vcpkg
          cd /opt/homebrew
          git clone https://github.com/Microsoft/vcpkg.git || true

      - name: Configure CMake
        run: |
          cmake -B build -G "Xcode" \
            -DCMAKE_BUILD_TYPE=${{ matrix.build_type }} \
            -DCMAKE_OSX_ARCHITECTURES=${{ matrix.arch }} \
            -DMORPHY_BUILD_VST3=ON \
            -DMORPHY_BUILD_AU=ON \
            -DMORPHY_BUILD_TESTS=ON

      - name: Build
        run: cmake --build build --config ${{ matrix.build_type }} --parallel

      - name: Codesign
        run: codesign --force --sign - build/Morphy_artefacts/Release/AU/Morphy.component

  build-linux:
    runs-on: [ubuntu-latest]
    container:
      image: ghcr.io/juce-framework/linux-builder:latest

    steps:
      - uses: actions/checkout@v3

      - name: Build
        run: |
          cmake -B build -G "Unix Makefiles" \
            -DCMAKE_BUILD_TYPE=Release \
            -DMORPHY_BUILD_VST3=ON
          cmake --build build --parallel

      - name: Test
        run: ctest --test-dir build --output-on-failure
```

---

## 3. Code Organization

### 3.1 Directory Structure

```
D:/morphy/
├── CMakeLists.txt                 # Root CMake configuration
├── vcpkg.json                     # Dependency manifest
├── README.md
├── LICENSE
│
├── docs/                          # Documentation
│   ├── CPP_IMPLEMENTATION_STRATEGY.md
│   ├── API_REFERENCE.md
│   ├── TESTING_GUIDE.md
│   └── ARCHITECTURE.md
│
├── cmake/                         # Build system modules
│   ├── CompilerFlags.cmake
│   ├── Dependencies.cmake
│   ├── PlatformSettings.cmake
│   ├── BuildTargets.cmake
│   ├── CrossCompilation.cmake
│   └── Sanitizers.cmake
│
├── src/                           # Source code
│   ├── CMakeLists.txt
│   │
│   ├── core/                      # Core plugin functionality
│   │   ├── MorphyAudioProcessor.h/cpp
│   │   ├── MorphyAudioProcessorEditor.h/cpp
│   │   ├── PluginProcessor.h/cpp
│   │   ├── PluginEditor.h/cpp
│   │   └── ParameterManager.h/cpp
│   │
│   ├── dsp/                       # DSP processing modules
│   │   ├── Processors/
│   │   │   ├── DynamicsProcessor.h/cpp
│   │   │   ├── FilterProcessor.h/cpp
│   │   │   └── ModulationProcessor.h/cpp
│   │   ├── Math/
│   │   │   ├── Interpolation.h/cpp
│   │   │   ├── FFTWrapper.h/cpp
│   │   │   └── LookupTables.h/cpp
│   │   └── SIMD/
│   │       ├── SIMDOperations.h
│   │       ├── SSEOps.h
│   │       ├── AVXOps.h
│   │       └── NEONOps.h
│   │
│   ├── morphing/                  # Morphing engine
│   │   ├── MorphingEngine.h/cpp
│   │   ├── MorphingPad.h/cpp
│   │   ├── ParameterState.h/cpp
│   │   ├── InterpolationCurves.h/cpp
│   │   └── SnapshotManager.h/cpp
│   │
│   ├── mcp/                       # MCP/AI integration
│   │   ├── MCPClient.h/cpp
│   │   ├── MCPProtocol.h/cpp
│   │   ├── AIIntegration.h/cpp
│   │   └── AIRequestHandler.h/cpp
│   │
│   ├── ui/                        # User interface
│   │   ├── Components/
│   │   │   ├── MorphingPadComponent.h/cpp
│   │   │   ├── ParameterSlider.h/cpp
│   │   │   └── WaveformDisplay.h/cpp
│   │   ├── Pages/
│   │   │   ├── MainEditorPage.h/cpp
│   │   │   ├── MorphingPage.h/cpp
│   │   │   └── SettingsPage.h/cpp
│   │   └── Theme/
│   │       ├── ColorScheme.h/cpp
│   │       └── ThemeManager.h/cpp
│   │
│   ├── util/                      # Utilities
│   │   ├── Memory/
│   │   │   ├── PoolAllocator.h
│   │   │   ├── ArenaAllocator.h
│   │   │   └── StackAllocator.h
│   │   ├── Concurrency/
│   │   │   ├── LockFreeQueue.h
│   │   │   ├── LockFreeStack.h
│   │   │   ├── AtomicHelpers.h
│   │   │   └── SPSCQueue.h
│   │   ├── Containers/
│   │   │   ├── RingBuffer.h
│   │   │   ├── FixedVector.h
│   │   │   └── OptionalRef.h
│   │   ├── Math/
│   │   │   ├── FastMath.h
│   │   │   └── Vec3.h
│   │   └── Logging/
│   │       └── Logger.h/cpp
│   │
│   └── config/                    # Configuration
│       ├── BuildConfig.h
│       └── FeatureFlags.h
│
├── include/                       # Public API headers
│   └── morphy/
│       └── API.h
│
├── tests/                         # Test suite
│   ├── CMakeLists.txt
│   ├── Unit/
│   │   ├── TestInterpolation.cpp
│   │   ├── TestParameterState.cpp
│   │   ├── TestMorphingEngine.cpp
│   │   └── TestLockFreeStructures.cpp
│   ├── Integration/
│   │   ├── TestMCPClient.cpp
│   │   └── TestStatePersistence.cpp
│   ├── Performance/
│   │   ├── BenchDSPProcessors.cpp
│   │   └── BenchMorphingEngine.cpp
│   └── Mocks/
│       ├── MockMCPClient.h
│       └── MockAudioProcessor.h
│
├── resources/                     # Resources
│   ├── icons/
│   │   ├── icon_128.png
│   │   └── icon_256.png
│   └── fonts/
│       └── Inter-Regular.ttf
│
└── scripts/                       # Build and deployment scripts
    ├── format_code.sh
    ├── run_tests.sh
    ├── package_plugin.sh
    └── install_vst3.sh
```

### 3.2 Module Responsibility Definitions

#### Core Module
- **Responsibility**: Plugin lifecycle, DAW integration, parameter management
- **Dependencies**: JUCE audio processor, all other modules
- **Real-time critical**: Yes (audio thread)

#### DSP Module
- **Responsibility**: Audio processing algorithms, mathematical operations
- **Dependencies**: JUCE DSP, util (containers, memory)
- **Real-time critical**: Yes (audio thread)

#### Morphing Module
- **Responsibility**: Parameter interpolation, state morphing
- **Dependencies**: DSP, util
- **Real-time critical**: Yes (audio thread)

#### MCP Module
- **Responsibility**: AI communication, WebSocket handling
- **Dependencies**: util (concurrency)
- **Real-time critical**: No (background thread)

#### UI Module
- **Responsibility**: Visual components, user interaction
- **Dependencies**: JUCE GUI, all other modules (read-only)
- **Real-time critical**: No (message thread)

#### Util Module
- **Responsibility**: Common utilities, memory management
- **Dependencies**: None
- **Real-time critical**: Varies by component

---

## 4. Performance Optimization

### 4.1 SIMD Abstraction Layer

```cpp
// src/dsp/SIMD/SIMDOperations.h
#pragma once
#include <cstdint>
#include <type_traits>

namespace morphy::dsp::simd {

enum class SIMDArchitecture : uint8_t {
    Scalar,
    SSE,
    SSE2,
    AVX,
    AVX2,
    AVX512,
    NEON,
    AutoDetect
};

// Compile-time detection
inline constexpr SIMDArchitecture getNativeArchitecture() {
#if defined(__AVX512F__)
    return SIMDArchitecture::AVX512;
#elif defined(__AVX2__)
    return SIMDArchitecture::AVX2;
#elif defined(__AVX__)
    return SIMDArchitecture::AVX;
#elif defined(__SSE2__)
    return SIMDArchitecture::SSE2;
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    return SIMDArchitecture::NEON;
#else
    return SIMDArchitecture::Scalar;
#endif
}

template<typename T, SIMDArchitecture Arch = getNativeArchitecture()>
struct VectorTraits;

// Specialization for float with AVX2
template<>
struct VectorTraits<float, SIMDArchitecture::AVX2> {
    using VectorType = __m256;
    static constexpr size_t ElementCount = 8;
    static constexpr size_t Alignment = 32;
};

// Scalar fallback
template<typename T>
struct VectorTraits<T, SIMDArchitecture::Scalar> {
    using VectorType = T;
    static constexpr size_t ElementCount = 1;
    static constexpr size_t Alignment = alignof(T);
};

// Generic SIMD vector operations
template<typename T, SIMDArchitecture Arch = getNativeArchitecture()>
class alignas(VectorTraits<T, Arch>::Alignment) SIMDVector {
public:
    using VectorType = typename VectorTraits<T, Arch>::VectorType;
    static constexpr size_t ElementCount = VectorTraits<T, Arch>::ElementCount;

    // Constructors
    SIMDVector() = default;
    explicit SIMDVector(T value);
    SIMDVector(const T* data);

    // Arithmetic operations
    SIMDVector operator+(const SIMDVector& other) const;
    SIMDVector operator-(const SIMDVector& other) const;
    SIMDVector operator*(const SIMDVector& other) const;
    SIMDVector operator/(const SIMDVector& other) const;

    // FMA (Fused Multiply-Add)
    static SIMDVector multiplyAdd(const SIMDVector& a, const SIMDVector& b,
                                   const SIMDVector& c);

    // Load/store
    void loadAligned(const T* data);
    void storeAligned(T* data) const;
    void loadUnaligned(const T* data);
    void storeUnaligned(T* data) const;

private:
    VectorType m_data;
};

// Audio processing using SIMD
template<AudioSampleType T>
void applyGainSIMD(T* buffer, size_t numSamples, T gain) {
    constexpr size_t VecSize = SIMDVector<T>::ElementCount;
    size_t numVectors = numSamples / VecSize;
    size_t remainder = numSamples % VecSize;

    SIMDVector<T> gainVec(gain);

    // Vectorized processing
    T* alignedBuffer = alignPointer(buffer, alignof(SIMDVector<T>));
    for (size_t i = 0; i < numVectors; ++i) {
        SIMDVector<T> samples(alignedBuffer + i * VecSize);
        samples = samples * gainVec;
        samples.storeAligned(alignedBuffer + i * VecSize);
    }

    // Scalar remainder
    for (size_t i = numVectors * VecSize; i < numSamples; ++i) {
        buffer[i] *= gain;
    }
}

} // namespace morphy::dsp::simd
```

### 4.2 Lock-Free Data Structures

```cpp
// src/util/Concurrency/LockFreeQueue.h
#pragma once
#include <atomic>
#include <cstring>

namespace morphy::util {

// SPSC (Single Producer Single Consumer) Queue
template<typename T, size_t Capacity>
class alignas(64) LockFreeSPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be power of 2");

public:
    LockFreeSPSCQueue() : m_readPos(0), m_writePos(0) {}

    // Producer only
    bool try_push(const T& value) {
        const size_t writePos = m_writePos.load(std::memory_order_relaxed);
        const size_t nextPos = increment(writePos);

        if (nextPos == m_readPos.load(std::memory_order_acquire)) {
            return false; // Full
        }

        m_data[writePos] = value;
        m_writePos.store(nextPos, std::memory_order_release);
        return true;
    }

    bool try_push(T&& value) {
        const size_t writePos = m_writePos.load(std::memory_order_relaxed);
        const size_t nextPos = increment(writePos);

        if (nextPos == m_readPos.load(std::memory_order_acquire)) {
            return false; // Full
        }

        m_data[writePos] = std::move(value);
        m_writePos.store(nextPos, std::memory_order_release);
        return true;
    }

    // Consumer only
    bool try_pop(T& outValue) {
        const size_t readPos = m_readPos.load(std::memory_order_relaxed);

        if (readPos == m_writePos.load(std::memory_order_acquire)) {
            return false; // Empty
        }

        outValue = std::move(m_data[readPos]);
        m_readPos.store(increment(readPos), std::memory_order_release);
        return true;
    }

    bool empty() const {
        return m_readPos.load(std::memory_order_acquire) ==
               m_writePos.load(std::memory_order_acquire);
    }

private:
    static constexpr size_t increment(size_t index) noexcept {
        return (index + 1) & (Capacity - 1);
    }

    // Separate cache lines to prevent false sharing
    alignas(64) std::atomic<size_t> m_readPos;
    alignas(64) T m_data[Capacity];
    alignas(64) std::atomic<size_t> m_writePos;
};

// MPMC (Multi Producer Multi Consumer) Queue using atomic operations
template<typename T, size_t Capacity>
class LockFreeMPMCQueue {
public:
    LockFreeMPMCQueue() : m_head(0), m_tail(0) {
        for (size_t i = 0; i < Capacity; ++i) {
            m_sequence[i].store(i, std::memory_order_relaxed);
        }
    }

    bool enqueue(const T& value) {
        Node* node;
        size_t pos = m_head.load(std::memory_order_relaxed);

        while (true) {
            node = &m_nodes[pos & (Capacity - 1)];
            size_t seq = node->sequence.load(std::memory_order_acquire);

            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (diff == 0) {
                if (m_head.compare_exchange_weak(pos, pos + 1,
                        std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false; // Full
            } else {
                pos = m_head.load(std::memory_order_relaxed);
            }
        }

        node->data = value;
        node->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

private:
    struct Node {
        T data;
        std::atomic<size_t> sequence;
    };

    std::atomic<size_t> m_head;
    std::atomic<size_t> m_tail;
    Node m_nodes[Capacity];
};

} // namespace morphy::util
```

### 4.3 Memory Pool Allocation

```cpp
// src/util/Memory/PoolAllocator.h
#pragma once
#include <cstddef>
#include <memory>
#include <vector>

namespace morphy::util::memory {

class PoolAllocator {
public:
    // Block size must be >= sizeof(void*)
    explicit PoolAllocator(size_t blockSize, size_t initialCapacity = 16);
    ~PoolAllocator();

    // Allocate from pool
    void* allocate();

    // Deallocate back to pool
    void deallocate(void* ptr);

    // Pool statistics
    size_t getUsedBlocks() const { return m_usedBlocks; }
    size_t getTotalCapacity() const { return m_totalCapacity; }

private:
    struct Block {
        alignas(std::max_align_t) char data[1];
    };

    struct Chunk {
        std::unique_ptr<char[]> data;
        std::unique_ptr<Chunk> next;
    };

    void* allocateFromFreeList();
    void* allocateNewBlock();
    void expandPool();

    const size_t m_blockSize;
    size_t m_usedBlocks = 0;
    size_t m_totalCapacity = 0;

    void* m_freeList = nullptr;
    std::unique_ptr<Chunk> m_chunks;
};

// Thread-safe pool allocator
class ThreadSafePoolAllocator {
public:
    explicit ThreadSafePoolAllocator(size_t blockSize, size_t initialCapacity = 16);

    void* allocate();
    void deallocate(void* ptr);

private:
    PoolAllocator m_pool;
    std::mutex m_mutex;
};

// Arena allocator for temporary allocations
class ArenaAllocator {
public:
    explicit ArenaAllocator(size_t initialSize = 4096);
    ~ArenaAllocator();

    template<typename T>
    T* allocate(size_t count = 1) {
        size_t size = sizeof(T) * count;
        size_t alignment = alignof(T);

        void* ptr = allocateImpl(size, alignment);
        return reinterpret_cast<T*>(ptr);
    }

    void reset();

private:
    void* allocateImpl(size_t size, size_t alignment);

    struct Block {
        size_t size;
        size_t used;
        std::unique_ptr<char[]> data;
        std::unique_ptr<Block> next;
    };

    std::unique_ptr<Block> m_currentBlock;
    std::unique_ptr<Block> m_blocks;
};

} // namespace morphy::util::memory
```

### 4.4 Cache-Friendly Data Layout

```cpp
// Structure of Arrays (SoA) for better cache utilization
namespace morphy::dsp {

// Traditional AoS (Array of Structures) - Cache inefficient
struct ParameterSnapshotAoS {
    float param1;
    float param2;
    float param3;
    float param4;
    // ... many more parameters
};

// SoA (Structure of Arrays) - Cache efficient
struct ParameterSnapshotSoA {
    // Each parameter in separate contiguous array
    alignas(64) std::vector<float> param1;
    alignas(64) std::vector<float> param2;
    alignas(64) std::vector<float> param3;
    alignas(64) std::vector<float> param4;

    ParameterSnapshotSoA(size_t capacity) {
        param1.reserve(capacity);
        param2.reserve(capacity);
        param3.reserve(capacity);
        param4.reserve(capacity);
    }

    // Process all snapshots of param1 (cache friendly)
    void processParam1(float multiplier) {
        for (size_t i = 0; i < param1.size(); ++i) {
            param1[i] *= multiplier;
        }
    }
};

// Cold/hot data splitting
class AudioProcessor {
public:
    // Hot data: accessed every audio frame
    struct HotData {
        alignas(64) float currentGain;
        alignas(64) float currentMix;
        float* audioBuffers[8];
        size_t numSamples;
    };

    // Cold data: accessed infrequently
    struct ColdData {
        std::string presetName;
        std::array<float, 128> parameterValues;
        juce::XmlElement* stateXML;
    };

private:
    HotData m_hot;   // Cache line 0-1
    char padding1[64 - sizeof(HotData) % 64];
    ColdData m_cold; // Cache line 2+
};

} // namespace morphy::dsp
```

### 4.5 Profiling Instrumentation

```cpp
// src/util/Profiling/Profiler.h
#pragma once
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace morphy::util::profiling {

class Profiler {
public:
    static Profiler& getInstance();

    // Scope-based timing
    struct ScopedTimer {
        ScopedTimer(const char* name);
        ~ScopedTimer();

    private:
        const char* m_name;
        std::chrono::high_resolution_clock::time_point m_start;
    };

    // Manual timing
    void beginSample(const char* name);
    void endSample(const char* name);

    // Statistics
    struct Statistics {
        size_t count = 0;
        double totalTime = 0.0;
        double minTime = 1e30;
        double maxTime = 0.0;
        double meanTime() const { return totalTime / count; }
    };

    Statistics getStatistics(const char* name) const;
    void reset();
    void printReport() const;

private:
    Profiler() = default;

    std::unordered_map<std::string, Statistics> m_stats;
};

// RAII timer macro
#define MORPHY_PROFILE_SCOPE(name) \
    morphy::util::profiling::Profiler::ScopedTimer CONCAT(timer_, __LINE__)(name)

#define MORPHY_PROFILE_FUNCTION() \
    MORPHY_PROFILE_SCOPE(__FUNCTION__)

// Usage in audio code
#ifdef MORPHY_ENABLE_PROFILING
    #define PROFILE_DSP_PROCESS() MORPHY_PROFILE_SCOPE("DSP::Process")
#else
    #define PROFILE_DSP_PROCESS()
#endif

// Real-time safe profiling (ring buffer based)
class RealTimeProfiler {
public:
    static constexpr size_t MAX_SAMPLES = 1024;

    struct Sample {
        std::chrono::nanoseconds duration;
        const char* name;
        std::chrono::high_resolution_clock::time_point timestamp;
    };

    void recordSample(const char* name, std::chrono::nanoseconds duration);

    // Read samples from non-audio thread
    std::vector<Sample> readAndClear();

private:
    std::array<Sample, MAX_SAMPLES> m_samples;
    std::atomic<size_t> m_writePos{0};
    std::atomic<size_t> m_readPos{0};
};

} // namespace morphy::util::profiling
```

---

## 5. Real-Time Safety

### 5.1 Audio Thread Guidelines

#### Banned Operations in Audio Thread

```cpp
// src/util/Concurrency/AudioThreadSafety.h
#pragma once
#include <atomic>
#include <new>

namespace morphy::util {

// Compile-time annotations for real-time safety
#define REALTIME_SAFE __attribute__((annotate("realtime_safe")))
#define REALTIME_UNSAFE __attribute__((annotate("realtime_unsafe")))
#define MAY_ALLOCATE __attribute__((annotate("may_allocate")))

// Runtime assertion for audio thread violations
#ifdef MORPHY_DEBUG
    #define ASSERT_AUDIO_THREAD_SAFE() \
        do { \
            if (!juce::Thread::getCurrentThread()->isRealtime()) { \
                LOG_ERROR("Audio thread violation detected at " __FILE__ ":" \
                          STRINGIZE(__LINE__)); \
                std::terminate(); \
            } \
        } while(0)
#else
    #define ASSERT_AUDIO_THREAD_SAFE()
#endif

// Real-time safe memory allocator
template<typename T>
class RealtimeAllocator {
public:
    using value_type = T;

    T* allocate(size_t n) {
        // Pre-allocated pool only - no dynamic allocation
        void* ptr = getFromPool(sizeof(T) * n, alignof(T));
        if (!ptr) {
            // In debug, this is a critical error
            // In release, we return nullptr (caller must handle)
            LOG_CRITICAL("Real-time allocation failed!");
            return nullptr;
        }
        return static_cast<T*>(ptr);
    }

    void deallocate(T* p, size_t n) noexcept {
        returnToPool(p, sizeof(T) * n);
    }
};

// No-heap container for audio thread
template<typename T, size_t Capacity>
class FixedArray {
public:
    static_assert(Capacity > 0, "Capacity must be positive");

    FixedArray() : m_size(0) {}

    bool push_back(const T& value) {
        if (m_size >= Capacity) return false;
        m_data[m_size++] = value;
        return true;
    }

    T& operator[](size_t index) {
        jassert(index < m_size);
        return m_data[index];
    }

    const T& operator[](size_t index) const {
        jassert(index < m_size);
        return m_data[index];
    }

    size_t size() const { return m_size; }
    size_t capacity() const { return Capacity; }

private:
    size_t m_size;
    T m_data[Capacity];
};

} // namespace morphy::util
```

### 5.2 Static Analysis Rules

#### Clang-Tidy Configuration (.clang-tidy)

```yaml
# .clang-tidy
---
Checks: >
  -*,
  bugprone-*,
  -bugprone-easily-swappable-parameters,
  -bugprone-narrowing-conversions,
  cert-*,
  -cert-err58-cpp,
  clang-analyzer-*,
  cppcoreguidelines-*,
  -cppcoreguidelines-avoid-magic-numbers,
  -cppcoreguidelines-pro-bounds-array-to-pointer-decay,
  -cppcoreguidelines-avoid-c-arrays,
  -cppcoreguidelines-owning-memory,
  misc-*,
  -misc-non-private-member-variables-in-classes,
  modernize-*,
  -modernize-use-trailing-return-type,
  -modernize-avoid-c-arrays,
  performance-*,
  readability-*,
  -readability-magic-numbers,
  -readability-identifier-length,
  portability-*,
  -portability-simd-intrinsics,

CheckOptions:
  - key: cert-dcl16-c.NewSuffixes
    value: 'L;LL;ULL;Ll;U;UL;ULL;u;ul;ull;i;ij;I;Ij;ui;uij;Ui;Uij'
  - key: cppcoreguidelines-special-member-functions.AllowSoleDefaultDtor
    value: '1'
  - key: cppcoreguidelines-macro-usage.CheckOnlyOnlyGamut
    value: '1'
  - key: modernize-use-nullptr.NullMacros
    value: 'NULL'

WarningsAsErrors: '*'
HeaderFilterRegex: '.*'
FormatStyle: file
```

#### Custom Audio Safety Checks

```cpp
// src/util/Concurrency/RealtimeSafetyChecker.h
#pragma once

#if MORPHY_ENABLE_REALTIME_CHECKS
    #define RT_BEGIN_BLOCK RealtimeSafetyChecker::beginBlock(__FILE__, __LINE__)
    #define RT_END_BLOCK RealtimeSafetyChecker::endBlock()
    #define RT_FORBID_ALLOC() RealtimeAllocationGuard CONCAT(guard_, __LINE__)
#else
    #define RT_BEGIN_BLOCK
    #define RT_END_BLOCK
    #define RT_FORBID_ALLOC()
#endif

namespace morphy::util {

class RealtimeSafetyChecker {
public:
    static void beginBlock(const char* file, int line);
    static void endBlock();
    static void markAllocation(const char* file, int line);
    static void printViolations();
};

class RealtimeAllocationGuard {
public:
    RealtimeAllocationGuard() : m_prevState(s_allocationsAllowed) {
        s_allocationsAllowed = false;
    }

    ~RealtimeAllocationGuard() {
        s_allocationsAllowed = m_prevState;
    }

    static bool areAllocationsAllowed() { return s_allocationsAllowed; }

private:
    bool m_prevState;
    static inline bool s_allocationsAllowed = true;
};

// Overload new/delete to detect allocations in audio thread
#ifdef MORPHY_DEBUG
void* operator new(size_t size) {
    if (!RealtimeAllocationGuard::areAllocationsAllowed()) {
        RealtimeSafetyChecker::markAllocation(__FILE__, __LINE__);
    }
    return std::malloc(size);
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}
#endif

} // namespace morphy::util
```

### 5.3 Testing for Audio Thread Violations

```cpp
// tests/AudioThreadViolationsTest.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include "core/PluginProcessor.h"

using namespace morphy;

TEMPLATE_TEST_CASE("Audio thread safety violations", "[audio][realtime]", float, double) {
    MorphyAudioProcessor processor;
    processor.prepareToPlay(48000.0, 512);

    SECTION("No allocations during audio processing") {
        juce::AudioBuffer<TestType> buffer(2, 512);
        juce::MidiBuffer midi;

        // Track allocations
        size_t allocationsBefore = getAllocationCount();

        processor.processBlock(buffer, midi);

        size_t allocationsAfter = getAllocationCount();

        REQUIRE(allocationsAfter == allocationsBefore);
    }

    SECTION("No blocking operations in audio thread") {
        juce::AudioBuffer<TestType> buffer(2, 512);
        juce::MidiBuffer midi;

        auto startTime = std::chrono::high_resolution_clock::now();

        processor.processBlock(buffer, midi);

        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - startTime).count();

        // Should complete well within audio deadline (512 samples @ 48kHz ~= 10ms)
        REQUIRE(duration < 10000); // 10ms in microseconds
    }

    SECTION("Parameter updates are lock-free") {
        REQUIRE_NOTHROW([&]() {
            // Simulate audio thread
            std::thread audioThread([&]() {
                juce::AudioBuffer<TestType> buffer(2, 512);
                juce::MidiBuffer midi;
                for (int i = 0; i < 1000; ++i) {
                    processor.processBlock(buffer, midi);
                }
            });

            // Simulate GUI thread
            std::thread guiThread([&]() {
                for (int i = 0; i < 1000; ++i) {
                    processor.setMorphingPosition(
                        static_cast<float>(i) / 1000.0f,
                        static_cast<float>(i) / 2000.0f
                    );
                }
            });

            audioThread.join();
            guiThread.join();
        }());
    }
}
```

---

## 6. Testing Strategy

### 6.1 Unit Test Framework (Catch2)

```cpp
// tests/Unit/TestInterpolation.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include "dsp/Math/Interpolation.h"
#include <cmath>

using namespace morphy::dsp;

TEMPLATE_TEST_CASE("Linear interpolation", "[dsp][interpolation]", float, double) {
    constexpr TestType epsilon = TestType(1e-6);

    SECTION("Interpolates between two values") {
        TestType a = TestType(0);
        TestType b = TestType(10);

        REQUIRE(Interpolator<InterpolationMode::Linear>::interpolate(a, b, TestType(0)) == Approx(a).epsilon(epsilon));
        REQUIRE(Interpolator<InterpolationMode::Linear>::interpolate(a, b, TestType(0.5)) == Approx(TestType(5)).epsilon(epsilon));
        REQUIRE(Interpolator<InterpolationMode::Linear>::interpolate(a, b, TestType(1)) == Approx(b).epsilon(epsilon));
    }

    SECTION("Handles edge cases") {
        TestType a = TestType(-5);
        TestType b = TestType(5);

        // t outside [0, 1] is clamped
        REQUIRE(Interpolator<InterpolationMode::Linear>::interpolate(a, b, TestType(-0.5)) == Approx(a).epsilon(epsilon));
        REQUIRE(Interpolator<InterpolationMode::Linear>::interpolate(a, b, TestType(1.5)) == Approx(b).epsilon(epsilon));
    }
}

TEMPLATE_TEST_CASE("Cosine interpolation", "[dsp][interpolation]", float, double) {
    constexpr TestType epsilon = TestType(1e-5);

    SECTION("Creates smooth curve") {
        TestType a = TestType(0);
        TestType b = TestType(1);

        // At t=0.5, cosine interpolation should give more weight to the center
        TestType linearResult = TestType(0.5);
        TestType cosineResult = Interpolator<InterpolationMode::Cosine>::interpolate(a, b, TestType(0.5));

        // Cosine result should be closer to the endpoint at t=0.5
        // due to the easing effect
        REQUIRE(cosineResult > TestType(0));
        REQUIRE(cosineResult < TestType(1));
    }
}

TEST_CASE("Interpolation performance benchmarks", "[!benchmark][dsp][interpolation]") {
    constexpr size_t iterations = 1000000;

    BENCHMARK("Linear interpolation (float)") {
        float result = 0.0f;
        for (size_t i = 0; i < iterations; ++i) {
            float t = static_cast<float>(i) / iterations;
            result += Interpolator<InterpolationMode::Linear>::interpolate(0.0f, 1.0f, t);
        }
        return result;
    };

    BENCHMARK("Cosine interpolation (float)") {
        float result = 0.0f;
        for (size_t i = 0; i < iterations; ++i) {
            float t = static_cast<float>(i) / iterations;
            result += Interpolator<InterpolationMode::Cosine>::interpolate(0.0f, 1.0f, t);
        }
        return result;
    };
}
```

### 6.2 Integration Tests

```cpp
// tests/Integration/TestMorphingEngine.cpp
#include <catch2/catch_test_macros.hpp>
#include "core/PluginProcessor.h"
#include "morphing/MorphingEngine.h"

using namespace morphy;

TEST_CASE("Morphing engine integration", "[integration][morphing]") {
    MorphyAudioProcessor processor;
    processor.prepareToPlay(48000.0, 512);

    SECTION("Parameter snapshots persist correctly") {
        // Set initial parameters
        processor.setMorphingPosition(0.0f, 0.0f);

        // Capture snapshot
        REQUIRE(processor.captureSnapshot(0));

        // Change parameters
        processor.setMorphingPosition(1.0f, 1.0f);
        REQUIRE(processor.getMorphingPosition() == juce::Point<float>(1.0f, 1.0f));

        // State persistence
        juce::MemoryBlock state;
        processor.getStateInformation(state);

        // Create new processor and load state
        MorphyAudioProcessor newProcessor;
        newProcessor.prepareToPlay(48000.0, 512);
        newProcessor.setStateInformation(state.getData(), state.getSize());

        // Verify state loaded
        REQUIRE(newProcessor.getMorphingPosition() == juce::Point<float>(1.0f, 1.0f));
    }

    SECTION("Morphing pad quadrant behavior") {
        // Set up four snapshots at corners
        processor.setMorphingPosition(0.0f, 0.0f);
        REQUIRE(processor.captureSnapshot(0));

        processor.setMorphingPosition(1.0f, 0.0f);
        REQUIRE(processor.captureSnapshot(1));

        processor.setMorphingPosition(0.0f, 1.0f);
        REQUIRE(processor.captureSnapshot(2));

        processor.setMorphingPosition(1.0f, 1.0f);
        REQUIRE(processor.captureSnapshot(3));

        // Test morphing at center (should be blend of all four)
        processor.setMorphingPosition(0.5f, 0.5f);

        juce::AudioBuffer<float> buffer(2, 256);
        juce::MidiBuffer midi;
        REQUIRE_NOTHROW(processor.processBlock(buffer, midi));
    }
}
```

### 6.3 Performance Benchmarks

```cpp
// tests/Performance/BenchMorphingEngine.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark_all.hpp>
#include "core/PluginProcessor.h"

using namespace morphy;

TEST_CASE("DSP processing benchmarks", "[!benchmark][performance][dsp]") {
    MorphyAudioProcessor processor;
    processor.prepareToPlay(48000.0, 512);

    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;

    // Generate test signal
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
        float* writePtr = buffer.getWritePointer(channel);
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
            writePtr[sample] = std::sin(2.0f * 3.14159f * 440.0f * sample / 48000.0f);
        }
    }

    BENCHMARK("Single audio block processing") {
        processor.processBlock(buffer, midi);
        return buffer.getMagnitude(0, buffer.getNumSamples());
    };

    BENCHMARK("Parameter update (morph position)") {
        processor.setMorphingPosition(0.5f, 0.5f);
        return processor.getMorphingPosition();
    };
}

TEST_CASE("Memory allocation benchmarks", "[!benchmark][performance][memory]") {
    BENCHMARK("Pool allocator allocation/deallocation") {
        PoolAllocator pool(64, 1024);
        std::vector<void*> ptrs;
        ptrs.reserve(1024);

        for (int i = 0; i < 1024; ++i) {
            ptrs.push_back(pool.allocate());
        }

        for (void* ptr : ptrs) {
            pool.deallocate(ptr);
        }

        return ptrs.size();
    };

    BENCHMARK("Standard allocator allocation/deallocation") {
        std::vector<void*> ptrs;
        ptrs.reserve(1024);

        for (int i = 0; i < 1024; ++i) {
            ptrs.push_back(std::malloc(64));
        }

        for (void* ptr : ptrs) {
            std::free(ptr);
        }

        return ptrs.size();
    };
}
```

### 6.4 Mock Objects for Testing

```cpp
// tests/Mocks/MockMCPClient.h
#pragma once
#include "mcp/MCPClient.h"
#include <gmock/gmock.h>

namespace morphy::test {

class MockMCPClient : public MCPClient {
public:
    MOCK_METHOD(bool, connect, (const std::string& uri), (override));
    MOCK_METHOD(void, disconnect, (), (override));
    MOCK_METHOD(bool, isConnected, (), (const, override));
    MOCK_METHOD(bool, sendRequest, (const MCPRequest& request), (override));
    MOCK_METHOD(void, setOnConnected, (std::function<void()> callback), (override));
    MOCK_METHOD(void, setOnDisconnected, (std::function<void()> callback), (override));
    MOCK_METHOD(void, setOnMessageReceived, (std::function<void(const std::string&)> callback), (override));
};

class MockAudioProcessorListener {
public:
    MOCK_METHOD(void, parameterChanged, (const juce::String& paramID, float newValue));
};

} // namespace morphy::test

// Usage in tests
TEST_CASE("AI Integration with mock MCP client", "[ai][mock]") {
    using namespace morphy;
    using namespace testing;

    MorphyAudioProcessor processor;
    auto mockClient = std::make_shared<test::MockMCPClient>();

    EXPECT_CALL(*mockClient, connect(_))
        .Times(1)
        .WillOnce(Return(true));

    EXPECT_CALL(*mockClient, isConnected())
        .WillRepeatedly(Return(true));

    processor.setMCPClientForTesting(mockClient);
    processor.connectToAIServer("ws://localhost:8080");

    // Verify connection was attempted
}

// tests/Mocks/FakeAudioProcessor.h
#pragma once
#include "core/PluginProcessor.h"

namespace morphy::test {

// Lightweight fake for unit tests
class FakeAudioProcessor {
public:
    FakeAudioProcessor(double sampleRate = 48000.0, int samplesPerBlock = 512)
        : m_sampleRate(sampleRate), m_samplesPerBlock(samplesPerBlock) {}

    double getSampleRate() const { return m_sampleRate; }
    int getSamplesPerBlock() const { return m_samplesPerBlock; }

    void setParameter(const juce::String& id, float value) {
        m_parameters[id] = value;
    }

    float getParameter(const juce::String& id) const {
        auto it = m_parameters.find(id);
        return it != m_parameters.end() ? it->second : 0.0f;
    }

    juce::AudioBuffer<float> createTestBuffer(int numChannels, int numSamples) const {
        juce::AudioBuffer<float> buffer(numChannels, numSamples);
        for (int ch = 0; ch < numChannels; ++ch) {
            buffer.clear(ch, 0, numSamples);
        }
        return buffer;
    }

private:
    double m_sampleRate;
    int m_samplesPerBlock;
    std::unordered_map<juce::String, float> m_parameters;
};

} // namespace morphy::test
```

---

## 7. Code Templates

### 7.1 DSP Processor Template

```cpp
// src/dsp/Processors/DSPProcessorTemplate.h
#pragma once
#include <juce_dsp/juce_dsp.h>
#include "../SIMD/SIMDOperations.h"
#include "../../util/Memory/PoolAllocator.h"
#include "../../util/Profiling/Profiler.h"

namespace morphy::dsp {

/**
 * Base class template for DSP processors.
 * Provides common functionality for audio processing modules.
 *
 * @tparam SampleType The floating-point type (float or double)
 * @tparam UseSIMD Enable SIMD optimizations
 */
template<typename SampleType, bool UseSIMD = true>
class DSPProcessorTemplate {
public:
    //==========================================================================
    // Type Aliases
    //==========================================================================

    using AudioBuffer = juce::AudioBuffer<SampleType>;
    using ProcessSpec = juce::dsp::ProcessSpec;

    //==========================================================================
    // Constructor / Destructor
    //==========================================================================

    DSPProcessorTemplate() = default;
    virtual ~DSPProcessorTemplate() = default;

    //==========================================================================
    // Audio Processing Interface
    //==========================================================================

    /**
     * Prepare the processor for playback.
     * Called once before audio processing begins.
     *
     * @param spec The processing specification (sample rate, block size, channels)
     *
     * REALTIME_SAFE: This is NOT called on the audio thread
     */
    virtual void prepare(const ProcessSpec& spec) {
        m_spec = spec;
        m_isPrepared = true;
    }

    /**
     * Reset the processor state.
     * Called when the audio stream is stopped or reset.
     *
     * REALTIME_SAFE: May be called on the audio thread
     */
    virtual void reset() noexcept {
        m_isPrepared = false;
    }

    /**
     * Process a block of audio samples.
     * Called for every audio buffer.
     *
     * @param context The processing context containing audio buffers
     *
     * REALTIME_SAFE: This IS called on the audio thread
     * REALTIME_SAFE: No allocations, no blocking operations
     */
    virtual void process(juce::dsp::ProcessContextReplacing<SampleType> context) noexcept = 0;

    /**
     * Process a block with sidechain input.
     * Override if your processor needs sidechain input.
     */
    virtual void process(juce::dsp::ProcessContextReplacing<SampleType> context,
                        const juce::dsp::ProcessContextNonReplacing<SampleType>& sidechain) noexcept {
        // Default implementation ignores sidechain
        process(context);
    }

    //==========================================================================
    // Parameter Management
    //==========================================================================

    /**
     * Set a parameter by ID.
     * Thread-safe from any thread.
     */
    template<typename T>
    void setParameter(const juce::String& paramID, T value) noexcept {
        juce::ScopedLock lock(m_parameterLock);
        m_parameters[paramID] = static_cast<SampleType>(value);
    }

    /**
     * Get a parameter value by ID.
     * Thread-safe from any thread.
     */
    SampleType getParameter(const juce::String& paramID) const noexcept {
        juce::ScopedLock lock(m_parameterLock);
        auto it = m_parameters.find(paramID);
        return it != m_parameters.end() ? it->second : SampleType(0);
    }

    //==========================================================================
    // State Management
    //==========================================================================

    /**
     * Save processor state to XML.
     * NOT realtime safe.
     */
    virtual std::unique_ptr<juce::XmlElement> saveState() const {
        auto state = std::make_unique<juce::XmlElement>(getStateTypeName());
        juce::ScopedLock lock(m_parameterLock);

        for (const auto& [paramID, value] : m_parameters) {
            state->setAttribute(paramID, value);
        }

        return state;
    }

    /**
     * Load processor state from XML.
     * NOT realtime safe.
     */
    virtual void loadState(const juce::XmlElement& state) {
        if (state.hasTagName(getStateTypeName())) {
            juce::ScopedLock lock(m_parameterLock);

            for (int i = 0; i < state.getNumAttributes(); ++i) {
                auto paramID = state.getAttributeName(i);
                auto value = static_cast<SampleType>(state.getDoubleAttribute(paramID));
                m_parameters[paramID.toString()] = value;
            }
        }
    }

protected:
    //==========================================================================
    // Protected Members
    //==========================================================================

    ProcessSpec m_spec;
    bool m_isPrepared = false;

    // Parameter storage (guarded by mutex for non-audio-thread access)
    juce::CriticalSection m_parameterLock;
    std::unordered_map<juce::String, SampleType> m_parameters;

private:
    //==========================================================================
    // Private Helpers
    //==========================================================================

    virtual const char* getStateTypeName() const noexcept = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DSPProcessorTemplate)
};

} // namespace morphy::dsp
```

### 7.2 Parameter Manager Template

```cpp
// src/core/ParameterManagerTemplate.h
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <type_traits>

namespace morphy {

/**
 * Type-safe parameter wrapper
 */
template<typename T>
class TypedParameter {
public:
    static_assert(std::is_arithmetic_v<T>, "Parameter type must be arithmetic");

    TypedParameter(juce::AudioProcessorValueTreeState& apvts,
                   const juce::String& paramID,
                   T defaultValue,
                   std::function<void(T)> callback = nullptr)
        : m_apvts(apvts)
        , m_paramID(paramID)
        , m_defaultValue(defaultValue)
        , m_callback(std::move(callback)) {}

    T get() const noexcept {
        if constexpr (std::is_same_v<T, float>) {
            return static_cast<T>(m_apvts.getRawParameterValue(m_paramID)->load());
        } else if constexpr (std::is_same_v<T, int>) {
            return static_cast<T>(static_cast<int>(
                m_apvts.getRawParameterValue(m_paramID)->load()));
        } else if constexpr (std::is_same_v<T, bool>) {
            return m_apvts.getRawParameterValue(m_paramID)->load() > 0.5f;
        }
    }

    void set(T value, juce::NotificationType notification = juce::sendNotification) {
        auto* param = m_apvts.getParameter(m_paramID);
        if (param) {
            if constexpr (std::is_same_v<T, float>) {
                param->setValueNotifyingHost(value);
            } else if constexpr (std::is_same_v<T, int>) {
                param->setValueNotifyingHost(static_cast<float>(value));
            } else if constexpr (std::is_same_v<T, bool>) {
                param->setValueNotifyingHost(value ? 1.0f : 0.0f);
            }

            if (m_callback) {
                m_callback(value);
            }
        }
    }

    operator T() const noexcept { return get(); }

private:
    juce::AudioProcessorValueTreeState& m_apvts;
    juce::String m_paramID;
    T m_defaultValue;
    std::function<void(T)> m_callback;
};

/**
 * Parameter group manager
 */
class ParameterGroup {
public:
    ParameterGroup(juce::AudioProcessorValueTreeState& apvts,
                   const juce::String& groupPrefix)
        : m_apvts(apvts), m_groupPrefix(groupPrefix) {}

    template<typename T>
    TypedParameter<T> add(const juce::String& name,
                          T defaultValue,
                          std::function<void(T)> callback = nullptr) {
        juce::String fullID = m_groupPrefix + "_" + name;
        return TypedParameter<T>(m_apvts, fullID, defaultValue, callback);
    }

private:
    juce::AudioProcessorValueTreeState& m_apvts;
    juce::String m_groupPrefix;
};

} // namespace morphy
```

### 7.3 UI Component Template

```cpp
// src/ui/Components/UIComponentTemplate.h
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace morphy::ui {

/**
 * Base class for Morphy UI components
 * Provides consistent styling and behavior
 */
class MorphyComponent : public juce::Component {
public:
    //==========================================================================
    // Color Scheme
    //==========================================================================

    struct Colors {
        static constexpr auto background = juce::Colour(0xff1a1a1a);
        static constexpr auto surface = juce::Colour(0xff2a2a2a);
        static constexpr auto primary = juce::Colour(0xff00d4ff);
        static constexpr auto secondary = juce::Colour(0xff6b00ff);
        static constexpr auto text = juce::Colour(0xffffffff);
        static constexpr auto textDim = juce::Colour(0xff808080);
        static constexpr auto border = juce::Colour(0xff404040);
        static constexpr auto accent = juce::Colour(0xffff0080);
    };

    //==========================================================================
    // Constructor / Destructor
    //==========================================================================

    MorphyComponent() {
        setOpaque(true);
    }

    virtual ~MorphyComponent() = default;

    //==========================================================================
    // Painting
    //==========================================================================

    void paint(juce::Graphics& g) override {
        g.fillAll(Colors::background);
    }

protected:
    //==========================================================================
    // Protected Helpers
    //==========================================================================

    void drawRoundedRect(juce::Graphics& g, juce::Rectangle<float> bounds,
                        float cornerSize, float borderWidth) {
        g.setColour(Colors::border);
        g.drawRoundedRectangle(bounds, cornerSize, borderWidth);
    }

    void drawText(juce::Graphics& g, const juce::String& text,
                 juce::Rectangle<float> bounds,
                 float height = 14.0f,
                 juce::Justification justification = juce::Justification::centred) {
        g.setColour(Colors::text);
        g.setFont(height);
        g.drawText(text, bounds, justification);
    }

    juce::MouseCursor getMouseCursorFor(juce::Component& component) override {
        if (component.isMouseOver()) {
            return juce::MouseCursor::PointingHandCursor;
        }
        return juce::MouseCursor::NormalCursor;
    }
};

/**
 * Animated UI component base
 */
class AnimatedComponent : public MorphyComponent,
                         public juce::Timer {
public:
    //==========================================================================
    // Animation
    //==========================================================================

    void startAnimation(int intervalMs = 16) { // ~60 FPS
        startTimer(intervalMs);
    }

    void stopAnimation() {
        stopTimer();
    }

protected:
    //==========================================================================
    // Timer Callback
    //==========================================================================

    void timerCallback() override {
        // Update animation state
        float deltaTimeMs = static_cast<float>(m_lastTimerTime - m_currentTimerTime);
        m_lastTimerTime = juce::Time::getMillisecondCounter();

        updateAnimation(deltaTimeMs / 1000.0f);
        repaint();
    }

    virtual void updateAnimation(float deltaTime) = 0;

private:
    juce::uint32 m_lastTimerTime = 0;
    juce::uint32 m_currentTimerTime = 0;
};

} // namespace morphy::ui
```

---

## Appendix A: Quick Reference

### Compiler Flags

```cmake
# Common flags for all platforms
target_compile_options(morphy PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror=return-type
    -Werror=non-virtual-dtor
    -Wshadow
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wmisleading-indentation
    -Wduplicated-cond
    -Wduplicated-branches
    -Wlogical-op
    -Wnull-dereference
    -Wuseless-cast
    -Wdouble-promotion
)

# Release optimizations
target_compile_options(morphy RELEASE
    -O3
    -march=native
    -ffast-math
    -fno-finite-math-only
    -fno-associative-math
    -DNDEBUG
)

# Debug flags
target_compile_options(morphy DEBUG
    -O0
    -g3
    -fno-omit-frame-pointer
    -fsanitize=address
    -fsanitize=undefined
)
```

### Pre-commit Hook (.git/hooks/pre-commit)

```bash
#!/bin/bash
# .git/hooks/pre-commit

echo "Running pre-commit checks..."

# Format code
echo "Formatting code..."
clang-format -i $(git diff --cached --name-only --diff-filter=ACM '*.cpp' '*.h')

# Run clang-tidy
echo "Running clang-tidy..."
git diff --cached --name-only --diff-filter=ACM '*.cpp' '*.h' | \
    xargs clang-tidy -p build --warnings-as-errors='*'

# Run unit tests
echo "Running unit tests..."
cd build && ctest --output-on-failure || exit 1

echo "Pre-commit checks passed!"
```

---

## Document Revision History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2025-02-18 | Morphy Audio Team | Initial release |

---
