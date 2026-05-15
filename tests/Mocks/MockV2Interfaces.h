/*
 * More-Phi — Tests/Mocks/MockV2Interfaces.h
 * Mock and stub implementations for V2 subsystem testing.
 *
 * These mocks are self-contained and do not depend on any V2 source files
 * that may not yet exist. They implement the V2 API contracts against which
 * future implementations will be validated.
 *
 * Design principles:
 *   - No heap allocations in audio-path methods (use pre-allocated buffers)
 *   - Fixed-size storage for RT-safety simulation
 *   - All mock state is explicit and inspectable from tests
 */
#pragma once

#include "Core/ModulationTypes.h"

#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <functional>
#include <string>
#include <cstdint>

namespace more_phi {
namespace test {

// ---------------------------------------------------------------------------
// SimpleAudioBuffer — lightweight audio buffer for tests that don't link JUCE
// ---------------------------------------------------------------------------

/**
 * A minimal audio buffer mock that avoids any dependency on juce::AudioBuffer.
 * Used by spectral and granular engine tests that operate on raw float arrays.
 */
struct SimpleAudioBuffer
{
    std::vector<std::vector<float>> channels;
    int numSamples = 0;

    SimpleAudioBuffer(int numChannels, int samples)
        : channels(static_cast<size_t>(numChannels),
                   std::vector<float>(static_cast<size_t>(samples), 0.0f)),
          numSamples(samples)
    {}

    float* getWritePointer(int ch)
    {
        return channels[static_cast<size_t>(ch)].data();
    }

    const float* getReadPointer(int ch) const
    {
        return channels[static_cast<size_t>(ch)].data();
    }

    int getNumChannels() const { return static_cast<int>(channels.size()); }
    int getNumSamples() const  { return numSamples; }

    void fillWith(float value)
    {
        for (auto& ch : channels)
            std::fill(ch.begin(), ch.end(), value);
    }

    void fillWithSine(int channel, float frequency, float sampleRate)
    {
        const float twoPi = 2.0f * 3.14159265358979f;
        for (int i = 0; i < numSamples; ++i)
            channels[static_cast<size_t>(channel)][static_cast<size_t>(i)] =
                std::sin(twoPi * frequency * static_cast<float>(i) / sampleRate);
    }

    /** Compute RMS of a single channel. */
    float rms(int channel) const
    {
        const auto& ch = channels[static_cast<size_t>(channel)];
        double acc = 0.0;
        for (float s : ch) acc += static_cast<double>(s) * s;
        return static_cast<float>(std::sqrt(acc / ch.size()));
    }

    /** Compute mean (DC) of a single channel. */
    float mean(int channel) const
    {
        const auto& ch = channels[static_cast<size_t>(channel)];
        double sum = 0.0;
        for (float s : ch) sum += s;
        return static_cast<float>(sum / ch.size());
    }

    /** Copy data from another buffer into this one. */
    void copyFrom(const SimpleAudioBuffer& src)
    {
        const int chCount = std::min(getNumChannels(), src.getNumChannels());
        const int n       = std::min(numSamples, src.numSamples);
        for (int c = 0; c < chCount; ++c)
            std::copy_n(src.channels[static_cast<size_t>(c)].begin(), n,
                        channels[static_cast<size_t>(c)].begin());
    }
};

// ---------------------------------------------------------------------------
// LFOState — minimal LFO implementation for modulation tests
// ---------------------------------------------------------------------------

/**
 * A self-contained LFO implementation used to validate modulation contracts
 * without depending on a ModulationEngine compilation unit.
 *
 * This mirrors the expected production interface so tests can switch to
 * using the real implementation once it exists.
 */
class LFOState
{
public:
    enum class Shape { Sine, Triangle, Saw, Square, SampleAndHold };

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate;
        phase_      = 0.0;
        heldValue_  = 0.0f;
    }

