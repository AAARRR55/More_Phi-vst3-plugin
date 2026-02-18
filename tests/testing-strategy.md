# Morphy VST3/AU Plugin - Comprehensive Testing & Validation Strategy

## Executive Summary

This document outlines the complete quality assurance approach for the Morphy parameter morphing engine plugin, covering all aspects from SDK validation to performance benchmarking and AI integration testing.

**Testing Philosophy**: Real-time safety first, followed by audio quality, then functionality
**Primary DAW Target**: FL Studio (primary compatibility requirement)
**Secondary DAWs**: Ableton Live, Reaper, Logic Pro, Cubase, Studio One

---

## 1. Plugin Validation

### 1.1 VST3 SDK Validator Integration

**Objective**: Ensure VST3 compliance and Steinberg SDK validation

**Test Environment Setup**:
```bash
# VST3 SDK Validator location
VST3_VALIDATOR_PATH="C:/Program Files/Steinberg/VST3SDK/validator"

# Build configuration
BUILD_TYPE="Release"  # Always test release builds
PLUGIN_PATH="build/Morphy_artefacts/Release/VST3/Morphy.vst3"
```

**Validation Command**:
```bash
# Windows
${VST3_VALIDATOR_PATH}/vst3_validator.exe -r ${PLUGIN_PATH}

# macOS
/Applications/VST3Validator.app/Contents/MacOS/vst3validator -r ${PLUGIN_PATH}
```

**Required Checks**:
- [ ] VST3 header compliance
- [ ] Audio processing correctness
- [ ] Parameter automation reliability
- [ ] State persistence (preset save/load)
- [ ] Bypass state handling
- [ ] Latency reporting accuracy
- [ ] Tail time accuracy
- [ ] I/O configuration validity
- [ ] MIDI handling (if applicable)
- [ ] Memory allocation safety (no allocations in processBlock)

**Acceptance Criteria**:
- 100% of mandatory tests pass
- 0 critical, 0 major, 0 minor warnings
- Process time < 10% of buffer size at 44.1kHz
- Zero memory leaks detected

**Automation Script**:
```python
# tests/scripts/vst3_validator.py
import subprocess
import json
import sys

def run_vst3_validator(plugin_path):
    result = subprocess.run(
        ["vst3_validator", "-r", plugin_path],
        capture_output=True,
        text=True
    )

    # Parse and validate output
    # Generate JSON report
    return {
        "status": "pass" if result.returncode == 0 else "fail",
        "details": parse_validator_output(result.stdout)
    }
```

### 1.2 AU Validation Tool (macOS only)

**Objective**: Apple Audio Unit compliance validation

**Test Commands**:
```bash
# AU Validation Tool location
AUVAL_PATH="/Developer/Applications/Audio/AU Lab.app/Contents/MacOS/auval"

# Validate AU component
auval -v aufx Mrph MPAU
```

**Required Checks**:
- [ ] Component registration validity
- [ ] AU factory presets validity
- [ ] Parameter ranges and scaling
- [ ] Parameter smoothing behavior
- [ ] Bypass mode correctness
- [ ] Channel configurations
- [ ] Real-time safety
- [ ] UI/Controller communication

**Acceptance Criteria**:
- All auval tests pass
- No "Could not open" or "Error" messages
- Proper component type (aufx for effect)
- Correct manufacturer code (MPAU)

### 1.3 FL Studio Compatibility Testing

**Priority**: HIGHEST (Primary target DAW)

**Test Matrix for FL Studio**:

| FL Version | Windows 10 | Windows 11 | Notes |
|------------|------------|------------|-------|
| 21.0+ | ✓ | ✓ | Current stable |
| 20.9 | ✓ | ✓ | Previous stable |
| 20.8 | ✓ | N/A | Legacy support |

**FL Studio Specific Tests**:

1. **Plugin Loading**:
   - [ ] Plugin appears in browser
   - [ ] Plugin loads without delay
   - [ ] Multiple instances load correctly
   - [ ] Plugin survives project reload
   - [ ] Preset browser integration works

2. **Parameter Automation**:
   - [ ] Automation curves record correctly
   - [ ] Automation playback is smooth
   - [ ] Plugin parameter changes update automation
   - [ ] Automation ranges match plugin ranges
   - [ ] Min/max values respected

3. **Morphing Pad Interaction**:
   - [ ] Mouse drag on pad works smoothly
   - [ ] Parameter updates are real-time
   - [ ] Visual feedback is accurate
   - [ ] Snapshots save/recall correctly
   - [ ] Pad state persists in project

4. **Project Export**:
   - [ ] Plugin renders correctly in export
   - [ ] Bypass state respected
   - [ ] No clicks/pops at loop points
   - [ ] Tail renders completely

5. **Performance**:
   - [ ] CPU usage < 5% at 44.1kHz
   - [ ] No buffer underruns at 64 samples
   - [ ] UI remains responsive under load
   - [ ] Plugin doesn't cause FL Studio crash

**FL Studio Test Procedure**:
```
1. Create empty project
2. Add Morphy to mixer slot 1
3. Load test audio clip
4. Record 2 minutes of automation
5. Save project
6. Close and reopen FL Studio
7. Load project and verify automation playback
8. Export 30-second audio
9. Compare export vs real-time bounce
```

### 1.4 Other DAW Testing Matrix

**Ableton Live**:

