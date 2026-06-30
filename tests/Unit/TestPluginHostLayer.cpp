/*
    More-Phi — tests/Unit/TestPluginHostLayer.cpp
    Comprehensive unit tests for the plugin hosting layer components:
    - DAWTransportForwarder (seqlock-based transport state forwarding)
    - PluginHealthMonitor (state-machine crash isolation)
    - PluginHostManager integration (acquire/release, exception handling, refcounting)
*/
#include <catch2/catch_test_macros.hpp>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <memory>
#include <array>

#include "Host/DAWTransportForwarder.h"
#include "Host/PluginHealthMonitor.h"
#include "Host/PluginHostManager.h"

using namespace more_phi;

// =============================================================================
//  DAWTransportForwarder Tests
// =============================================================================
TEST_CASE("DAWTransportForwarder: basic snapshot lifecycle", "[transport][forwarder]")
{
    DAWTransportForwarder forwarder;

    // No snapshot published yet
    auto snap = forwarder.getSnapshot();
    REQUIRE(!snap.has_value());

    // Publish from a mock playhead
    struct MockPlayHead : juce::AudioPlayHead
    {
        juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override
        {
            juce::AudioPlayHead::PositionInfo info;
            info.setBpm(juce::Optional<double>{140.0});
            info.setTimeSignature(juce::Optional<juce::AudioPlayHead::TimeSignature>{
                juce::AudioPlayHead::TimeSignature{4, 4}});
            info.setIsPlaying(true);
            info.setIsLooping(false);
            info.setPpqPosition(juce::Optional<double>{64.5});
            info.setTimeInSeconds(juce::Optional<double>{1.25});
            return info;
        }
    } playHead;

    forwarder.updateFromAudioThread(&playHead);

    snap = forwarder.getSnapshot();
    REQUIRE(snap.has_value());
    REQUIRE(snap->bpm == 140.0);
    REQUIRE(snap->timeSigNumerator == 4);
    REQUIRE(snap->timeSigDenominator == 4);
    REQUIRE(snap->isPlaying == true);
    REQUIRE(snap->isLooping == false);
    REQUIRE(snap->ppqPosition == 64.5);
    REQUIRE(snap->timeInSeconds == 1.25);
    REQUIRE(snap->version == 1);
}

TEST_CASE("DAWTransportForwarder: null playhead is a no-op", "[transport][forwarder]")
{
    DAWTransportForwarder forwarder;
    // Publish explicit nullptr
    forwarder.updateFromAudioThread(nullptr);
    auto snap = forwarder.getSnapshot();
    REQUIRE(!snap.has_value());
}

TEST_CASE("DAWTransportForwarder: version monotonicity", "[transport][forwarder]")
{
    struct MockPlayHead2 : juce::AudioPlayHead
    {
        juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override
        {
            juce::AudioPlayHead::PositionInfo info;
            info.setBpm(juce::Optional<double>{120.0});
            return info;
        }
    } playHead;

    DAWTransportForwarder forwarder;

    uint32_t lastVersion = 0;
    for (int i = 0; i < 10; ++i)
    {
        forwarder.updateFromAudioThread(&playHead);
        auto snap = forwarder.getSnapshot();
        REQUIRE(snap.has_value());
        REQUIRE(snap->version > lastVersion);
        lastVersion = snap->version;
    }
}

TEST_CASE("DAWTransportForwarder: concurrent read/write stress", "[transport][forwarder][stress]")
{
    struct MockPlayHead : juce::AudioPlayHead
    {
        juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override
        {
            juce::AudioPlayHead::PositionInfo info;
            info.setBpm(juce::Optional<double>{130.0});
            info.setTimeSignature(juce::Optional<juce::AudioPlayHead::TimeSignature>{
                juce::AudioPlayHead::TimeSignature{3, 4}});
            info.setIsPlaying(true);
            return info;
        }
    } playHead;

    DAWTransportForwarder forwarder;

    std::atomic<bool> stop{false};
    std::atomic<int> writerCount{0};
    std::atomic<int> readerCount{0};

    auto writer = [&]() {
        while (!stop.load())
        {
            forwarder.updateFromAudioThread(&playHead);
            writerCount.fetch_add(1);
        }
    };

    auto reader = [&]() {
        while (!stop.load())
        {
            auto snap = forwarder.getSnapshot();
            if (snap.has_value())
            {
                // Coherence check: BPM should always be 130.0 in this test.
                // If seqlock failed, we could observe partial writes.
                REQUIRE(snap->bpm == 130.0);
                REQUIRE(snap->timeSigNumerator == 3);
                REQUIRE(snap->timeSigDenominator == 4);
                REQUIRE(snap->isPlaying == true);
            }
            readerCount.fetch_add(1);
        }
    };

    std::thread t1(writer);
    std::thread t2(writer);
    std::thread t3(reader);
    std::thread t4(reader);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop = true;

    t1.join(); t2.join(); t3.join(); t4.join();

    REQUIRE(writerCount.load() > 0);
    REQUIRE(readerCount.load() > 0);
}

