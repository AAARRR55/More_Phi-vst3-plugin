# Morphy Architecture Guide

This document provides a detailed technical overview of Morphy's architecture for developers and contributors.

## System Architecture

### Audio Processing Pipeline

```
┌──────────────────────────────────────────────────────────────────┐
│                        Audio Thread                               │
│  ┌────────────┐    ┌────────────┐    ┌────────────────┐         │
│  │  Input     │───▶│  Morphing  │───▶│   Output       │         │
│  │  Buffer    │    │   Engine   │    │   Buffer       │         │
│  └────────────┘    └─────┬──────┘    └────────────────┘         │
│                          │                                       │
│                    ┌─────▼──────┐                               │
│                    │  Analysis  │                               │
│                    │  (RMS/FFT) │                               │
│                    └─────┬──────┘                               │
└──────────────────────────┼───────────────────────────────────────┘
                           │
                    Lock-Free Queue
                           │
┌──────────────────────────▼───────────────────────────────────────┐
│                        UI Thread                                  │
│  ┌────────────┐    ┌────────────┐    ┌────────────────┐         │
│  │  Visual    │◀───│  Message   │◀───│   AI Client    │         │
│  │  Feedback  │    │  Handler   │    │   (Optional)   │         │
│  └────────────┘    └────────────┘    └────────────────┘         │
└──────────────────────────────────────────────────────────────────┘
```

### Thread Safety Model

Morphy uses a careful thread safety model to ensure reliable real-time audio processing:

1. **Audio Thread**: Never blocks, uses lock-free data structures
2. **UI Thread**: Can block, handles all user interaction
3. **AI Thread**: Separate thread for MCP communication

#### Thread-Safe Communication

- `LockFreeQueue<T, Capacity>`: Single-producer, single-consumer queue
- `std::atomic<T>`: For simple value updates
- `juce::CriticalSection`: For non-real-time operations

## Core Components

### PluginProcessor

The main audio processor handles:
- Audio buffer processing
- Parameter management
- Plugin state persistence
- Coordination between subsystems

```cpp
class MorphyAudioProcessor : public juce::AudioProcessor {
    // Real-time processing
    void processBlock(juce::AudioBuffer<float>& buffer) override;
    
    // Thread-safe parameter access
    juce::AudioProcessorValueTreeState m_APVTS;
    
    // Morphing engine
    MorphingEngine m_morphingEngine;
    
    // AI integration (optional)
    std::unique_ptr<MCPClient> m_mcpClient;
};
```

### MorphingEngine

The morphing engine is responsible for:
- Storing parameter snapshots
- Interpolating between states
- Managing morphing trajectories

```cpp
class MorphingEngine {
    // Snapshot storage
    std::array<SnapshotSlot, 4> m_slots;
    
    // Current position (atomic for thread safety)
    std::atomic<float> m_morphX{0.5f};
    std::atomic<float> m_morphY{0.5f};
    
    // Interpolation function
    InterpolationCurves::InterpolationFunction m_interpolationFunc;
};
```

### Interpolation System

Multiple interpolation curves are supported:

```cpp
namespace InterpolationCurves {
    float linear(float a, float b, float t);
    float cosine(float a, float b, float t);
    float cubic(float a, float b, float t);
    float smoothstep(float a, float b, float t);
    float bezier(float p0, float p1, float p2, float p3, float t);
}
```

### MCP Client Architecture

The MCP client enables AI integration:

```
┌─────────────────────────────────────────────────────┐
│                    MCP Client                        │
│  ┌─────────────┐    ┌─────────────┐                │
│  │  WebSocket  │◀──▶│   Message   │                │
│  │   Layer     │    │   Handler   │                │
│  └─────────────┘    └──────┬──────┘                │
│                            │                        │
│  ┌─────────────┐    ┌──────▼──────┐                │
│  │   Request   │◀──▶│  Response   │                │
│  │   Manager   │    │   Router    │                │
│  └─────────────┘    └─────────────┘                │
└─────────────────────────────────────────────────────┘
```

### UI Component Hierarchy

```
MorphyAudioProcessorEditor
├── MorphingPadComponent (central 2D pad)
│   ├── Grid overlay
│   ├── Snapshot slots (4 corners)
│   ├── Position indicator
│   └── Trajectory display
├── ParameterSnapshotComponent
│   └── Snapshot slot buttons
├── VisualFeedback (overlay)
│   ├── Level meters
│   └── Spectrum display
├── WaveformDisplay
├── Control panels
│   ├── Left panel (morphing controls)
│   └── Right panel (output + AI controls)
```

## Parameter System

### Parameter Layout

```cpp
static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout() {
    return {
        // Morphing position
        std::make_unique<juce::AudioParameterFloat>("morphX", "Morph X", 0.0f, 1.0f, 0.5f),
        std::make_unique<juce::AudioParameterFloat>("morphY", "Morph Y", 0.0f, 1.0f, 0.5f),
        
        // Morphing settings
        std::make_unique<juce::AudioParameterChoice>("morphMode", "Morph Mode", {...}),
        std::make_unique<juce::AudioParameterFloat>("morphSpeed", "Morph Speed", 0.01f, 10.0f, 1.0f),
        
        // AI integration
        std::make_unique<juce::AudioParameterBool>("aiEnabled", "AI Automation", false),
        // ...
    };
}
```