| Live Version | Win 10 | Win 11 | macOS Intel | macOS ARM |
|--------------|--------|--------|-------------|-----------|
| 11.x | ✓ | ✓ | ✓ | ✓ |
| 10.x | ✓ | N/A | ✓ | ✓ |

**Ableton Specific Tests**:
- [ ] Browser categorization
- [ ] Plugin delay compensation (PDC) accuracy
- [ ] Audio engine timing at 64 samples
- [ ] Max for Live device compatibility

**Reaper**:

| Reaper Version | Win 10 | Win 11 | macOS Intel | macOS ARM |
|----------------|--------|--------|-------------|-----------|
| 7.x | ✓ | ✓ | ✓ | ✓ |
| 6.x | ✓ | ✓ | ✓ | ✓ |

**Reaper Specific Tests**:
- [ ] FX chain navigation
- [ ] Parameter modulation via LFO/Envelope
- [ ] ReaSave script compatibility
- [ ] 64-bit processing accuracy

**Logic Pro**:

| Logic Version | macOS Intel | macOS ARM |
|---------------|-------------|-----------|
| 10.8+ | ✓ | ✓ |
| 10.7 | ✓ | ✓ |

**Logic Specific Tests**:
- [ ] Smart Controls mapping
- [ ] MainStage compatibility
- [ ] Logic's auto-normalize interaction
- [ ] Flex Time interaction

---

## 2. Real-Time Safety Testing

### 2.1 Audio Thread Violation Detection

**Objective**: Detect and prevent any blocking operations on the audio thread

**Detection Method**:
```cpp
// tests/src/RealTimeSafetyMonitor.cpp
class RealTimeSafetyMonitor {
public:
    void enterProcessBlock() {
        m_processStartTime = std::chrono::high_resolution_clock::now();
        m_threadId = std::this_thread::get_id();
    }

    void exitProcessBlock() {
        auto duration = std::chrono::high_resolution_clock::now() - m_processStartTime;
        if (duration > MAX_PROCESS_TIME) {
            logViolation("processBlock exceeded time limit");
        }
    }

    void checkMemoryAllocation() {
        // Track allocations during process block
        m_allocationCount++;
        if (m_allocationCount > MAX_ALLOCATIONS) {
            logViolation("Too many allocations in processBlock");
        }
    }

private:
    std::chrono::time_point<std::chrono::high_resolution_clock> m_processStartTime;
    std::thread::id m_threadId;
    static constexpr auto MAX_PROCESS_TIME = std::chrono::microseconds(100);
    static constexpr int MAX_ALLOCATIONS = 0;  // Zero tolerance
};
```

**Violation Categories**:
1. **Memory Allocation**: No dynamic allocations allowed
2. **Lock Contention**: No mutex locks that may block
3. **System Calls**: No file I/O, network calls, or GUI updates
4. **CPU Spikes**: No operations causing >50% CPU usage spike
5. **Cache Misses**: Minimize non-contiguous memory access

**Testing Protocol**:
```
1. Compile with RT_SAFETY_MONITORING defined
2. Run stress test with 64-sample buffer
3. Monitor for violations over 1-hour continuous run
4. Generate violation report with stack traces
```

### 2.2 Lock Contention Analysis

**Objective**: Ensure lock-free operations in audio path

**Tools**:
- ThreadSanitizer (clang/tsan)
- Visual Studio Concurrency Visualizer
- custom lock-contention detector

**Test Cases**:
```cpp
// tests/src/LockContentionTest.cpp
TEST_CASE("Parameter update from UI thread during audio processing") {
    MorphyAudioProcessor processor;
    processor.prepareToPlay(44100.0, 64);

    // Start audio processing thread
    std::atomic<bool> running{true};
    std::thread audioThread([&]() {
        juce::AudioBuffer<float> buffer(2, 64);
        while (running) {
            processor.processBlock(buffer, juce::MidiBuffer());
        }
    });

    // Simulate UI thread parameter changes
    for (int i = 0; i < 1000; i++) {
        processor.getAPVTS().getParameter("morphX")->setValue(0.5f);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    running = false;
    audioThread.join();

    // Check for lock contention events
    REQUIRE(lockContentionEvents == 0);
}
```

**Acceptance Criteria**:
- Zero lock contentions in audio path
- All parameter updates use atomic operations
- Lock-free queues used for audio-to-UI communication

### 2.3 Memory Allocation Tracking

**Objective**: Verify zero dynamic allocations during audio processing

**Memory Tracker Implementation**:
```cpp
// tests/src/MemoryAllocationTracker.cpp
#ifdef DEBUG_MEMORY_ALLOCATIONS
static thread_local int allocationCount = 0;
static thread_local std::vector<void*> allocations;

void* operator new(std::size_t size) {
    if (isAudioThread()) {
        allocationCount++;
        allocations.push_back(malloc(size));
        return allocations.back();
    }
    return malloc(size);
}

void operator delete(void* ptr) noexcept {
    if (isAudioThread()) {
        auto it = std::find(allocations.begin(), allocations.end(), ptr);
        if (it != allocations.end()) {
            allocations.erase(it);
            allocationCount--;
        }
    }
    free(ptr);
}
#endif
```

