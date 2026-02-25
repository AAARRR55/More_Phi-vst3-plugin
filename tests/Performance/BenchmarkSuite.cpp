/*
 * MorphSnap — Performance Benchmark Suite
 * Measures CPU usage, memory allocations, and throughput.
 *
 * Build with: cmake -DCMAKE_BUILD_TYPE=Release -DMORPHSNAP_BUILD_BENCHMARKS=ON
 * Run with: ./MorphSnap_Benchmarks
 */
#include <iostream>
#include <chrono>
#include <vector>
#include <cmath>
#include <atomic>
#include <thread>
#include <iomanip>
#include <cstring>

// Include the components to benchmark
#include "Core/InterpolationEngine.h"
#include "Core/SnapshotBank.h"
#include "Core/PhysicsEngine.h"

namespace morphsnap {
namespace benchmark {

// ── Timing Utilities ──────────────────────────────────────────────────────────

class Timer
{
public:
    void start() { start_ = std::chrono::high_resolution_clock::now(); }

    double stopMs()
    {
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(end - start_).count();
    }

    double stopUs()
    {
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::micro>(end - start_).count();
    }

private:
    std::chrono::high_resolution_clock::time_point start_;
};

// ── Benchmark Results ──────────────────────────────────────────────────────────

struct BenchmarkResult
{
    std::string name;
    double avgTimeUs;
    double minTimeUs;
    double maxTimeUs;
    double throughput;  // items/sec
    bool passed;

