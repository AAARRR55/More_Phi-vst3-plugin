# Morphy - Complete Architectural Blueprint
## High-Performance VST3/AU Parameter Morphing Plugin with AI Integration

**Version:** 1.0
**Date:** 2026-02-18
**Status:** Architecture Complete - Ready for Implementation

---

## Executive Summary

Morphy is an advanced parameter morphing engine for professional audio production, compatible with FL Studio and all major DAWs. The plugin enables real-time interpolation and crossfading between parameter states of hosted plugins, with groundbreaking MCP (Model Context Protocol) integration for AI-driven automation.

### Key Features
- **Parameter Morphing Engine**: Capture, interpolate, and crossfade between plugin states (similar to Kilohearts Snap Heap)
- **AI Integration**: Bidirectional MCP client for intelligent parameter automation
- **Vector-Based UI**: Hardware-accelerated morphing pad with real-time visual feedback
- **Cross-Platform**: VST3 (Windows/macOS/Linux) and AU (macOS) support
- **Real-Time Safe**: Lock-free architecture for glitch-free audio

---

## System Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              MORPHY PLUGIN                                   │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌────────────────────────────────────────────────────────────────────┐    │
│  │                         UI LAYER (Message Thread)                   │    │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────────┐ │    │
│  │  │  MorphingPad │  │  Snapshot    │  │   AI Control Panel       │ │    │
│  │  │  (Vector/GL) │  │  Panel       │  │   - Status Indicator     │ │    │
│  │  │  - 2D/3D Viz │  │  - 12 Slots  │  │   - Suggestion Cards     │ │    │
│  │  │  - Path Viz  │  │  - Drag/Drop │  │   - Confidence Display   │ │    │
│  │  └──────────────┘  └──────────────┘  └──────────────────────────┘ │    │
│  └────────────────────────────────────────────────────────────────────┘    │
│                                      │                                       │
│                        ┌─────────────┴─────────────┐                        │
│                        │    Lock-Free Queue       │                        │
│                        │    (Parameter Updates)   │                        │
│                        └─────────────┬─────────────┘                        │
│                                      │                                       │
│  ┌────────────────────────────────────────────────────────────────────┐    │
│  │                      AUDIO LAYER (Real-Time Thread)                 │    │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────────┐ │    │
│  │  │ MorphEngine  │  │ Smoothed     │  │   Audio Analysis         │ │    │
│  │  │ - 4 Modes    │  │ Parameters   │  │   - RMS/Peak Metering    │ │    │
│  │  │ - Interp.    │  │ - Per-param  │  │   - FFT Spectrum         │ │    │
│  │  │ - 12 Slots   │  │   smoothing  │  │   - Transient Detection  │ │    │
│  │  └──────────────┘  └──────────────┘  └──────────────────────────┘ │    │
│  └────────────────────────────────────────────────────────────────────┘    │
│                                      │                                       │
│                        ┌─────────────┴─────────────┐                        │
│                        │    Command Queue         │                        │
│                        │    (AI <-> Audio)        │                        │
│                        └─────────────┬─────────────┘                        │
│                                      │                                       │
│  ┌────────────────────────────────────────────────────────────────────┐    │
│  │                     AI LAYER (Background Thread)                    │    │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────────┐ │    │
│  │  │ MCPClient    │  │ AudioBridge  │  │   Feature Extraction     │ │    │
│  │  │ - WebSocket  │  │ - Thread     │  │   - Spectral Features    │ │    │
│  │  │ - HTTP/SSE   │  │   Isolation  │  │   - Temporal Features    │ │    │
│  │  │ - JSON-RPC   │  │ - Async Cmd  │  │   - Parameter Encoding   │ │    │
│  │  └──────────────┘  └──────────────┘  └──────────────────────────┘ │    │
│  └────────────────────────────────────────────────────────────────────┘    │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      ▼
                        ┌─────────────────────────┐
                        │    External MCP Server   │
                        │    (AI Model Backend)    │
                        │    - OpenAI             │
                        │    - Anthropic          │
                        │    - Local LLM          │
                        └─────────────────────────┘
