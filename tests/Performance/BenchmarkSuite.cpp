/*
 * More-Phi — Performance Benchmark Suite
 * Measures CPU usage, memory allocations, and throughput.
 *
 * Build with: cmake -DCMAKE_BUILD_TYPE=Release -DMORE_PHI_BUILD_BENCHMARKS=ON
 * Run with: ./MorePhiBenchmarks
 *
 * NOTE: These benchmarks measure core math subsystems only
 * (InterpolationEngine, PhysicsEngine, MorphProcessor, NeuralMasteringController).
 * They do NOT measure full-plugin processBlock() cost. DAW-hosted
 * CPU profiling is a separate release gate.
 */
#include <iostream>
#include <chrono>
#include <vector>
#include <cmath>
#include <atomic>
#include <thread>
#include <iomanip>
#include <cstring>
#include <limits>
#include <algorithm>
#include <numeric>
#include <random>

// Platform-specific runtime memory measurement
#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <psapi.h>
#elif defined(__APPLE__) || defined(__linux__)
    #include <sys/resource.h>
    #include <sys/time.h>
#endif

// Include the components to benchmark
#include "Core/InterpolationEngine.h"
#include "Core/SnapshotBank.h"
#include "Core/PhysicsEngine.h"
#include "Core/MorphProcessor.h"
#include "AI/Dataset/NeuralMasteringFeatureExtractor.h"
#include "AI/NeuralMasteringController.h"