    /** Advance by one sample and return the current waveform value in [-1, 1]. */
    float tick(float rateHz)
    {
        const double phaseInc = rateHz / sampleRate_;
        double prevPhase = phase_;
        phase_ += phaseInc;

        bool wrapped = false;
        if (phase_ >= 1.0) { phase_ -= 1.0; wrapped = true; }

        switch (shape_)
        {
            case Shape::Sine:
                return static_cast<float>(
                    std::sin(2.0 * 3.14159265358979 * phase_));

            case Shape::Triangle:
            {
                float t = static_cast<float>(phase_);
                return (t < 0.5f) ? (4.0f * t - 1.0f) : (3.0f - 4.0f * t);
            }

            case Shape::Saw:
                return static_cast<float>(phase_ * 2.0 - 1.0);

            case Shape::Square:
                return (phase_ < 0.5) ? 1.0f : -1.0f;

            case Shape::SampleAndHold:
                if (wrapped)
                {
                    // Simple pseudo-random based on phase position
                    heldValue_ = static_cast<float>((prevPhase * 12345.6789) -
                                                    std::floor(prevPhase * 12345.6789));
                    heldValue_ = heldValue_ * 2.0f - 1.0f;
                }
                return heldValue_;
        }
        return 0.0f;
    }

    void setShape(Shape s) { shape_ = s; }
    Shape getShape() const { return shape_; }

    double getPhase() const { return phase_; }
    void   resetPhase()     { phase_ = 0.0; }

private:
    double  sampleRate_ = 44100.0;
    double  phase_      = 0.0;
    float   heldValue_  = 0.0f;
    Shape   shape_      = Shape::Sine;
};

// ---------------------------------------------------------------------------
// EnvelopeFollowerState — minimal envelope follower for modulation tests
// ---------------------------------------------------------------------------

/**
 * Minimal peak-follower envelope detector.
 * Attack and release times are in milliseconds.
 */
class EnvelopeFollowerState
{
public:
    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate;
        envelope_   = 0.0f;
        updateCoefficients();
    }

    void setAttackMs(float ms)   { attackMs_  = ms;  updateCoefficients(); }
    void setReleaseMs(float ms)  { releaseMs_ = ms;  updateCoefficients(); }
    void setSensitivity(float s) { sensitivity_ = std::max(0.0f, std::min(1.0f, s)); }

    float process(float inputSample)
    {
        float absIn = std::abs(inputSample) * sensitivity_;
        if (absIn > envelope_)
            envelope_ = attackCoeff_  * envelope_ + (1.0f - attackCoeff_)  * absIn;
        else
            envelope_ = releaseCoeff_ * envelope_;
        return envelope_;
    }

    float getEnvelope() const { return envelope_; }

private:
    void updateCoefficients()
    {
        const float sr = static_cast<float>(sampleRate_);
        attackCoeff_  = (attackMs_  > 0.0f) ? std::exp(-1000.0f / (attackMs_  * sr)) : 0.0f;
        releaseCoeff_ = (releaseMs_ > 0.0f) ? std::exp(-1000.0f / (releaseMs_ * sr)) : 0.0f;
    }

    double sampleRate_  = 44100.0;
    float  attackMs_    = 10.0f;
    float  releaseMs_   = 100.0f;
    float  sensitivity_ = 1.0f;
    float  attackCoeff_ = 0.0f;
    float  releaseCoeff_= 0.0f;
    float  envelope_    = 0.0f;
};

// ---------------------------------------------------------------------------
// StepSequencerState — minimal step sequencer for modulation tests
// ---------------------------------------------------------------------------

/**
 * A fixed-size step sequencer with forward, backward, and ping-pong modes.
 */
class StepSequencerState
{
public:
    enum class Direction { Forward, Backward, PingPong };

    static constexpr int MAX_STEPS = 32;

    void setStepCount(int n) { stepCount_ = std::max(1, std::min(n, MAX_STEPS)); }
    int  getStepCount() const { return stepCount_; }

    void  setStep(int i, float value) { steps_[static_cast<size_t>(i)] = value; }
    float getStep(int i) const        { return steps_[static_cast<size_t>(i)]; }

    void setDirection(Direction d) { direction_ = d; }