**Test Script**:
```python
# tests/scripts/test_memory_allocation.py
def test_no_audio_thread_allocations(duration_seconds=300):
    """Run plugin for 5 minutes and verify no allocations"""
    violations = []

    def monitor_callback(allocation_info):
        violations.append(allocation_info)

    plugin.load()
    plugin.set_monitor_callback(monitor_callback)
    plugin.process_audio(duration_seconds)

    assert len(violations) == 0, f"Found {len(violations)} allocations"
```

### 2.4 CPU Spike Detection

**Objective**: Ensure consistent CPU usage without spikes

**Measurement Points**:
1. Per-block processing time
2. Moving average (100 blocks)
3. Peak detection (3x average)
4. Sustained load detection

**Test Configuration**:
```cpp
// tests/src/CPUSpikeDetector.cpp
struct CPUMetrics {
    double averageTime;
    double peakTime;
    double percentile95;
    double percentile99;
    int spikeCount;
};

CPUMetrics measureCPUSpike(MorphyAudioProcessor& processor, int testDurationMs) {
    CPUMetrics metrics{};
    std::vector<double> processingTimes;

    auto startTime = std::chrono::steady_clock::now();
    juce::AudioBuffer<float> buffer(2, 64);

    while (elapsedTime(startTime) < testDurationMs) {
        auto blockStart = std::chrono::steady_clock::now();
        processor.processBlock(buffer, juce::MidiBuffer());
        auto blockDuration = elapsedMicroseconds(blockStart);

        processingTimes.push_back(blockDuration);

        // Detect spike (3x moving average)
        double avg = calculateMovingAverage(processingTimes, 100);
        if (blockDuration > avg * 3.0) {
            metrics.spikeCount++;
        }
    }

    metrics.averageTime = calculateMean(processingTimes);
    metrics.peakTime = *std::max_element(processingTimes.begin(), processingTimes.end());
    metrics.percentile95 = calculatePercentile(processingTimes, 95);
    metrics.percentile99 = calculatePercentile(processingTimes, 99);

    return metrics;
}
```

**Acceptance Criteria**:
- Average processing time < 200µs (at 64 samples, 44.1kHz)
- 95th percentile < 400µs
- 99th percentile < 600µs
- Spike count < 1 per 10,000 blocks
- Peak time < 1000µs

### 2.5 Latency Measurement

**Objective**: Verify accurate latency reporting

**Test Setup**:
1. Impulse response measurement
2. Phase delay measurement
3. Group delay measurement

**Test Procedure**:
```cpp
// tests/src/LatencyMeasurement.cpp
double measureActualLatency(MorphyAudioProcessor& processor) {
    juce::AudioBuffer<float> buffer(2, 4096);

    // Create impulse
    buffer.clear();
    buffer.setSample(0, 0, 1.0f);
    buffer.setSample(1, 0, 1.0f);

    // Process
    juce::MidiBuffer midi;
    processor.processBlock(buffer, midi);

    // Find first non-zero sample
    for (int channel = 0; channel < 2; channel++) {
        for (int sample = 0; sample < buffer.getNumSamples(); sample++) {
            if (buffer.getSample(channel, sample) > 0.001f) {
                return static_cast<double>(sample);
            }
        }
    }

    return 0.0;
}

TEST_CASE("Latency reporting accuracy") {
    MorphyAudioProcessor processor;
    processor.prepareToPlay(44100.0, 512);

    double reportedLatency = processor.getLatencySamples();
    double actualLatency = measureActualLatency(processor);

    REQUIRE(reportedLatency == actualLatency);
    REQUIRE(reportLatency >= 0);
}
```

---

## 3. Audio Quality Testing

### 3.1 Parameter Smoothing Verification

**Objective**: Ensure smooth parameter transitions without clicks

**Test Cases**:
1. **Step Response**: Instant parameter change (0% to 100%)
2. **Ramp Response**: Linear automation
3. **Curve Response**: Exponential/log curves
4. **Morphing**: Full morph between snapshots

**Test Procedure**:
```cpp
// tests/src/ParameterSmoothingTest.cpp
TEST_CASE("Parameter smoothing prevents clicks") {
    MorphyAudioProcessor processor;
    processor.prepareToPlay(44100.0, 64);

    juce::AudioBuffer<float> buffer(2, 44100);  // 1 second

    // Generate 1kHz tone
    for (int i = 0; i < buffer.getNumSamples(); i++) {
        float sample = std::sin(2.0 * juce::MathConstants<double>::pi * 1000.0 * i / 44100.0);
        buffer.setSample(0, i, sample);
        buffer.setSample(1, i, sample);
    }

    // Apply instant parameter change at sample 10000
    juce::MidiBuffer midi;
    processor.processBlock(buffer, midi);
    processor.setMorphingPosition(0.0f, 0.0f);  // Start position
    processor.setMorphingPosition(1.0f, 1.0f);  // End position (instant)

    // Check for clicks around transition
    for (int channel = 0; channel < 2; channel++) {
        for (int i = 9990; i < 10010; i++) {
            float prev = buffer.getSample(channel, i - 1);
            float curr = buffer.getSample(channel, i);
            float diff = std::abs(curr - prev);

            // Allow gradual change but prevent instant jumps
            REQUIRE(diff < 0.1f);  // Maximum allowed change
        }
    }
}
```

**Acceptance Criteria**:
- Max derivative < 0.1 per sample
- No discontinuities > 0.1 linear amplitude
- Smoothing time configurable (default: 20ms)
- Monotonic approach to target value

### 3.2 Click/Pop Detection

**Objective**: Detect clicks and pops in various scenarios

