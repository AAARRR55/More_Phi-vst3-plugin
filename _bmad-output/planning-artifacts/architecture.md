---
stepsCompleted:
  - step-01-init
  - step-02-discovery
  - step-03-starter
  - step-04-decisions
  - step-05-patterns
  - step-06-structure
  - step-07-validation
  - step-08-complete
inputDocuments:
  - _bmad-output/planning-artifacts/prd.md
  - _bmad-output/planning-artifacts/product-brief-morphy-2026-02-24.md
workflowType: 'architecture'
project_name: 'morphy'
user_name: '20190'
date: '2026-02-24'
---

# Architecture Decision Document

_This document builds collaboratively through step-by-step discovery. Sections are appended as we work through each architectural decision together._

---

## Starter Template Evaluation

### Primary Technology Domain

**Desktop Audio Plugin** - Real-time VST3/AU plugin using JUCE framework

### Starter Options Considered

Audio plugin development differs from web/app development:

| Option | Approach | Suitability |
|--------|----------|-------------|
| **Projucer** | JUCE's GUI project generator | Legacy approach, less flexible |
| **CMake + JUCE** | Modern CMake-based setup | ✅ Current standard, what you're using |
| **Greenfield Scaffolding** | New project from template | ❌ Not applicable - brownfield refactor |

### Selected Approach: CMake + JUCE (Brownfield Refactor)

**Rationale for Selection:**
- Existing codebase already uses CMake + JUCE
- 30-day MVP requires refactoring, not new scaffolding
- Core structure exists: `src/`, `cmake/`, `tests/`
- Focus is on *reducing* scope, not establishing new patterns

**Existing Architectural Foundation:**

| Decision | Current Setup | MVP Strategy |
|----------|---------------|--------------|
| **Build Tool** | CMake 3.20+ with JUCE | Keep, fix compilation errors |
| **Project Structure** | Modular (Core/, UI/, AI/, Tests/) | Simplify, delete broken components |
| **Testing** | Catch2 or similar | Keep core tests, defer advanced |
| **CI/CD** | GitHub Actions (assumed) | Validate in 2-3 DAWs |

**Refactor Command Strategy:**

```bash
# NOT: npx create-app
# INSTEAD: Brownfield surgery
git checkout -b mvp-refactor

# Delete for MVP:
rm src/AI/MCPToolsExtended.cpp src/AI/MCPToolsExtended.h
rm src/AI/TokenOptimizer.cpp src/AI/TokenOptimizer.h

# Simplify:
# ParameterClassifier → Name-matching only
# 12→4 snapshots
# 26→3 MCP tools

# Fix:
# Add missing includes
# Fix const-correctness
# Resolve interface mismatches
```

**Note:** This is a *refactor* workflow, not a starter template. The first implementation story is "Fix Compilation Errors," not "Initialize Project."

---

## Core Architectural Decisions

### Decision Priority Analysis

**Critical Decisions (Block Implementation):**

| Decision | Selection | Rationale |
|----------|-----------|-----------|
| **Thread Architecture** | Lock-Free SPSC Queues | Audio thread must NEVER block; UI/AI push via wait-free queue |
| **AI Communication** | Async Command Queue | Background TCP server; commands queued for audio thread |
| **Plugin Hosting** | JUCE AudioPluginInstance | Cross-platform abstraction; faster development |

**Important Decisions (Shape Architecture):**

| Decision | Selection | Rationale |
|----------|-----------|-----------|
| **Snapshot Data** | Fixed Array (std::array) | Eliminates allocation concerns; atomic state flags |
| **Parameter Interpolation** | Inverse-Distance Weighting | Natural proximity falloff; smoothstep curves |
| **Physics Engine** | Direct (MVP) → Elastic/Drift (v0.2) | Prove core concept first; add physics later |

**Deferred Decisions (Post-MVP):**

| Decision | Deferred To | Rationale |
|----------|-------------|-----------|
| Cloud Sync | v0.3+ | Community features after core stability |
| Community Hub | v1.0 | Network effects require user base first |
| Custom Physics Scripting | v2.0 | Power-user feature; not MVP-critical |

