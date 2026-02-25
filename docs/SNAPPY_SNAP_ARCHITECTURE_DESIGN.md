# SnappySnap VST3 Plugin - Complete Technical Architecture Design

> **Document Type**: Technical Architecture Specification  
> **Framework**: JUCE 8.0  
> **Format**: VST3 (AU optional)  
> **Standard**: C++20

---

## Table of Contents

1. [System Overview](#1-system-overview)
2. [DSP Architecture](#2-dsp-architecture)
3. [Class Hierarchy](#3-class-hierarchy)
4. [Module Specifications](#4-module-specifications)
5. [Thread Safety Model](#5-thread-safety-model)
6. [Build System](#6-build-system)
7. [Implementation Roadmap](#7-implementation-roadmap)

---

## 1. System Overview

### 1.1 Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                              SnappySnap VST3 Plugin                              │
├─────────────────────────────────────────────────────────────────────────────────┤
│                                                                                  │
│  ┌──────────────────────────────────────────────────────────────────────────┐   │
│  │                         Audio Thread (Real-Time)                          │   │
│  │  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐    ┌───────────┐  │   │
│  │  │   MIDI      │───▶│   Morph     │───▶│ Parameter   │───▶│  Hosted   │  │   │
│  │  │  Router     │    │  Processor  │    │   Bridge    │    │  Plugin   │  │   │
│  │  └─────────────┘    └──────┬──────┘    └─────────────┘    └─────┬─────┘  │   │
│  │                            │                                     │        │   │
│  │                     ┌──────┴──────┐                      ┌──────┴──────┐ │   │
│  │                     │  Snapshot   │                      │ Audio Pass  │ │   │
│  │                     │    Bank     │                      │  Through    │ │   │
│  │                     └─────────────┘                      └─────────────┘ │   │
│  └──────────────────────────────────────────────────────────────────────────┘   │
│                                    │                                             │
│                         Lock-Free Queue │ Lock-Free Queue                        │
│                                    │                                             │
│  ┌─────────────────────────────────┴────────────────────────────────────────┐   │
│  │                      UI Thread (Message Thread)                           │   │
│  │  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐    ┌───────────┐  │   │
│  │  │  MorphPad   │◀──▶│   Editor    │◀──▶│   MCP       │◀──▶│   AI      │  │   │
│  │  │  Component  │    │   Logic     │    │   Server    │    │  Client   │  │   │
│  │  └─────────────┘    └─────────────┘    └─────────────┘    └───────────┘  │   │
│  │                                                                            │   │
│  │  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐                     │   │
│  │  │ SnapFader   │    │  Breeding   │    │   Preset    │                     │   │
│  │  │  Component  │    │   Panel     │    │  Manager    │                     │   │
│  │  └─────────────┘    └─────────────┘    └─────────────┘                     │   │
│  └────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                  │
└─────────────────────────────────────────────────────────────────────────────────┘
```

### 1.2 Key Design Principles

| Principle | Implementation |
|-----------|----------------|
| **Zero Audio Thread Allocation** | Pre-allocated buffers, no heap in processBlock |
| **Lock-Free Communication** | SPSC queues between UI and audio threads |
| **SIMD Optimization** | AVX2/SSE for parameter interpolation |
| **Thread-Safe Parameters** | std::atomic for all morph positions |
| **Plugin Agnostic** | Works with any VST3/AU plugin |

---

## 2. DSP Architecture

### 2.1 Processing Chain

```
Input Audio/MIDI
       │
       ▼
┌──────────────┐
│ MIDI Router  │ ──► Filter trigger notes, route CC to morph
└──────┬───────┘
       │
       ▼
┌──────────────┐
│   Command    │ ──► Drain MCP/queue commands
│    Queue     │
└──────┬───────┘
       │
       ▼
┌──────────────┐     ┌──────────────┐
│   Physics    │◀────│   Target     │
│   Engine     │     │   Position   │
└──────┬───────┘     └──────────────┘
       │
       ▼
┌──────────────┐
│ Interpolation│ ──► IDW for 2D, Linear for 1D
│   Engine     │
└──────┬───────┘
       │
       ▼
┌──────────────┐
│   Hosted     │ ──► Pass audio + params to wrapped plugin
│   Plugin     │
└──────┬───────┘
       │
       ▼
   Output Audio
```

### 2.2 Interpolation Algorithms

**1D Linear Interpolation (Snap Fader):**
```cpp
// Map fader position [0,1] across occupied snapshots
float scaled = faderPos * (occupiedCount - 1);
int loIdx = floor(scaled);
int hiIdx = min(loIdx + 1, occupiedCount - 1);
float alpha = scaled - loIdx;

// For each parameter
value = slot[loIdx][i] * (1 - alpha) + slot[hiIdx][i] * alpha;
```

**2D Inverse Distance Weighting (XY Pad):**
```cpp
// For each occupied snapshot at position P[i]
float weight[i] = 1.0f / (distance(cursor, P[i]) ^ 2);
float totalWeight = sum(weights);
float normalizedWeight[i] = weight[i] / totalWeight;

// Weighted sum for each parameter
value[j] = sum(snapshot[i][j] * normalizedWeight[i]);
```

### 2.3 Physics Simulation

**Elastic Mode (Spring-Damper):**
```cpp
struct ElasticState {
    float x, y;      // Current position
    float vx, vy;    // Velocity
};

void updateElastic(ElasticState& s, float targetX, float targetY, 
                   float stiffness, float damping, float dt) {
    float fx = stiffness * (targetX - s.x) - damping * s.vx;
    float fy = stiffness * (targetY - s.y) - damping * s.vy;
    s.vx += fx * dt;
    s.vy += fy * dt;
    s.x += s.vx * dt;
    s.y += s.vy * dt;
}
```

**Drift Mode (Perlin Noise):**
```cpp
// Perlin noise with octaves
float driftX = perlinOctaves(time * speed, 0.0f, octaves) * distance;
float driftY = perlinOctaves(0.0f, time * speed, octaves) * distance;

// Apply based on mode (Free, Locked, Orbit)
```

---

## 3. Class Hierarchy

### 3.1 Complete Class Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              JUCE Framework                                  │
│                    juce::AudioProcessor (base class)                         │
└─────────────────────────────┬───────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                        SnappySnapProcessor                                   │
│                    (Main Audio Processor)                                    │
├─────────────────────────────────────────────────────────────────────────────┤
│ - apvts: AudioProcessorValueTreeState                                       │
│ - hostManager: PluginHostManager                                            │
│ - snapshotBank: SnapshotBank                                                │
│ - morphProcessor: MorphProcessor                                            │
│ - paramBridge: ParameterBridge                                              │
│ - midiRouter: MIDIRouter                                                    │
│ - mcpServer: MCPServer                                                      │
├─────────────────────────────────────────────────────────────────────────────┤
│ + processBlock(): void                                                      │
│ + prepareToPlay(): void                                                     │
│ + createEditor(): AudioProcessorEditor*                                     │
│ + enqueueParameterSet(): bool                                               │
│ + recallSnapshotQueued(): bool                                              │
└─────────────────────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
        ▼                     ▼                     ▼
┌───────────────┐   ┌──────────────────┐   ┌──────────────────┐
│  Core::       │   │   Host::         │   │    AI::          │
│  MorphProcessor│   │ PluginHostManager│   │   MCPServer      │
│               │   │                  │   │                  │
│ - interpolation│  │ - pluginInstance │   │ - jsonRpcServer  │
│ - physics     │   │ - pluginFormatMgr│   │ - toolHandler    │
│ - snapshotBank│   │ - hostedPlugin   │   │ - instanceId     │
├───────────────┤   ├──────────────────┤   ├──────────────────┤
│ + process()   │   │ + loadPlugin()   │   │ + startServer()  │
│ + setMode()   │   │ + unloadPlugin() │   │ + stopServer()   │
└───────────────┘   │ + processBlock() │   │ + handleRequest()│
                    └──────────────────┘   └──────────────────┘
        │
        ▼
┌─────────────────────────────────────────────────────────┐
│                      Core::                             │
│                InterpolationEngine                      │
├─────────────────────────────────────────────────────────┤
│ + compute1D(): void    (linear interpolation)           │
│ + compute2D(): void    (IDW interpolation)              │
│ + interpolateBatch_SIMD(): void (AVX/SSE)               │
└─────────────────────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────────────────────┐
│                      Core::                             │
│                   PhysicsEngine                         │
├─────────────────────────────────────────────────────────┤
│ + updateElastic(): void    (spring-damper)              │
│ + updateDrift(): void      (perlin noise)               │
│ + perlin(): float          (noise generation)           │
└─────────────────────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────────────────────┐
│                      Core::                             │
│                   SnapshotBank                          │
├─────────────────────────────────────────────────────────┤
│ - slots: array<ParameterState, 12>                      │
│ - seqlock: SeqLock                                      │
├─────────────────────────────────────────────────────────┤
│ + capture(): void                                       │
│ + recall(): void                                        │
│ + isOccupied(): bool                                    │
│ + tryReadLocked(): bool                                 │
└─────────────────────────────────────────────────────────┘
```

### 3.2 Header File Structure

```
include/
├── SnappySnapProcessor.h          # Main processor
├── SnappySnapEditor.h             # Main editor
│
├── Core/
│   ├── InterpolationEngine.h      # 1D/2D interpolation
│   ├── PhysicsEngine.h            # Elastic/Drift physics
│   ├── GeneticEngine.h            # Breeding algorithm
│   ├── MorphProcessor.h           # Processing coordinator
│   ├── SnapshotBank.h             # Snapshot storage
│   ├── ParameterState.h           # Parameter snapshot data
│   └── LockFreeQueue.h            # Thread-safe queue
│
├── Host/
│   ├── PluginHostManager.h        # VST3/AU hosting
│   ├── ParameterBridge.h          # Parameter mapping
│   └── PluginScanner.h            # Plugin discovery
│
├── AI/
│   ├── MCPServer.h                # MCP JSON-RPC server
│   ├── MCPToolHandler.h           # Tool implementations
│   ├── InstanceRegistry.h         # Multi-instance tracking
│   └── InstanceIdentity.h         # Instance identification
│
├── MIDI/
│   └── MIDIRouter.h               # MIDI routing/filtering
│
├── Preset/
│   ├── MetaPresetManager.h        # Preset management
│   └── PresetSerializer.h         # State serialization
│
└── UI/
    ├── MorphPad.h                 # XY pad component
    ├── SnapFader.h                # Vertical fader
    ├── SnapshotRing.h             # Clock layout display
    ├── BreedingPanel.h            # Genetic breeding UI
    ├── MacroKnobStrip.h           # 8 macro knobs
    ├── PluginBrowserPanel.h       # Plugin selection
    └── AIStatusPanel.h            # MCP connection status
```

---

## 4. Module Specifications

### 4.1 Core::InterpolationEngine

```cpp
#pragma once
#include <JuceHeader.h>
#include "SnapshotBank.h"

namespace snap {

class InterpolationEngine {
public:
    // 1D linear interpolation through occupied snapshots
    static void compute1D(float faderPos, 
                          const SnapshotBank& bank,
                          std::vector<float>& output) noexcept;
    
    // 2D IDW interpolation for XY pad
    static void compute2D(float cursorX, float cursorY,
                          const SnapshotBank& bank,
                          std::vector<float>& output) noexcept;
    
    // SIMD batch interpolation
    static void interpolateBatch_SIMD(const float* srcA, 
                                      const float* srcB,
                                      float* dest, 
                                      float t, 
                                      size_t count) noexcept;
    
    // CPU feature detection
    static bool hasAVXSupport();
    static bool hasSSESupport();

private:
    static constexpr float kEpsilon = 1e-6f;
    
    // Clock positions for 12 snapshots (12 o'clock start)
    static std::array<juce::Point<float>, 12> getClockPositions(
        float radius = 1.0f);
    
    // Scalar fallback
    static void interpolateBatch_Scalar(const float* srcA,
                                          const float* srcB,
                                          float* dest,
                                          float t,
                                          size_t count) noexcept;
};

} // namespace snap
```

### 4.2 Core::PhysicsEngine

```cpp
#pragma once
#include <JuceHeader.h>

namespace snap {

enum class ElasticPreset { Slow, Medium, Heavy };
enum class DriftMode { Free, Locked, Orbit };

struct ElasticState {
    float x = 0.5f, y = 0.5f;
    float vx = 0.0f, vy = 0.0f;
};

class PhysicsEngine {
public:
    // Spring-damper physics
    static void updateElastic(ElasticState& state,
                              float targetX, float targetY,
                              ElasticPreset preset,
                              float dt) noexcept;
    
    // Perlin noise drift
    static void updateDrift(float& outX, float& outY,
                            float time, float speed, 
                            float distance, float chaos,
                            DriftMode mode,
                            float anchorX = 0.5f, 
                            float anchorY = 0.5f,
                            float gravity = 0.5f) noexcept;

private:
    // Perlin noise implementation
    static float perlin(float x, float y) noexcept;
    static float perlinOctaves(float x, float y, int octaves) noexcept;
    static float fade(float t) noexcept { return t * t * t * (t * (t * 6 - 15) + 10); }
    static float lerp(float a, float b, float t) noexcept { return a + t * (b - a); }
    static float grad(int hash, float x, float y) noexcept;
};

} // namespace snap
```

### 4.3 Core::SnapshotBank

```cpp
#pragma once
#include <JuceHeader.h>
#include "ParameterState.h"

namespace snap {

class SnapshotBank {
public:
    static constexpr int NUM_SLOTS = 12;
    
    SnapshotBank();
    
    // Capture current parameter state to slot
    void capture(int slot, const class ParameterBridge& bridge);
    
    // Recall slot to parameter bridge
    void recall(int slot, class ParameterBridge& bridge) const;
    
    // Query
    bool isOccupied(int slot) const noexcept { 
        return slots[slot].occupied; 
    }
    bool hasAnyOccupied() const noexcept;
    int getOccupiedCount() const noexcept;
    
    // Thread-safe read with seqlock
    bool tryReadLocked(std::function<void(const std::array<ParameterState, NUM_SLOTS>&)> reader) const noexcept;
    
    // Get slot values copy
    bool getSlotValuesCopy(int slot, std::vector<float>& values) const;
    
    // Naming
    void setSlotName(int slot, const juce::String& name);
    juce::String getSlotName(int slot) const;

private:
    std::array<ParameterState, NUM_SLOTS> slots;
    
    // SeqLock for wait-free reads
    mutable std::atomic<uint32_t> sequence{0};
    mutable std::mutex writeMutex;
    
    void beginWrite() noexcept;
    void endWrite() noexcept;
};

} // namespace snap
```

### 4.4 Core::MorphProcessor

```cpp
#pragma once
#include <JuceHeader.h>
#include "SnapshotBank.h"
#include "InterpolationEngine.h"
#include "PhysicsEngine.h"

namespace snap {

enum class MorphSource { XY, Fader };
enum class MorphMode { Direct, Elastic, Drift };

class MorphProcessor {
public:
    explicit MorphProcessor(const SnapshotBank& bank);
    
    void prepare(int maxParameterCount);
    
    // Main processing
    void process(float targetX, float targetY, float faderPos,
                 MorphSource source, MorphMode mode,
                 float dt, std::vector<float>& output);
    
    // Physics tuning
    void setElasticPreset(ElasticPreset preset) { elasticPreset = preset; }
    void setDriftSpeed(float speed) { driftSpeed = speed; }
    void setDriftDistance(float dist) { driftDistance = dist; }
    void setDriftChaos(float chaos) { driftChaos = chaos; }
    void setSmoothingRate(float rate) { smoothingRate = rate; }

private:
    const SnapshotBank& snapshotBank;
    
    // Physics state
    ElasticState elasticState;
    float driftTime = 0.0f;
    
    // Settings
    ElasticPreset elasticPreset = ElasticPreset::Medium;
    float driftSpeed = 0.3f;
    float driftDistance = 0.4f;
    float driftChaos = 0.5f;
    float smoothingRate = 0.95f;
    
    // Direct smoothing
    float smoothedX = 0.5f, smoothedY = 0.5f;
    
    // Scratch buffer
    std::vector<float> tempOutput;
};

} // namespace snap
```

### 4.5 Host::PluginHostManager

```cpp
#pragma once
#include <JuceHeader.h>

namespace snap {

class PluginHostManager {
public:
    PluginHostManager();
    ~PluginHostManager();
    
    // Plugin lifecycle
    bool loadPlugin(const juce::PluginDescription& description);
    void unloadPlugin();
    bool isPluginLoaded() const noexcept { return pluginInstance != nullptr; }
    
    // Audio processing
    void prepare(double sampleRate, int blockSize, int numChannels);
    void releaseResources();
    void processBlock(juce::AudioBuffer<float>& buffer, 
                      juce::MidiBuffer& midi);
    
    // Access
    juce::AudioPluginInstance* getPlugin() const { return pluginInstance.get(); }
    juce::PluginDescription* getLastDescription() const;
    
    // Scanning
    static juce::KnownPluginList scanForPlugins();

private:
    juce::AudioPluginFormatManager formatManager;
    std::unique_ptr<juce::AudioPluginInstance> pluginInstance;
    std::unique_ptr<juce::PluginDescription> lastDescription;
    
    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;
    bool isPrepared = false;
};

} // namespace snap
```

### 4.6 Host::ParameterBridge

```cpp
#pragma once
#include <JuceHeader.h>

namespace snap {

class ParameterBridge {
public:
    explicit ParameterBridge(const PluginHostManager& hostManager);
    
    void refreshParameterList();
    int getParameterCount() const noexcept { return parameterCache.size(); }
    
    // Parameter access
    float getParameterNormalized(int index) const;
    void setParameterNormalized(int index, float value);
    juce::String getParameterName(int index) const;
    
    // Bulk operations
    void captureAllParameters(std::vector<float>& values) const;
    void applyParameterState(const std::vector<float>& values);
    
    // Sanity mode - check if parameter should be excluded
    bool isSanityProtected(int index) const;

private:
    const PluginHostManager& hostManager;
    std::vector<juce::AudioProcessorParameter*> parameterCache;
    
    // Sanity keywords
    static const std::vector<juce::String> sanityKeywords;
};

} // namespace snap
```

### 4.7 AI::MCPServer

```cpp
#pragma once
#include <JuceHeader.h>
#include <thread>

namespace snap {

class MCPToolHandler;
class SnappySnapProcessor;
struct InstanceIdentity;

class MCPServer {
public:
    explicit MCPServer(SnappySnapProcessor& processor);
    ~MCPServer();
    
    bool startServer(int port);
    void stopServer();
    bool isRunning() const noexcept { return running; }
    
    void setIdentity(const InstanceIdentity& identity);

private:
    SnappySnapProcessor& processor;
    std::unique_ptr<MCPToolHandler> toolHandler;
    
    std::atomic<bool> running{false};
    std::atomic<int> serverPort{0};
    std::thread serverThread;
    
    void runServer(int port);
    void handleClient(int clientSocket);
    
    juce::String processRequest(const juce::String& jsonRequest);
};

} // namespace snap
```

### 4.8 MIDI::MIDIRouter

```cpp
#pragma once
#include <JuceHeader.h>
#include <functional>

namespace snap {

class MIDIRouter {
public:
    using SnapshotCallback = std::function<void(int slot)>;
    using MorphCallback = std::function<void(float value)>;
    
    MIDIRouter();
    
    void processMidi(const juce::MidiBuffer& input, 
                     juce::MidiBuffer& output);
    
    // Callbacks
    void setSnapshotCallback(SnapshotCallback cb) { snapshotCallback = cb; }
    void setMorphCallback(MorphCallback cb) { morphCallback = cb; }
    
    // Configuration
    void setTriggerOctave(int octave) { triggerOctave = octave; }
    void setFaderCC(int cc) { faderCC = cc; }

private:
    SnapshotCallback snapshotCallback;
    MorphCallback morphCallback;
    
    int triggerOctave = 2;  // C2-B2 for snapshots 1-12
    int faderCC = 1;        // Mod wheel
};

} // namespace snap
```

### 4.9 Main Processor

```cpp
#pragma once
#include <JuceHeader.h>
#include "Core/MorphProcessor.h"
#include "Core/SnapshotBank.h"
#include "Host/PluginHostManager.h"
#include "Host/ParameterBridge.h"
#include "MIDI/MIDIRouter.h"
#include "AI/MCPServer.h"

namespace snap {

class SnappySnapProcessor : public juce::AudioProcessor {
public:
    SnappySnapProcessor();
    ~SnappySnapProcessor() override;
    
    // AudioProcessor interface
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    
    const juce::String getName() const override { return "SnappySnap"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;
    
    // Component access
    PluginHostManager& getHostManager() { return hostManager; }
    ParameterBridge& getParameterBridge() { return paramBridge; }
    SnapshotBank& getSnapshotBank() { return snapshotBank; }
    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts; }
    
    // Morph control (thread-safe)
    void setMorphX(float x) { morphX.store(x); }
    void setMorphY(float y) { morphY.store(y); }
    void setFaderPos(float pos) { faderPos.store(pos); }
    void setMorphSource(MorphSource source) { morphSource.store(static_cast<int>(source)); }
    void setPhysicsMode(MorphMode mode) { physicsMode.store(static_cast<int>(mode)); }
    
    float getMorphX() const { return morphX.load(); }
    float getMorphY() const { return morphY.load(); }
    float getFaderPos() const { return faderPos.load(); }
    int getMorphSource() const { return morphSource.load(); }
    
    // Command queue for MCP
    bool enqueueParameterSet(int paramIndex, float normalizedValue);
    bool recallSnapshotQueued(int slot);
    int enqueueParameterState(const std::vector<float>& normalizedValues);

private:
    juce::AudioProcessorValueTreeState apvts;
    PluginHostManager hostManager;
    ParameterBridge paramBridge;
    SnapshotBank snapshotBank;
    MorphProcessor morphProcessor;
    MIDIRouter midiRouter;
    MCPServer mcpServer;
    
    // Atomic parameters (thread-safe)
    std::atomic<float> morphX{0.5f};
    std::atomic<float> morphY{0.5f};
    std::atomic<float> faderPos{0.0f};
    std::atomic<int> morphSource{0};  // 0=XY, 1=Fader
    std::atomic<int> physicsMode{0};  // 0=Direct, 1=Elastic, 2=Drift
    std::atomic<int> elasticPreset{1}; // 0=Slow, 1=Medium, 2=Heavy
    std::atomic<float> driftSpeed{0.3f};
    std::atomic<float> driftDistance{0.4f};
    std::atomic<float> driftChaos{0.5f};
    std::atomic<float> smoothingRate{0.95f};
    
    // Command queue for MCP -> Audio thread
    struct ParamCommand {
        int paramIndex;
        float value;
    };
    static constexpr int QUEUE_SIZE = 1024;
    LockFreeQueue<ParamCommand, QUEUE_SIZE> commandQueue;
    std::mutex commandQueueProducerMutex;
    
    // Processing state
    std::vector<float> morphOutput;
    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;
    std::atomic<bool> prepared{false};
    
    // Instance identity
    InstanceIdentity instanceIdentity;
    
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SnappySnapProcessor)
};

} // namespace snap
```

---

## 5. Thread Safety Model

### 5.1 Thread Architecture

| Thread | Responsibilities | Constraints |
|--------|------------------|-------------|
| **Audio Thread** | processBlock(), parameter interpolation, hosted plugin processing | **NO blocking, NO allocation** |
| **Message Thread** | UI updates, user interaction | Can block |
| **MCP Thread** | JSON-RPC server, AI command processing | Can block, commands queued to audio |

### 5.2 Synchronization Primitives

```cpp
// Lock-Free Queue (SPSC)
template<typename T, size_t Capacity>
class LockFreeQueue {
    std::array<T, Capacity> buffer;
    std::atomic<size_t> head{0};
    std::atomic<size_t> tail{0};
    
public:
    bool push(const T& item);
    bool pop(T& item);
    size_t freeSpaceApprox() const;
};

// SeqLock for snapshot bank
class SeqLock {
    std::atomic<uint32_t> sequence{0};
    std::mutex writeMutex;
    
public:
    void beginWrite();
    void endWrite();
    bool tryReadLocked(std::function<void()> reader);
};

// Atomic parameters
std::atomic<float> morphX{0.5f};  // Lock-free reads/writes
```

### 5.3 Data Flow Safety

```
UI Thread (User interaction)
    │
    ▼
Atomic writes ───────────────► Audio Thread reads (lock-free)
    │
    ▼
Lock-Free Queue ─────────────► Audio Thread drains (SPSC)
    │
    ▼
SeqLock (SnapshotBank) ──────► Audio Thread reads (wait-free)
```

---

## 6. Build System

### 6.1 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.24)
project(SnappySnap VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE
add_subdirectory(JUCE)

# nlohmann/json for MCP
include(FetchContent)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(json)

# Plugin
juce_add_plugin(SnappySnap
    COMPANY_NAME "Electric Smudge"
    PLUGIN_MANUFACTURER_CODE "Esmu"
    PLUGIN_CODE "Snap"
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "SnappySnap"
    
    VST3_CATEGORIES "Tools"
    AU_MAIN_TYPE "kAudioUnitType_Effect"
    
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    
    COPY_PLUGIN_AFTER_BUILD TRUE
)

target_sources(SnappySnap PRIVATE
    # Plugin
    src/Plugin/SnappySnapProcessor.cpp
    src/Plugin/SnappySnapEditor.cpp
    
    # Core
    src/Core/InterpolationEngine.cpp
    src/Core/PhysicsEngine.cpp
    src/Core/GeneticEngine.cpp
    src/Core/MorphProcessor.cpp
    src/Core/SnapshotBank.cpp
    
    # Host
    src/Host/PluginHostManager.cpp
    src/Host/ParameterBridge.cpp
    src/Host/PluginScanner.cpp
    
    # AI
    src/AI/MCPServer.cpp
    src/AI/MCPToolHandler.cpp
    src/AI/InstanceRegistry.cpp
    
    # MIDI
    src/MIDI/MIDIRouter.cpp
    
    # Preset
    src/Preset/MetaPresetManager.cpp
    src/Preset/PresetSerializer.cpp
    
    # UI
    src/UI/MorphPad.cpp
    src/UI/SnapFader.cpp
    src/UI/SnapshotRing.cpp
    src/UI/BreedingPanel.cpp
    src/UI/MacroKnobStrip.cpp
    src/UI/PluginBrowserPanel.cpp
    src/UI/AIStatusPanel.cpp
)

target_include_directories(SnappySnap PRIVATE
    src
    ${json_SOURCE_DIR}
)

target_link_libraries(SnappySnap PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_dsp
    juce::juce_gui_extra
)

# Platform-specific
if(WIN32)
    target_compile_options(SnappySnap PRIVATE /W4 /WX-)
    # Large stack for FL Studio compatibility
    target_link_options(SnappySnap PRIVATE /STACK:4194304)
elseif(APPLE)
    target_compile_options(SnappySnap PRIVATE -Wall -Wextra)
endif()

# SIMD flags
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-mavx2" COMPILER_SUPPORTS_AVX2)
if(COMPILER_SUPPORTS_AVX2)
    target_compile_options(SnappySnap PRIVATE -mavx2)
endif()
```

---

## 7. Implementation Roadmap

### Phase 1: Core Infrastructure (Week 1-2)
- [ ] Set up CMake build system with JUCE
- [ ] Implement LockFreeQueue and SeqLock
- [ ] Create basic processor/editor skeleton
- [ ] Implement ParameterState and SnapshotBank

### Phase 2: Plugin Hosting (Week 3-4)
- [ ] Implement PluginHostManager (VST3/AU loading)
- [ ] Create ParameterBridge for parameter mapping
- [ ] Add plugin browser/scanner
- [ ] Implement hosted plugin window embedding

### Phase 3: DSP Engine (Week 5-6)
- [ ] Implement InterpolationEngine (1D/2D)
- [ ] Add SIMD optimizations (AVX/SSE)
- [ ] Implement PhysicsEngine (Elastic/Drift)
- [ ] Create MorphProcessor coordinator

### Phase 4: UI Components (Week 7-8)
- [ ] MorphPad (XY pad with clock layout)
- [ ] SnapFader (vertical fader)
- [ ] SnapshotRing (12-slot display)
- [ ] BreedingPanel (genetic algorithm UI)
- [ ] MacroKnobStrip (8 assignable knobs)
- [ ] PluginBrowserPanel

### Phase 5: MIDI & Control (Week 9)
- [ ] Implement MIDIRouter
- [ ] Add snapshot triggering via MIDI notes
- [ ] CC control for fader position
- [ ] Program change for preset switching

### Phase 6: AI Integration (Week 10-11)
- [ ] Implement MCPServer (JSON-RPC)
- [ ] Create MCPToolHandler
- [ ] Add InstanceRegistry for multi-instance
- [ ] Test with Claude Desktop

### Phase 7: Preset System (Week 12)
- [ ] MetaPresetManager (16 banks × 128 presets)
- [ ] PresetSerializer (XML/JSON)
- [ ] State persistence for hosted plugins

### Phase 8: Polish & Testing (Week 13-14)
- [ ] DAW compatibility testing
- [ ] Performance optimization
- [ ] UI theming and aesthetics
- [ ] Documentation

---

*This architecture provides a complete blueprint for building a SnappySnap-style parameter morphing plugin host with professional-grade audio threading, AI integration, and real-time performance.*