**Detection Algorithm**:
```cpp
// tests/src/ClickPopDetector.cpp
struct ClickPopMetrics {
    int clickCount;
    int popCount;
    double maxClickAmplitude;
    std::vector<int> clickLocations;
};

ClickPopMetrics detectClicksAndPops(const juce::AudioBuffer<float>& buffer) {
    ClickPopMetrics metrics{};

    for (int channel = 0; channel < buffer.getNumChannels(); channel++) {
        for (int i = 1; i < buffer.getNumSamples(); i++) {
            float prev = buffer.getSample(channel, i - 1);
            float curr = buffer.getSample(channel, i);
            float diff = std::abs(curr - prev);

            // Click threshold: 20% change in one sample
            if (diff > 0.2f) {
                metrics.clickCount++;
                metrics.clickLocations.push_back(i);
                metrics.maxClickAmplitude = std::max(metrics.maxClickAmplitude, (double)diff);
            }

            // Pop detection: high-frequency burst
            if (i > 10) {
                float localEnergy = 0.0f;
                for (int j = i - 10; j < i; j++) {
                    localEnergy += std::abs(buffer.getSample(channel, j));
                }
                if (localEnergy / 10.0f > 0.5f) {
                    metrics.popCount++;
                }
            }
        }
    }

    return metrics;
}
```

**Test Scenarios**:
1. Bypass toggle
2. Morphing pad drag
3. Snapshot recall
4. Preset load
5. Plugin load/unload
6. Sample rate change
7. Buffer size change

### 3.3 Sample Accuracy Tests

**Objective**: Verify sample-accurate processing

**Test Cases**:
```cpp
// tests/src/SampleAccuracyTest.cpp
TEST_CASE("Sample-accurate automation") {
    MorphyAudioProcessor processor;
    processor.prepareToPlay(44100.0, 64);

    juce::AudioBuffer<float> buffer(2, 44100);

    // Create automation points at specific samples
    std::map<int, float> automationPoints;
    automationPoints[1000] = 0.0f;
    automationPoints[5000] = 0.5f;
    automationPoints[10000] = 1.0f;

    // Process with automation
    for (int i = 0; i < buffer.getNumSamples(); i++) {
        if (automationPoints.count(i)) {
            processor.setMorphingPosition(automationPoints[i], 0.0f);
        }
        // Process single sample
        juce::AudioBuffer<float> singleSample(2, 1);
        singleSample.copyFrom(0, 0, buffer, 0, i, 1);
        juce::MidiBuffer midi;
        processor.processBlock(singleSample, midi);
        buffer.copyFrom(0, i, singleSample, 0, 0, 1);
    }

    // Verify exact sample locations
    for (const auto& [sample, value] : automationPoints) {
        // Verify parameter change took effect at exact sample
        juce::Point<float> pos = processor.getMorphingPosition();
        // Check buffer at and after sample
    }
}
```

### 3.4 Automation Precision

**Objective**: Verify precise parameter automation

**Precision Requirements**:
- Parameter resolution: 32-bit float
- Automation interpolation: Linear or curve
- Time quantization: Sample-accurate
- Value quantization: None (full float range)

**Test Matrix**:
| Automation Type | Resolution | Interpolation |
|-----------------|------------|---------------|
| Volume | 0.001 dB | Linear |
| Morph X/Y | 0.0001 | Spline |
| Mix | 0.1% | Linear |

### 3.5 Crossfade Quality

**Objective**: Smooth crossfades between morph states

**Test Cases**:
1. Linear crossfade
2. Equal-power crossfade
3. Custom curve crossfade
4. Snapshot morphing crossfade

**Quality Metrics**:
```cpp
// tests/src/CrossfadeQualityTest.cpp
struct CrossfadeMetrics {
    double monotonicity;      // 0-1, higher is better
    double symmetry;          // 0-1, higher is better
    double energyConservation; // 0-1, higher is better
    double smoothness;        // 0-1, higher is better
};

CrossfadeMetrics measureCrossfadeQuality(const juce::AudioBuffer<float>& buffer) {
    CrossfadeMetrics metrics{};

    // Measure monotonicity (output should change smoothly)
    int directionChanges = 0;
    for (int i = 1; i < buffer.getNumSamples() - 1; i++) {
        float prevDiff = buffer.getSample(0, i) - buffer.getSample(0, i - 1);
        float nextDiff = buffer.getSample(0, i + 1) - buffer.getSample(0, i);
        if (prevDiff * nextDiff < 0) {
            directionChanges++;
        }
    }
    metrics.monotonicity = 1.0 - (double)directionChanges / buffer.getNumSamples();

    // Measure energy conservation
    double totalEnergy = 0.0;
    for (int i = 0; i < buffer.getNumSamples(); i++) {
        totalEnergy += buffer.getSample(0, i) * buffer.getSample(0, i);
    }
    double expectedEnergy = buffer.getNumSamples() * 0.5;  // Expected RMS
    metrics.energyConservation = 1.0 - std::abs(totalEnergy - expectedEnergy) / expectedEnergy;

    return metrics;
}
```

**Acceptance Criteria**:
- Monotonicity > 0.95
- Energy conservation > 0.9
- Smoothness > 0.9
- Symmetry > 0.85

---

## 4. AI Integration Testing

### 4.1 MCP Protocol Compliance

**Objective**: Verify correct implementation of Model Context Protocol

