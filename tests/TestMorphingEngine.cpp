/*
 * Morphy - Advanced Parameter Morphing Engine
 * Test Suite
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "../src/morphing/MorphingEngine.h"
#include "../src/morphing/ParameterState.h"
#include "../src/morphing/InterpolationCurves.h"

using Catch::Approx;
using namespace morphy;

//==============================================================================
// Interpolation Tests
//==============================================================================

TEST_CASE("Linear interpolation", "[interpolation]")
{
    REQUIRE(InterpolationCurves::linear(0.0f, 1.0f, 0.0f) == Approx(0.0f));
    REQUIRE(InterpolationCurves::linear(0.0f, 1.0f, 0.5f) == Approx(0.5f));
    REQUIRE(InterpolationCurves::linear(0.0f, 1.0f, 1.0f) == Approx(1.0f));
    
    REQUIRE(InterpolationCurves::linear(10.0f, 20.0f, 0.0f) == Approx(10.0f));
    REQUIRE(InterpolationCurves::linear(10.0f, 20.0f, 0.5f) == Approx(15.0f));
    REQUIRE(InterpolationCurves::linear(10.0f, 20.0f, 1.0f) == Approx(20.0f));
}

TEST_CASE("Cosine interpolation", "[interpolation]")
{
    float result = InterpolationCurves::cosine(0.0f, 1.0f, 0.5f);
    REQUIRE(result > 0.4f);
    REQUIRE(result < 0.6f);
    
    // Cosine should be smoother than linear at endpoints
    float linearStart = InterpolationCurves::linear(0.0f, 1.0f, 0.1f);
    float cosineStart = InterpolationCurves::cosine(0.0f, 1.0f, 0.1f);
    REQUIRE(cosineStart < linearStart); // Cosine eases in
}

TEST_CASE("Smoothstep interpolation", "[interpolation]")
{
    REQUIRE(InterpolationCurves::smoothstep(0.0f, 1.0f, 0.0f) == Approx(0.0f));
    REQUIRE(InterpolationCurves::smoothstep(0.0f, 1.0f, 1.0f) == Approx(1.0f));
    
    // Smoothstep should be symmetric around 0.5
    float below = InterpolationCurves::smoothstep(0.0f, 1.0f, 0.3f);
    float above = InterpolationCurves::smoothstep(0.0f, 1.0f, 0.7f);
    REQUIRE(below < 0.5f);
    REQUIRE(above > 0.5f);
}

TEST_CASE("Bilinear interpolation", "[interpolation]")
{
    SECTION("Center position")
    {
        float result = InterpolationCurves::bilinear(0.0f, 1.0f, 0.0f, 1.0f, 0.5f, 0.5f);
        REQUIRE(result == Approx(0.5f));
    }
    
    SECTION("Corner positions")
    {
        REQUIRE(InterpolationCurves::bilinear(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f) == Approx(0.0f));
        REQUIRE(InterpolationCurves::bilinear(0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f) == Approx(1.0f));
        REQUIRE(InterpolationCurves::bilinear(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f) == Approx(0.0f));
        REQUIRE(InterpolationCurves::bilinear(0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f) == Approx(1.0f));
    }
    
    SECTION("Asymmetric values")
    {
        float result = InterpolationCurves::bilinear(0.0f, 1.0f, 2.0f, 3.0f, 0.5f, 0.5f);
        REQUIRE(result == Approx(1.5f));
    }
}

//==============================================================================
// Parameter State Tests
//==============================================================================

TEST_CASE("ParameterState normalized value", "[parameter]")
{
    ParameterState state;
    state.minValue = 0.0f;
    state.maxValue = 100.0f;
    state.currentValue = 50.0f;
    
    REQUIRE(state.getNormalized() == Approx(0.5f));
    
    state.setNormalized(0.75f);
    REQUIRE(state.currentValue == Approx(75.0f));
}

TEST_CASE("ParameterState serialization", "[parameter]")
{
    ParameterState original;
    original.parameterID = "test_param";
    original.parameterName = "Test Parameter";
    original.currentValue = 0.5f;
    original.targetValue = 0.75f;
    original.minValue = 0.0f;
    original.maxValue = 1.0f;
    
    auto json = original.toJSON();
    ParameterState restored = ParameterState::fromJSON(json);
    
    REQUIRE(restored.parameterID == original.parameterID);
    REQUIRE(restored.parameterName == original.parameterName);
    REQUIRE(restored.currentValue == Approx(original.currentValue));
    REQUIRE(restored.targetValue == Approx(original.targetValue));
}

TEST_CASE("StateSnapshot serialization", "[parameter]")
{
    StateSnapshot original;
    original.snapshotID = "test-id";
    original.snapshotName = "Test Snapshot";
    original.timestamp = 1234567890;
    original.color = juce::Colour(0xffff0000);
    
    ParameterState param;
    param.parameterID = "param1";
    param.currentValue = 0.5f;
    original.setParameter(param);
    
    auto json = original.toJSON();
    StateSnapshot restored = StateSnapshot::fromJSON(json);
    
    REQUIRE(restored.snapshotID == original.snapshotID);
    REQUIRE(restored.snapshotName == original.snapshotName);
    REQUIRE(restored.timestamp == original.timestamp);
    REQUIRE(restored.color == original.color);
    REQUIRE(restored.parameters.size() == 1);
}

//==============================================================================
// Morphing Engine Tests
//==============================================================================

TEST_CASE("MorphingEngine initialization", "[morphing]")
{
    MorphingEngine engine;
    engine.initialize();
    
    auto pos = engine.getMorphPosition();
    REQUIRE(pos.x == Approx(0.5f));
    REQUIRE(pos.y == Approx(0.5f));
}

TEST_CASE("MorphingEngine position setting", "[morphing]")
{
    MorphingEngine engine;
    engine.initialize();
    
    engine.setMorphPosition(0.25f, 0.75f);
    
    auto pos = engine.getMorphPosition();
    REQUIRE(pos.x == Approx(0.25f));
    REQUIRE(pos.y == Approx(0.75f));
}

TEST_CASE("MorphingEngine position clamping", "[morphing]")
{
    MorphingEngine engine;
    engine.initialize();
    
    engine.setMorphPosition(-0.5f, 1.5f);
    
    auto pos = engine.getMorphPosition();
    REQUIRE(pos.x == Approx(0.0f));
    REQUIRE(pos.y == Approx(1.0f));
}

TEST_CASE("MorphingEngine snapshot management", "[morphing]")
{
    MorphingEngine engine;
    engine.initialize();
    
    StateSnapshot snapshot;
    snapshot.snapshotID = "test-snapshot";
    snapshot.snapshotName = "Test";
    
    engine.setSnapshot(0, snapshot);
    
    const auto& slot = engine.getSnapshot(0);
    REQUIRE(slot.hasData);
    REQUIRE(slot.snapshot.snapshotID == "test-snapshot");
    
    engine.clearSnapshot(0);
    REQUIRE_FALSE(engine.getSnapshot(0).hasData);
}

TEST_CASE("MorphingEngine interpolation mode", "[morphing]")
{
    MorphingEngine engine;
    engine.initialize();
    
    engine.setInterpolationMode(InterpolationMode::Cosine);
    REQUIRE(engine.getInterpolationMode() == InterpolationMode::Cosine);
    
    engine.setInterpolationMode(InterpolationMode::Bezier);
    REQUIRE(engine.getInterpolationMode() == InterpolationMode::Bezier);
}

//==============================================================================
// Trajectory Tests
//==============================================================================

TEST_CASE("Trajectory evaluation", "[trajectory]")
{
    MorphingTrajectory trajectory;
    trajectory.totalDuration = 2.0f;
    
    trajectory.points = {
        {0.0f, 0.0f, 0.0f, 0.0f},
        {1.0f, 1.0f, 1.0f, 0.0f},
        {0.5f, 0.5f, 2.0f, 0.0f}
    };
    
    SECTION("Start position")
    {
        auto pos = trajectory.evaluateAt(0.0f);
        REQUIRE(pos.x == Approx(0.0f));
        REQUIRE(pos.y == Approx(0.0f));
    }
    
    SECTION("Mid position")
    {
        auto pos = trajectory.evaluateAt(1.0f);
        REQUIRE(pos.x == Approx(1.0f));
        REQUIRE(pos.y == Approx(1.0f));
    }
    
    SECTION("End position")
    {
        auto pos = trajectory.evaluateAt(2.0f);
        REQUIRE(pos.x == Approx(0.5f));
        REQUIRE(pos.y == Approx(0.5f));
    }
}

TEST_CASE("Trajectory serialization", "[trajectory]")
{
    MorphingTrajectory original;
    original.trajectoryID = "test-traj";
    original.trajectoryName = "Test Trajectory";
    original.totalDuration = 5.0f;
    original.loopEnabled = true;
    original.points = {
        {0.0f, 0.0f, 0.0f, 0.5f},
        {1.0f, 1.0f, 2.5f, 0.5f}
    };
    
    auto json = original.toJSON();
    MorphingTrajectory restored = MorphingTrajectory::fromJSON(json);
    
    REQUIRE(restored.trajectoryID == original.trajectoryID);
    REQUIRE(restored.trajectoryName == original.trajectoryName);
    REQUIRE(restored.totalDuration == Approx(original.totalDuration));
    REQUIRE(restored.loopEnabled == original.loopEnabled);
    REQUIRE(restored.points.size() == original.points.size());
}

//==============================================================================
// Lock-Free Queue Tests
//==============================================================================

TEST_CASE("LockFreeQueue basic operations", "[util]")
{
    LockFreeQueue<int, 16> queue;
    
    REQUIRE(queue.isEmpty());
    REQUIRE_FALSE(queue.isFull());
    
    REQUIRE(queue.push(42));
    REQUIRE_FALSE(queue.isEmpty());
    
    int value;
    REQUIRE(queue.pop(value));
    REQUIRE(value == 42);
    REQUIRE(queue.isEmpty());
}

TEST_CASE("LockFreeQueue capacity", "[util]")
{
    LockFreeQueue<int, 8> queue;
    
    // Should be able to push Capacity - 1 items
    for (int i = 0; i < 7; ++i) {
        REQUIRE(queue.push(i));
    }
    
    REQUIRE(queue.isFull());
    REQUIRE_FALSE(queue.push(100));
}

TEST_CASE("LockFreeQueue FIFO order", "[util]")
{
    LockFreeQueue<int, 16> queue;
    
    for (int i = 0; i < 10; ++i) {
        queue.push(i);
    }
    
    for (int i = 0; i < 10; ++i) {
        int value;
        REQUIRE(queue.pop(value));
        REQUIRE(value == i);
    }
}