```

---

## 1. Core Plugin Architecture

### 1.1 Plugin Framework Layer

**Technology Stack:**
- **Framework**: JUCE 7.x (industry standard)
- **Formats**: VST3, AU, Standalone
- **Platforms**: Windows 10/11, macOS (Intel + Apple Silicon), Linux

```cpp
// Core plugin class hierarchy
class MorphyAudioProcessor : public juce::AudioProcessor {
    // Real-time audio processing
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    // Thread-safe parameter tree
    juce::AudioProcessorValueTreeState m_APVTS;

    // Core subsystems
    std::unique_ptr<MorphingEngine>       m_morphingEngine;
    std::unique_ptr<ParameterManager>     m_parameterManager;
    std::unique_ptr<MCPClient>            m_mcpClient;
    std::unique_ptr<AudioAnalysisEngine>  m_analysisEngine;
};
```

### 1.2 Thread Model

| Thread | Responsibility | Safety Requirements |
|--------|---------------|---------------------|
| **Audio Thread** | Process audio, interpolate parameters | Lock-free, no allocations |
| **UI Thread** | Render graphics, handle input | Can block, uses locks |
| **AI Thread** | MCP communication, feature extraction | Async, isolated from audio |
| **Worker Pool** | State serialization, file I/O | Background tasks |

### 1.3 Parameter System

```cpp
// Parameter layout (normalized 0.0-1.0)
static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout() {
    return {
        // Morphing position (XY pad)
        std::make_unique<juce::AudioParameterFloat>("morphX", "Morph X", 0.0f, 1.0f, 0.5f),
        std::make_unique<juce::AudioParameterFloat>("morphY", "Morph Y", 0.0f, 1.0f, 0.5f),

        // Morphing mode
        std::make_unique<juce::AudioParameterChoice>(
            "morphMode", "Morph Mode",
            juce::StringArray{"Linear 1D", "XY 2D", "Elastic", "Drift"}, 1),

        // Interpolation settings
        std::make_unique<juce::AudioParameterChoice>(
            "interpCurve", "Interpolation Curve",
            juce::StringArray{"Linear", "Smoothstep", "Exponential", "Bezier"}, 1),
        std::make_unique<juce::AudioParameterFloat>("morphSpeed", "Morph Speed", 0.01f, 10.0f, 1.0f),

        // AI integration
        std::make_unique<juce::AudioParameterBool>("aiEnabled", "AI Automation", false),
        std::make_unique<juce::AudioParameterChoice>(
            "aiMode", "AI Mode",
            juce::StringArray{"Suggest", "Automate", "Learn"}, 0),
    };
}
```

---

## 2. Parameter Morphing Engine

### 2.1 Architecture

The morphing engine is based on 12 snapshots arranged in a clock-like layout around the XY pad, supporting four morphing modes:

```cpp
enum class MorphMode {
    Linear1D,   // Single knob through all snapshots
    XY2D,       // Distance-weighted interpolation (Shepard's method)
    Elastic,    // Spring-mass-damper physics
    Drift       // Perlin noise autonomous movement
};

class MorphingEngine {
public:
    // Snapshot management
    bool captureSnapshot(int slotIndex);
    bool loadSnapshot(int slotIndex);
    void clearSnapshot(int slotIndex);

    // Morphing control
    void setMorphPosition(float x, float y);
    void setMorphMode(MorphMode mode);
    void setInterpolationCurve(InterpolationCurve curve);

    // Real-time processing (called from audio thread)
    void process(juce::AudioBuffer<float>& buffer);

private:
    std::array<SnapshotSlot, 12> m_slots;
    std::atomic<float> m_morphX{0.5f}, m_morphY{0.5f};
    MorphMode m_mode = MorphMode::XY2D;

    // Lock-free parameter queue for thread-safe updates
    LockFreeSPSCQueue<ParameterUpdate, 256> m_paramQueue;
};
```

### 2.2 Interpolation Algorithms

```cpp
namespace InterpolationCurves {
    // Linear: a + t * (b - a)
    float linear(float a, float b, float t);

    // Smoothstep: 3t² - 2t³
    float smoothstep(float a, float b, float t);

    // Smootherstep: 6t⁵ - 15t⁴ + 10t³
    float smootherstep(float a, float b, float t);

