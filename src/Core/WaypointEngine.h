/*
 * More-Phi — Core/WaypointEngine.h
 * BPM-synced waypoint sequencer for the XY morph pad.
 * Audio-thread safe: process() is noexcept, zero-allocation after configure().
 *
 * Phase 6: each waypoint is a (x, y) target with hold/transition beat counts.
 * The engine advances through the sequence at the configured BPM.
 */
#pragma once

#include <vector>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <juce_audio_basics/juce_audio_basics.h>

namespace more_phi {

class WaypointEngine
{
public:
    static constexpr int MAX_WAYPOINTS = 16;

    struct Waypoint
    {
        float x = 0.5f;
        float y = 0.5f;
        float holdBeats = 1.0f;
        float transitionBeats = 1.0f;
    };

    WaypointEngine() = default;

    void configure(const std::vector<Waypoint>& waypoints)
    {
        waypoints_ = waypoints;
        if (waypoints_.size() > MAX_WAYPOINTS)
            waypoints_.resize(MAX_WAYPOINTS);
        if (waypoints_.empty())
        {
            Waypoint def;
            waypoints_.push_back(def);
        }
        reset();
    }

    void reset()
    {
        currentIndex_.store(0, std::memory_order_relaxed);
        progress_.store(0.0f, std::memory_order_relaxed);
        phaseTime_.store(0.0, std::memory_order_relaxed);
        posX_.store(waypoints_.empty() ? 0.5f : waypoints_[0].x, std::memory_order_relaxed);
        posY_.store(waypoints_.empty() ? 0.5f : waypoints_[0].y, std::memory_order_relaxed);
        playing_.store(false, std::memory_order_relaxed);
        // Reset: start in the hold phase of the first waypoint
        phaseTime_.store(0.0, std::memory_order_relaxed);
        transitioning_.store(false, std::memory_order_relaxed);
    }

    void setPlaying(bool playing) { playing_.store(playing, std::memory_order_relaxed); }
    bool isPlaying() const { return playing_.load(std::memory_order_relaxed); }

    void setBPM(float bpm) { bpm_.store(std::max(1.0f, bpm), std::memory_order_relaxed); }
    float getBPM() const { return bpm_.load(std::memory_order_relaxed); }

    // Audio-thread safe: call per block. dt is block duration in seconds.
    // Returns the current morph target (x, y).
    void process(float dt, float& outX, float& outY) noexcept
    {
        if (!playing_.load(std::memory_order_relaxed) || waypoints_.empty())
        {
            outX = posX_.load(std::memory_order_relaxed);
            outY = posY_.load(std::memory_order_relaxed);
            return;
        }

        const float bpm = bpm_.load(std::memory_order_relaxed);
        const double beatsPerSec = bpm / 60.0;
        const double phaseDelta = static_cast<double>(dt) * beatsPerSec;

        double phase = phaseTime_.load(std::memory_order_relaxed);
        phase += phaseDelta;
        phaseTime_.store(phase, std::memory_order_relaxed);

        const int count = static_cast<int>(waypoints_.size());
        int idx = currentIndex_.load(std::memory_order_relaxed);

        // Clamp to valid range
        const auto& wp = waypoints_[static_cast<size_t>(idx)];
        const float transition = wp.transitionBeats;
        const float hold = wp.holdBeats;
        const float total = transition + hold;

        // Phase accumulates. Check if we need to advance.
        // transition_ starts from 0, goes to transition, then hold phase
        double localPhase = std::fmod(phase, static_cast<double>(total));
        bool advanceToNext = false;

        if (std::abs(localPhase - static_cast<double>(total)) < 1e-9 || localPhase >= static_cast<double>(total))
        {
            localPhase = 0.0;
            advanceToNext = true;
        }
        else if (localPhase < 0.0)
        {
            localPhase = 0.0;
        }

        if (advanceToNext)
        {
            idx = (idx + 1) % count;
            currentIndex_.store(idx, std::memory_order_relaxed);
            phaseTime_.store(0.0, std::memory_order_relaxed);
            phase = 0.0;
            localPhase = 0.0;
        }

        const bool inTransition = localPhase < static_cast<double>(transition);
        const float p = static_cast<float>(inTransition && transition > 0.0f
            ? localPhase / static_cast<double>(transition)
            : 1.0);

        const size_t prevIdx = static_cast<size_t>((idx - 1 + count) % count);
        const size_t curIdx = static_cast<size_t>(idx);

        const float x0 = waypoints_[prevIdx].x;
        const float y0 = waypoints_[prevIdx].y;
        const float x1 = waypoints_[curIdx].x;
        const float y1 = waypoints_[curIdx].y;

        const float t = juce::jlimit(0.0f, 1.0f, p);
        const float x = x0 + (x1 - x0) * t;
        const float y = y0 + (y1 - y0) * t;

        posX_.store(x, std::memory_order_relaxed);
        posY_.store(y, std::memory_order_relaxed);
        progress_.store(p, std::memory_order_relaxed);
        transitioning_.store(inTransition, std::memory_order_relaxed);

        outX = x;
        outY = y;
    }

    // UI-thread read access
    int getCurrentIndex() const { return currentIndex_.load(std::memory_order_relaxed); }
    float getProgress() const { return progress_.load(std::memory_order_relaxed); }
    bool isTransitioning() const { return transitioning_.load(std::memory_order_relaxed); }
    float getPositionX() const { return posX_.load(std::memory_order_relaxed); }
    float getPositionY() const { return posY_.load(std::memory_order_relaxed); }
    int getNumWaypoints() const { return static_cast<int>(waypoints_.size()); }

    const Waypoint& getWaypoint(int index) const
    {
        static const Waypoint defaultWp;
        if (index < 0 || index >= static_cast<int>(waypoints_.size()))
            return defaultWp;
        return waypoints_[static_cast<size_t>(index)];
    }

private:
    std::vector<Waypoint> waypoints_{{0.5f, 0.5f, 1.0f, 1.0f}};
    std::atomic<int> currentIndex_{0};
    std::atomic<float> progress_{0.0f};
    std::atomic<double> phaseTime_{0.0};
    std::atomic<float> bpm_{120.0f};
    std::atomic<float> posX_{0.5f};
    std::atomic<float> posY_{0.5f};
    std::atomic<bool> playing_{false};
    // AUDIT-FIX (C4): was a plain bool written on the audio thread (process())
    // and read on the UI thread (isTransitioning()) — data-race UB. Relaxed
    // ordering is sufficient: this is a UI hint, not a synchronization primitive.
    std::atomic<bool> transitioning_{false};
};

} // namespace more_phi