### Thread Architecture

**Decision:** Lock-Free Single-Producer-Single-Consumer (SPSC) Queues

**Problem:** Audio thread must process 44,100+ samples/second with <3ms latency. ANY blocking (mutex, allocation, I/O) causes dropouts.

**Solution:** 
- **Audio Thread:** Lock-free queue reads only; pre-allocated buffers
- **UI Thread:** Pushes parameter updates to queue; never waits
- **AI Thread:** Pushes commands to queue; background processing

**Implementation:**
```cpp
template<typename T, size_t Size>
class LockFreeSPSCQueue {
    std::array<T, Size> buffer;
    std::atomic<size_t> writeIndex{0};
    std::atomic<size_t> readIndex{0};
    
    bool push(const T& item);  // Called from UI/AI thread
    bool pop(T& item);         // Called from audio thread
};
```

**Affects:** MorphEngine, ParameterBridge, MCPAudioBridge

### AI Communication Architecture

**Decision:** Background TCP Server with Async Command Queue

**Protocol:** MCP (Model Context Protocol) over TCP/JSON-RPC 2.0
**Port:** 30001 (configurable)
**Transport:** Localhost only (security + latency)

**Architecture:**
```
┌─────────────┐    TCP/JSON-RPC    ┌─────────────┐
│ AI Client   │ ─────────────────→ │ MCP Server  │ (Background Thread)
│ (Claude)    │                    │ (Port 30001)│
└─────────────┘                    └──────┬──────┘
                                          │ Lock-Free Queue
                                          ▼
                                    ┌─────────────┐
                                    │ Audio Thread │ (Process Commands)
                                    └─────────────┘
```

**Commands (MVP - 3 tools):**
- `set_parameter(index, value)` - Set hosted plugin parameter
- `capture_snapshot(slot)` - Capture current state to slot
- `set_morph_position(x, y)` - Move XY pad cursor

**Affects:** MCPClient, MCPToolHandler, PluginProcessor

### Snapshot Data Architecture

**Decision:** Pre-Allocated Fixed Array with Atomic State Tracking

**Structure:**
```cpp
struct SnapshotSlot {
    std::array<ParameterValue, MAX_PARAMS> parameters;
    juce::String name;
    juce::Colour color;
    std::atomic<bool> isEmpty{true};
    juce::int64 timestamp;
};

std::array<SnapshotSlot, 12> m_slots;  // MVP: 4 slots
```

**Serialization:**
- **Save:** XML (human-readable for debugging)
- **Export:** JSON (compact for sharing)

**Affects:** SnapshotBank, PresetManager, PluginProcessor

### Parameter Interpolation

**Decision:** Inverse-Distance Weighting with Smoothstep Curves

**Algorithm:**
```cpp
float calculateWeight(float distance, float power = 2.0f) {
    if (distance < 0.0001f) return 1.0f;  // At snapshot center
    return 1.0f / std::pow(distance, power);
}

float smoothstep(float t) {
    return t * t * (3.0f - 2.0f * t);  // 3t² - 2t³
}
```

**Behavior:**
- Closer snapshots have exponentially more influence
- Smoothstep removes "robotic" linear feel
- All 12 snapshots contribute (not just nearest 2)

**Affects:** MorphEngine, XY Pad visualization

### Physics Engine

**Decision:** Three-Mode State Machine

**Modes:**
1. **Direct** (MVP): Instant response - cursor follows input exactly
2. **Elastic** (v0.2): Spring-damper physics - overshoot and settle
3. **Drift** (v0.2): Perlin noise - autonomous organic movement

**State Machine:**
```cpp
enum class MorphMode {
    Direct,   // MVP
    Elastic,  // v0.2
    Drift     // v0.2
};
```

**Rationale:** Direct mode proves the core morphing concept. Physics adds polish for live performers (Alex) and ambient producers (Taylor).

**Affects:** MorphEngine, UI mode selector, preset compatibility

### Plugin Hosting Architecture

**Decision:** JUCE AudioPluginInstance Wrapper