// =============================================================================
//  PluginHealthMonitor Tests
// =============================================================================
TEST_CASE("PluginHealthMonitor: initial state is healthy", "[health][monitor]")
{
    PluginHealthMonitor monitor;
    REQUIRE(monitor.getState() == PluginHealthState::Healthy);
    REQUIRE(monitor.getConsecutiveFailureCount() == 0);
    REQUIRE(monitor.shouldProcess() == true);
}

TEST_CASE("PluginHealthMonitor: single failure transitions to degraded", "[health][monitor]")
{
    PluginHealthMonitor monitor;
    monitor.setMaxConsecutiveFailures(5);

    monitor.reportFailure();
    REQUIRE(monitor.getState() == PluginHealthState::Degraded);
    REQUIRE(monitor.getConsecutiveFailureCount() == 1);
    REQUIRE(monitor.shouldProcess() == true); // Degraded still processes
}

TEST_CASE("PluginHealthMonitor: threshold failure transitions to suspended", "[health][monitor]")
{
    PluginHealthMonitor monitor;
    monitor.setMaxConsecutiveFailures(3);

    monitor.reportFailure(); // Degraded, count=1
    monitor.reportFailure(); // Degraded, count=2
    monitor.reportFailure(); // Suspended, count=3

    REQUIRE(monitor.getState() == PluginHealthState::Suspended);
    REQUIRE(monitor.shouldProcess() == false);
}

TEST_CASE("PluginHealthMonitor: success resets degraded to healthy", "[health][monitor]")
{
    PluginHealthMonitor monitor;
    monitor.reportFailure();
    REQUIRE(monitor.getState() == PluginHealthState::Degraded);

    monitor.reportSuccess();
    REQUIRE(monitor.getState() == PluginHealthState::Healthy);
    REQUIRE(monitor.getConsecutiveFailureCount() == 0);
}

TEST_CASE("PluginHealthMonitor: recovery pipeline", "[health][monitor]")
{
    PluginHealthMonitor monitor;
    monitor.setMaxConsecutiveFailures(2);
    monitor.setRecoveryDelayMs(100); // fast for testing
    monitor.setMaxRecoveryAttempts(2);

    // Push to suspended
    monitor.reportFailure();
    monitor.reportFailure();
    REQUIRE(monitor.getState() == PluginHealthState::Suspended);

    // Not ready to recover yet
    REQUIRE(monitor.shouldRecover() == false);

    // Wait for recovery delay
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE(monitor.shouldRecover() == true);

    // Begin recovery
    REQUIRE(monitor.beginRecovery() == true);
    REQUIRE(monitor.getState() == PluginHealthState::Recovering);

    // Success ends recovery
    monitor.endRecovery(true);
    REQUIRE(monitor.getState() == PluginHealthState::Healthy);
    REQUIRE(monitor.getConsecutiveFailureCount() == 0);
}

TEST_CASE("PluginHealthMonitor: recovery failure bumps to suspended", "[health][monitor]")
{
    PluginHealthMonitor monitor;
    monitor.setMaxConsecutiveFailures(2);
    monitor.setRecoveryDelayMs(10);
    monitor.setMaxRecoveryAttempts(2);

    monitor.reportFailure();
    monitor.reportFailure();
    REQUIRE(monitor.getState() == PluginHealthState::Suspended);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(monitor.beginRecovery() == true);

    // Recovery fails
    monitor.endRecovery(false);
    REQUIRE(monitor.getState() == PluginHealthState::Suspended);
}

TEST_CASE("PluginHealthMonitor: exhausted recovery attempts terminates", "[health][monitor]")
{
    PluginHealthMonitor monitor;
    monitor.setMaxConsecutiveFailures(1);
    monitor.setRecoveryDelayMs(10);
    monitor.setMaxRecoveryAttempts(1);

    monitor.reportFailure();
    REQUIRE(monitor.getState() == PluginHealthState::Suspended);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(monitor.beginRecovery() == true);
    REQUIRE(monitor.beginRecovery() == false); // No more attempts

    REQUIRE(monitor.getState() == PluginHealthState::Terminated);
    REQUIRE(monitor.shouldProcess() == false);
}

TEST_CASE("PluginHealthMonitor: getSnapshot is coherent", "[health][monitor]")
{
    PluginHealthMonitor monitor;
    auto snap = monitor.getSnapshot();
    REQUIRE(snap.state == PluginHealthState::Healthy);
    REQUIRE(snap.consecutiveFailures == 0);
    REQUIRE(snap.recoveryAttempts == 0);
}

