/*
 * Morphy - Advanced Parameter Morphing Engine
 * Unit Tests for Parameter Manager
 *
 * Tests parameter management functionality including:
 * - Parameter creation and initialization
 * - Parameter value updates
 * - Parameter smoothing
 * - Parameter state persistence
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include "../../src/core/ParameterManager.h"
#include "../../src/morphing/ParameterState.h"

using namespace morphy;

// ============================================================================
// Test Fixture
// ============================================================================

class ParameterManagerFixture {
protected:
    std::unique_ptr<ParameterManager> manager;

    void SetUp() {
        manager = std::make_unique<ParameterManager>();
    }

    void TearDown() {
        manager.reset();
    }
};

// ============================================================================
// Parameter Creation Tests
// ============================================================================

TEST_CASE_METHOD(ParameterManagerFixture, "ParameterManager: Create parameter with valid ID") {
    SetUp();

    bool result = manager->addParameter("test_param", 0.0f, 1.0f, 0.5f);

    REQUIRE(result == true);
    REQUIRE(manager->getParameterCount() == 1);
}

TEST_CASE_METHOD(ParameterManagerFixture, "ParameterManager: Reject duplicate parameter IDs") {
    SetUp();

    manager->addParameter("duplicate", 0.0f, 1.0f, 0.5f);
    bool result = manager->addParameter("duplicate", 0.0f, 1.0f, 0.5f);

    REQUIRE(result == false);
    REQUIRE(manager->getParameterCount() == 1);
}

TEST_CASE_METHOD(ParameterManagerFixture, "ParameterManager: Create parameter with custom range") {
    SetUp();

    manager->addParameter("custom_range", -10.0f, 10.0f, 0.0f);
    auto param = manager->getParameter("custom_range");

    REQUIRE(param != nullptr);
    REQUIRE(param->getMinValue() == -10.0f);
    REQUIRE(param->getMaxValue() == 10.0f);
    REQUIRE(param->getDefaultValue() == 0.0f);
}

// ============================================================================
// Parameter Value Tests
// ============================================================================

TEST_CASE_METHOD(ParameterManagerFixture, "ParameterManager: Set parameter value within range") {
    SetUp();

    manager->addParameter("test", 0.0f, 1.0f, 0.5f);
    bool result = manager->setParameterValue("test", 0.75f);

    REQUIRE(result == true);
    REQUIRE(manager->getParameterValue("test") == 0.75f);
}

TEST_CASE_METHOD(ParameterManagerFixture, "ParameterManager: Clamp parameter value to max") {
    SetUp();

    manager->addParameter("test", 0.0f, 1.0f, 0.5f);
    manager->setParameterValue("test", 1.5f);  // Above range

    REQUIRE(manager->getParameterValue("test") == 1.0f);  // Clamped
}

TEST_CASE_METHOD(ParameterManagerFixture, "ParameterManager: Clamp parameter value to min") {
    SetUp();

    manager->addParameter("test", 0.0f, 1.0f, 0.5f);
    manager->setParameterValue("test", -0.5f);  // Below range

    REQUIRE(manager->getParameterValue("test") == 0.0f);  // Clamped
}

TEST_CASE_METHOD(ParameterManagerFixture, "ParameterManager: Return default for non-existent parameter") {
    SetUp();

    float value = manager->getParameterValue("nonexistent");

    REQUIRE(value == 0.0f);  // Default return
}

// ============================================================================
// Parameter Smoothing Tests
// ============================================================================

TEST_CASE_METHOD(ParameterManagerFixture, "ParameterManager: Apply smoothing to parameter change") {
    SetUp();

    manager->addParameter("smoothed", 0.0f, 1.0f, 0.0f);
    manager->setSmoothingTime("smoothed", 20.0f);  // 20ms smoothing
    manager->setParameterValue("smoothed", 1.0f);

    // Process samples at 44.1kHz
    const int sampleRate = 44100;
    const int samplesToProcess = sampleRate / 100;  // 10ms

    for (int i = 0; i < samplesToProcess; i++) {
        manager->updateSmoothedValues(1.0 / sampleRate);
    }

    float currentValue = manager->getParameterValue("smoothed");
    REQUIRE(currentValue < 1.0f);  // Should not have reached target yet
    REQUIRE(currentValue > 0.0f);  // Should have progressed
}

TEST_CASE_METHOD(ParameterManagerFixture, "ParameterManager: Complete smoothing within specified time") {
    SetUp();

    manager->addParameter("smoothed", 0.0f, 1.0f, 0.0f);
    manager->setSmoothingTime("smoothed", 10.0f);  // 10ms smoothing
    manager->setParameterValue("smoothed", 1.0f);

    // Process samples for 20ms (double the smoothing time)
    const int sampleRate = 44100;
    const int samplesToProcess = sampleRate / 50;  // 20ms

    for (int i = 0; i < samplesToProcess; i++) {
        manager->updateSmoothedValues(1.0 / sampleRate);
    }

    float currentValue = manager->getParameterValue("smoothed");
    REQUIRE(currentValue == Approx(1.0f).epsilon(0.01f));  // Should be at target
}

// ============================================================================
// Parameter State Tests
// ============================================================================

TEST_CASE("ParameterState: Save and restore parameter state") {
    ParameterManager manager;
    manager.addParameter("param1", 0.0f, 1.0f, 0.5f);
    manager.addParameter("param2", -10.0f, 10.0f, 0.0f);

    // Set values
    manager.setParameterValue("param1", 0.75f);
    manager.setParameterValue("param2", 5.0f);

    // Save state
    auto state = manager.saveState();

    // Modify values
    manager.setParameterValue("param1", 0.25f);
    manager.setParameterValue("param2", -5.0f);

    // Restore state
    manager.restoreState(state);

    // Verify restoration
    REQUIRE(manager.getParameterValue("param1") == 0.75f);
    REQUIRE(manager.getParameterValue("param2") == 5.0f);
}

TEST_CASE("ParameterState: Serialize state to JSON") {
    ParameterManager manager;
    manager.addParameter("param1", 0.0f, 1.0f, 0.5f);
    manager.setParameterValue("param1", 0.75f);

    auto json = manager.serializeToJson();

    REQUIRE(json.contains("param1"));
    REQUIRE(json["param1"] == 0.75f);
}

TEST_CASE("ParameterState: Deserialize state from JSON") {
    nlohmann::json json = {
        {"param1", 0.75f},
        {"param2", 5.0f}
    };

    ParameterManager manager;
    manager.addParameter("param1", 0.0f, 1.0f, 0.5f);
    manager.addParameter("param2", -10.0f, 10.0f, 0.0f);

    manager.deserializeFromJson(json);

    REQUIRE(manager.getParameterValue("param1") == 0.75f);
    REQUIRE(manager.getParameterValue("param2") == 5.0f);
}

// ============================================================================
// Thread Safety Tests
// ============================================================================

TEST_CASE_METHOD(ParameterManagerFixture, "ParameterManager: Thread-safe parameter updates") {
    SetUp();

    manager->addParameter("thread_safe", 0.0f, 1.0f, 0.5f);

    const int numIterations = 1000;
    std::atomic<int> successCount{0};

    // Thread 1: Update parameter
    std::thread thread1([&]() {
        for (int i = 0; i < numIterations; i++) {
            if (manager->setParameterValue("thread_safe", 0.75f)) {
                successCount++;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    // Thread 2: Read parameter
    std::thread thread2([&]() {
        for (int i = 0; i < numIterations; i++) {
            float value = manager->getParameterValue("thread_safe");
            REQUIRE(value >= 0.0f);
            REQUIRE(value <= 1.0f);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    thread1.join();
    thread2.join();

    REQUIRE(successCount == numIterations);
}

// ============================================================================
// Performance Tests
// ============================================================================

TEST_CASE_METHOD(ParameterManagerFixture, "ParameterManager: Parameter lookup performance") {
    SetUp();

    // Create 1000 parameters
    for (int i = 0; i < 1000; i++) {
        std::string id = "param_" + std::to_string(i);
        manager->addParameter(id, 0.0f, 1.0f, 0.5f);
    }

    // Measure lookup performance
    auto start = std::chrono::high_resolution_clock::now();

    const int iterations = 100000;
    for (int i = 0; i < iterations; i++) {
        manager->getParameterValue("param_500");
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avgTime = static_cast<double>(duration.count()) / iterations;

    // Should be very fast (< 1 microsecond per lookup)
    REQUIRE(avgTime < 1.0);
}

TEST_CASE_METHOD(ParameterManagerFixture, "ParameterManager: Batch parameter update performance") {
    SetUp();

    // Create 100 parameters
    for (int i = 0; i < 100; i++) {
        std::string id = "param_" + std::to_string(i);
        manager->addParameter(id, 0.0f, 1.0f, 0.5f);
    }

    // Measure batch update performance
    auto start = std::chrono::high_resolution_clock::now();

    const int iterations = 10000;
    for (int i = 0; i < iterations; i++) {
        for (int j = 0; j < 100; j++) {
            std::string id = "param_" + std::to_string(j);
            manager->setParameterValue(id, 0.75f);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // Should complete in reasonable time
    REQUIRE(duration.count() < 1000);  // Less than 1 second
}