**Why JUCE:**
- Cross-platform (Windows, macOS, Linux)
- Handles VST3/AU format differences
- Well-tested in production (thousands of plugins)
- Active community and documentation

**Integration Points:**
```cpp
class PluginHostManager {
    std::unique_ptr<juce::AudioPluginInstance> hostedPlugin;
    
    // Parameter access
    float getParameter(int index);
    void setParameter(int index, float value);
    
    // State management
    void getState(juce::MemoryBlock& data);
    void setState(const juce::MemoryBlock& data);
};
```

**Affects:** PluginHostManager, ParameterBridge, PluginProcessor

### Decision Impact Analysis

**Implementation Sequence:**
1. Fix compilation errors (CMake, includes, const-correctness)
2. Implement LockFreeSPSCQueue (core infrastructure)
3. Refactor SnapshotBank (4 slots, atomic flags)
4. Implement MorphEngine (IDW interpolation)
5. Integrate MCP server (async command queue)
6. Test in Ableton + FL Studio

**Cross-Component Dependencies:**
- **MCP Server** depends on **LockFree Queue** for audio thread safety
- **MorphEngine** depends on **SnapshotBank** for parameter states
- **PluginHostManager** depends on **JUCE** for cross-platform hosting
- **UI** depends on **MorphEngine** for XY pad visualization

---

## Implementation Patterns & Consistency Rules

### Pattern Categories Defined

**Critical Conflict Points Identified:** 6 areas where AI agents could make different choices

### Naming Patterns

| Category | Pattern | Example | Anti-Pattern |
|----------|---------|---------|--------------|
| **Classes** | PascalCase | `MorphEngine`, `SnapshotBank` | `morphEngine`, `snapshot_bank` |
| **Files** | Match class name | `MorphEngine.cpp`, `MorphEngine.h` | `morph-engine.cpp` |
| **Functions** | camelCase | `captureSnapshot()`, `setMorphPosition()` | `CaptureSnapshot()`, `capture_snapshot()` |
| **Member Variables** | m_camelCase | `m_morphX`, `m_snapshotSlots` | `morphX_`, `snapshotSlots` |
| **Parameters** | camelCase | `snapshotIndex`, `normalizedValue` | `snapshot_index`, `NormalizedValue` |
| **Constants** | UPPER_SNAKE_CASE | `MAX_SNAPSHOTS = 12` | `kMaxSnapshots`, `maxSnapshots` |
| **Namespaces** | lowercase | `morphy::core`, `morphy::ui` | `Morphy::Core`, `morphy_core` |
| **Private Methods** | camelCase with trailing underscore | `updateInterpolation_()` | `updateInterpolation()`, `update_interpolation()` |

### Structure Patterns

**Project Organization:**
```
src/
├── Core/           # Audio thread, morphing engine
│   ├── MorphEngine.cpp/.h
│   ├── SnapshotBank.cpp/.h
│   └── ParameterBridge.cpp/.h
├── AI/             # MCP server, AI integration
│   ├── MCPClient.cpp/.h
│   └── MCPToolHandler.cpp/.h
├── UI/             # Interface components
│   ├── MorphPad.cpp/.h
│   └── SnapshotRing.cpp/.h
├── Plugin/         # JUCE plugin wrapper
│   ├── PluginProcessor.cpp/.h
│   └── PluginEditor.cpp/.h
└── Utils/          # Helpers, lock-free queues
    └── LockFreeQueue.h

tests/
├── Unit/           # Unit tests (co-located or separate)
├── Integration/    # DAW integration tests
└── Performance/    # Real-time benchmarks
```

**Header Organization:**
- Use `.h` for headers (not `.hpp`)
- Use `#pragma once` (not include guards)
- Order includes: System → JUCE → Project

### Format Patterns

**Error Handling:**
```cpp
// Good: std::optional for fallible operations
std::optional<Snapshot> getSnapshot(int index);

// Good: bool return for expected failures
bool loadPlugin(const juce::String& path);

// Bad: Exceptions for non-fatal errors
try {
    snapshot = bank.getSnapshot(i);  // Don't throw
} catch (...) {}

// Bad: Raw pointers without ownership semantics
Snapshot* getSnapshot(int index);  // Use optional or smart pointer
```