**Test Cases**:
```cpp
// tests/src/MCPProtocolTest.cpp
TEST_CASE("MCP message serialization") {
    nlohmann::json message = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "analyze"},
        {"params", {
            {"audioData", "base64_encoded_data"},
            {"parameters", {
                {"morphX", 0.5},
                {"morphY", 0.5}
            }}
        }}
    };

    std::string serialized = MCPProtocol::serialize(message);
    auto deserialized = MCPProtocol::deserialize(serialized);

    REQUIRE(deserialized == message);
}

TEST_CASE("MCP error handling") {
    // Test invalid JSON
    REQUIRE_THROWS(MCPProtocol::deserialize("invalid json"));

    // Test missing required fields
    nlohmann::json invalidMessage = {
        {"jsonrpc", "2.0"}
        // Missing "id" and "method"
    };
    REQUIRE_THROWS(MCPProtocol::validate(invalidMessage));
}
```

**Required Protocol Features**:
- [ ] JSON-RPC 2.0 compliance
- [ ] WebSocket communication
- [ ] Message batching support
- [ ] Binary data transmission (base64)
- [ ] Timeout handling
- [ ] Reconnection logic
- [ ] Authentication (if required)

### 4.2 Message Serialization Correctness

**Test Suite**:
```cpp
// tests/src/MessageSerializationTest.cpp
TEST_CASE("Parameter state serialization") {
    ParameterState state;
    state.setParameter("morphX", 0.75f);
    state.setParameter("morphY", 0.25f);

    nlohmann::json json = state.toJSON();
    ParameterState restored = ParameterState::fromJSON(json);

    REQUIRE(restored.getParameter("morphX") == 0.75f);
    REQUIRE(restored.getParameter("morphY") == 0.25f);
}

TEST_CASE("Snapshot serialization preserves all data") {
    auto snapshot = std::make_unique<ParameterSnapshot>();
    snapshot->capture(processor);

    nlohmann::json json = snapshot->toJSON();
    auto restored = ParameterSnapshot::fromJSON(json);

    REQUIRE(restored->getParameterCount() == snapshot->getParameterCount());
    for (int i = 0; i < snapshot->getParameterCount(); i++) {
        REQUIRE(restored->getParameterValue(i) == snapshot->getParameterValue(i));
    }
}
```

### 4.3 Timeout Handling

**Test Cases**:
```cpp
// tests/src/TimeoutHandlingTest.cpp
TEST_CASE("Request timeout triggers fallback") {
    MCPClient client;
    client.setTimeoutMs(5000);  // 5 second timeout

    bool callbackCalled = false;
    bool timedOut = false;

    client.sendRequest("analyze", {}, [&](MCPResponse response) {
        callbackCalled = true;
        if (response.isTimeout()) {
            timedOut = true;
        }
    });

    // Wait for timeout
    std::this_thread::sleep_for(std::chrono::milliseconds(6000));

    REQUIRE(callbackCalled);
    REQUIRE(timedOut);
}

TEST_CASE("Timeout doesn't block audio thread") {
    MorphyAudioProcessor processor;
    processor.prepareToPlay(44100.0, 64);
    processor.connectToAIServer("ws://localhost:8080");

    // Request with timeout
    processor.requestAIAnalysis();

    // Process audio (should not block)
    juce::AudioBuffer<float> buffer(2, 64);
    juce::MidiBuffer midi;

    auto startTime = std::chrono::steady_clock::now();
    processor.processBlock(buffer, midi);
    auto duration = elapsedMicroseconds(startTime);

    // Should return immediately, even with timeout
    REQUIRE(duration < 1000);  // Less than 1ms
}
```

### 4.4 Error Recovery

**Test Scenarios**:
1. Server disconnect during processing
2. Invalid server response
3. Network interruption
4. Server timeout
5. Malformed JSON response

**Recovery Test**:
```cpp
// tests/src/ErrorRecoveryTest.cpp
TEST_CASE("Graceful degradation on server failure") {
    MorphyAudioProcessor processor;
    processor.prepareToPlay(44100.0, 64);
    processor.setAIAutomationEnabled(true);

    // Connect to server
    processor.connectToAIServer("ws://localhost:8080");

    // Process with AI enabled
    juce::AudioBuffer<float> buffer(2, 64);
    juce::MidiBuffer midi;
    processor.processBlock(buffer, midi);

    // Simulate server failure
    killServer();

    // Should continue processing without AI
    processor.processBlock(buffer, midi);

    // Audio should still be processed
    REQUIRE(buffer.getMagnitude(0, 0, 64) > 0.0f);

    // AI should be disabled
    REQUIRE(!processor.isAIConnected());
}

TEST_CASE("Automatic reconnection") {
    MCPClient client;
    client.setReconnectIntervalMs(1000);
    client.setMaxReconnectAttempts(5);

    client.connect("ws://localhost:8080");

    // Kill server
    killServer();

    // Wait for reconnect attempts
    std::this_thread::sleep_for(std::chrono::milliseconds(6000));

    // Start server
    startServer();

    // Should reconnect
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    REQUIRE(client.isConnected());
}
```

### 4.5 Performance Under Load

**Objective**: Ensure AI integration doesn't impact audio performance

