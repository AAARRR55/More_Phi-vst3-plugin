# SnappySnap VST3 Plugin - Implementation Guide

> **Status**: Architecture Complete, Core Components Implemented  
> **Framework**: JUCE 8.0  
> **Build System**: CMake 3.24+

---

## Quick Start

### Prerequisites
- CMake 3.24 or higher
- C++20 compatible compiler (MSVC 2022+, Clang 14+, GCC 11+)
- JUCE 8.0 (will be fetched automatically)

### Build Instructions

```bash
# Clone repository
git clone https://github.com/electricsmudge/snappysnap.git
cd snappysnap

# Configure
cmake -B build -S .

# Build
cmake --build build --config Release

# Output locations:
# Windows: build/SnappySnap_artefacts/Release/VST3/SnappySnap.vst3
# macOS:   build/SnappySnap_artefacts/Release/VST3/SnappySnap.vst3
#          build/SnappySnap_artefacts/Release/SnappySnap.component
```

---

## Project Structure

```
snappysnap/
├── CMakeLists.txt                    # Main build configuration
├── IMPLEMENTATION_GUIDE.md           # This file
├── docs/
│   ├── SNAPPY_SNAP_ARCHITECTURE_DESIGN.md  # Full architecture spec
│   └── SNAPPY_SNAP_COMPLETE_DOCUMENTATION.md # User documentation
│
├── src/
│   ├── Plugin/
│   │   ├── SnappySnapProcessor.h     # Main audio processor
│   │   ├── SnappySnapProcessor.cpp   # Implementation
│   │   ├── SnappySnapEditor.h        # UI editor
│   │   └── SnappySnapEditor.cpp
│   │
│   ├── Core/                         # DSP Engine
│   │   ├── InterpolationEngine.h/.cpp    # 1D/2D interpolation
│   │   ├── PhysicsEngine.h/.cpp          # Elastic/Drift physics
│   │   ├── GeneticEngine.h/.cpp          # Breeding algorithm
│   │   ├── MorphProcessor.h/.cpp         # Processing coordinator
│   │   ├── SnapshotBank.h/.cpp           # Snapshot storage
│   │   ├── ParameterState.h              # Snapshot data structure
│   │   └── LockFreeQueue.h               # Thread-safe queue
│   │
│   ├── Host/                         # Plugin Hosting
│   │   ├── PluginHostManager.h/.cpp      # VST3/AU loading
│   │   ├── ParameterBridge.h/.cpp        # Parameter mapping
│   │   └── PluginScanner.h/.cpp          # Plugin discovery
│   │
│   ├── AI/                           # MCP AI Integration
│   │   ├── MCPServer.h/.cpp              # JSON-RPC server
│   │   ├── MCPToolHandler.h/.cpp         # Tool implementations
│   │   ├── InstanceRegistry.h/.cpp       # Multi-instance tracking
│   │   └── InstanceIdentity.h            # Instance identification
│   │
│   ├── MIDI/                         # MIDI Control
│   │   └── MIDIRouter.h/.cpp             # MIDI routing
│   │
│   ├── Preset/                       # Preset Management
│   │   ├── MetaPresetManager.h/.cpp      # 16×128 preset banks
│   │   └── PresetSerializer.h/.cpp       # State serialization
│   │
│   └── UI/                           # User Interface
│       ├── MorphPad.h/.cpp               # XY pad component
│       ├── SnapFader.h/.cpp              # Vertical fader
│       ├── SnapshotRing.h/.cpp           # Clock layout display
│       ├── BreedingPanel.h/.cpp          # Genetic breeding UI
│       ├── MacroKnobStrip.h/.cpp         # 8 assignable knobs
│       ├── PluginBrowserPanel.h/.cpp     # Plugin selection
│       ├── AIStatusPanel.h/.cpp          # MCP status display
│       └── SnappySnapLookAndFeel.h/.cpp  # Custom UI theme
│
└── tests/                            # Unit tests (future)
```

---

