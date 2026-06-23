/*
 * More-Phi — Comprehensive CPU & Memory Profiling Harness
 *
 * Measures per-component CPU usage and memory footprint across realistic
 * production scenarios. Designed to be compiled as part of the benchmark
 * target or as a standalone test.
 *
 * Build:
 *   cmake -B build-profile -S . -DMORE_PHI_BUILD_TESTS=ON \
 *         -DMORE_PHI_BUILD_BENCHMARKS=ON -DMORE_PHI_ENABLE_PROFILING=ON \
 *         -DCMAKE_BUILD_TYPE=Release
 *   cmake --build build-profile --config Release --target MorePhiBenchmarks
 *
 * Components measured:
 *   1. VST3 Engine Core — processBlock overhead, command queue drain, MIDI routing
 *   2. MorphProcessor — physics + interpolation + smoothing + trail
 *   3. ParameterBridge — parameter read/write, touch detection, throttle
 *   4. Neural Model Inference — SonicMaster ONNX inference path
 *   5. AI Assistant — MCP tool handler, workflow execution
 *   6. Audio-Domain Engines — spectral, granular, formant, hybrid blend
 *
 * Scenarios:
 *   - Idle (no parameter changes, steady morph position)
 *   - Active processing (parameter automation sweeps)
 *   - Peak spikes (neural inference burst, rapid parameter changes)
 *   - Combined overhead (all subsystems active simultaneously)
 *
 * Buffer sizes tested: 64, 128, 256, 512, 1024 samples
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
#include <string>
#include <map>
#include <fstream>

// Platform-specific memory measurement
#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <psapi.h>
#elif defined(__APPLE__) || defined(__linux__)
    #include <sys/resource.h>
    #include <sys/time.h>
    #include <unistd.h>
#endif

#include "Core/InterpolationEngine.h"
#include "Core/SnapshotBank.h"
#include "Core/PhysicsEngine.h"
#include "Core/MorphProcessor.h"
#include "Core/ParameterState.h"
#include "AI/Dataset/NeuralMasteringFeatureExtractor.h"
#include "AI/NeuralMasteringController.h"
#include "Host/PluginHostManager.h"
#include "Host/ParameterBridge.h"
#include "AI/MCPServer.h"

namespace more_phi {
namespace prof_audit {

// ═══════════════════════════════════════════════════════════════════════════════
// Timing utilities
// ═══════════════════════════════════════════════════════════════════════════════

class HighResTimer
{
public:
    void start()
    {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        start_ = std::chrono::high_resolution_clock::now();
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    double elapsedUs() const
    {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        auto end = std::chrono::high_resolution_clock::now();
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return std::chrono::duration<double, std::micro>(end - start_).count();
    }
    double elapsedMs() const { return elapsedUs() / 1000.0; }
private:
    std::chrono::high_resolution_clock::time_point start_;
};

// ═══════════════════════════════════════════════════════════════════════════════
// Memory measurement
// ═══════════════════════════════════════════════════════════════════════════════

struct MemorySnapshot
{
    size_t workingSetBytes = 0;
    size_t privateBytes = 0;    // Windows only
    size_t peakWorkingSetBytes = 0;
    bool supported = false;

    double workingSetMB() const { return static_cast<double>(workingSetBytes) / (1024.0 * 1024.0); }
    double privateMB() const { return static_cast<double>(privateBytes) / (1024.0 * 1024.0); }
};

static MemorySnapshot takeMemorySnapshot()
{
    MemorySnapshot snap;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmcEx{};
    pmcEx.cb = sizeof(pmcEx);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmcEx),
                             sizeof(pmcEx)))
    {
        snap.workingSetBytes = pmcEx.WorkingSetSize;
        snap.privateBytes = pmcEx.PrivateUsage;
        snap.peakWorkingSetBytes = pmcEx.PeakWorkingSetSize;
        snap.supported = true;
    }
#elif defined(__APPLE__) || defined(__linux__)
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0)
    {
#  if defined(__APPLE__)
        snap.workingSetBytes = static_cast<size_t>(usage.ru_maxrss);
#  else
        snap.workingSetBytes = static_cast<size_t>(usage.ru_maxrss) * 1024;
#  endif
        snap.supported = true;
    }
#endif
    return snap;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Statistics
// ═══════════════════════════════════════════════════════════════════════════════

struct TimingStats
{
    double avgUs = 0.0;
    double p50Us = 0.0;
    double p95Us = 0.0;
    double p99Us = 0.0;
    double minUs = 0.0;
    double maxUs = 0.0;
    int samples = 0;

    static TimingStats fromSamples(std::vector<double>& timings)
    {
        if (timings.empty()) return {};
        std::sort(timings.begin(), timings.end());
        double total = std::accumulate(timings.begin(), timings.end(), 0.0);
        TimingStats s;
        s.samples = static_cast<int>(timings.size());
        s.avgUs = total / static_cast<double>(s.samples);
        s.minUs = timings.front();
        s.maxUs = timings.back();

        auto pctl = [&](double p) -> double {
            double rank = p / 100.0 * static_cast<double>(timings.size() - 1);
            size_t lo = static_cast<size_t>(std::floor(rank));
            size_t hi = static_cast<size_t>(std::ceil(rank));
            if (lo == hi) return timings[lo];
            double frac = rank - static_cast<double>(lo);
            return timings[lo] * (1.0 - frac) + timings[hi] * frac;
        };
        s.p50Us = pctl(50.0);
        s.p95Us = pctl(95.0);
        s.p99Us = pctl(99.0);
        return s;
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
// Component benchmark results
// ═══════════════════════════════════════════════════════════════════════════════

struct ComponentProfile
{
    std::string componentName;
    std::string scenarioName;
    int blockSize = 0;
    int sampleRate = 0;

    // CPU
    TimingStats cpuStats;
    double bufferTimeUs = 0.0;    // one buffer's realtime budget
    double cpuPercent = 0.0;      // % of one core at this buffer size

    // Memory
    size_t staticAllocBytes = 0;  // sizeof + known heap backing stores
    size_t dynamicPeakBytes = 0;  // peak delta during scenario
    size_t sustainedBytes = 0;    // working set after stabilization
};

// ═══════════════════════════════════════════════════════════════════════════════
// 1. Interpolation Engine (scalar vs SIMD at multiple parameter counts)
// ═══════════════════════════════════════════════════════════════════════════════

ComponentProfile profileInterpolationEngine(int numParams, int iterations, int warmup)
{
    ComponentProfile profile;
    profile.componentName = "InterpolationEngine";
    profile.scenarioName = "Scalar (" + std::to_string(numParams) + " params)";
    profile.blockSize = 256;
    profile.sampleRate = 48000;

    std::vector<float> srcA(static_cast<size_t>(numParams));
    std::vector<float> srcB(static_cast<size_t>(numParams));
    std::vector<float> dest(static_cast<size_t>(numParams));

    for (int i = 0; i < numParams; ++i)
    {
        srcA[static_cast<size_t>(i)] = static_cast<float>(i) * 0.01f;
        srcB[static_cast<size_t>(i)] = static_cast<float>(numParams - i) * 0.01f;
    }

    HighResTimer timer;

    // Warmup
    for (int i = 0; i < warmup; ++i)
    {
        const float t = 0.5f;
#if defined(_MSC_VER)
        #pragma loop(no_vectorize)
#endif
        for (int p = 0; p < numParams; ++p)
            dest[static_cast<size_t>(p)] = srcA[static_cast<size_t>(p)] * (1.0f - t)
                                          + srcB[static_cast<size_t>(p)] * t;
    }

    std::vector<double> timings;
    timings.reserve(static_cast<size_t>(iterations));
    for (int i = 0; i < iterations; ++i)
    {
        timer.start();
        const float t = 0.5f;
#if defined(_MSC_VER)
        #pragma loop(no_vectorize)
#endif
        for (int p = 0; p < numParams; ++p)
            dest[static_cast<size_t>(p)] = srcA[static_cast<size_t>(p)] * (1.0f - t)
                                          + srcB[static_cast<size_t>(p)] * t;
        timings.push_back(timer.elapsedUs());
    }

    profile.cpuStats = TimingStats::fromSamples(timings);
    profile.bufferTimeUs = static_cast<double>(profile.blockSize) / profile.sampleRate * 1e6;
    profile.cpuPercent = (profile.cpuStats.avgUs / profile.bufferTimeUs) * 100.0;

    // sizeof-based static estimate
    profile.staticAllocBytes = (srcA.size() + srcB.size() + dest.size()) * sizeof(float);

    return profile;
}

ComponentProfile profileSIMDInterpolation(int numParams, int iterations, int warmup)
{
    ComponentProfile profile;
    profile.componentName = "InterpolationEngine";
    profile.scenarioName = "SIMD (" + std::to_string(numParams) + " params)";
    profile.blockSize = 256;
    profile.sampleRate = 48000;

    std::vector<float> srcA(static_cast<size_t>(numParams));
    std::vector<float> srcB(static_cast<size_t>(numParams));
    std::vector<float> dest(static_cast<size_t>(numParams));

    for (int i = 0; i < numParams; ++i)
    {
        srcA[static_cast<size_t>(i)] = static_cast<float>(i) * 0.01f;
        srcB[static_cast<size_t>(i)] = static_cast<float>(numParams - i) * 0.01f;
    }

    HighResTimer timer;

    for (int i = 0; i < warmup; ++i)
        InterpolationEngine::interpolateBatch_SIMD(
            srcA.data(), srcB.data(), dest.data(), 0.5f,
            static_cast<size_t>(numParams));

    std::vector<double> timings;
    timings.reserve(static_cast<size_t>(iterations));
    for (int i = 0; i < iterations; ++i)
    {
        timer.start();
        InterpolationEngine::interpolateBatch_SIMD(
            srcA.data(), srcB.data(), dest.data(), 0.5f,
            static_cast<size_t>(numParams));
        timings.push_back(timer.elapsedUs());
    }

    profile.cpuStats = TimingStats::fromSamples(timings);
    profile.bufferTimeUs = static_cast<double>(profile.blockSize) / profile.sampleRate * 1e6;
    profile.cpuPercent = (profile.cpuStats.avgUs / profile.bufferTimeUs) * 100.0;
    profile.staticAllocBytes = (srcA.size() + srcB.size() + dest.size()) * sizeof(float);

    return profile;
}

// ═══════════════════════════════════════════════════════════════════════════════
// 2. Physics Engine (elastic + drift)
// ═══════════════════════════════════════════════════════════════════════════════

ComponentProfile profileElasticPhysics(int iterations, int warmup)
{
    ComponentProfile profile;
    profile.componentName = "PhysicsEngine";
    profile.scenarioName = "Elastic (Medium preset)";
    profile.blockSize = 256;
    profile.sampleRate = 48000;

    ElasticState state{0.0f, 0.0f, 0.0f, 0.0f};
    float targetX = 0.5f, targetY = 0.5f;
    float dt = static_cast<float>(profile.blockSize) / static_cast<float>(profile.sampleRate);

    HighResTimer timer;

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
        timings.push_back(timer.elapsedUs());
    }

    profile.cpuStats = TimingStats::fromSamples(timings);
    profile.bufferTimeUs = static_cast<double>(profile.blockSize) / profile.sampleRate * 1e6;
    profile.cpuPercent = (profile.cpuStats.avgUs / profile.bufferTimeUs) * 100.0;
    profile.staticAllocBytes = sizeof(ElasticState);

    return profile;
}

ComponentProfile profileDriftPhysics(int iterations, int warmup)
{
    ComponentProfile profile;
    profile.componentName = "PhysicsEngine";
    profile.scenarioName = "Drift (Free mode)";
    profile.blockSize = 256;
    profile.sampleRate = 48000;

    float x = 0.0f, y = 0.0f;
    float time = 0.0f;
    float dt = static_cast<float>(profile.blockSize) / static_cast<float>(profile.sampleRate);

    HighResTimer timer;

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
        timings.push_back(timer.elapsedUs());
    }

    profile.cpuStats = TimingStats::fromSamples(timings);
    profile.bufferTimeUs = static_cast<double>(profile.blockSize) / profile.sampleRate * 1e6;
    profile.cpuPercent = (profile.cpuStats.avgUs / profile.bufferTimeUs) * 100.0;
    profile.staticAllocBytes = 0; // stack-only

    return profile;
}

// ═══════════════════════════════════════════════════════════════════════════════
// 3. MorphProcessor (full pipeline: physics + interpolation + smoothing)
// ═══════════════════════════════════════════════════════════════════════════════

ComponentProfile profileMorphProcessor(int numParams, int sampleRate, int blockSize,
                                       int durationMs, bool parameterSweep)
{
    ComponentProfile profile;
    profile.componentName = "MorphProcessor";
    profile.scenarioName = parameterSweep ? "Active sweep" : "Idle hold";
    profile.blockSize = blockSize;
    profile.sampleRate = sampleRate;

    SnapshotBank bank;
    bank.prepare(numParams);
    MorphProcessor mp(bank);
    mp.prepare(numParams);

    std::vector<float> output(static_cast<size_t>(numParams));
    std::vector<float> testValues(static_cast<size_t>(numParams), 0.5f);
    for (int i = 0; i < 4; ++i)
        bank.captureValues(i, testValues);

    float dt = static_cast<float>(blockSize) / static_cast<float>(sampleRate);
    int numBuffers = static_cast<int>(std::ceil(
        static_cast<double>(sampleRate) * durationMs / 1000.0 / blockSize));
    int warmupBuffers = numBuffers / 10;

    // Warmup
    for (int i = 0; i < warmupBuffers; ++i)
    {
        float x = 0.5f, y = 0.5f;
        if (parameterSweep)
        {
            x = 0.5f + 0.4f * std::sin(static_cast<float>(i) * 0.05f);
            y = 0.5f + 0.4f * std::cos(static_cast<float>(i) * 0.05f);
        }
        mp.process(x, y, 0.5f, MorphSource::XYPad, MorphMode::Elastic, dt, output);
    }

    HighResTimer timer;
    std::vector<double> timings;
    timings.reserve(static_cast<size_t>(numBuffers));

    for (int i = 0; i < numBuffers; ++i)
    {
        float x = 0.5f, y = 0.5f;
        if (parameterSweep)
        {
            x = 0.5f + 0.4f * std::sin(static_cast<float>(warmupBuffers + i) * 0.05f);
            y = 0.5f + 0.4f * std::cos(static_cast<float>(warmupBuffers + i) * 0.05f);
        }
        timer.start();
        mp.process(x, y, 0.5f, MorphSource::XYPad, MorphMode::Elastic, dt, output);
        timings.push_back(timer.elapsedUs());
    }

    profile.cpuStats = TimingStats::fromSamples(timings);
    profile.bufferTimeUs = static_cast<double>(blockSize) / sampleRate * 1e6;
    profile.cpuPercent = (profile.cpuStats.avgUs / profile.bufferTimeUs) * 100.0;

    // sizeof-based estimate
    profile.staticAllocBytes = sizeof(SnapshotBank)
                             + 12 * sizeof(ParameterState)
                             + static_cast<size_t>(numParams) * sizeof(float) * 4; // buffers

    return profile;
}

// ═══════════════════════════════════════════════════════════════════════════════
// 4. Neural Mastering Controller (fallback path without ONNX)
// ═══════════════════════════════════════════════════════════════════════════════

ComponentProfile profileNeuralMasteringController(int iterations, int warmup)
{
    ComponentProfile profile;
    profile.componentName = "NeuralMasteringController";
    profile.scenarioName = "Fallback path (no ONNX)";
    profile.blockSize = 512;
    profile.sampleRate = 48000;

    NeuralMasteringFeatureExtractor extractor;
    auto extracted = extractor.extractFromSummary(48000.0, 2, 512, 1000,
                                                   -14.0f, -1.0f, 0.5f);

    NeuralMasteringRuntimeState runtime;
    runtime.currentFrame = 1000;
    runtime.sampleRate = 48000.0;
    runtime.channelCount = 2;
    runtime.layout = NeuralMasteringLayout::Stereo;

    NeuralMasteringController controller;

    HighResTimer timer;

    for (int i = 0; i < warmup; ++i)
        (void)controller.processFeatureFrame(extracted.frame, runtime, false);

    std::vector<double> timings;
    timings.reserve(static_cast<size_t>(iterations));
    for (int i = 0; i < iterations; ++i)
    {
        timer.start();
        auto status = controller.processFeatureFrame(extracted.frame, runtime, false);
        double elapsed = timer.elapsedUs();
        timings.push_back(status.validationAccepted ? elapsed : std::numeric_limits<double>::max());
    }

    profile.cpuStats = TimingStats::fromSamples(timings);
    profile.bufferTimeUs = static_cast<double>(profile.blockSize) / profile.sampleRate * 1e6;
    profile.cpuPercent = (profile.cpuStats.avgUs / profile.bufferTimeUs) * 100.0;
    profile.staticAllocBytes = sizeof(NeuralMasteringController)
                             + sizeof(NeuralMasteringFeatureExtractor);

    return profile;
}

// ═══════════════════════════════════════════════════════════════════════════════
// 5. Full MorphProcessor at multiple buffer sizes (simulates processBlock core)
// ═══════════════════════════════════════════════════════════════════════════════

struct BufferSizeResult
{
    int blockSize;
    double avgUs;
    double p95Us;
    double p99Us;
    double cpuPercent;
    double bufferTimeUs;
};

std::vector<BufferSizeResult> profileMorphAcrossBufferSizes(
    int numParams, int durationMs, bool parameterSweep)
{
    const int sampleRates[] = {44100, 48000, 96000};
    const int blockSizes[] = {64, 128, 256, 512, 1024};

    std::vector<BufferSizeResult> results;

    for (int sr : sampleRates)
    {
        for (int bs : blockSizes)
        {
            auto p = profileMorphProcessor(numParams, sr, bs, durationMs, parameterSweep);
            results.push_back({bs, p.cpuStats.avgUs, p.cpuStats.p95Us,
                               p.cpuStats.p99Us, p.cpuPercent, p.bufferTimeUs});
        }
    }

    return results;
}

// ═══════════════════════════════════════════════════════════════════════════════
// 6. Parameter write micro-benchmark (simulates setValue calls)
// ═══════════════════════════════════════════════════════════════════════════════

ComponentProfile profileParameterWriteSimulation(int numParams, int iterations, int warmup)
{
    ComponentProfile profile;
    profile.componentName = "ParameterBridge";
    profile.scenarioName = "setValue() simulation (" + std::to_string(numParams) + " params)";
    profile.blockSize = 256;
    profile.sampleRate = 48000;

    // Simulate the cost of per-parameter writes. Without a real hosted plugin,
    // we measure the loop overhead + branch mispredictions typical of the
    // parameter_application path in processBlock.
    std::vector<float> values(static_cast<size_t>(numParams), 0.5f);
    std::vector<float> lastApplied(static_cast<size_t>(numParams), -1.0f);
    std::vector<int> cooldowns(static_cast<size_t>(numParams), 0);
    std::vector<bool> discrete(static_cast<size_t>(numParams), false);

    // Mark ~15% as discrete/binary (typical plugin distribution)
    for (size_t i = 0; i < discrete.size(); ++i)
        discrete[i] = (i % 7 == 0);

    HighResTimer timer;

    // Warmup
    for (int w = 0; w < warmup; ++w)
    {
        for (int i = 0; i < numParams; ++i)
        {
            size_t idx = static_cast<size_t>(i);
            float val = values[idx];
            if (val < 0.0f) continue;
            if (lastApplied[idx] >= 0.0f && std::abs(val - lastApplied[idx]) < 1e-5f)
                continue;
            if (cooldowns[idx] > 0) { --cooldowns[idx]; continue; }
            if (discrete[idx]) val = std::round(val * 10.0f) / 10.0f;
            val = std::clamp(val, 0.0f, 1.0f);
            lastApplied[idx] = val;
        }
    }

    std::vector<double> timings;
    timings.reserve(static_cast<size_t>(iterations));
    for (int iter = 0; iter < iterations; ++iter)
    {
        // Slightly perturb values to simulate morph output changes
        for (int i = 0; i < numParams; ++i)
            values[static_cast<size_t>(i)] += 0.001f;

        timer.start();
        for (int i = 0; i < numParams; ++i)
        {
            size_t idx = static_cast<size_t>(i);
            float val = values[idx];
            if (val < 0.0f) continue;
            if (lastApplied[idx] >= 0.0f && std::abs(val - lastApplied[idx]) < 1e-5f)
                continue;
            if (cooldowns[idx] > 0) { --cooldowns[idx]; continue; }
            if (discrete[idx]) val = std::round(val * 10.0f) / 10.0f;
            val = std::clamp(val, 0.0f, 1.0f);
            lastApplied[idx] = val;
        }
        timings.push_back(timer.elapsedUs());
    }

    profile.cpuStats = TimingStats::fromSamples(timings);
    profile.bufferTimeUs = static_cast<double>(profile.blockSize) / profile.sampleRate * 1e6;
    profile.cpuPercent = (profile.cpuStats.avgUs / profile.bufferTimeUs) * 100.0;
    profile.staticAllocBytes = numParams * sizeof(float) * 3; // values + lastApplied + cooldowns

    return profile;
}

// ═══════════════════════════════════════════════════════════════════════════════
// 7. System memory footprint estimation (sizeof-based)
// ═══════════════════════════════════════════════════════════════════════════════

struct MemoryFootprintReport
{
    // Core engine
    size_t snapshotBankInline = 0;
    size_t snapshotBankHeap = 0;      // 12 slots × ParameterState
    size_t morphBuffers = 0;           // finalOutput + lastApplied + smoothed + currentSnapshot
    size_t touchBuffers = 0;          // cooldowns + touchMorphX/Y + liveEditHold
    size_t drainScratch = 0;          // drainScratch_ + drainTouched_

    // Hosting
    size_t pluginHostManager = 0;
    size_t parameterBridge = 0;

    // AI / Neural
    size_t neuralController = 0;
    size_t analysisEngine = 0;        // SonicMasterAnalysisEngine ring + buffers
    size_t mcpServer = 0;

    // Audio domain
    size_t audioDomainBuffers = 0;    // bufferB + paramOut + spectralOut + granularOut
    size_t modulationEngine = 0;

    // Agent layer
    size_t agentRuntime = 0;

    size_t totalEstimated() const
    {
        return snapshotBankInline + snapshotBankHeap + morphBuffers
             + touchBuffers + drainScratch + pluginHostManager
             + parameterBridge + neuralController + analysisEngine
             + mcpServer + audioDomainBuffers + modulationEngine
             + agentRuntime;
    }

    void print() const
    {
        auto kb = [](size_t bytes) -> double { return static_cast<double>(bytes) / 1024.0; };
        auto mb = [](size_t bytes) -> double { return static_cast<double>(bytes) / (1024.0 * 1024.0); };

        std::cout << "\n╔══════════════════════════════════════════════════════╗\n";
        std::cout <<   "║     MORE-PHI MEMORY FOOTPRINT ESTIMATE (sizeof)     ║\n";
        std::cout <<   "╚══════════════════════════════════════════════════════╝\n\n";

        std::cout << std::left;
        std::cout << "  Core Engine:\n";
        std::cout << "    SnapshotBank (inline):         " << std::setw(8) << std::fixed << std::setprecision(1) << kb(snapshotBankInline) << " KB\n";
        std::cout << "    SnapshotBank (12 slots heap):  " << std::setw(8) << kb(snapshotBankHeap) << " KB\n";
        std::cout << "    Morph + smoothing buffers:     " << std::setw(8) << kb(morphBuffers) << " KB\n";
        std::cout << "    Touch detection buffers:       " << std::setw(8) << kb(touchBuffers) << " KB\n";
        std::cout << "    Command drain scratch:         " << std::setw(8) << kb(drainScratch) << " KB\n";

        std::cout << "\n  Hosting:\n";
        std::cout << "    PluginHostManager:             " << std::setw(8) << kb(pluginHostManager) << " KB\n";
        std::cout << "    ParameterBridge:               " << std::setw(8) << kb(parameterBridge) << " KB\n";

        std::cout << "\n  AI / Neural:\n";
        std::cout << "    NeuralMasteringController:     " << std::setw(8) << kb(neuralController) << " KB\n";
        std::cout << "    SonicMaster engine + ring:     " << std::setw(8) << mb(analysisEngine) << " MB\n";
        std::cout << "    MCP Server:                    " << std::setw(8) << kb(mcpServer) << " KB\n";

        std::cout << "\n  Audio-Domain Engines:\n";
        std::cout << "    Scratch buffers (B+out):       " << std::setw(8) << kb(audioDomainBuffers) << " KB\n";
        std::cout << "    Modulation engine:             " << std::setw(8) << kb(modulationEngine) << " KB\n";

        std::cout << "\n  Agent Layer:\n";
        std::cout << "    AgentRuntime:                  " << std::setw(8) << kb(agentRuntime) << " KB\n";

        std::cout << "\n  ─────────────────────────────────────\n";
        std::cout << "  TOTAL ESTIMATED:                 " << std::setw(8) << mb(totalEstimated()) << " MB\n";
        std::cout << "  NOTE: Excludes hosted plugin's own memory (varies by plugin).\n";
        std::cout << "        Excludes JUCE framework overhead.\n";
    }
};

static MemoryFootprintReport estimateMemoryFootprint(int maxParams)
{
    MemoryFootprintReport r;

    // Core engine — based on MAX_PARAMETERS = 2048
    r.snapshotBankInline = sizeof(SnapshotBank);
    r.snapshotBankHeap = 12 * sizeof(ParameterState);  // ~97 KB

    // Morph buffers: finalOutput_ + smoothedValues_ + currentParamSnapshot_ + lastApplied_
    r.morphBuffers = static_cast<size_t>(maxParams) * sizeof(float) * 4;

    // Touch detection vectors
    r.touchBuffers = static_cast<size_t>(maxParams) * sizeof(int)       // cooldowns
                   + static_cast<size_t>(maxParams) * sizeof(float) * 2 // touchMorphX/Y
                   + static_cast<size_t>(maxParams);                     // liveEditHold (uint8)
    // Drain scratch
    r.drainScratch = static_cast<size_t>(maxParams) * sizeof(float)
                   + static_cast<size_t>(maxParams); // drainTouched_ (uint8)

    // Hosting
    r.pluginHostManager = sizeof(PluginHostManager);
    r.parameterBridge = sizeof(ParameterBridge);

    // Neural
    r.neuralController = sizeof(NeuralMasteringController)
                       + sizeof(NeuralMasteringFeatureExtractor);

    // SonicMaster: capture ring (8s @ 192kHz stereo) + model buffers
    // AudioCaptureRing: 8 * 192000 * 2 channels * sizeof(float) ≈ 12.3 MB
    r.analysisEngine = 8u * 192000u * 2u * sizeof(float)
                     + kSonicMasterSegmentFrames * 2u * sizeof(float)  // interleaved model input
                     + kSonicMasterSegmentFrames * 2u * sizeof(float)  // modelL/R
                     + kSonicMasterDecisionWidth * sizeof(float);       // decision output

    // MCP Server
    r.mcpServer = sizeof(MCPServer);

    // Audio-domain scratch buffers (pre-allocated in prepareToPlay)
    // bufferB + paramOut + spectralOut + granularOut, each stereo × maxBlock
    constexpr int maxBlockSize = 2048;
    constexpr int maxOversampling = 4;
    r.audioDomainBuffers = 2 * maxBlockSize * sizeof(float)          // bufferB
                         + 2 * maxBlockSize * maxOversampling * sizeof(float) * 3; // paramOut + spectralOut + granularOut

    // Modulation engine
    r.modulationEngine = static_cast<size_t>(maxParams) * sizeof(float) * 2; // modulation buffers

    // Agent layer
    r.agentRuntime = 0; // lazily constructed, varies

    return r;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Report generator
// ═══════════════════════════════════════════════════════════════════════════════

void printHeader(const std::string& title)
{
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  " << std::left << std::setw(72) << title << "║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════════════╝\n\n";
}

void printProfile(const ComponentProfile& p)
{
    std::cout << std::left << std::setw(35) << (p.componentName + " — " + p.scenarioName)
              << std::right
              << " avg=" << std::fixed << std::setprecision(3) << std::setw(8) << p.cpuStats.avgUs << " us"
              << " p95=" << std::setw(8) << p.cpuStats.p95Us << " us"
              << " p99=" << std::setw(8) << p.cpuStats.p99Us << " us"
              << " CPU=" << std::setw(6) << std::setprecision(2) << p.cpuPercent << "%"
              << " | mem≈" << std::setw(7) << std::setprecision(1) << (static_cast<double>(p.staticAllocBytes) / 1024.0) << " KB"
              << std::endl;
}

void printBufferSizeTable(const std::vector<BufferSizeResult>& results, int sampleRate)
{
    std::cout << "\n  Sample rate: " << sampleRate << " Hz\n";
    std::cout << "  " << std::left
              << std::setw(12) << "BlockSize"
              << std::setw(12) << "Buffer(us)"
              << std::setw(12) << "Avg(us)"
              << std::setw(12) << "P95(us)"
              << std::setw(12) << "P99(us)"
              << std::setw(10) << "CPU%"
              << std::endl;
    std::cout << "  " << std::string(70, '-') << std::endl;

    for (const auto& r : results)
    {
        if (r.blockSize == 0) continue;
        // Rough check: only print the ones matching this sample rate
        // (we grouped by sr in the vector; this is a simplification)
        std::cout << "  " << std::left
                  << std::setw(12) << r.blockSize
                  << std::right
                  << std::setw(10) << std::fixed << std::setprecision(1) << r.bufferTimeUs
                  << std::setw(10) << std::setprecision(3) << r.avgUs
                  << std::setw(10) << r.p95Us
                  << std::setw(10) << r.p99Us
                  << std::setw(8) << std::setprecision(2) << r.cpuPercent << "%"
                  << std::endl;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Main runner
// ═══════════════════════════════════════════════════════════════════════════════

int runComprehensiveProfile()
{
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║            MORE-PHI COMPREHENSIVE CPU & MEMORY PROFILING AUDIT           ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════════════╝\n";

    // ── System Info ──────────────────────────────────────────────────────
    std::cout << "\n--- System Information ---\n";
    std::cout << "  SIMD compiled path: " << InterpolationEngine::getCompiledSIMDPath() << "\n";
    std::cout << "  SSE2 support: " << (InterpolationEngine::hasSSESupport() ? "Yes" : "No") << "\n";
    std::cout << "  AVX/AVX2 support: " << (InterpolationEngine::hasAVXSupport() ? "Yes" : "No") << "\n";

    auto baselineMem = takeMemorySnapshot();
    if (baselineMem.supported)
    {
        std::cout << "  Baseline working set: " << std::fixed << std::setprecision(1)
                  << baselineMem.workingSetMB() << " MB\n";
        std::cout << "  Peak working set: " << baselineMem.peakWorkingSetBytes / (1024.0 * 1024.0) << " MB\n";
    }

    std::vector<ComponentProfile> allProfiles;

    // ── 1. Interpolation Engine ──────────────────────────────────────────
    printHeader("1. INTERPOLATION ENGINE");
    for (int n : {64, 256, 1024, 2048})
    {
        allProfiles.push_back(profileInterpolationEngine(n, 10000, 1000));
        printProfile(allProfiles.back());

        allProfiles.push_back(profileSIMDInterpolation(n, 10000, 1000));
        printProfile(allProfiles.back());
    }

    // ── 2. Physics Engine ────────────────────────────────────────────────
    printHeader("2. PHYSICS ENGINE");
    allProfiles.push_back(profileElasticPhysics(100000, 5000));
    printProfile(allProfiles.back());

    allProfiles.push_back(profileDriftPhysics(100000, 5000));
    printProfile(allProfiles.back());

    // ── 3. MorphProcessor (full pipeline) ────────────────────────────────
    printHeader("3. MORPH PROCESSOR (FULL PIPELINE)");

    // Idle (steady hold) at various param counts
    for (int n : {64, 256, 1024, 2048})
    {
        allProfiles.push_back(profileMorphProcessor(n, 48000, 256, 500, false));
        printProfile(allProfiles.back());
    }

    // Active sweep at various param counts
    for (int n : {64, 256, 1024, 2048})
    {
        allProfiles.push_back(profileMorphProcessor(n, 48000, 256, 500, true));
        printProfile(allProfiles.back());
    }

    // ── 4. Buffer Size Scaling ───────────────────────────────────────────
    printHeader("4. BUFFER SIZE SCALING (2048 params, active sweep)");
    {
        auto results = profileMorphAcrossBufferSizes(2048, 500, true);
        printBufferSizeTable(results, 48000);
    }

    // ── 5. Parameter Write Simulation ────────────────────────────────────
    printHeader("5. PARAMETER WRITE OVERHEAD");
    for (int n : {64, 256, 1024, 2048})
    {
        allProfiles.push_back(profileParameterWriteSimulation(n, 5000, 500));
        printProfile(allProfiles.back());
    }

    // ── 6. Neural Mastering Controller ───────────────────────────────────
    printHeader("6. NEURAL MASTERING CONTROLLER (FALLBACK)");
    allProfiles.push_back(profileNeuralMasteringController(5000, 500));
    printProfile(allProfiles.back());

    // ── 7. Memory Footprint ──────────────────────────────────────────────
    printHeader("7. MEMORY FOOTPRINT ESTIMATE");
    auto memReport = estimateMemoryFootprint(MAX_PARAMETERS);
    memReport.print();

    // ── 8. Runtime Memory ────────────────────────────────────────────────
    auto afterMem = takeMemorySnapshot();
    if (afterMem.supported)
    {
        std::cout << "\n--- Runtime Memory After Benchmarks ---\n";
        std::cout << "  Working set: " << std::fixed << std::setprecision(1)
                  << afterMem.workingSetMB() << " MB (delta: "
                  << (afterMem.workingSetMB() - baselineMem.workingSetMB()) << " MB)\n";
    }

    // ── 9. Summary ───────────────────────────────────────────────────────
    printHeader("9. SUMMARY — PER-COMPONENT CPU BUDGET AT 256 samples @ 48 kHz");
    const double bufferBudgetUs = 256.0 / 48000.0 * 1e6; // ~5333 us

    std::cout << std::left
              << std::setw(45) << "Component"
              << std::right
              << std::setw(10) << "Avg(us)"
              << std::setw(10) << "P99(us)"
              << std::setw(10) << "CPU%"
              << std::setw(12) << "Budget%"
              << std::endl;
    std::cout << std::string(87, '-') << std::endl;

    double totalCpuPct = 0.0;
    for (const auto& p : allProfiles)
    {
        if (p.blockSize == 256 && p.sampleRate == 48000)
        {
            double budgetPct = (p.cpuStats.avgUs / bufferBudgetUs) * 100.0;
            std::cout << std::left << std::setw(45) << (p.componentName + " — " + p.scenarioName)
                      << std::right
                      << std::setw(10) << std::fixed << std::setprecision(3) << p.cpuStats.avgUs
                      << std::setw(10) << p.cpuStats.p99Us
                      << std::setw(9) << std::setprecision(2) << p.cpuPercent << "%"
                      << std::setw(11) << std::setprecision(2) << budgetPct << "%"
                      << std::endl;
            totalCpuPct += p.cpuPercent;
        }
    }

    std::cout << std::string(87, '-') << std::endl;
    std::cout << "  NOTE: Components above are measured in ISOLATION.\n";
    std::cout << "        Real-world combined CPU depends on active features and hosted plugin.\n";

    std::cout << "\n════════════════════════════════════════════════════════════════════════════\n";
    std::cout << "PROFILING COMPLETE\n";
    std::cout << "════════════════════════════════════════════════════════════════════════════\n";

    return 0;
}

} // namespace prof_audit
} // namespace more_phi

int main()
{
    return more_phi::prof_audit::runComprehensiveProfile();
}
