/*
 * More-Phi — Core/MeterWindowAccumulator.h
 */
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <juce_core/juce_core.h>

namespace more_phi {

class MeterWindowAccumulator
{
public:
    static constexpr int kCapacity = 300;
    static constexpr int kMaxMetrics = 11;

    enum MetricIndex
    {
        rms = 0,
        lufsMomentary,
        lufsShortTerm,
        lufsIntegrated,
        lra,
        truePeakDBTP,
        limiterGRDB,
        spectralCentroidHz,
        spectralTiltDBPerOctave,
        stereoWidth,
        midBandCorrelation
    };

    struct MeterSample
    {
        double timestampSeconds = 0.0;
        float rms = 0.0f;
        float lufsMomentary = -100.0f;
        float lufsShortTerm = -100.0f;
        float lufsIntegrated = -100.0f;
        float lra = 0.0f;
        float truePeakDBTP = -100.0f;
        float limiterGRDB = 0.0f;
        float spectralCentroidHz = 0.0f;
        float spectralTiltDBPerOctave = 0.0f;
        float stereoWidth = 0.0f;
        float midBandCorrelation = 0.0f;
    };

    struct MetricStatistics
    {
        float min = 0.0f;
        float max = 0.0f;
        float mean = 0.0f;
        float p10 = 0.0f;
        float p50 = 0.0f;
        float p90 = 0.0f;
        int count = 0;
    };

    struct WindowStatistics
    {
        bool success = false;
        float requestedSeconds = 0.0f;
        float actualSeconds = 0.0f;
        int sampleCount = 0;
        double startTimestampSeconds = 0.0;
        double endTimestampSeconds = 0.0;
        std::array<MetricStatistics, kMaxMetrics> metrics {};
    };

    void reset() noexcept;
    void pushSample(const MeterSample& sample) noexcept;
    [[nodiscard]] WindowStatistics computeWindow(float windowSeconds) const noexcept;

private:
    [[nodiscard]] static MetricStatistics computeMetric(float* values, int count) noexcept;

    std::array<MeterSample, kCapacity> samples_ {};
    std::atomic<uint32_t> sequence_ { 0 };
    std::atomic<int> head_ { 0 };
    std::atomic<int> count_ { 0 };
};

} // namespace more_phi
