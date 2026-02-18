/*
 * Morphy - Advanced Parameter Morphing Engine
 * Real-Time Safety Tests - Audio Thread Violations
 *
 * Tests for detecting and preventing audio thread violations:
 * - Memory allocation detection
 * - Lock contention detection
 * - System call detection
 * - Blocking operation detection
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <atomic>
#include <thread>
#include <chrono>
#include "../../src/core/PluginProcessor.h"
#include "../../src/util/LockFreeQueue.h"

using namespace morphy;

// ============================================================================
// Test Configuration
// ============================================================================

namespace {
    constexpr int SAMPLE_RATE = 44100;
    constexpr int BUFFER_SIZE = 64;
    constexpr int STRESS_TEST_DURATION_MS = 5000;

    std::atomic<bool> g_isAudioThread{false};
    thread_local bool t_isAudioThread = false;
}

// ============================================================================
// Memory Allocation Detection
// ============================================================================

#ifdef MORPHY_RT_SAFETY_MONITORING
TEST_CASE("RT Safety: No memory allocations in processBlock") {
    MorphyAudioProcessor processor;
    processor.prepareToPlay(SAMPLE_RATE, BUFFER_SIZE);

    juce::AudioBuffer<float> buffer(2, BUFFER_SIZE);
    juce::MidiBuffer midi;

    // Track allocations
    g_isAudioThread = true;
    t_isAudioThread = true;

    size_t allocationCountBefore = getAllocationCount();
    processor.processBlock(buffer, midi);
    size_t allocationCountAfter = getAllocationCount();

    g_isAudioThread = false;
    t_isAudioThread = false;

    REQUIRE(allocationCountBefore == allocationCountAfter);
}
#endif

TEST_CASE("RT Safety: No allocations during stress test") {
    MorphyAudioProcessor processor;
    processor.prepareToPlay(SAMPLE_RATE, BUFFER_SIZE);

    juce::AudioBuffer<float> buffer(2, BUFFER_SIZE);
    juce::MidiBuffer midi;

    // Generate test audio
    for (int channel = 0; channel < buffer.getNumChannels(); channel++) {
        for (int sample = 0; sample < buffer.getNumSamples(); sample++) {
            buffer.setSample(channel, sample, 0.5f);
        }
    }

    size_t totalAllocations = 0;
    auto startTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::milliseconds(STRESS_TEST_DURATION_MS);

    while (std::chrono::steady_clock::now() - startTime < duration) {
        size_t before = getAllocationCount();
        processor.processBlock(buffer, midi);
        size_t after = getAllocationCount();

        totalAllocations += (after - before);
    }

    REQUIRE(totalAllocations == 0);
}

// ============================================================================
// Lock Contention Detection
// ============================================================================

TEST_CASE("RT Safety: No lock contention during parameter updates") {
    MorphyAudioProcessor processor;
    processor.prepareToPlay(SAMPLE_RATE, BUFFER_SIZE);

    juce::AudioBuffer<float> buffer(2, BUFFER_SIZE);
    juce::MidiBuffer midi;

    std::atomic<int> lockContentions{0};
    std::atomic<bool> running{true};

    // Start audio processing thread
    std::thread audioThread([&]() {
        while (running) {
            processor.processBlock(buffer, midi);
        }
    });

    // Simulate UI thread parameter changes
    for (int i = 0; i < 1000; i++) {
        auto start = std::chrono::steady_clock::now();

        processor.setMorphingPosition(0.5f, 0.5f);

        auto end = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

        // If parameter update took too long, there might be lock contention
        if (elapsed.count() > 100) {  // 100 microseconds threshold
            lockContentions++;
        }

        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    running = false;
    audioThread.join();

    // Allow minimal contentions due to scheduling
    REQUIRE(lockContentions < 10);
}

TEST_CASE("RT Safety: Lock-free queue communication") {
    LockFreeQueue<int, 1024> queue;

    std::atomic<bool> running{true};
    std::atomic<int> pushCount{0};
    std::atomic<int> popCount{0};

    // Producer thread
    std::thread producer([&]() {
        int value = 0;
        while (running && value < 10000) {
            if (queue.push(value)) {
                pushCount++;
                value++;
            }
        }
    });

    // Consumer thread (simulating audio thread)
    std::thread consumer([&]() {
        while (running || !queue.isEmpty()) {
            int value;
            if (queue.pop(value)) {
                popCount++;
            }
        }
    });

    producer.join();
    running = false;
    consumer.join();

    REQUIRE(pushCount == 10000);
    REQUIRE(popCount == 10000);
}

// ============================================================================
// CPU Spike Detection
// ============================================================================

TEST_CASE("RT Safety: CPU usage remains consistent") {
    MorphyAudioProcessor processor;
    processor.prepareToPlay(SAMPLE_RATE, BUFFER_SIZE);

    juce::AudioBuffer<float> buffer(2, BUFFER_SIZE);
    juce::MidiBuffer midi;

    const int iterations = 1000;
    std::vector<double> processingTimes;

    processingTimes.reserve(iterations);

    for (int i = 0; i < iterations; i++) {
        auto start = std::chrono::high_resolution_clock::now();
        processor.processBlock(buffer, midi);
        auto end = std::chrono::high_resolution_clock::now();

        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        processingTimes.push_back(static_cast<double>(duration.count()));
    }

    // Calculate statistics
    double mean = std::accumulate(processingTimes.begin(), processingTimes.end(), 0.0) / iterations;
    double max = *std::max_element(processingTimes.begin(), processingTimes.end());

    // Calculate moving average for spike detection
    const int windowSize = 100;
    std::vector<double> movingAverage;
    for (size_t i = windowSize; i < processingTimes.size(); i++) {
        double sum = 0.0;
        for (size_t j = i - windowSize; j < i; j++) {
            sum += processingTimes[j];
        }
        movingAverage.push_back(sum / windowSize);
    }

    // Detect spikes (3x moving average)
    int spikeCount = 0;
    for (size_t i = 0; i < movingAverage.size(); i++) {
        if (processingTimes[i + windowSize] > movingAverage[i] * 3.0) {
            spikeCount++;
        }
    }

    // Requirements
    REQUIRE(mean < 200.0);  // Average < 200 microseconds
    REQUIRE(max < 1000.0);  // Peak < 1 millisecond
    REQUIRE(spikeCount < iterations / 1000);  // < 0.1% spikes
}

TEST_CASE("RT Safety: CPU spike during parameter change") {
    MorphyAudioProcessor processor;
    processor.prepareToPlay(SAMPLE_RATE, BUFFER_SIZE);

    juce::AudioBuffer<float> buffer(2, BUFFER_SIZE);
    juce::MidiBuffer midi;

    // Measure baseline
    std::vector<double> baselineTimes;
    for (int i = 0; i < 100; i++) {
        auto start = std::chrono::high_resolution_clock::now();
        processor.processBlock(buffer, midi);
        auto end = std::chrono::high_resolution_clock::now();

        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        baselineTimes.push_back(static_cast<double>(duration.count()));
    }

    double baselineMean = std::accumulate(baselineTimes.begin(), baselineTimes.end(), 0.0) / baselineTimes.size();

    // Measure during parameter changes
    std::vector<double> parameterChangeTimes;
    for (int i = 0; i < 100; i++) {
        processor.setMorphingPosition(0.5f, 0.5f);

        auto start = std::chrono::high_resolution_clock::now();
        processor.processBlock(buffer, midi);
        auto end = std::chrono::high_resolution_clock::now();

        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        parameterChangeTimes.push_back(static_cast<double>(duration.count()));
    }

    double paramChangeMean = std::accumulate(parameterChangeTimes.begin(), parameterChangeTimes.end(), 0.0) / parameterChangeTimes.size();

    // Parameter changes should not significantly increase processing time
    REQUIRE(paramChangeMean < baselineMean * 2.0);
}

// ============================================================================
// Latency Tests
// ============================================================================

TEST_CASE("RT Safety: Consistent processing time") {
    MorphyAudioProcessor processor;
    processor.prepareToPlay(SAMPLE_RATE, BUFFER_SIZE);

    juce::AudioBuffer<float> buffer(2, BUFFER_SIZE);
    juce::MidiBuffer midi;

    const int iterations = 1000;
    std::vector<double> processingTimes;

    for (int i = 0; i < iterations; i++) {
        auto start = std::chrono::high_resolution_clock::now();
        processor.processBlock(buffer, midi);
        auto end = std::chrono::high_resolution_clock::now();

        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        processingTimes.push_back(static_cast<double>(duration.count()));
    }

    // Calculate standard deviation
    double mean = std::accumulate(processingTimes.begin(), processingTimes.end(), 0.0) / iterations;
    double variance = 0.0;
    for (double time : processingTimes) {
        variance += (time - mean) * (time - mean);
    }
    variance /= iterations;
    double stdDev = std::sqrt(variance);

    // Coefficient of variation should be low (< 50%)
    double cv = stdDev / mean;
    REQUIRE(cv < 0.5);
}

TEST_CASE("RT Safety: ProcessBlock completes within buffer time") {
    MorphyAudioProcessor processor;
    processor.prepareToPlay(SAMPLE_RATE, BUFFER_SIZE);

    juce::AudioBuffer<float> buffer(2, BUFFER_SIZE);
    juce::MidiBuffer midi;

    const int iterations = 1000;
    const double bufferTimeUs = (BUFFER_SIZE * 1000000.0) / SAMPLE_RATE;  // Buffer time in microseconds

    for (int i = 0; i < iterations; i++) {
        auto start = std::chrono::high_resolution_clock::now();
        processor.processBlock(buffer, midi);
        auto end = std::chrono::high_resolution_clock::now();

        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

        // Should complete well within buffer time
        REQUIRE(duration.count() < bufferTimeUs * 0.5);  // 50% of buffer time
    }
}

// ============================================================================
// Priority Inversion Tests
// ============================================================================

TEST_CASE("RT Safety: No priority inversion with parameter updates") {
    MorphyAudioProcessor processor;
    processor.prepareToPlay(SAMPLE_RATE, BUFFER_SIZE);

    juce::AudioBuffer<float> buffer(2, BUFFER_SIZE);
    juce::MidiBuffer midi;

    std::atomic<int> audioThreadPreemptions{0};
    std::atomic<bool> audioThreadRunning{false};
    std::atomic<bool> parameterThreadRunning{false};

    // Audio thread (high priority simulation)
    std::thread audioThread([&]() {
        audioThreadRunning = true;
        const int iterations = 1000;

        for (int i = 0; i < iterations; i++) {
            auto start = std::chrono::high_resolution_clock::now();

            processor.processBlock(buffer, midi);

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

            // If processing took unusually long, might be priority inversion
            if (duration.count() > 1000) {  // 1ms threshold
                if (parameterThreadRunning) {
                    audioThreadPreemptions++;
                }
            }
        }

        audioThreadRunning = false;
    });

    // Parameter update thread (lower priority simulation)
    std::thread paramThread([&]() {
        parameterThreadRunning = true;
        for (int i = 0; i < 100; i++) {
            processor.setMorphingPosition(0.5f, 0.5f);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        parameterThreadRunning = false;
    });

    audioThread.join();
    paramThread.join();

    // Should have minimal priority inversion
    REQUIRE(audioThreadPreemptions < 5);
}

// ============================================================================
// Stack Overflow Prevention
// ============================================================================

TEST_CASE("RT Safety: No deep recursion in processBlock") {
    MorphyAudioProcessor processor;
    processor.prepareToPlay(SAMPLE_RATE, BUFFER_SIZE);

    juce::AudioBuffer<float> buffer(2, BUFFER_SIZE);
    juce::MidiBuffer midi;

    // Monitor stack depth (platform-specific)
    size_t stackDepthBefore = getCurrentStackDepth();
    processor.processBlock(buffer, midi);
    size_t stackDepthAfter = getCurrentStackDepth();

    // Stack depth should not increase significantly
    REQUIRE(stackDepthAfter - stackDepthBefore < 100);
}

// ============================================================================
// Exception Safety
// ============================================================================

TEST_CASE("RT Safety: No exceptions thrown from processBlock") {
    MorphyAudioProcessor processor;
    processor.prepareToPlay(SAMPLE_RATE, BUFFER_SIZE);

    juce::AudioBuffer<float> buffer(2, BUFFER_SIZE);
    juce::MidiBuffer midi;

    bool exceptionThrown = false;

    try {
        for (int i = 0; i < 1000; i++) {
            processor.processBlock(buffer, midi);
        }
    } catch (...) {
        exceptionThrown = true;
    }

    REQUIRE(exceptionThrown == false);
}

TEST_CASE("RT Safety: Handle invalid buffer gracefully") {
    MorphyAudioProcessor processor;
    processor.prepareToPlay(SAMPLE_RATE, BUFFER_SIZE);

    // Test with empty buffer
    juce::AudioBuffer<float> emptyBuffer;
    juce::MidiBuffer midi;

    REQUIRE_NOTHROW(processor.processBlock(emptyBuffer, midi));

    // Test with mismatched channels
    juce::AudioBuffer<float> mismatchedBuffer(8, BUFFER_SIZE);

    REQUIRE_NOTHROW(processor.processBlock(mismatchedBuffer, midi));
}