## Core DSP Architecture

### 1. Interpolation Engine

**Files**: `src/Core/InterpolationEngine.h`, `.cpp`

**Purpose**: Fast parameter interpolation for morphing

**Algorithms**:
- **1D Mode**: Linear interpolation across occupied snapshots
- **2D Mode**: Inverse Distance Weighting (IDW) for XY pad

**SIMD Optimization**:
- AVX2: 8 floats per operation
- SSE2: 4 floats per operation  
- Scalar fallback for small batches

**Usage**:
```cpp
// 1D interpolation
InterpolationEngine::compute1D(faderPos, snapshotBank, outputBuffer);

// 2D interpolation
InterpolationEngine::compute2D(cursorX, cursorY, snapshotBank, outputBuffer);
```

### 2. Physics Engine

**Files**: `src/Core/PhysicsEngine.h`, `.cpp`

**Purpose**: Physics-based morphing behavior

**Modes**:
- **Elastic**: Spring-damper system with 3 presets (Slow/Medium/Heavy)
- **Drift**: Perlin noise-based autonomous wandering (Free/Locked/Orbit)

**Elastic Physics**:
```cpp
ElasticState state;
PhysicsEngine::updateElastic(state, targetX, targetY, 
                             ElasticPreset::Medium, deltaTime);
```

**Drift Physics**:
```cpp
float driftX, driftY;
PhysicsEngine::updateDrift(driftX, driftY, time, speed, 
                          distance, chaos, DriftMode::Free);
```

### 3. Snapshot Bank

**Files**: `src/Core/SnapshotBank.h`, `.cpp`

**Purpose**: Thread-safe storage for 12 parameter snapshots

**Thread Safety**: SeqLock pattern
- Readers: Lock-free, retry on write
- Writers: Brief mutex lock

**Capacity**: 2048 parameters per snapshot (fixed array for real-time safety)

---

## Thread Safety Model

### Thread Architecture

| Thread | Purpose | Constraints |
|--------|---------|-------------|
| **Audio** | processBlock(), interpolation | NO blocking, NO allocation |
| **Message** | UI, user input | Can block |
| **MCP** | AI command server | Can block, queues to audio |

### Synchronization

```
┌─────────────┐      Atomic      ┌─────────────┐
│   UI/API    │ ═══════════════► │    Audio    │
│   Thread    │                  │   Thread    │
└─────────────┘                  └─────────────┘
       │                                ▲
       │       Lock-Free Queue          │
       └────────────────────────────────┘
              (MCP commands)
```

**Primitives**:
- `std::atomic<T>`: Parameter positions
- `LockFreeQueue<SPSC>`: MCP commands → Audio
- `SeqLock`: Snapshot bank reads

---

## MCP AI Integration

### Protocol

- **Transport**: TCP localhost
- **Protocol**: JSON-RPC 2.0
- **Default Port**: 30001
- **Security**: Localhost only (no auth for local use)

### Available Tools

| Tool | Description |
|------|-------------|
| `get_plugin_info` | Returns hosted plugin info |
| `list_parameters` | Lists all parameters with values |
| `get_parameter` | Gets single parameter value |
| `set_parameter` | Sets single parameter |
| `set_parameters_batch` | Sets multiple parameters |
| `capture_snapshot` | Captures current state to slot |
| `recall_snapshot` | Recalls slot to current state |
| `set_morph_position` | Sets XY/fader position |
| `get_morph_state` | Gets current morph state |

### Example Request

```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "set_morph_position",
    "arguments": { "x": 0.5, "y": 0.3, "source": "xy" }
  },
  "id": 1
}
```

---

## MIDI Implementation

### Note Triggers

- **C3-B3** (configurable octave) → Snapshots 1-12
- Trigger notes filtered from hosted plugin

### CC Mapping

| CC | Function |
|----|----------|
| CC#0 | Bank Select (1-16) |
| CC#1 | Mod Wheel → Fader position |
| CC#110-121 | Remote save to snapshot 1-12 (value > 64) |