**Thread Safety Annotations:**
```cpp
// REQUIRED: Document thread requirements
class MorphEngine {
public:
    // Called on audio thread - MUST be lock-free
    void processBlock(float* buffer, int numSamples);
    
    // Called on message thread - can block/allocate
    void setMorphPosition(float x, float y);
    
    // Thread-safe (uses atomics)
    std::atomic<float> m_morphX{0.5f};
};
```

**JSON (MCP Protocol):**
```cpp
// Use nlohmann::json for MCP communication
nlohmann::json request;
request["jsonrpc"] = "2.0";
request["method"] = "set_morph_position";
request["params"] = {{"x", 0.5f}, {"y", 0.3f}};
```

### Communication Patterns

**MCP Event Naming:**
```cpp
// Use action.resource pattern (snake_case)
"set.morph_position"      // Good
"SetMorphPosition"        // Bad (PascalCase)
"set_morph_position"      // Bad (missing dot)

"capture.snapshot"        // Good
"capture_snapshot"        // Bad (missing dot)
```

**Lock-Free Queue Usage:**
```cpp
// Audio thread: Pop only (never push)
void audioCallback() {
    ParameterUpdate update;
    while (m_paramQueue.pop(update)) {
        applyUpdate(update);
    }
}

// UI thread: Push only (never pop)
void onMorphPadDragged(float x, float y) {
    m_paramQueue.push({x, y});  // Non-blocking
}
```

### Process Patterns

**Real-Time Safety Checklist:**
```cpp
// Before committing code, verify:
// ☐ No 'new' or 'delete' in audio thread
// ☐ No std::mutex, CriticalSection, or locks
// ☐ No std::vector::push_back (may reallocate)
// ☐ No file I/O or network calls
// ☐ No juce::String operations (may allocate)
// ☐ All buffers pre-allocated in prepareToPlay()
```

**Error Recovery:**
```cpp
// Graceful degradation pattern
if (!mcpServer.isConnected()) {
    // Fallback to manual control
    enableManualMode();
    logWarning("MCP disconnected, manual mode enabled");
}
```

### Enforcement Guidelines

**All AI Agents MUST:**
1. ✅ Follow naming conventions consistently
2. ✅ Never use `std::mutex` in audio thread code
3. ✅ Document thread requirements in function comments
4. ✅ Use `m_` prefix for member variables
5. ✅ Use `#pragma once` for header guards
6. ✅ Prefer `juce::String` for JUCE API interactions
7. ✅ Use `std::optional` instead of raw pointers for fallible returns
8. ✅ Keep audio thread code lock-free and allocation-free

**All AI Agents MUST NOT:**
1. ❌ Use exceptions for non-fatal errors
2. ❌ Use `std::cout` or `printf` in audio thread
3. ❌ Use `dynamic_cast` (RTTI may be disabled)
4. ❌ Use `std::function` (may allocate)
5. ❌ Use global state (singletons) without thread safety

**Pattern Verification:**
- Code review checklist in `docs/CODE_STYLE.md`
- CI enforces naming via `.clang-format`
- Static analysis: `clang-tidy` with JUCE-aware rules
- Runtime validation: Debug builds assert on audio thread violations

---

## Project Structure & Boundaries

### Complete Directory Structure