    /** Advance to the next step, return the current step value. */
    float advance()
    {
        float value = steps_[static_cast<size_t>(currentStep_)];

        if (smoothing_ > 0.0f)
        {
            smoothed_ = smoothed_ * smoothing_ + value * (1.0f - smoothing_);
            value = smoothed_;
        }

        // Advance index
        if (direction_ == Direction::Forward)
        {
            ++currentStep_;
            if (currentStep_ >= stepCount_) currentStep_ = 0;
        }
        else if (direction_ == Direction::Backward)
        {
            --currentStep_;
            if (currentStep_ < 0) currentStep_ = stepCount_ - 1;
        }
        else // PingPong
        {
            if (pingPongForward_)
            {
                ++currentStep_;
                if (currentStep_ >= stepCount_)
                {
                    currentStep_ = stepCount_ - 2;
                    pingPongForward_ = false;
                }
            }
            else
            {
                --currentStep_;
                if (currentStep_ < 0)
                {
                    currentStep_ = 1;
                    pingPongForward_ = true;
                }
            }
        }

        return value;
    }

    void reset() { currentStep_ = 0; pingPongForward_ = true; smoothed_ = 0.0f; }
    int  getCurrentStep() const { return currentStep_; }

    void setSmoothing(float s) { smoothing_ = std::max(0.0f, std::min(0.9999f, s)); }

private:
    std::array<float, MAX_STEPS> steps_{};
    int        stepCount_       = 8;
    int        currentStep_     = 0;
    bool       pingPongForward_ = true;
    Direction  direction_       = Direction::Forward;
    float      smoothing_       = 0.0f;
    float      smoothed_        = 0.0f;
};

// ---------------------------------------------------------------------------
// ModulationMatrixState — minimal modulation matrix for routing tests
// ---------------------------------------------------------------------------

/**
 * Implements ModulationState routing logic for unit tests.
 * Mimics the production ModulationMatrix interface.
 */
class ModulationMatrixState
{
public:
    static constexpr int MAX_ROUTES = ModulationState::MAX_ROUTES;

    ModulationMatrixState() { state_.activeRouteCount = 0; }

    /**
     * Add a route. Returns false if the matrix is full.
     */
    bool addRoute(ModSourceId source, int destParamIndex, float depth)
    {
        if (state_.activeRouteCount >= MAX_ROUTES) return false;

        ModRoute& r          = state_.routes[static_cast<size_t>(state_.activeRouteCount++)];
        r.source             = source;
        r.destParamIndex     = destParamIndex;
        r.depth              = depth;
        r.enabled            = true;
        return true;
    }

    /**
     * Remove the route at a given index. Shifts remaining routes down.
     * Returns false if index is out of range.
     */
    bool removeRoute(int index)
    {
        if (index < 0 || index >= state_.activeRouteCount) return false;
        for (int i = index; i < state_.activeRouteCount - 1; ++i)
            state_.routes[static_cast<size_t>(i)] = state_.routes[static_cast<size_t>(i + 1)];
        --state_.activeRouteCount;
        return true;
    }

    void setRouteEnabled(int index, bool enabled)
    {
        if (index >= 0 && index < state_.activeRouteCount)
            state_.routes[static_cast<size_t>(index)].enabled = enabled;
    }

    /**
     * Apply modulation to a parameter vector.
     * Each route's depth * sourceValue is added to the destination parameter,
     * then the result is clamped to [0, 1].
     *
     * @param params   Current parameter values (modified in-place)
     * @param sourceValues  Source values indexed by ModSourceId integer
     */
    void apply(std::vector<float>& params,
               const std::array<float, NUM_MOD_SOURCES>& sourceValues) const
    {
        for (int r = 0; r < state_.activeRouteCount; ++r)
        {
            const ModRoute& route = state_.routes[static_cast<size_t>(r)];
            if (!route.enabled) continue;
            if (route.destParamIndex < 0 ||
                route.destParamIndex >= static_cast<int>(params.size())) continue;

            float srcVal = sourceValues[static_cast<size_t>(route.source)];
            float delta  = srcVal * route.depth;
            params[static_cast<size_t>(route.destParamIndex)] =
                std::max(0.0f, std::min(1.0f,
                    params[static_cast<size_t>(route.destParamIndex)] + delta));
        }
    }