### State Persistence

Plugin state is saved as XML:

```xml
<MORPHY_STATE>
  <MORPHY_STATE param1="value1" .../>
  <MORPHING_ENGINE>
    <SNAPSHOTS>
      <SLOT index="0" color="...">
        {snapshot JSON data}
      </SLOT>
    </SNAPSHOTS>
  </MORPHING_ENGINE>
  <AI_INTEGRATION mode="0" automationActive="false"/>
</MORPHY_STATE>
```

## MCP Protocol Implementation

### Message Types

1. **Request**: Requires response
   ```json
   {
     "jsonrpc": "2.0",
     "id": "req_123",
     "method": "tools/call",
     "params": { ... }
   }
   ```

2. **Response**: Answer to request
   ```json
   {
     "jsonrpc": "2.0",
     "id": "req_123",
     "result": { ... }
   }
   ```

3. **Notification**: No response expected
   ```json
   {
     "jsonrpc": "2.0",
     "method": "notifications/initialized"
   }
   ```

### Available AI Tools

| Tool | Description |
|------|-------------|
| `analyze_audio` | Analyze current audio context |
| `generate_morph_trajectory` | Create morphing path |
| `suggest_parameters` | Get parameter suggestions |
| `learn_pattern` | Learn from user gesture |

### AI Command Types

```cpp
enum class AICommandType {
    AnalyzeAudio,           // Request audio analysis
    GenerateMorphPath,      // Generate trajectory
    SuggestParameters,      // Parameter suggestions
    AutomateMorphing,       // Take control
    SetMorphPosition,       // Set pad position
    StartTrajectory,        // Begin playback
    StopTrajectory          // Stop playback
};
```

## Performance Considerations

### Real-Time Safety

- No memory allocation in audio thread
- Lock-free queues for cross-thread communication
- Atomic operations for parameter updates
- Pre-allocated buffers for analysis

### CPU Optimization

- SIMD-ready interpolation functions
- Efficient FFT for spectrum analysis
- Cached interpolation functions
- Minimal UI repaint regions

### Memory Management

- Fixed-size ring buffers
- Pre-allocated snapshot storage
- Object pooling for messages
- Smart pointers for owned objects

## Build Configuration

### Debug vs Release

```cmake
# Debug: Full logging, assertions enabled
set(CMAKE_BUILD_TYPE Debug)

# Release: Optimized, logging disabled
set(CMAKE_BUILD_TYPE Release)
```

### Platform-Specific Notes

**Windows**:
- VST3 output to `C:/Program Files/Common Files/VST3`
- Requires Visual Studio 2019+

**macOS**:
- AU output to `/Library/Audio/Plug-Ins/Components`
- VST3 output to `/Library/Audio/Plug-Ins/VST3`
- Requires Xcode 12+

**Linux**:
- VST3 output to `~/.vst3`
- Requires GCC 10+ or Clang 12+

## Testing

### Unit Tests

Located in `tests/` directory:

```cpp
TEST_CASE("MorphingEngine interpolation") {
    MorphingEngine engine;
    
    // Set up snapshots
    engine.setSnapshot(0, createTestSnapshot(0.0f));
    engine.setSnapshot(1, createTestSnapshot(1.0f));
    
    // Test interpolation
    engine.setMorphPosition(0.5f, 0.5f);
    auto result = engine.captureCurrentState();
    
    REQUIRE(result.getParameter("test")->currentValue == Approx(0.5f));
}
```

### Running Tests

```bash
cmake --build build --target MorphyTests
./build/bin/MorphyTests
```

## Extending Morphy

### Adding New Interpolation Modes

1. Add mode to `InterpolationMode` enum
2. Implement in `InterpolationCurves` namespace
3. Update `getFunction()` switch
4. Add to UI combo box

### Adding New AI Tools

1. Define tool in MCP server
2. Add handling in `AIIntegration::handleMessage()`
3. Implement response logic
4. Update documentation

### Creating New Themes

1. Add theme type to `ThemeType` enum
2. Implement `initializeXxxTheme()` method
3. Update `setThemeType()` switch
4. Add to theme selector

## Troubleshooting

### Common Issues

1. **Plugin not loading in DAW**
   - Check plugin format compatibility
   - Verify build architecture (x64/ARM)
   - Check code signing (macOS)

2. **AI connection fails**
   - Verify MCP server is running
   - Check WebSocket URI format
   - Review firewall settings

3. **Audio glitches**
   - Increase buffer size in DAW
   - Check for CPU overload
   - Disable unnecessary analysis

### Debug Logging

Enable verbose logging:

```cpp
Logger::getInstance().setLogLevel(LogLevel::Debug);
Logger::getInstance().setLogFile(juce::File::getSpecialLocation(
    juce::File::tempDirectory).getChildFile("morphy.log"));
```