```
morphy/
├── CMakeLists.txt              # Main build configuration
├── vcpkg.json                  # Dependencies (nlohmann-json, etc.)
├── .clang-format               # Code formatting rules
├── .clang-tidy                 # Static analysis rules
├── README.md                   # Build instructions
├── docs/
│   ├── CODE_STYLE.md          # Naming/format conventions
│   ├── ARCHITECTURE.md        # This document
│   └── API.md                 # MCP protocol docs
├── src/
│   ├── Core/                  # Audio thread components
│   │   ├── MorphEngine.h/.cpp       # XY interpolation
│   │   ├── SnapshotBank.h/.cpp      # 4-slot state capture
│   │   ├── ParameterBridge.h/.cpp   # Plugin parameter access
│   │   └── Interpolation.h          # IDW math functions
│   ├── AI/                    # MCP server (background thread)
│   │   ├── MCPClient.h/.cpp         # TCP/WebSocket server
│   │   ├── MCPToolHandler.h/.cpp    # 3 MCP tools (MVP)
│   │   └── MCPAudioBridge.h/.cpp    # Thread-safe command queue
│   ├── UI/                    # Message thread components
│   │   ├── MorphPad.h/.cpp          # XY pad component
│   │   ├── SnapshotRing.h/.cpp      # 4-slot visual ring
│   │   └── PluginEditor.h/.cpp      # JUCE editor wrapper
│   ├── Plugin/                # JUCE plugin interface
│   │   ├── PluginProcessor.h/.cpp   # AudioProcessor implementation
│   │   └── PluginHostManager.h/.cpp # VST3/AU hosting
│   └── Utils/                 # Shared utilities
│       └── LockFreeQueue.h          # SPSC queue template
├── tests/
│   ├── Unit/
│   │   ├── TestMorphEngine.cpp
│   │   ├── TestSnapshotBank.cpp
│   │   └── TestLockFreeQueue.cpp
│   ├── Integration/
│   │   └── TestDAWCompatibility.cpp
│   └── Performance/
│       └── TestRealTimeSafety.cpp
├── cmake/
│   ├── JUCEConfig.cmake
│   └── FindVST3.cmake
└── resources/
    └── presets/               # Default snapshot banks
```

### Component Boundaries

| Component | Thread | Responsibilities | Dependencies |
|-----------|--------|------------------|--------------|
| **MorphEngine** | Audio | Interpolation, physics | SnapshotBank, LockFreeQueue |
| **SnapshotBank** | Audio | State storage | None (self-contained) |
| **MCPClient** | Background | TCP server, JSON-RPC | MCPAudioBridge |
| **MCPAudioBridge** | Both | Command queue | LockFreeQueue |
| **MorphPad** | Message | UI rendering, input | MorphEngine (read-only) |
| **PluginProcessor** | Audio | JUCE audio callback | MorphEngine, ParameterBridge |

### Requirements Mapping

| PRD Requirement | Component | File |
|-----------------|-----------|------|
| FR1-4: Plugin hosting | PluginHostManager | `src/Plugin/PluginHostManager.cpp` |
| FR5-9: Snapshots | SnapshotBank | `src/Core/SnapshotBank.cpp` |
| FR10-12: XY morphing | MorphEngine | `src/Core/MorphEngine.cpp` |
| FR17-21: MCP server | MCPClient, MCPToolHandler | `src/AI/*.cpp` |
| FR22-24: Performance Mode | PluginProcessor | `src/Plugin/PluginProcessor.cpp` |

---

## Architecture Summary

### Key Decisions

| Area | Decision | Rationale |
|------|----------|-----------|
| **Threading** | Lock-Free SPSC Queues | Audio thread must never block |
| **AI Comm** | Async TCP + Command Queue | Non-blocking, background processing |
| **Snapshots** | Fixed Array (4→12) | Eliminates allocation concerns |
| **Interpolation** | Inverse-Distance Weighting | Natural proximity falloff |
| **Physics** | Direct (MVP) → Elastic/Drift (v0.2) | Prove core concept first |
| **Hosting** | JUCE AudioPluginInstance | Cross-platform, well-tested |

### MVP Deliverables (30 Days)

1. Working build with 4 snapshots
2. XY pad morphing with IDW interpolation
3. Basic MCP server (3 tools)
4. Direct physics mode only
5. VST3 support (AU deferred)
6. Tested in Ableton + FL Studio

### Success Criteria

- Time to first morph: < 2 minutes
- MCP response: < 100ms
- Audio thread CPU: < 3%
- Crash-free rate: > 95%

---

**Document Status:** Complete

**Next Steps:**
1. Create Epics & Stories from this architecture
2. Begin implementation (fix compilation errors)
3. Run code review on existing codebase vs. these decisions

---

## Project Context Analysis

### Requirements Overview

**Functional Requirements (24 total):**