namespace more_phi {
namespace benchmark {

// ── Timing Utilities ──────────────────────────────────────────────────────────

class Timer
{
public:
    void start()
    {
        // Full memory fence to reduce out-of-order effects around the timed region.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        start_ = std::chrono::high_resolution_clock::now();
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    double stopMs()
    {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        auto end = std::chrono::high_resolution_clock::now();
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return std::chrono::duration<double, std::milli>(end - start_).count();
    }

    double stopUs()
    {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        auto end = std::chrono::high_resolution_clock::now();
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return std::chrono::duration<double, std::micro>(end - start_).count();
    }

private:
    std::chrono::high_resolution_clock::time_point start_;
};

// ── Percentile Helpers ────────────────────────────────────────────────────────

static double percentile(std::vector<double>& sorted, double p)
{
    if (sorted.empty()) return 0.0;
    double rank = p / 100.0 * static_cast<double>(sorted.size() - 1);
    size_t lo = static_cast<size_t>(std::floor(rank));
    size_t hi = static_cast<size_t>(std::ceil(rank));
    if (lo == hi) return sorted[lo];
    double frac = rank - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

// ── Benchmark Results ──────────────────────────────────────────────────────────

struct BenchmarkResult
{
    std::string name;
    double avgTimeUs;
    double p50TimeUs;
    double p95TimeUs;
    double p99TimeUs;
    double throughput;  // items/sec
    bool passed;
    int warmupIterations;
    int measuredIterations;

    void print() const
    {
        std::cout << std::left << std::setw(45) << name
                  << std::right << std::setw(10) << std::fixed << std::setprecision(3) << avgTimeUs << " us"
                  << std::setw(10) << p50TimeUs << " us"
                  << std::setw(10) << p95TimeUs << " us"
                  << std::setw(10) << p99TimeUs << " us"
                  << std::setw(14) << std::scientific << std::setprecision(2) << throughput << "/s"
                  << std::setw(8) << (passed ? "PASS" : "FAIL")
                  << std::endl;
    }
};

// ── Benchmark Helpers ─────────────────────────────────────────────────────────

static BenchmarkResult buildResult(const std::string& name,
                                   std::vector<double>& timings,
                                   double throughputMultiplier,
                                   double passThresholdUs,
                                   int warmup)
{
    std::sort(timings.begin(), timings.end());
    double total = std::accumulate(timings.begin(), timings.end(), 0.0);
    double avg = total / static_cast<double>(timings.size());

    return {
        name,
        avg,
        percentile(timings, 50.0),
        percentile(timings, 95.0),
        percentile(timings, 99.0),
        throughputMultiplier / (avg * 1e-6),
        avg < passThresholdUs,
        warmup,
        static_cast<int>(timings.size())
    };
}

// ── Interpolation Benchmarks ───────────────────────────────────────────────────

BenchmarkResult benchmarkScalarInterpolation(size_t count, int iterations, int warmup)
{
    std::vector<float> srcA(count), srcB(count), dest(count);

    for (size_t i = 0; i < count; ++i)
    {
        srcA[i] = static_cast<float>(i) * 0.01f;
        srcB[i] = static_cast<float>(count - i) * 0.01f;
    }

    Timer timer;

    // Warmup: run without recording
    for (int i = 0; i < warmup; ++i)
    {
        const float t = 0.5f;
        #if defined(_MSC_VER)
            #pragma loop(no_vectorize)
        #endif
        for (size_t p = 0; p < count; ++p)
            dest[p] = srcA[p] * (1.0f - t) + srcB[p] * t;
    }

    // Measured iterations
    std::vector<double> timings;
    timings.reserve(static_cast<size_t>(iterations));

    for (int i = 0; i < iterations; ++i)
    {
        timer.start();
        const float t = 0.5f;
        #if defined(_MSC_VER)
            #pragma loop(no_vectorize)
        #endif
        for (size_t p = 0; p < count; ++p)
            dest[p] = srcA[p] * (1.0f - t) + srcB[p] * t;
        timings.push_back(timer.stopUs());
    }

    return buildResult(
        "Scalar Interpolation (" + std::to_string(count) + " params)",
        timings, static_cast<double>(count), 100.0, warmup);
}

BenchmarkResult benchmarkSIMDInterpolation(size_t count, int iterations, int warmup)
{
    std::vector<float> srcA(count), srcB(count), dest(count);

    for (size_t i = 0; i < count; ++i)
    {
        srcA[i] = static_cast<float>(i) * 0.01f;
        srcB[i] = static_cast<float>(count - i) * 0.01f;
    }

    Timer timer;

    for (int i = 0; i < warmup; ++i)
        InterpolationEngine::interpolateBatch_SIMD(
            srcA.data(), srcB.data(), dest.data(), 0.5f, count);

    std::vector<double> timings;
    timings.reserve(static_cast<size_t>(iterations));

    for (int i = 0; i < iterations; ++i)
    {
        timer.start();
        InterpolationEngine::interpolateBatch_SIMD(
            srcA.data(), srcB.data(), dest.data(), 0.5f, count);
        timings.push_back(timer.stopUs());
    }

    bool hasSimd = InterpolationEngine::hasSSESupport() || InterpolationEngine::hasAVXSupport();

    return buildResult(
        "SIMD Interpolation (" + std::to_string(count) + " params)",
        timings, static_cast<double>(count),
        hasSimd ? 30.0 : 100.0, warmup);
}

// ── Physics Benchmarks ─────────────────────────────────────────────────────────

BenchmarkResult benchmarkElasticPhysics(int iterations, int warmup)
{
    ElasticState state{0.0f, 0.0f, 0.0f, 0.0f};
    float targetX = 0.5f, targetY = 0.5f;
    float dt = 256.0f / 48000.0f;

    Timer timer;

    for (int i = 0; i < warmup; ++i)
        PhysicsEngine::updateElastic(state, targetX, targetY,
                                      ElasticPreset::Medium, dt);

    std::vector<double> timings;
    timings.reserve(static_cast<size_t>(iterations));

    for (int i = 0; i < iterations; ++i)
    {
        timer.start();
        PhysicsEngine::updateElastic(state, targetX, targetY,
                                      ElasticPreset::Medium, dt);
        timings.push_back(timer.stopUs());
    }

    return buildResult("Elastic Physics (per update)",
                       timings, 1.0, 1.0, warmup);
}

BenchmarkResult benchmarkDriftPhysics(int iterations, int warmup)
{
    float x = 0.0f, y = 0.0f;
    float time = 0.0f;
    float dt = 256.0f / 48000.0f;

    Timer timer;

    for (int i = 0; i < warmup; ++i)
    {
        PhysicsEngine::updateDrift(x, y, time, 0.3f, 0.4f, 0.5f,
                                   DriftMode::Free, 0.0f, 0.0f, 0.5f);
        time += dt;
    }

    std::vector<double> timings;
    timings.reserve(static_cast<size_t>(iterations));

    for (int i = 0; i < iterations; ++i)
    {
        timer.start();
        PhysicsEngine::updateDrift(x, y, time, 0.3f, 0.4f, 0.5f,
                                   DriftMode::Free, 0.0f, 0.0f, 0.5f);
        time += dt;
        timings.push_back(timer.stopUs());
    }

    return buildResult("Drift Physics (per update)",
                       timings, 1.0, 5.0, warmup);
}

// ── 2D Interpolation Benchmark ─────────────────────────────────────────────────

BenchmarkResult benchmark2DInterpolation(int iterations, int warmup)
{
    SnapshotBank bank;
    bank.prepare(256);
    std::vector<float> output(256);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    std::uniform_real_distribution<float> dist11(-1.0f, 1.0f);

    std::vector<float> testValues(256, 0.5f);
    for (int i = 0; i < 4; ++i)
    {
        for (auto& v : testValues)
            v = dist01(gen);
        bank.captureValues(i, testValues);
    }

    // Pre-generate random cursor positions outside the timed loop
    std::vector<float> randX(static_cast<size_t>(warmup + iterations));
    std::vector<float> randY(static_cast<size_t>(warmup + iterations));
    for (size_t i = 0; i < randX.size(); ++i)
    {
        randX[i] = dist11(gen);
        randY[i] = dist11(gen);
    }

    Timer timer;

    for (int i = 0; i < warmup; ++i)
        InterpolationEngine::compute2D(randX[static_cast<size_t>(i)],
                                        randY[static_cast<size_t>(i)],
                                        bank, output);

    std::vector<double> timings;
    timings.reserve(static_cast<size_t>(iterations));

    for (int i = 0; i < iterations; ++i)
    {
        size_t idx = static_cast<size_t>(warmup + i);
        timer.start();
        InterpolationEngine::compute2D(randX[idx], randY[idx], bank, output);
        timings.push_back(timer.stopUs());
    }

    return buildResult("2D Interpolation (256 params)",
                       timings, 256.0, 50.0, warmup);
}

BenchmarkResult benchmarkNeuralMasteringController(int iterations, int warmup)
{
    NeuralMasteringFeatureExtractor extractor;
    auto extracted = extractor.extractFromSummary(48000.0, 2, 512, 1000, -14.0f, -1.0f, 0.5f);

    NeuralMasteringRuntimeState runtime;
    runtime.currentFrame = 1000;
    runtime.sampleRate = 48000.0;
    runtime.channelCount = 2;
    runtime.layout = NeuralMasteringLayout::Stereo;

    NeuralMasteringController controller;

    Timer timer;

    for (int i = 0; i < warmup; ++i)
        (void)controller.processFeatureFrame(extracted.frame, runtime, false);

    std::vector<double> timings;
    timings.reserve(static_cast<size_t>(iterations));

    for (int i = 0; i < iterations; ++i)
    {
        timer.start();
        auto status = controller.processFeatureFrame(extracted.frame, runtime, false);
        double elapsed = timer.stopUs();
        timings.push_back(elapsed);
        if (!status.validationAccepted)
            timings.back() = std::numeric_limits<double>::max();
    }

    auto result = buildResult(
        "Neural mastering controller fallback path (outside callback)",
        timings, 1.0, 100.0, warmup);

    // Additional pass criterion: p99 < 1000 us
    if (result.p99TimeUs >= 1000.0)
        result.passed = false;

    return result;
}

// ── Memory Footprint Test ──────────────────────────────────────────────────────

struct MemoryReading
{
    size_t workingSetBytes = 0;
    size_t privateBytes = 0;
    bool supported = false;
};

static MemoryReading getCurrentMemory()
{
    MemoryReading reading;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
    {
        reading.workingSetBytes = pmc.WorkingSetSize;
        reading.privateBytes = pmc.PagefileUsage;
        reading.supported = true;
    }
#elif defined(__APPLE__) || defined(__linux__)
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0)
    {
        // ru_maxrss is in kilobytes on Linux, bytes on macOS
#if defined(__APPLE__)
        reading.workingSetBytes = static_cast<size_t>(usage.ru_maxrss);
#else
        reading.workingSetBytes = static_cast<size_t>(usage.ru_maxrss) * 1024;
#endif
        reading.supported = true;
    }
#endif
    return reading;
}

static size_t getMemoryDeltaBytes()
{
    auto before = getCurrentMemory();
    if (!before.supported)
        return 0;

    // Force a few allocations/deallocations to get a coarse working-set delta.
    // This is intentionally approximate; OS page accounting has noise.
    constexpr size_t scratchBytes = 1024 * 1024;
    auto scratch = std::make_unique<char[]>(scratchBytes);
    std::memset(scratch.get(), 0, scratchBytes);

    auto after = getCurrentMemory();
    if (!after.supported)
        return 0;

    return (after.workingSetBytes > before.workingSetBytes)
               ? (after.workingSetBytes - before.workingSetBytes)
               : 0;
}

bool testMemoryFootprint()
{
    // sizeof-based estimate of core structures.
    // This is NOT a runtime heap measurement — it sums compile-time sizes
    // of known structures and their heap-allocated backing stores.

    size_t snapshotBankInline = sizeof(SnapshotBank);

    // Heap-allocated slot array: 12 slots × ParameterState (2048 floats + metadata)
    constexpr int numSlots = 12;
    size_t slotHeapSize = numSlots * sizeof(ParameterState);

    // Morph output and smoothing buffers use MAX_PARAMETERS (2048)
    size_t morphOutput = MAX_PARAMETERS * sizeof(float);
    size_t smoothedValues = MAX_PARAMETERS * sizeof(float);

    size_t totalEstimate = snapshotBankInline + slotHeapSize
                         + morphOutput + smoothedValues;

    std::cout << "sizeof-based core memory estimate (not a runtime measurement):" << std::endl;
    std::cout << "  sizeof(SnapshotBank) inline:  "
              << std::fixed << std::setprecision(1)
              << snapshotBankInline / 1024.0 << " KB" << std::endl;
    std::cout << "  Heap slot array (12 slots):    "
              << slotHeapSize / 1024.0 << " KB" << std::endl;
    std::cout << "  Morph + smoothing buffers:     "
              << (morphOutput + smoothedValues) / 1024.0 << " KB" << std::endl;
    std::cout << "  Total estimate:                "
              << totalEstimate / 1024.0 << " KB" << std::endl;
    std::cout << "  NOTE: Excludes JUCE overhead, hosted plugin, MCP, "
              << "modulation, audio-domain engines." << std::endl;

    auto runtime = getCurrentMemory();
    if (runtime.supported)
    {
        std::cout << "  Current process working set:   "
                  << std::fixed << std::setprecision(1)
                  << runtime.workingSetBytes / 1024.0 << " KB"
                  << " (includes benchmark executable, JUCE, etc.)" << std::endl;
    }
    else
    {
        std::cout << "  Runtime working-set measurement not supported on this platform." << std::endl;
    }

    // Should be well under 10MB for core structures
    return totalEstimate < 10 * 1024 * 1024;
}

// ── CPU Usage Simulation ───────────────────────────────────────────────────────

bool simulateCoreRealtimeLoad(int sampleRate, int blockSize, int durationMs)
{
    const int numBuffers = static_cast<int>(std::ceil((static_cast<double>(sampleRate) * durationMs / 1000.0) / blockSize));
    const float dt = static_cast<float>(blockSize) / static_cast<float>(sampleRate);

    SnapshotBank bank;
    bank.prepare(256);
    MorphProcessor morphProcessor(bank);
    morphProcessor.prepare(256);

    std::vector<float> output(256);
    std::vector<float> testValues(256, 0.5f);
    for (int i = 0; i < 4; ++i)
        bank.captureValues(i, testValues);

    // Warmup: 10% of buffers
    int warmupBuffers = numBuffers / 10;
    for (int i = 0; i < warmupBuffers; ++i)
    {
        float x = 0.5f + 0.1f * std::sin(i * 0.01f);
        float y = 0.5f + 0.1f * std::cos(i * 0.01f);
        morphProcessor.process(x, y, 0.5f, MorphSource::XYPad, MorphMode::Elastic, dt, output);
    }

    Timer timer;
    double totalProcessTime = 0.0;

    for (int i = 0; i < numBuffers; ++i)
    {
        timer.start();

        float x = 0.5f + 0.1f * std::sin((warmupBuffers + i) * 0.01f);
        float y = 0.5f + 0.1f * std::cos((warmupBuffers + i) * 0.01f);

        morphProcessor.process(x, y, 0.5f, MorphSource::XYPad, MorphMode::Elastic, dt, output);

        totalProcessTime += timer.stopUs();
    }

    double bufferTimeUs = static_cast<double>(blockSize) / sampleRate * 1e6;
    double avgProcessTimeUs = totalProcessTime / numBuffers;
    double cpuPercent = (avgProcessTimeUs / bufferTimeUs) * 100.0;

    std::cout << "Simulated " << numBuffers << " buffers at "
              << sampleRate << " Hz, " << blockSize << " samples" << std::endl;
    std::cout << std::defaultfloat << std::setprecision(6);
    std::cout << "Buffer time: " << bufferTimeUs << " us" << std::endl;
    std::cout << "Avg process time: " << avgProcessTimeUs << " us" << std::endl;
    std::cout << "Core math CPU: " << cpuPercent << "%" << std::endl;
    std::cout << "  NOTE: Measures MorphProcessor (physics + interpolation + smoothing + trail)." << std::endl;
    std::cout << "  Does NOT include LockFreeQueue drain, MIDIRouter, ParameterBridge, hosted plugin, " << std::endl;
    std::cout << "  or concurrent UI/MCP writes. Full-plugin CPU requires DAW-hosted profiling." << std::endl;

    return cpuPercent < 2.0;  // Target: < 2% for core math subsystem
}

// ── Main Benchmark Runner ──────────────────────────────────────────────────────

int runBenchmarks()
{
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                        MORE-PHI PERFORMANCE BENCHMARKS                      ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════════════════╝\n\n";

    std::cout << "Scope: Core math subsystems only (not full-plugin processBlock).\n\n";

    // Print SIMD support. Distinguish compiled path from runtime CPU capabilities.
    std::cout << "SIMD Support:\n";
    std::cout << "  Compiled interpolation path: " << InterpolationEngine::getCompiledSIMDPath() << "\n";
    std::cout << "  Runtime CPU SSE2: " << (InterpolationEngine::hasSSESupport() ? "Yes" : "No") << "\n";
    std::cout << "  Runtime CPU AVX/AVX2: " << (InterpolationEngine::hasAVXSupport() ? "Yes" : "No") << "\n";
    std::cout << "  NOTE: 'Scalar' benchmark uses #pragma loop(no_vectorize) on MSVC to inhibit auto-vectorization.\n\n";

    // Header
    std::cout << std::left << std::setw(45) << "Benchmark"
              << std::right << std::setw(10) << "Avg"
              << std::setw(12) << "p50"
              << std::setw(12) << "p95"
              << std::setw(12) << "p99"
              << std::setw(14) << "Throughput"
              << std::setw(8) << "Status"
              << std::endl;
    std::cout << std::string(113, '-') << std::endl;

    std::vector<BenchmarkResult> results;
    int passCount = 0;
    int failCount = 0;

    // Run benchmarks (iterations, warmup)
    results.push_back(benchmarkScalarInterpolation(256, 10000, 1000));
    results.push_back(benchmarkSIMDInterpolation(256, 10000, 1000));
    results.push_back(benchmarkElasticPhysics(100000, 5000));
    results.push_back(benchmarkDriftPhysics(100000, 5000));
    results.push_back(benchmark2DInterpolation(5000, 500));
    results.push_back(benchmarkNeuralMasteringController(5000, 500));

    for (const auto& r : results)
    {
        r.print();
        if (r.passed) passCount++;
        else failCount++;
    }

    std::cout << std::string(113, '-') << std::endl;
    std::cout << "\n";

    // Memory test
    std::cout << "Memory Footprint Test:\n";
    bool memPass = testMemoryFootprint();
    std::cout << "  Status: " << (memPass ? "PASS" : "FAIL") << "\n\n";
    if (memPass) passCount++; else failCount++;

    // Core math realtime simulation — standard configuration
    std::cout << "Core Math RT Load Simulation (48kHz, 256 samples):\n";
    bool rtPass1 = simulateCoreRealtimeLoad(48000, 256, 1000);
    std::cout << "  Status: " << (rtPass1 ? "PASS" : "FAIL") << "\n\n";
    if (rtPass1) passCount++; else failCount++;

    // Acceptance-criteria-matching configuration (most demanding realistic config)
    std::cout << "Core Math RT Load Simulation (44.1kHz, 64 samples — acceptance criteria):\n";
    bool rtPass2 = simulateCoreRealtimeLoad(44100, 64, 1000);
    std::cout << "  Status: " << (rtPass2 ? "PASS" : "FAIL") << "\n\n";
    if (rtPass2) passCount++; else failCount++;

    // Summary
    std::cout << "════════════════════════════════════════════════════════════════════════════════\n";
    std::cout << "SUMMARY: " << passCount << " passed, " << failCount << " failed\n";
    std::cout << "════════════════════════════════════════════════════════════════════════════════\n";

    return failCount > 0 ? 1 : 0;
}

} // namespace benchmark
} // namespace more_phi

int main()
{
    return more_phi::benchmark::runBenchmarks();
}