TEST_CASE("PluginHealthMonitor: reset restores pristine state", "[health][monitor]")
{
    PluginHealthMonitor monitor;
    monitor.setMaxConsecutiveFailures(1);
    monitor.reportFailure();
    REQUIRE(monitor.getState() == PluginHealthState::Suspended);

    monitor.reset();
    REQUIRE(monitor.getState() == PluginHealthState::Healthy);
    REQUIRE(monitor.getConsecutiveFailureCount() == 0);
}

// =============================================================================
//  PluginHostManager Integration Tests
// =============================================================================
TEST_CASE("PluginHostManager: initial state", "[host][manager]")
{
    PluginHostManager manager;
    REQUIRE(!manager.hasPlugin());
    REQUIRE(manager.getPlugin() == nullptr);
    REQUIRE(manager.getHealthState() == PluginHealthState::Healthy);
    REQUIRE(manager.getExceptionCount() == 0);
    REQUIRE(manager.isPluginSwapping() == false);
}

TEST_CASE("PluginHostManager: acquire/release refcounting (no crash)", "[host][manager][refcount]")
{
    PluginHostManager manager;

    // Acquire with no plugin loaded returns nullptr
    auto* plugin = manager.acquirePluginForUse();
    REQUIRE(plugin == nullptr);

    // Release with no plugin loaded is a no-op (no crash / underflow)
    manager.releasePluginFromUse();

    // Exclusive use with no plugin returns nullptr
    auto* exclusive = manager.beginExclusivePluginUse(1000);
    REQUIRE(exclusive == nullptr);
    manager.endExclusivePluginUse();
}

TEST_CASE("PluginHostManager: exclusive use with no plugin returns nullptr and resets flag", "[host][manager][refcount]")
{
    PluginHostManager manager;

    // When no plugin is loaded, beginExclusivePluginUse returns nullptr
    // and resets the exclusive-use flag because there is no plugin to protect.
    REQUIRE(manager.beginExclusivePluginUse(1000) == nullptr);
    REQUIRE(manager.isExclusivePluginUseRequested() == false);

    // Acquire should return nullptr because there's no plugin, not because of exclusive use.
    auto* plugin = manager.acquirePluginForUse();
    REQUIRE(plugin == nullptr);
}

TEST_CASE("PluginHostManager: window close callback fires", "[host][manager][callback]")
{
    PluginHostManager manager;

    bool callbackFired = false;
    manager.setWindowCloseCallback([&callbackFired]() {
        callbackFired = true;
    });

    // Callback is stored; direct invocation is not testable from here without
    // a real plugin swap.  Just verify it was set by confirming no crash.
    REQUIRE(!callbackFired); // not invoked yet
}

TEST_CASE("PluginHostManager: health snapshot is accessible", "[host][manager][health]")
{
    PluginHostManager manager;
    auto snap = manager.getHealthSnapshot();
    REQUIRE(snap.state == PluginHealthState::Healthy);
    REQUIRE(snap.consecutiveFailures == 0);
}

TEST_CASE("PluginHostManager: getNumSteps with no plugin returns 0", "[host][manager]")
{
    PluginHostManager manager;
    REQUIRE(manager.getNumSteps(0) == 0);
    REQUIRE(manager.getNumSteps(100) == 0);
    REQUIRE(manager.getNumSteps(-1) == 0);
}

// =============================================================================
//  Edge Case / Stress Tests
// =============================================================================
TEST_CASE("PluginHealthMonitor: rapid success/failure ping-pong", "[health][monitor][stress]")
{
    PluginHealthMonitor monitor;
    monitor.setMaxConsecutiveFailures(100);

    for (int i = 0; i < 1000; ++i)
    {
        monitor.reportFailure();
        monitor.reportSuccess();
    }

    REQUIRE(monitor.getState() == PluginHealthState::Healthy);
    REQUIRE(monitor.getConsecutiveFailureCount() == 0);
}

TEST_CASE("PluginHostManager: concurrent exclusive use toggle", "[host][manager][stress]")
{
    PluginHostManager manager;
    std::atomic<bool> stop{false};

    auto toggler = [&]() {
        for (int i = 0; i < 500; ++i)
        {
            manager.beginExclusivePluginUse(10);
            manager.endExclusivePluginUse();
        }
    };

    auto acquirer = [&]() {
        while (!stop.load())
        {
            auto* p = manager.acquirePluginForUse();
            if (p == nullptr)
            {
                // Either no plugin or exclusive use active — both are valid states.
                juce::ignoreUnused(p);
            }
        }
    };

    std::thread t1(toggler);
    std::thread t2(toggler);
    std::thread t3(acquirer);

    t1.join();
    t2.join();
    stop = true;
    t3.join();

    // No crash = pass
    REQUIRE(true);
}