| Category | Requirements | Architectural Implication |
|----------|--------------|---------------------------|
| **Plugin Hosting** | VST3/AU hosting, GUI display, parameter mapping | Plugin wrapper architecture; host bridge pattern |
| **Snapshot System** | 4→12 slot capture/recall, bank save/load | State serialization; atomic parameter capture |
| **XY Morphing** | Inverse-distance interpolation, real-time updates | Lock-free parameter queue; smooth interpolation curves |
| **MCP Server** | TCP/WebSocket, JSON-RPC, AI client auth | Background thread isolation; secure local comms |
| **Physics Engine** | Elastic (spring-damper), Drift (Perlin noise) | Real-time simulation; autonomous XY movement |
| **Performance Mode** | Toggle, disable AI, prioritize audio thread | Mode-based architecture; graceful degradation |

**Non-Functional Requirements:**

| Requirement | Target | Challenge |
|-------------|--------|-----------|
| Audio Thread CPU | < 3% overhead | Efficient interpolation; SIMD optimization |
| Parameter Latency | < 2.9ms (1 block @ 44.1kHz/128) | Lock-free queues; pre-allocated buffers |
| MCP Response | < 100ms | Async processing; local AI only |
| Crash-Free Rate | > 95% | Exception handling; graceful degradation |
| DAW Compatibility | Ableton, FL Studio, Logic | SDK compliance; sandbox adherence |

**Scale & Complexity:**

- **Primary Domain:** Desktop Audio Plugin (Real-time DSP)
- **Complexity Level:** Medium-High
- **Estimated Architectural Components:** 8-10 major subsystems

### Technical Constraints & Dependencies

**Framework:**
- **JUCE 7.x** - Industry standard for cross-platform audio plugins
- **C++20** - Modern C++ with std::atomic, std::thread
- **CMake 3.20+** - Build system with cross-platform support

**Real-Time Constraints (Audio Thread):**
- ❌ No heap allocations (new/delete)
- ❌ No mutex locks (std::mutex, CriticalSection)
- ❌ No file I/O, network calls
- ❌ No std::map/std::vector push_back (may reallocate)
- ✅ Pre-allocated buffers, lock-free queues, atomics only

**Plugin Format Support:**
- **MVP:** VST3 (Windows, macOS, Linux)
- **v0.3:** AU (macOS Audio Unit)
- **Future:** Standalone app, AAX (Pro Tools)

**AI Integration:**
- **MCP Protocol:** JSON-RPC 2.0 over TCP (default port 30001)
- **Local Only:** No cloud AI in MVP (privacy + latency)
- **Fallback:** Manual XY control if MCP disconnected

### Cross-Cutting Concerns Identified

| Concern | Impact | Mitigation Strategy |
|---------|--------|---------------------|
| **Thread Safety** | Audio/UI/AI threads must not interfere | Lock-free SPSC queues; atomic shared state |
| **Plugin Compatibility** | Must work with any VST3/AU plugin | Parameter enumeration; state chunk handling |
| **DAW Stability** | Must not crash host under any circumstances | Exception boundaries; graceful degradation |
| **AI Integration** | MCP must not block audio thread | Background thread; async command queue |
| **Physics Simulation** | Elastic/Drift must update smoothly | Fixed timestep; interpolation between frames |
| **Memory Management** | Real-time safe allocation strategy | Memory pools; pre-allocation at init |

### Brownfield Considerations

**Current State (from PRD):**
- Extensive existing codebase with advanced features
- Build errors in ~40% of components
- Architecture designed for v3.3.0, need v0.1 MVP

**Refactor Strategy:**
1. **Delete:** MCPToolsExtended, TokenOptimizer (for MVP)
2. **Simplify:** ParameterClassifier to name-matching only
3. **Reduce:** 12→4 snapshots, 26→3 MCP tools
4. **Focus:** Core XY morphing + basic MCP only

**30-Day MVP Target:**
- 4 snapshots, XY morphing, 3 MCP tools
- Direct physics only (Elastic/Drift deferred)
- VST3 only (AU deferred)
- Working build in Ableton + FL Studio