    // Exponential: a * pow(b/a, t)
    float exponential(float a, float b, float t);

    // Cubic Bezier with control points
    float bezier(float p0, float p1, float p2, float p3, float t);

    // Sigmoid (S-curve)
    float sigmoid(float a, float b, float t, float steepness = 1.0f);
}
```

### 2.3 Snapshot Data Structure

```cpp
struct ParameterValue {
    enum Type { Float, Int, Bool, Enum } type;
    union {
        float floatValue;
        int intValue;
        bool boolValue;
        int enumValue;
    };
    float normalizedValue;  // Always 0.0-1.0 for interpolation
};

struct SnapshotSlot {
    juce::String name;
    juce::Colour color;
    std::vector<ParameterValue> parameters;
    juce::int64 timestamp;
    bool isEmpty = true;

    // Serialization
    juce::XmlElement* toXML() const;
    static SnapshotSlot fromXML(const juce::XmlElement&);
};
```

### 2.4 Real-Time Safety Features

- **Lock-Free Queue**: SPSC queue for parameter updates from UI to audio thread
- **Pre-Allocated Buffers**: All buffers allocated during `prepareToPlay()`
- **Atomic Positions**: Morph X/Y use `std::atomic<float>` for thread-safe updates
- **Smoothed Parameters**: Per-parameter linear ramping to prevent clicks
- **Protected Parameters**: Sanity mode filters bypass/mute/volume from morphing

---

## 3. MCP Client Integration

### 3.1 Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      MCP CLIENT LAYER                        │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌─────────────┐     ┌─────────────┐     ┌─────────────┐   │
│  │ MCPClient   │     │ AIService   │     │ MCPAudio    │   │
│  │ (Abstract)  │────▶│ Manager     │────▶│ Bridge      │   │
│  └─────────────┘     └─────────────┘     └─────────────┘   │
│        │                   │                    │           │
│        ▼                   ▼                    ▼           │
│  ┌─────────────┐     ┌─────────────┐     ┌─────────────┐   │
│  │ HTTP Client │     │ OpenAI      │     │ Feature     │   │
│  │ WS Client   │     │ Anthropic   │     │ Extractor   │   │
│  │ SSE Client  │     │ Local LLM   │     │ Serializer  │   │
│  └─────────────┘     └─────────────┘     └─────────────┘   │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 Protocol Layer

```cpp
// MCP Protocol message types
struct MCPRequest {
    std::string jsonrpc = "2.0";
    std::string id;
    std::string method;
    nlohmann::json params;
};

struct MCPResponse {
    std::string jsonrpc = "2.0";
    std::string id;
    std::optional<nlohmann::json> result;
    std::optional<MCPError> error;
};

// Client interface
class MCPClient {
public:
    virtual ~MCPClient() = default;

    virtual bool connect(const std::string& uri) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    virtual std::future<MCPResponse> sendRequest(const MCPRequest&) = 0;
    virtual void sendNotification(const std::string& method, const nlohmann::json& params) = 0;

    // Callbacks
    virtual void setOnMessageReceived(std::function<void(const MCPResponse&)>) = 0;
    virtual void setOnConnected(std::function<void()>) = 0;
    virtual void setOnDisconnected(std::function<void()>) = 0;
};
```

### 3.3 AI Integration Features

```cpp
// AI tools exposed to MCP server
namespace AITools {
    // Analyze current audio/parameter state
    nlohmann::json analyzeAudio(const AudioFeatures& features);
    nlohmann::json analyzeParameters(const std::vector<ParameterValue>& params);

    // Generate morphing sequences
    nlohmann::json generateMorphPath(const MorphPathRequest& request);

    // Suggest parameter configurations
    nlohmann::json suggestParameters(const std::string& description);

    // Learn from user gestures
    void learnPattern(const MorphGesture& gesture);
}