    int  getRouteCount() const { return state_.activeRouteCount; }
    void clearAllRoutes()      { state_.activeRouteCount = 0; }

    const ModRoute& getRoute(int index) const
    {
        return state_.routes[static_cast<size_t>(index)];
    }

private:
    ModulationState state_;
};

// ---------------------------------------------------------------------------
// GrainDescriptor — used by granular engine tests
// ---------------------------------------------------------------------------

struct GrainDescriptor
{
    bool  active       = false;
    int   startSample  = 0;
    int   currentAge   = 0;
    int   grainSize    = 512;
    float sourceMix    = 0.0f; // 0 = source A, 1 = source B
    float amplitude    = 1.0f;
};

// ---------------------------------------------------------------------------
// GrainPool — minimal grain pool implementation for granular tests
// ---------------------------------------------------------------------------

/**
 * Manages a fixed pool of grains with activation/deactivation.
 * Mirrors the expected GrainPool production interface.
 */
class GrainPool
{
public:
    static constexpr int MAX_GRAINS = 128;

    GrainPool() { grains_.fill({}); }

    /** Activate a free grain and return its index, or -1 if pool is full. */
    int activate()
    {
        for (int i = 0; i < MAX_GRAINS; ++i)
        {
            if (!grains_[static_cast<size_t>(i)].active)
            {
                grains_[static_cast<size_t>(i)].active     = true;
                grains_[static_cast<size_t>(i)].currentAge = 0;
                return i;
            }
        }
        return -1; // pool exhausted
    }

    void deactivate(int index)
    {
        if (index >= 0 && index < MAX_GRAINS)
            grains_[static_cast<size_t>(index)].active = false;
    }

    bool isActive(int index) const
    {
        if (index < 0 || index >= MAX_GRAINS) return false;
        return grains_[static_cast<size_t>(index)].active;
    }

    GrainDescriptor& get(int index)
    {
        return grains_[static_cast<size_t>(index)];
    }

    const GrainDescriptor& get(int index) const
    {
        return grains_[static_cast<size_t>(index)];
    }

    /** Iterate over active grains and invoke fn(index, grain). */
    template <typename Fn>
    void forEachActive(Fn&& fn)
    {
        for (int i = 0; i < MAX_GRAINS; ++i)
            if (grains_[static_cast<size_t>(i)].active)
                fn(i, grains_[static_cast<size_t>(i)]);
    }

    int countActive() const
    {
        int count = 0;
        for (const auto& g : grains_)
            if (g.active) ++count;
        return count;
    }

    void reset()
    {
        for (auto& g : grains_)
            g = GrainDescriptor{};
    }

    /** Hann window envelope evaluated at normalized position [0, 1]. */
    static float hannEnvelope(float position)
    {
        const float pi = 3.14159265358979f;
        return 0.5f * (1.0f - std::cos(2.0f * pi * position));
    }

private:
    std::array<GrainDescriptor, MAX_GRAINS> grains_;
};

// ---------------------------------------------------------------------------
// PresetEntry — minimal preset metadata struct for library tests
// ---------------------------------------------------------------------------

struct PresetEntry
{
    std::string id;
    std::string name;
    std::string author;
    std::string category;
    std::vector<std::string> tags;
    std::string hostedPluginName;
    std::string hostedPluginId;
    float       rating     = 0.0f;
    int64_t     createdAt  = 0;
    int64_t     updatedAt  = 0;
    std::string filePath;

    /** A preset is valid if it has a non-empty id and name. */
    bool isValid() const
    {
        return !id.empty() && !name.empty();
    }
};

// ---------------------------------------------------------------------------
// PresetSearchQuery — search/filter parameters for library queries
// ---------------------------------------------------------------------------

struct PresetSearchQuery
{
    std::string             textQuery;      // Matches name or author (case-insensitive)
    std::vector<std::string> tags;          // All specified tags must be present
    std::string             pluginFilter;   // Restrict to a hosted plugin name
    float                   minRating = 0.0f;
    int                     maxResults = 0; // 0 = unlimited

    enum class SortBy { None, Name, DateNewest } sortBy = SortBy::None;
};

} // namespace test
} // namespace more_phi