---

## Implementation Status

### ✅ Completed

- [x] Core architecture design
- [x] InterpolationEngine (SIMD optimized)
- [x] PhysicsEngine (Elastic + Drift)
- [x] LockFreeQueue (SPSC)
- [x] ParameterState data structure
- [x] SnapshotBank header
- [x] Main processor header
- [x] CMake build system

- [x] SnappySnapProcessor.h/.cpp (main processing loop)
- [x] SnappySnapEditor.h/.cpp (UI)
- [x] SnapshotBank.h/.cpp (seqlock implementation)
- [x] MorphProcessor.h/.cpp (coordinator)
- [x] PluginHostManager.h/.cpp (VST3 hosting)
- [x] ParameterBridge.h/.cpp (parameter mapping)
- [x] MCPServer.h/.cpp (JSON-RPC)
- [x] MCPToolHandler.h/.cpp (tool implementations)
- [x] MIDIRouter.h/.cpp (MIDI handling)
- [x] UI components (MorphPad, SnapFader, etc.)
- [x] LinkBroadcaster.h/.cpp (Cross-instance sync)
- [x] ParameterClassifier.h/.cpp (Learn filtering)
- [x] TokenOptimizer.h/.cpp (MCP Token tracking)

### 🚧 Pending Implementation

- [ ] VST2 fallback support (deprecated format)
- [ ] Standalone application wrapper

---

## Key Design Decisions

### 1. Fixed-Size Arrays
- **Why**: Real-time audio thread safety
- **Trade-off**: Maximum 2048 parameters per snapshot (sufficient for all known plugins)

### 2. Lock-Free Communication
- **Why**: No audio thread blocking
- **Techniques**: Atomics, SPSC queues, SeqLock

### 3. SIMD Interpolation
- **Why**: Process hundreds of parameters efficiently
- **Fallback**: Scalar for small batches or old CPUs

### 4. Perlin Noise for Drift
- **Why**: Organic, coherent randomness
- **Alternative**: Simple random → too jittery

### 5. MCP for AI
- **Why**: Standard protocol, multiple AI support
- **Security**: Localhost only, no auth needed

---

## Performance Targets

| Metric | Target | Notes |
|--------|--------|-------|
| **CPU Overhead** | < 0.5% | Per instance, morphing active |
| **Latency** | 0 samples | Parameter control only |
| **Interpolation** | 1000+ params | @ 44.1kHz, < 1% CPU |
| **MCP Response** | < 10ms | Localhost round-trip |

---

## Testing Recommendations

### 1. Thread Safety Tests
- Concurrent snapshot capture/recall
- MCP command flooding
- UI stress testing during playback

### 2. Performance Tests
- 1000+ parameter plugins
- Rapid morphing at low buffer sizes
- Multiple instances

### 3. Compatibility Tests
- Major DAWs (Ableton, Logic, FL Studio, etc.)
- Various plugin types (synths, effects, different parameter counts)
- MIDI controllers

### 4. AI Integration Tests
- MCP connection stability
- Command batching
- Error handling

---

## Contributing

### Code Style
- C++20 features encouraged
- `noexcept` for real-time functions
- `const` correctness
- Clear ownership semantics

### Commit Messages
```
[component] Brief description

Detailed explanation if needed.

- Bullet points for changes
- Reference issue numbers
```

### Pull Request Process
1. Ensure builds on Windows, macOS
2. Run tests (when available)
3. Update documentation
4. Request review from maintainers

---

## Resources

- **JUCE Documentation**: https://docs.juce.com/
- **VST3 SDK**: https://developer.steinberg.help/
- **MCP Specification**: https://modelcontextprotocol.io/
- **Original SnappySnap**: https://electricsmudge.com

---

## License

This implementation is released under the MIT License.

Third-party dependencies:
- **JUCE**: Dual-licensed (AGPLv3 or Commercial)
- **nlohmann/json**: MIT License

---

*Built with ❤️ for music producers and live performers*