// Audio feature extraction (for AI analysis)
struct AudioFeatures {
    float rmsLevel;           // Overall level
    float peakLevel;          // Peak amplitude
    float spectralCentroid;   // Brightness
    float spectralFlatness;   // Noisiness
    float zeroCrossingRate;   // Texture
    float transientDensity;   // Rhythmic activity
    std::array<float, 32> spectrumBands;  // Frequency bands
};
```

### 3.4 Thread Isolation

```cpp
// Bridge between audio thread and AI thread
class MCPAudioBridge {
public:
    // Called from audio thread (real-time safe)
    void pushAudioFeatures(const AudioFeatures& features);
    void pushAICommand(AICommand cmd);

    // Called from AI thread
    std::optional<AudioFeatures> popAudioFeatures();
    std::optional<AICommand> popAICommand();

private:
    // Lock-free queues for thread isolation
    LockFreeSPSCQueue<AudioFeatures, 64> m_featureQueue;
    LockFreeSPSCQueue<AICommand, 256> m_commandQueue;
};
```

---

## 4. Vector-Based UI System

### 4.1 Component Hierarchy

```
MorphyAudioProcessorEditor
├── MorphingPadComponent (central 2D/3D pad)
│   ├── GridOverlay (optional grid lines)
│   ├── SnapshotMarkers[12] (clock arrangement)
│   ├── PositionIndicator (current morph position)
│   ├── PathHistory (recent morph path)
│   └── AIOverlay (AI suggestions display)
│
├── LeftPanel
│   ├── SnapshotPanel (12 snapshot slots)
│   ├── InterpolationPanel (curve selector)
│   └── MorphModePanel (mode selector)
│
├── RightPanel
│   ├── AIControlPanel
│   │   ├── ConnectionStatus
│   │   ├── SuggestionCards
│   │   └── ConfidenceMeter
│   ├── AnalysisPanel
│   │   ├── LevelMeters
│   │   └── SpectrumDisplay
│   └── TransportControls
│
└── BottomBar
    ├── PresetSelector
    ├── SettingsButton
    └── HelpButton
```

### 4.2 Rendering Pipeline

```cpp
// Vector rendering with hardware acceleration
class VectorRenderer {
public:
    // Platform-specific backend selection
    enum class Backend { OpenGL, Metal, D3D11 };

    void initialize(juce::Component& component);
    void beginFrame();
    void endFrame();

    // Vector primitives
    void drawCircle(juce::Point<float> center, float radius, juce::Colour fill, juce::Colour stroke);
    void drawPath(const std::vector<juce::Point<float>>& points, float thickness, juce::Colour color);
    void drawText(const juce::String& text, juce::Rectangle<float> bounds, const Font& font);

    // Performance features
    void cacheShape(ShapeID id, const ShapeData& data);
    void drawCachedShape(ShapeID id, juce::AffineTransform transform);

private:
    Backend m_backend;
    std::unique_ptr<juce::OpenGLContext> m_glContext;
    // Shape cache for performance
    std::unordered_map<ShapeID, CachedShape> m_shapeCache;
};
```

### 4.3 Morphing Pad Design

```cpp
class MorphingPadComponent : public AnimatedComponent {
public:
    // Interaction
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

    // Visualization modes
    enum class VisualizationMode { Flat2D, Surface3D };
    void setVisualizationMode(VisualizationMode mode);

    // AI overlay
    void showAISuggestions(const std::vector<AISuggestion>& suggestions);
    void hideAISuggestions();

protected:
    void updateAnimation(float deltaTime) override;
    void renderContent(VectorRenderer& renderer);

private:
    VisualizationMode m_vizMode = VisualizationMode::Flat2D;

    // 12 snapshots in clock arrangement
    std::array<SnapshotMarker, 12> m_snapshots;

    // Path history (last N positions)
    RingBuffer<juce::Point<float>, 256> m_pathHistory;

    // AI suggestion overlays
    std::vector<AISuggestionOverlay> m_aiOverlays;
};
```

### 4.4 Performance Targets

| Metric | Target | Minimum |
|--------|--------|---------|
| Frame Rate | 60 FPS | 30 FPS |
| Input Latency | <16ms | <33ms |
| Memory | ~62 MB | ~100 MB |
| GPU Vertices | <50K/frame | <100K |
| Draw Calls | <500/frame | <1000 |

### 4.5 Theming System

```cpp
struct Theme {
    juce::Colour background;
    juce::Colour surface;
    juce::Colour primary;
    juce::Colour secondary;
    juce::Colour accent;
    juce::Colour text;
    juce::Colour textDim;
    juce::Colour border;