**Load Test Configuration**:
```cpp
// tests/src/AIPerformanceTest.cpp
struct AIPerformanceMetrics {
    double averageAudioTime;
    double averageAIRequestTime;
    int successfulRequests;
    int failedRequests;
    int timeoutRequests;
};

AIPerformanceMetrics measureAIPerformance(MorphyAudioProcessor& processor,
                                           int durationSeconds) {
    AIPerformanceMetrics metrics{};

    auto startTime = std::chrono::steady_clock::now();
    juce::AudioBuffer<float> buffer(2, 64);

    while (elapsedSeconds(startTime) < durationSeconds) {
        // Measure audio processing
        auto audioStart = std::chrono::steady_clock::now();
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
        auto audioDuration = elapsedMicroseconds(audioStart);

        metrics.averageAudioTime = updateAverage(metrics.averageAudioTime, audioDuration);

        // Send AI requests periodically
        if (rand() % 100 == 0) {
            auto requestStart = std::chrono::steady_clock::now();
            processor.requestAIAnalysis();
            auto requestDuration = elapsedMicroseconds(requestStart);

            metrics.averageAIRequestTime = updateAverage(metrics.averageAIRequestTime, requestDuration);
        }
    }

    return metrics;
}
```

**Acceptance Criteria**:
- Audio processing time unaffected (< 5% variance)
- AI request timeout < 5 seconds
- Failed requests < 1% under normal load
- No audio dropouts during AI communication

---

## 5. Cross-Platform Testing

### 5.1 Windows 10/11 Compatibility

**Test Matrix**:

| Windows Version | Build | Architecture | Tested |
|-----------------|-------|--------------|--------|
| Windows 10 | 1909+ | x64 | ✓ |
| Windows 10 | 21H2+ | x64 | ✓ |
| Windows 11 | 21H2+ | x64 | ✓ |
| Windows 11 | 22H2+ | x64 | ✓ |

**Windows-Specific Tests**:
- [ ] DLL dependencies verified
- [ ] Windows Defender compatibility
- [ ] ASIO driver compatibility
- [ ] Windows audio session handling
- [ ] Plugin appearance in high DPI
- [ ] Registry usage (if any)

**Dependency Check**:
```powershell
# tests/scripts/check_windows_dependencies.ps1
$pluginPath = "build\Morphy_artefacts\Release\VST3\Morphy.vst3"
dumpbin /DEPENDENTS $pluginPath | Select-String "DLL"

# Expected output:
# Only standard Windows DLLs and VST3 SDK
```

### 5.2 macOS Compatibility

**Test Matrix**:

| macOS Version | Architecture | Tested |
|---------------|--------------|--------|
| 10.15 Catalina | x64 | ✓ |
| 11.0 Big Sur | x64 | ✓ |
| 11.0 Big Sur | ARM64 | ✓ |
| 12.0 Monterey | x64 | ✓ |
| 12.0 Monterey | ARM64 | ✓ |
| 13.0 Ventura | x64 | ✓ |
| 13.0 Ventura | ARM64 | ✓ |
| 14.0 Sonoma | ARM64 | ✓ |

**macOS-Specific Tests**:
- [ ] Code signing validity
- [ ] Hardened runtime compliance
- [ ] Notarization (for distribution)
- [ ] Apple Silicon native performance
- [ ] Universal Binary creation
- [ ] AU plugin registration
- [ ] CoreAudio integration

**Build Commands**:
```bash
# Intel build
cmake -DCMAKE_OSX_ARCHITECTURES=x86_64 ..
make

# ARM build
cmake -DCMAKE_OSX_ARCHITECTURES=arm64 ..
make

# Universal binary
cmake -DCMAKE_OSX_ARCHITECTURES=x86_64;arm64 ..
make

# Verify
lipo -info Morphy.vst3/Contents/MacOS/Morphy
```

### 5.3 DAW Version Matrix

**Comprehensive Testing Coverage**:

| DAW | Version Range | Priority | Platforms |
|-----|---------------|----------|-----------|
| FL Studio | 20.8 - 21.x | Critical | Win, Mac |
| Ableton Live | 10.x - 11.x | High | Win, Mac |
| Reaper | 6.x - 7.x | High | Win, Mac |
| Logic Pro | 10.7 - 10.8 | High | Mac |
| Cubase | 12 - 13 | Medium | Win, Mac |
| Studio One | 5 - 6 | Medium | Win, Mac |
| Bitwig | 4 - 5 | Low | Win, Mac |
| GarageBand | Latest | Low | Mac |

**Test Priority Levels**:
- **Critical**: Must work perfectly
- **High**: Full compatibility required
- **Medium**: Basic functionality required
- **Low**: Best effort

### 5.4 Hardware Configuration Testing

**Test Systems**:

**Minimum Spec**:
- CPU: Intel i3 / AMD Ryzen 3 (4 cores)
- RAM: 8GB
- Storage: HDD
- Audio: Integrated audio

**Recommended Spec**:
- CPU: Intel i5 / AMD Ryzen 5 (6 cores)
- RAM: 16GB
- Storage: SSD
- Audio: Dedicated interface

**High-Performance Spec**:
- CPU: Intel i9 / AMD Ryzen 9 (12+ cores)
- RAM: 32GB+
- Storage: NVMe SSD
- Audio: Professional interface

**Audio Interfaces**:
- Focusrite Scarlett series
- Universal Audio Apollo
- RME interfaces
- Audient interfaces
- Native Instruments Komplete Audio

---

## 6. Performance Benchmarking

