/*
 * More-Phi — Core/MeterWindowAccumulator.cpp
 */
#include "MeterWindowAccumulator.h"

#include <algorithm>
#include <cmath>

namespace more_phi {

namespace {

float finiteOr(float value, float fallback) noexcept
{
    return std::isfinite(value) ? value : fallback;
}

float percentile(float* values, int count, float p) noexcept
{
    if (count <= 0)
        return 0.0f;

    const float position = std::clamp(p, 0.0f, 1.0f) * static_cast<float>(count - 1);
    const int lower = static_cast<int>(std::floor(position));
    const int upper = std::min(lower + 1, count - 1);
    const float fraction = position - static_cast<float>(lower);
    return values[lower] + (values[upper] - values[lower]) * fraction;
}

} // namespace

void MeterWindowAccumulator::reset() noexcept
{
    sequence_.fetch_add(1, std::memory_order_acq_rel);
    samples_ = {};
    head_.store(0, std::memory_order_release);
    count_.store(0, std::memory_order_release);
    sequence_.fetch_add(1, std::memory_order_release);
}

void MeterWindowAccumulator::pushSample(const MeterSample& sample) noexcept
{
    sequence_.fetch_add(1, std::memory_order_acq_rel);

    const int head = head_.load(std::memory_order_relaxed);
    samples_[static_cast<size_t>(head)] = sample;
    head_.store((head + 1) % kCapacity, std::memory_order_release);

    const int oldCount = count_.load(std::memory_order_relaxed);
    if (oldCount < kCapacity)
        count_.store(oldCount + 1, std::memory_order_release);

    sequence_.fetch_add(1, std::memory_order_release);
}

MeterWindowAccumulator::WindowStatistics MeterWindowAccumulator::computeWindow(float windowSeconds) const noexcept
{
    WindowStatistics result;
    result.requestedSeconds = std::max(0.0f, windowSeconds);

    std::array<MeterSample, kCapacity> localSamples {};
    int localCount = 0;
    int localHead = 0;

    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const uint32_t seqBefore = sequence_.load(std::memory_order_acquire);
        if ((seqBefore & 1u) != 0u)
            continue;

        localCount = count_.load(std::memory_order_acquire);
        localHead = head_.load(std::memory_order_acquire);
        localSamples = samples_;

        const uint32_t seqAfter = sequence_.load(std::memory_order_acquire);
        if (seqBefore == seqAfter && (seqAfter & 1u) == 0u)
            break;

        localCount = 0;
    }

    if (localCount <= 0)
        return result;

    const int newestIndex = (localHead - 1 + kCapacity) % kCapacity;
    const double newestTime = localSamples[static_cast<size_t>(newestIndex)].timestampSeconds;
    const double oldestAllowed = newestTime - static_cast<double>(result.requestedSeconds);

    std::array<std::array<float, kCapacity>, kMaxMetrics> metricValues {};
    std::array<int, kMaxMetrics> metricCounts {};

    double earliestTime = newestTime;
    double latestTime = newestTime;

    for (int i = 0; i < localCount; ++i)
    {
        const int ringIndex = (localHead - localCount + i + kCapacity) % kCapacity;
        const auto& sample = localSamples[static_cast<size_t>(ringIndex)];
        if (result.requestedSeconds > 0.0f && sample.timestampSeconds < oldestAllowed)
            continue;

        earliestTime = std::min(earliestTime, sample.timestampSeconds);
        latestTime = std::max(latestTime, sample.timestampSeconds);
        ++result.sampleCount;

        const std::array<float, kMaxMetrics> sampleMetrics {
            finiteOr(sample.rms, 0.0f),
            finiteOr(sample.lufsMomentary, -100.0f),
            finiteOr(sample.lufsShortTerm, -100.0f),
            finiteOr(sample.lufsIntegrated, -100.0f),
            finiteOr(sample.lra, 0.0f),
            finiteOr(sample.truePeakDBTP, -100.0f),
            finiteOr(sample.limiterGRDB, 0.0f),
            finiteOr(sample.spectralCentroidHz, 0.0f),
            finiteOr(sample.spectralTiltDBPerOctave, 0.0f),
            finiteOr(sample.stereoWidth, 0.0f),
            finiteOr(sample.midBandCorrelation, 0.0f)
        };

        for (int metric = 0; metric < kMaxMetrics; ++metric)
        {
            const int dst = metricCounts[static_cast<size_t>(metric)]++;
            metricValues[static_cast<size_t>(metric)][static_cast<size_t>(dst)] = sampleMetrics[static_cast<size_t>(metric)];
        }
    }

    if (result.sampleCount <= 0)
        return result;

    result.success = true;
    result.startTimestampSeconds = earliestTime;
    result.endTimestampSeconds = latestTime;
    result.actualSeconds = static_cast<float>(std::max(0.0, latestTime - earliestTime));

    for (int metric = 0; metric < kMaxMetrics; ++metric)
    {
        auto& metricArray = metricValues[static_cast<size_t>(metric)];
        result.metrics[static_cast<size_t>(metric)] = computeMetric(metricArray.data(), metricCounts[static_cast<size_t>(metric)]);
    }

    return result;
}

MeterWindowAccumulator::MetricStatistics MeterWindowAccumulator::computeMetric(float* values, int count) noexcept
{
    MetricStatistics stats;
    stats.count = count;
    if (count <= 0)
        return stats;

    std::sort(values, values + count);

    float sum = 0.0f;
    for (int i = 0; i < count; ++i)
        sum += values[i];

    stats.min = values[0];
    stats.max = values[count - 1];
    stats.mean = sum / static_cast<float>(count);
    stats.p10 = percentile(values, count, 0.1f);
    stats.p50 = percentile(values, count, 0.5f);
    stats.p90 = percentile(values, count, 0.9f);
    return stats;
}

} // namespace more_phi