    // Animation timing
    float animationDuration = 0.2f;
    juce::String animationEasing = "cubic";
};

// Built-in themes
namespace Themes {
    Theme dark();        // Default studio theme
    theme light();       // Light mode
    theme neon();        // High-contrast neon
    theme minimal();     // Minimal grayscale
}
```

---

## 5. C++ Implementation Strategy

### 5.1 Code Organization

```
D:/morphy/
├── CMakeLists.txt
├── vcpkg.json
├── docs/
│   ├── MASTER_ARCHITECTURAL_BLUEPRINT.md (this file)
│   ├── ARCHITECTURE.md
│   └── CPP_IMPLEMENTATION_STRATEGY.md
├── src/
│   ├── core/
│   │   ├── MorphyAudioProcessor.h/cpp
│   │   ├── MorphyAudioProcessorEditor.h/cpp
│   │   └── ParameterManager.h/cpp
│   ├── dsp/
│   │   ├── Processors/
│   │   ├── Math/
│   │   └── SIMD/
│   ├── morphing/
│   │   ├── MorphEngine.h/cpp
│   │   ├── Snapshot.h/cpp
│   │   ├── InterpolationCurves.h
│   │   └── SmoothedParameter.h
│   ├── mcp/
│   │   ├── MCPClient.h/cpp
│   │   ├── MCPProtocol.h
│   │   ├── AIServiceManager.h/cpp
│   │   └── MCPAudioBridge.h/cpp
│   ├── ui/
│   │   ├── Components/
│   │   ├── Pages/
│   │   └── Theme/
│   └── util/
│       ├── Memory/
│       ├── Concurrency/
│       └── Logging/
└── tests/
    ├── Unit/
    ├── Integration/
    └── Performance/