### 6.1 CPU Usage Profiles

**Benchmark Suite**:
```cpp
// tests/src/CPUBenchmark.cpp
struct CPUBenchmarkResults {
    double idleUsage;
    double lightUsage;
    double moderateUsage;
    double heavyUsage;
    double peakUsage;
};

CPUBenchmarkResults runCPUBenchmark(MorphyAudioProcessor& processor) {
    CPUBenchmarkResults results{};

    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;

    // Idle test (no audio)
    results.idleUsage = measureCPUUsage([&]() {
        buffer.clear();
        processor.processBlock(buffer, midi);
    }, 10000);

    // Light load (1 instance)
    generateTestTone(buffer, 1000.0, 0.1f);
    results.lightUsage = measureCPUUsage([&]() {
        processor.processBlock(buffer, midi);
    }, 10000);

    // Moderate load (with morphing)
    processor.setMorphingPosition(0.5f, 0.5f);
    results.moderateUsage = measureCPUUsage([&]() {
        processor.processBlock(buffer, midi);
    }, 10000);

    // Heavy load (multiple morph changes)
    results.heavyUsage = measureCPUUsage([&]() {
        for (int i = 0; i < buffer.getNumSamples(); i += 10) {
            processor.setMorphingPosition(
                static_cast<float>(rand()) / RAND_MAX,
                static_cast<float>(rand()) / RAND_MAX
            );
        }
        processor.processBlock(buffer, midi);
    }, 10000);

    return results;
}
```

**Acceptance Criteria**:
- Idle: < 0.1% CPU
- Light: < 1% CPU
- Moderate: < 3% CPU
- Heavy: < 8% CPU
- Peak: < 15% CPU

### 6.2 Memory Footprint

**Measurement Points**:
1. Static memory (code, constants)
2. Per-instance memory
3. Peak memory (during processing)
4. Memory growth rate (leak detection)

**Memory Benchmark**:
```cpp
// tests/src/MemoryBenchmark.cpp
struct MemoryMetrics {
    size_t staticMemory;
    size_t perInstanceMemory;
    size_t peakMemory;
    size_t memoryGrowthPerHour;
};

MemoryMetrics measureMemoryFootprint() {
    MemoryMetrics metrics{};

    // Measure static memory
    auto baseline = getCurrentMemoryUsage();
    MorphyAudioProcessor processor1;
    metrics.staticMemory = getCurrentMemoryUsage() - baseline;

    // Measure per-instance memory
    baseline = getCurrentMemoryUsage();
    MorphyAudioProcessor processor2;
    metrics.perInstanceMemory = getCurrentMemoryUsage() - baseline;

    // Measure peak memory
    juce::AudioBuffer<float> buffer(2, 4096);
    juce::MidiBuffer midi;
    for (int i = 0; i < 1000; i++) {
        processor2.processBlock(buffer, midi);
    }
    metrics.peakMemory = getCurrentMemoryUsage() - baseline;

    return metrics;
}
```

**Acceptance Criteria**:
- Static memory: < 5MB
- Per instance: < 10MB
- Peak memory: < 20MB
- Growth: < 1MB/hour

### 6.3 UI Responsiveness

**Test Cases**:
1. Parameter update latency
2. Morphing pad drag responsiveness
3. Menu open/close performance
4. Window resize performance

**Responsiveness Test**:
```cpp
// tests/src/UIResponsivenessTest.cpp
struct UIMetrics {
    double averageLatency;
    double maxLatency;
    double frameRate;
};

UIMetrics measureUIResponsiveness(MorphyAudioProcessor& processor) {
    UIMetrics metrics{};
    std::vector<double> latencies;

    // Measure parameter update latency
    for (int i = 0; i < 100; i++) {
        auto start = std::chrono::steady_clock::now();
        processor.setMorphingPosition(0.5f, 0.5f);
        auto end = std::chrono::steady_clock::now();

        double latency = elapsedMicroseconds(start, end);
        latencies.push_back(latency);
    }

    metrics.averageLatency = calculateMean(latencies);
    metrics.maxLatency = *std::max_element(latencies.begin(), latencies.end());
    metrics.frameRate = 1000.0 / metrics.averageLatency;

    return metrics;
}
```

**Acceptance Criteria**:
- Average latency: < 16ms (60 FPS)
- Max latency: < 33ms (30 FPS)
- Frame rate: > 30 FPS

### 6.4 Network Latency Impact

**Objective**: Measure impact of AI server latency on audio

**Test Configuration**:
```cpp
// tests/src/NetworkLatencyTest.cpp
struct NetworkLatencyImpact {
    double audioProcessingTime;
    double uiUpdateTime;
    int droppedFrames;
    double perceivedLatency;
};

NetworkLatencyImpact measureNetworkImpact(MorphyAudioProcessor& processor,
                                          int simulatedLatencyMs) {
    NetworkLatencyImpact impact{};

    // Simulate network latency
    processor.setSimulatedLatency(simulatedLatencyMs);

    juce::AudioBuffer<float> buffer(2, 64);
    juce::MidiBuffer midi;

    auto startTime = std::chrono::steady_clock::now();
    processor.requestAIAnalysis();
    processor.processBlock(buffer, midi);
    auto duration = elapsedMicroseconds(startTime);

    impact.audioProcessingTime = duration;

    // Measure UI update
    auto uiStart = std::chrono::steady_clock::now();
    // Trigger UI update
    auto uiEnd = std::chrono::steady_clock::now();
    impact.uiUpdateTime = elapsedMicroseconds(uiStart, uiEnd);

    return impact;
}
```