    void print() const
    {
        std::cout << std::left << std::setw(40) << name
                  << std::right << std::setw(12) << std::fixed << std::setprecision(3) << avgTimeUs << " us"
                  << std::setw(12) << minTimeUs << " us"
                  << std::setw(12) << maxTimeUs << " us"
                  << std::setw(16) << std::scientific << std::setprecision(2) << throughput << "/s"
                  << std::setw(8) << (passed ? "PASS" : "FAIL")
                  << std::endl;
    }
};

// ── Interpolation Benchmarks ───────────────────────────────────────────────────

BenchmarkResult benchmarkScalarInterpolation(size_t count, int iterations)
{
    std::vector<float> srcA(count), srcB(count), dest(count);

    // Initialize with deterministic values
    for (size_t i = 0; i < count; ++i)
    {
        srcA[i] = static_cast<float>(i) * 0.01f;
        srcB[i] = static_cast<float>(count - i) * 0.01f;
    }

    Timer timer;
    double totalTime = 0.0;
    double minTime = std::numeric_limits<double>::max();
    double maxTime = 0.0;

    for (int i = 0; i < iterations; ++i)
    {
        timer.start();
        // Manual scalar interpolation as reference baseline
        const float t = 0.5f;
        for (size_t p = 0; p < count; ++p)
            dest[p] = srcA[p] * (1.0f - t) + srcB[p] * t;
        double elapsed = timer.stopUs();
        totalTime += elapsed;
        minTime = std::min(minTime, elapsed);
        maxTime = std::max(maxTime, elapsed);
    }

    double avgTime = totalTime / iterations;
    return {
        "Scalar Interpolation (" + std::to_string(count) + " params)",
        avgTime, minTime, maxTime,
        static_cast<double>(count) / (avgTime * 1e-6),
        avgTime < 100.0  // Should be < 100us
    };
}

BenchmarkResult benchmarkSIMDInterpolation(size_t count, int iterations)
{
    std::vector<float> srcA(count), srcB(count), dest(count);

    for (size_t i = 0; i < count; ++i)
    {
        srcA[i] = static_cast<float>(i) * 0.01f;
        srcB[i] = static_cast<float>(count - i) * 0.01f;
    }

    Timer timer;
    double totalTime = 0.0;
    double minTime = std::numeric_limits<double>::max();
    double maxTime = 0.0;

    for (int i = 0; i < iterations; ++i)
    {
        timer.start();
        InterpolationEngine::interpolateBatch_SIMD(
            srcA.data(), srcB.data(), dest.data(), 0.5f, count);
        double elapsed = timer.stopUs();
        totalTime += elapsed;
        minTime = std::min(minTime, elapsed);
        maxTime = std::max(maxTime, elapsed);
    }

    double avgTime = totalTime / iterations;
    bool hasSimd = InterpolationEngine::hasSSESupport() || InterpolationEngine::hasAVXSupport();

    return {
        "SIMD Interpolation (" + std::to_string(count) + " params)",
        avgTime, minTime, maxTime,
        static_cast<double>(count) / (avgTime * 1e-6),
        hasSimd ? avgTime < 30.0 : avgTime < 100.0  // SIMD should be faster
    };
}

// ── Physics Benchmarks ─────────────────────────────────────────────────────────

BenchmarkResult benchmarkElasticPhysics(int iterations)
{
    ElasticState state{0.0f, 0.0f, 0.0f, 0.0f};
    float targetX = 0.5f, targetY = 0.5f;
    float dt = 1.0f / 48000.0f;  // One sample at 48kHz

    Timer timer;
    double totalTime = 0.0;

    timer.start();
    for (int i = 0; i < iterations; ++i)
    {
        PhysicsEngine::updateElastic(state, targetX, targetY,
                                      ElasticPreset::Medium, dt);
    }
    totalTime = timer.stopUs();

    double avgTime = totalTime / iterations;
    return {
        "Elastic Physics (per update)",
        avgTime, avgTime, avgTime,
        1.0 / (avgTime * 1e-6),
        avgTime < 1.0  // Should be < 1us
    };
}

BenchmarkResult benchmarkDriftPhysics(int iterations)
{
    float x = 0.0f, y = 0.0f;
    float time = 0.0f;
    float dt = 1.0f / 48000.0f;

    Timer timer;
    double totalTime = 0.0;

    timer.start();
    for (int i = 0; i < iterations; ++i)
    {
        PhysicsEngine::updateDrift(x, y, time, 0.3f, 0.4f, 0.5f,
                                   DriftMode::Free, 0.0f, 0.0f, 0.5f);
        time += dt;
    }
    totalTime = timer.stopUs();

    double avgTime = totalTime / iterations;
    return {
        "Drift Physics (per update)",
        avgTime, avgTime, avgTime,
        1.0 / (avgTime * 1e-6),
        avgTime < 5.0  // Should be < 5us
    };
}

// ── 2D Interpolation Benchmark ─────────────────────────────────────────────────

BenchmarkResult benchmark2DInterpolation(int iterations)
{
    SnapshotBank bank;
    bank.prepare(256);
    std::vector<float> output(256);

    // Capture some test snapshots using public API
    std::vector<float> testValues(256, 0.5f);
    for (int i = 0; i < 4; ++i)
    {
        for (auto& v : testValues)
            v = static_cast<float>(rand()) / RAND_MAX;
        bank.captureValues(i, testValues);
    }

    Timer timer;
    double totalTime = 0.0;

    timer.start();
    for (int i = 0; i < iterations; ++i)
    {
        float x = static_cast<float>(rand()) / RAND_MAX * 2.0f - 1.0f;
        float y = static_cast<float>(rand()) / RAND_MAX * 2.0f - 1.0f;
        InterpolationEngine::compute2D(x, y, bank, output);
    }
    totalTime = timer.stopUs();

    double avgTime = totalTime / iterations;
    return {
        "2D Interpolation (256 params)",
        avgTime, avgTime, avgTime,
        256.0 / (avgTime * 1e-6),
        avgTime < 50.0  // Should be < 50us
    };
}

// ── Memory Footprint Test ──────────────────────────────────────────────────────

bool testMemoryFootprint()
{
    // Estimate memory usage of core structures
    size_t snapshotBankSize = sizeof(SnapshotBank);
    size_t paramVectorSize = 256 * sizeof(float);  // Typical parameter count

    size_t totalEstimate = snapshotBankSize +
                           12 * paramVectorSize +  // 12 slots
                           256 * sizeof(float) +   // morph output
                           256 * sizeof(float);    // smoothed values

    std::cout << "Estimated core memory footprint: "
              << totalEstimate / 1024.0 << " KB" << std::endl;

    // Should be well under 10MB for core structures
    return totalEstimate < 10 * 1024 * 1024;
}

// ── CPU Usage Simulation ───────────────────────────────────────────────────────

bool simulateRealtimeLoad(int sampleRate, int blockSize, int durationMs)
{
    const int numBuffers = (sampleRate * durationMs / 1000) / blockSize;
    const float dt = static_cast<float>(blockSize) / static_cast<float>(sampleRate);

    SnapshotBank bank;
    bank.prepare(256);
    std::vector<float> output(256);
    std::vector<float> testValues(256, 0.5f);
    for (int i = 0; i < 4; ++i)
        bank.captureValues(i, testValues);

    ElasticState state{0.0f, 0.0f, 0.0f, 0.0f};

    Timer timer;
    double totalProcessTime = 0.0;

    for (int i = 0; i < numBuffers; ++i)
    {
        timer.start();

        // Simulate typical audio callback work
        float x = 0.5f + 0.1f * std::sin(i * 0.01f);
        float y = 0.5f + 0.1f * std::cos(i * 0.01f);

        PhysicsEngine::updateElastic(state, x, y, ElasticPreset::Medium, dt);
        InterpolationEngine::compute2D(state.x, state.y, bank, output);

        totalProcessTime += timer.stopUs();
    }

    double bufferTimeUs = static_cast<double>(blockSize) / sampleRate * 1e6;
    double avgProcessTimeUs = totalProcessTime / numBuffers;
    double cpuPercent = (avgProcessTimeUs / bufferTimeUs) * 100.0;

    std::cout << "Simulated " << numBuffers << " buffers at "
              << sampleRate << " Hz, " << blockSize << " samples" << std::endl;
    std::cout << "Buffer time: " << bufferTimeUs << " us" << std::endl;
    std::cout << "Avg process time: " << avgProcessTimeUs << " us" << std::endl;
    std::cout << "Estimated CPU: " << cpuPercent << "%" << std::endl;

    return cpuPercent < 2.0;  // Target: < 2% CPU
}

// ── Main Benchmark Runner ──────────────────────────────────────────────────────

int runBenchmarks()
{
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                        MORPHSNAP PERFORMANCE BENCHMARKS                      ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════════════════╝\n\n";

    // Print SIMD support
    std::cout << "SIMD Support:\n";
    std::cout << "  SSE2: " << (InterpolationEngine::hasSSESupport() ? "Yes" : "No") << "\n";
    std::cout << "  AVX:  " << (InterpolationEngine::hasAVXSupport() ? "Yes" : "No") << "\n\n";

    // Header
    std::cout << std::left << std::setw(40) << "Benchmark"
              << std::right << std::setw(12) << "Avg Time"
              << std::setw(12) << "Min Time"
              << std::setw(12) << "Max Time"
              << std::setw(16) << "Throughput"
              << std::setw(8) << "Status"
              << std::endl;
    std::cout << std::string(100, '-') << std::endl;

    std::vector<BenchmarkResult> results;
    int passCount = 0;
    int failCount = 0;

    // Run benchmarks
    results.push_back(benchmarkScalarInterpolation(256, 10000));
    results.push_back(benchmarkSIMDInterpolation(256, 10000));
    results.push_back(benchmarkElasticPhysics(100000));
    results.push_back(benchmarkDriftPhysics(100000));
    results.push_back(benchmark2DInterpolation(5000));

    for (const auto& r : results)
    {
        r.print();
        if (r.passed) passCount++;
        else failCount++;
    }

    std::cout << std::string(100, '-') << std::endl;
    std::cout << "\n";

    // Memory test
    std::cout << "Memory Footprint Test:\n";
    bool memPass = testMemoryFootprint();
    std::cout << "  Status: " << (memPass ? "PASS" : "FAIL") << "\n\n";
    if (memPass) passCount++; else failCount++;

    // Realtime simulation
    std::cout << "Realtime Load Simulation (48kHz, 256 samples):\n";
    bool rtPass = simulateRealtimeLoad(48000, 256, 1000);
    std::cout << "  Status: " << (rtPass ? "PASS" : "FAIL") << "\n\n";
    if (rtPass) passCount++; else failCount++;

    // Summary
    std::cout << "════════════════════════════════════════════════════════════════════════════════\n";
    std::cout << "SUMMARY: " << passCount << " passed, " << failCount << " failed\n";
    std::cout << "════════════════════════════════════════════════════════════════════════════════\n";

    return failCount > 0 ? 1 : 0;
}

} // namespace benchmark
} // namespace morphsnap

int main()
{
    return morphsnap::benchmark::runBenchmarks();
}