```

### 5.2 Build System (CMake)

```cmake
# Key CMake configuration
cmake_minimum_required(VERSION 3.20)
project(Morphy VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Plugin formats
option(MORPHY_BUILD_VST3 "Build VST3 plugin" ON)
option(MORPHY_BUILD_AU "Build Audio Unit (macOS)" ON)
option(MORPHY_BUILD_STANDALONE "Build standalone app" ON)

# JUCE configuration
juce_add_plugin(Morphy
    PLUGIN_NAME "Morphy"
    PLUGIN_MANUFACTURER "Morphy Audio"
    PLUGIN_CODE Morph

    FORMATS ${MORPHY_PLUGIN_FORMATS}

    # Copy to DAW plugin folders after build
    COPY_PLUGIN_AFTER_BUILD ON
)
```

### 5.3 Real-Time Safety Guidelines

**Banned in Audio Thread:**
- `new`/`delete` (any heap allocation)
- `malloc`/`free`
- `std::vector::push_back()` (may reallocate)
- `std::string` operations (may allocate)
- `std::map`/`std::unordered_map` access
- Mutex locks (`std::mutex`, `juce::CriticalSection`)
- File I/O
- Network operations
- `std::cout`/`printf`

**Required in Audio Thread:**
- Pre-allocated buffers
- Lock-free data structures
- Atomic operations
- Stack-only allocations
- Memory pools

### 5.4 SIMD Optimization

```cpp
// SIMD abstraction layer
namespace morphy::dsp::simd {

enum class SIMDArchitecture { Scalar, SSE, AVX, AVX2, NEON };

// Runtime detection
SIMDArchitecture detectCPUArchitecture();

// Template dispatch
template<SIMDArchitecture Arch>
void applyGain(float* buffer, size_t samples, float gain);

// Specialized implementations
template<> void applyGain<SIMDArchitecture::AVX2>(float*, size_t, float);
template<> void applyGain<SIMDArchitecture::NEON>(float*, size_t, float);

}
```

---

## 6. Testing Strategy

### 6.1 Test Categories

| Category | Focus | Tools |
|----------|-------|-------|
| **Unit Tests** | Individual components | Catch2 |
| **Integration Tests** | Component interactions | Catch2 + Mocks |
| **Performance Tests** | CPU/memory benchmarks | Google Benchmark |
| **Real-Time Tests** | Audio thread violations | Custom framework |
| **Plugin Validation** | VST3/AU compliance | SDK validators |
| **DAW Compatibility** | Host integration | Manual + Automated |

### 6.2 CI/CD Pipeline

```yaml
# GitHub Actions workflow
jobs:
  build-windows:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v3
      - name: Configure
        run: cmake -B build -G "Visual Studio 17 2022"
      - name: Build
        run: cmake --build build --config Release
      - name: Test
        run: ctest --test-dir build --output-on-failure
      - name: Validate VST3
        run: python tests/scripts/run_vst3_validator.py

  build-macos:
    runs-on: macos-latest
    strategy:
      matrix:
        arch: [x86_64, arm64]
    steps:
      - name: Build Universal
        run: cmake --build build --config Release
      - name: Validate AU
        run: auval -v aufx Morph Morp
```

### 6.3 Acceptance Criteria

**Real-Time Safety:**
- [ ] Zero allocations in audio thread during processBlock()
- [ ] No lock contention under 1000+ concurrent parameter updates
- [ ] CPU usage < 5% at 48kHz/512 samples

**Audio Quality:**
- [ ] No clicks/pops during parameter changes
- [ ] THD+N < -100dB at unity gain
- [ ] Sample-accurate automation

**DAW Compatibility:**
- [ ] FL Studio 20+ (Windows/macOS)
- [ ] Ableton Live 11+
- [ ] Reaper 6+
- [ ] Logic Pro (macOS)
- [ ] Cubase 12+

---

## 7. Implementation Roadmap

### Phase 1: Foundation (Weeks 1-4)
- [ ] Project setup (CMake, vcpkg, JUCE)
- [ ] Core AudioProcessor implementation
- [ ] Parameter system
- [ ] Basic morphing engine (2 snapshots, linear interpolation)

### Phase 2: Core Features (Weeks 5-8)
- [ ] Full 12-snapshot system
- [ ] All interpolation curves
- [ ] XY2D morphing mode
- [ ] State persistence

### Phase 3: Advanced Morphing (Weeks 9-12)
- [ ] Elastic physics mode
- [ ] Drift mode (Perlin noise)
- [ ] Morph path recording
- [ ] Parameter smoothing

### Phase 4: UI Implementation (Weeks 13-16)
- [ ] Vector rendering system
- [ ] Morphing pad component
- [ ] Snapshot panel
- [ ] Theme system

### Phase 5: AI Integration (Weeks 17-20)
- [ ] MCP client implementation
- [ ] Feature extraction
- [ ] AI control panel
- [ ] Suggestion system

### Phase 6: Polish & Testing (Weeks 21-24)
- [ ] DAW compatibility testing
- [ ] Performance optimization
- [ ] Documentation
- [ ] Beta release

---

## 8. Document References

| Document | Location | Description |
|----------|----------|-------------|
| Architecture Guide | `docs/ARCHITECTURE.md` | Detailed component architecture |
| C++ Implementation | `docs/CPP_IMPLEMENTATION_STRATEGY.md` | Coding standards, build system |
| UI Design Summary | `docs/design/UI-DESIGN-SUMMARY.md` | Visual design specifications |
| UI System Design | `docs/design/ui-system-design.md` | UI architecture details |
| UI Wireframes | `docs/design/ui-wireframes-interaction-flows.md` | Wireframes and user flows |
| Testing Strategy | `tests/testing-strategy.md` | Complete testing approach |

---

## Appendix: Technology Stack Summary

| Component | Technology | Version |
|-----------|------------|---------|
| Language | C++ | 20 |
| Framework | JUCE | 7.x |
| Build System | CMake | 3.20+ |
| Package Manager | vcpkg | Latest |
| Test Framework | Catch2 | 3.x |
| JSON Library | nlohmann/json | 3.x |
| Logging | spdlog | 1.x |
| SIMD | SSE/AVX/NEON | Runtime detection |
| Graphics | OpenGL/Metal | 3.3+/Metal 2 |

---

**Document Status:** Complete
**Last Updated:** 2026-02-18
**Next Review:** After Phase 1 implementation