**Latency Scenarios**:
- Local server: < 1ms
- LAN: 1-10ms
- Internet (fast): 50-100ms
- Internet (slow): 100-500ms

**Acceptance Criteria**:
- Local/LAN: No perceptible impact
- Fast internet: Minimal UI lag, audio unaffected
- Slow internet: UI may lag, audio must remain real-time

---

## 7. Test Automation & CI/CD

### 7.1 Automated Test Suite

**CMake Configuration**:
```cmake
# tests/CMakeLists.txt
add_subdirectory(unit_tests)
add_subdirectory(integration_tests)
add_subdirectory(performance_tests)

# Custom target for all tests
add_custom_target(run_all_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS unit_tests integration_tests performance_tests
)
```

### 7.2 Continuous Integration

**GitHub Actions Workflow**:
```yaml
# .github/workflows/test.yml
name: Plugin Tests

on: [push, pull_request]

jobs:
  windows-tests:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v3
      - name: Build and Test
        run: |
          cmake -B build -DCMAKE_BUILD_TYPE=Release
          cmake --build build --config Release
          ctest --test-dir build --output-on-failure

  macos-tests:
    runs-on: macos-latest
    steps:
      - uses: actions/checkout@v3
      - name: Build and Test
        run: |
          cmake -B build -DCMAKE_BUILD_TYPE=Release
          cmake --build build --config Release
          ctest --test-dir build --output-on-failure
```

### 7.3 Test Reports

**Report Generation**:
```python
# tests/scripts/generate_report.py
import json
import xml.etree.ElementTree as ET

def generate_test_report(test_results):
    report = {
        "summary": {
            "total": len(test_results),
            "passed": sum(1 for r in test_results if r["status"] == "passed"),
            "failed": sum(1 for r in test_results if r["status"] == "failed"),
            "skipped": sum(1 for r in test_results if r["status"] == "skipped")
        },
        "tests": test_results
    }

    with open("test_report.json", "w") as f:
        json.dump(report, f, indent=2)

    # Generate JUnit XML
    root = ET.Element("testsuite")
    for test in test_results:
        testcase = ET.SubElement(root, "testcase")
        testcase.set("name", test["name"])
        if test["status"] == "failed":
            failure = ET.SubElement(testcase, "failure")
            failure.text = test["error"]

    tree = ET.ElementTree(root)
    tree.write("test_report.xml")
```

---

## 8. Acceptance Criteria Summary

### 8.1 Must Have (Blocking Issues)

- [ ] VST3 validator: 100% pass rate
- [ ] AU validator: 100% pass rate
- [ ] FL Studio: Full compatibility, no crashes
- [ ] Real-time safety: Zero violations in 1-hour test
- [ ] Audio quality: No clicks/pops in any scenario
- [ ] CPU usage: < 8% under heavy load
- [ ] Memory: No leaks, < 20MB peak

### 8.2 Should Have (High Priority)

- [ ] DAW coverage: All major DAWs tested
- [ ] Cross-platform: Windows 10/11, macOS 10.15+
- [ ] AI integration: Graceful degradation on failure
- [ ] Documentation: Complete test documentation
- [ ] Automation: CI/CD pipeline functional

### 8.3 Nice to Have (Medium Priority)

- [ ] Performance profiling data
- [ ] Custom test fixtures
- [ ] Automated DAW testing
- [ ] Performance regression detection

---

## 9. Testing Timeline

**Phase 1: Foundation (Week 1-2)**
- Set up test framework
- Create basic unit tests
- Implement real-time safety monitoring

**Phase 2: Core Testing (Week 3-4)**
- Audio quality tests
- Parameter smoothing tests
- SDK validation

**Phase 3: Integration (Week 5-6)**
- DAW compatibility testing
- AI integration tests
- Cross-platform builds

**Phase 4: Performance (Week 7-8)**
- CPU/memory profiling
- Load testing
- Network latency testing

**Phase 5: Release (Week 9-10)**
- Final validation
- Documentation
- Release candidate testing

---

## 10. Test Execution Checklist

### Pre-Release Checklist

```
Code Quality:
□ All compiler warnings resolved
□ Static analysis passed
□ Code review completed

SDK Validation:
□ VST3 validator passed (100%)
□ AU validator passed (100%)
□ No memory leaks detected

Real-Time Safety:
□ 1-hour stress test passed
□ Zero lock contentions
□ Zero audio thread allocations
□ CPU usage within limits

Audio Quality:
□ Click/pop detection passed
□ Parameter smoothing verified
□ Sample accuracy confirmed

DAW Testing:
□ FL Studio (all versions)
□ Ableton Live (current version)
□ Reaper (current version)
□ Logic Pro (current version)

Platform Testing:
□ Windows 10 (x64)
□ Windows 11 (x64)
□ macOS 10.15+ (x64)
□ macOS 11+ (ARM64)

AI Integration:
□ MCP protocol compliance
□ Timeout handling
□ Error recovery
□ Performance under load

Documentation:
□ Test results documented
□ Known issues listed
□ Release notes prepared
```

---

This testing strategy provides a comprehensive approach to ensuring the Morphy plugin meets the highest standards for quality, performance, and compatibility across all supported platforms and DAWs.
