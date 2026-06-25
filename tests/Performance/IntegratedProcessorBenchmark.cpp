/*
 * More-Phi — Integrated Processor Benchmark (CPU & Memory Audit)
 *
 * Build:
 *   cmake -B build-audit -S . -DMORE_PHI_BUILD_TESTS=ON \
 *         -DMORE_PHI_BUILD_BENCHMARKS=ON -DMORE_PHI_ENABLE_PROFILING=ON \
 *         -DMORE_PHI_ENABLE_ONNX=ON -DCMAKE_BUILD_TYPE=Release
 *   cmake --build build-audit --config Release --target MorePhiBenchmarks
 *   build-audit/Release/MorePhiBenchmarks.exe [--passes 5] [--buffer 256,512,1024]
 *
 * Emits:
 *   - a fixed-width stdout table (matches BenchmarkSuite.cpp's style)
 *   - build-audit/profile-results.json (machine-readable, for the report)
 *
 * Scenarios (see plan / report):
 *   S1 Idle (no audio)        S5 Neural isolation (ORT loop)
 *   S2 Active, null plugin    S6 Neural in-context (runOneCycleForTest)
 *   S3 Automation (sweep)     S7 Combined (null plugin)
 *   S4 Automation (rapid)     S8 Combined + Ozone (optional)
 */
#include "IntegratedProcessorBenchmark.h"

#include "Plugin/PluginProcessor.h"
#include "AI/SonicMasterDecisionRunner.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32) && defined(_M_X64)
    // AUDIT-FIX (M1): FTZ/DAZ intrinsics for measurement-hygiene setup.
    #include <xmmintrin.h>
#endif

namespace more_phi::audit {

namespace {

// ─── CLI config ────────────────────────────────────────────────────────────
struct Config
{
    int passes              = 5;
    int blocksPerPass       = 1200;     // ~6.4s @ 48k/256 — crosses all analysis gates
    int inferenceIterations = 8;        // ORT is ~300ms/decision; keep small
    std::vector<int> bufferSizes = { 256, 512, 1024 };
    int  sampleRate       = 48000;
    bool runOzonePass     = true;       // S8 (auto-skips if Ozone absent)
    bool realtimePacing   = false;      // true = pace to wall-clock (faithful CPU%)
    std::string jsonPath  = "profile-results.json";
};

Config parseArgs(int argc, char** argv)
{
    Config c;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };

        if      (a == "--passes")         c.passes              = std::max(1, std::atoi(next().c_str()));
        else if (a == "--blocks")         c.blocksPerPass       = std::max(50, std::atoi(next().c_str()));
        else if (a == "--inference-iters") c.inferenceIterations = std::max(1, std::atoi(next().c_str()));
        else if (a == "--no-ozone")       c.runOzonePass        = false;
        else if (a == "--realtime")       c.realtimePacing      = true;
        else if (a == "--json")           c.jsonPath            = next();
        else if (a == "--buffers")
        {
            c.bufferSizes.clear();
            std::stringstream ss(next());
            std::string tok;
            while (std::getline(ss, tok, ',')) c.bufferSizes.push_back(std::atoi(tok.c_str()));
        }
        else if (a == "--help" || a == "-h")
        {
            std::cout << "MorePhiBenchmarks [--passes N] [--blocks N] [--inference-iters N]\n"
                         "                  [--buffers 256,512,1024] [--no-ozone] [--realtime]\n"
                         "                  [--json path]\n";
            std::exit(0);
        }
    }
    return c;
}

// ─── AUDIT-FIX (M1): measurement hygiene ───────────────────────────────────
// Flush denormals to zero (FTZ + DAZ) and pin the benchmark thread to a
// non-core-0 logical CPU. Without this, any engine that produces a denormal
// runs 10–100× slower on those blocks, contaminating every relative-timing
// claim; core migration and turbo variance add noise on top.
void prepareMeasurementHygiene()
{
#if defined(_WIN32) && defined(_M_X64)
    // FTZ (flush-to-zero) + DAZ (denormals-are-zero) via the MXCSR intrinsics.
    // These match what juce::ScopedNoDenormals sets per-block, but process-wide
    // so the whole audit (including the timing loop's own scalar state) is clean.
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);

    // Pin to logical CPU 2 (avoid core 0 — the OS schedules housekeeping there).
    // Raise priority so OS preemption noise is minimized. Both are advisory: a
    // noisy machine still produces noisy numbers, but the setup is now declared.
    SetThreadAffinityMask(GetCurrentThread(), reinterpret_cast<DWORD_PTR>(1) << 2);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif
}

// ─── APVTS automation helper (mirrors TestVST3ParameterAutomation.cpp) ─────
void setNormalizedWithGesture(juce::RangedAudioParameter& p, float v)
{
    p.beginChangeGesture();
    p.setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, v));
    p.endChangeGesture();
}

// Direct atomic write — no host gesture. Cheaper, still exercises the audio-
// thread morph-position read path. Used for per-block sweeps.
void setMorphPos(MorePhiProcessor& p, float x, float y, float fader)
{
    p.setMorphX(x);
    p.setMorphY(y);
    p.setFaderPos(fader);
}

// ─── Fills a stereo buffer with a continuous sine across blocks ────────────
void fillSineBlock(juce::AudioBuffer<float>& buf, int block, double sampleRate)
{
    const int n = buf.getNumSamples();
    constexpr double freq = 1000.0;
    for (int i = 0; i < n; ++i)
    {
        const auto idx = static_cast<double>(block * n + i);
        const float s = 0.2f * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * freq * idx / sampleRate));
        buf.setSample(0, i, s);
        buf.setSample(1, i, s * 0.8f);
    }
}

// ─── Per-section profiler report parser ────────────────────────────────────
// getProfilingReport() emits (per section):
//   <name>:
//     Calls:      <n>
//     Avg:        <us> µs
//     Max:        <us> µs
//     Total:      <us> µs
//     Percentage: <p>%
std::vector<ScenarioResult::SectionStat> parseSections(const juce::String& report)
{
    std::vector<ScenarioResult::SectionStat> out;
    if (report.isEmpty() || report.startsWith("No profiling data")) return out;

    auto extract = [&](const juce::String& block, const juce::String& key) -> double {
        const int k = block.indexOf(key);
        if (k < 0) return 0.0;
        const juce::String tail = block.substring(k + key.length()).trimStart();
        // first numeric run
        int end = 0;
        while (end < tail.length() && (tail[end] == '.' || tail[end] == '-' ||
               (tail[end] >= '0' && tail[end] <= '9'))) ++end;
        return tail.substring(0, end).getDoubleValue();
    };

    // Split on the leading-name lines: a line ending with ':' that is not indented.
    const juce::StringArray lines = juce::StringArray::fromLines(report);
    int i = 0;
    while (i < lines.size())
    {
        const juce::String ln = lines[i].trim();
        // Section headers are non-indented, end with ':', and the NEXT line starts with "Calls:".
        if (ln.endsWith(":") && !ln.startsWith("===") && !ln.startsWith("---") &&
            i + 1 < lines.size() && lines[i + 1].trim().startsWith("Calls:"))
        {
            const std::string name = ln.dropLastCharacters(1).trim().toStdString();
            // gather the block until the next header
            juce::String blockText = ln + "\n";
            int j = i + 1;
            while (j < lines.size())
            {
                const juce::String t = lines[j].trim();
                if (t.endsWith(":") && !t.startsWith("Calls") && !t.startsWith("Avg") &&
                    !t.startsWith("Max") && !t.startsWith("Total") && !t.startsWith("Percentage") &&
                    !t.startsWith("===") && !t.startsWith("---"))
                    break;
                blockText << lines[j] << "\n";
                ++j;
            }
            ScenarioResult::SectionStat s;
            s.name  = name;
            s.avgUs = extract(blockText, "Avg:");
            s.pct   = extract(blockText, "Percentage:");
            s.calls = static_cast<std::uint64_t>(std::llround(extract(blockText, "Calls:")));
            out.push_back(s);
            i = j;
        }
        else
        {
            ++i;
        }
    }
    return out;
}

// ─── Formatters ────────────────────────────────────────────────────────────
void printHeader(const std::string& title)
{
    std::cout << "\n┌─" << std::string(static_cast<int>(title.size()) + 2, '-') << "─┐\n";
    std::cout << "│  " << title << "  │\n";
    std::cout << "└─" << std::string(static_cast<int>(title.size()) + 2, '-') << "─┘\n\n";
}

void printResult(const ScenarioResultSet& r)
{
    if (r.skipped)
    {
        std::cout << std::left << std::setw(28) << (r.id + " " + r.name)
                  << " SKIPPED (" << r.skipReason << ")\n";
        return;
    }
    std::cout << std::left << std::setw(28) << (r.id + " " + r.name)
              << " bs=" << std::setw(5) << r.blockSize
              << std::right
              << " avg=" << std::fixed << std::setprecision(3) << std::setw(9) << r.folded.avgUs << "us"
              << " p95=" << std::setw(9) << r.folded.p95Us << "us"
              << " p99=" << std::setw(9) << r.folded.p99Us << "us"
              << " CPU=" << std::setw(6) << std::setprecision(2) << r.cpuPercent << "%"
              << " | WSΔ=" << std::setprecision(1) << std::setw(6)
              << (static_cast<double>(r.workingSetDeltaBytes) / (1024.0 * 1024.0)) << "MB"
              << " peak=" << std::setw(6)
              << (static_cast<double>(r.peakWorkingSetBytes) / (1024.0 * 1024.0)) << "MB"
              << std::endl;
}

// ─── JSON emitter (minimal, hand-rolled; no deps) ──────────────────────────
void emitJson(const std::string& path, const Config& cfg,
              const std::vector<ScenarioResultSet>& results,
              const MemorySnapshot& baseline)
{
    std::ofstream f(path);
    if (! f) { std::cerr << "WARN: could not write " << path << "\n"; return; }
    f << std::fixed << std::setprecision(3);
    f << "{\n";
    f << "  \"config\": {\n";
    f << "    \"passes\": " << cfg.passes << ",\n";
    f << "    \"blocksPerPass\": " << cfg.blocksPerPass << ",\n";
    f << "    \"inferenceIterations\": " << cfg.inferenceIterations << ",\n";
    f << "    \"sampleRate\": " << cfg.sampleRate << ",\n";
    f << "    \"realtimePacing\": " << (cfg.realtimePacing ? "true" : "false") << ",\n";
    f << "    \"bufferSizes\": [";
    for (size_t i = 0; i < cfg.bufferSizes.size(); ++i)
        f << cfg.bufferSizes[i] << (i + 1 < cfg.bufferSizes.size() ? "," : "");
    f << "]\n  },\n";
    f << "  \"baselineMemoryMB\": { \"workingSet\": " << baseline.workingSetMB()
      << ", \"private\": " << baseline.privateMB() << ", \"peak\": " << baseline.peakMB() << " },\n";
    f << "  \"results\": [\n";
    for (size_t i = 0; i < results.size(); ++i)
    {
        const auto& r = results[i];
        f << "    {\n";
        f << "      \"id\": \"" << r.id << "\", \"name\": \"" << r.name << "\",\n";
        f << "      \"blockSize\": " << r.blockSize << ", \"sampleRate\": " << r.sampleRate << ",\n";
        f << "      \"passes\": " << r.passes << ", \"skipped\": "
          << (r.skipped ? "true" : "false");
        if (r.skipped) { f << ", \"skipReason\": \"" << r.skipReason << "\""; f << "\n    }" << (i + 1 < results.size() ? "," : "") << "\n"; continue; }
        f << ",\n";
        f << "      \"cpu\": { \"avgUs\": " << r.folded.avgUs << ", \"p50Us\": " << r.folded.p50Us
          << ", \"p95Us\": " << r.folded.p95Us << ", \"p99Us\": " << r.folded.p99Us
          << ", \"minUs\": " << r.folded.minUs << ", \"maxUs\": " << r.folded.maxUs
          << ", \"samples\": " << r.folded.samples << " },\n";
        f << "      \"bufferTimeUs\": " << r.bufferTimeUs << ", \"cpuPercent\": " << r.cpuPercent << ",\n";
        f << "      \"memory\": { \"workingSetDeltaMB\": " << (static_cast<double>(r.workingSetDeltaBytes) / (1024.0 * 1024.0))
          << ", \"peakWorkingSetMB\": " << (static_cast<double>(r.peakWorkingSetBytes) / (1024.0 * 1024.0)) << " },\n";
        f << "      \"perPassAvgUs\": [";
        for (size_t k = 0; k < r.perPassAvgUs.size(); ++k)
            f << r.perPassAvgUs[k] << (k + 1 < r.perPassAvgUs.size() ? "," : "");
        f << "],\n";
        f << "      \"sections\": [\n";
        for (size_t k = 0; k < r.sections.size(); ++k)
        {
            const auto& s = r.sections[k];
            f << "        { \"name\": \"" << s.name << "\", \"avgUs\": " << s.avgUs
              << ", \"pct\": " << s.pct << ", \"calls\": " << s.calls << " }"
              << (k + 1 < r.sections.size() ? "," : "") << "\n";
        }
        f << "      ]\n    }" << (i + 1 < results.size() ? "," : "") << "\n";
    }
    f << "  ]\n}\n";
    std::cout << "\n→ JSON written: " << path << "\n";
}

// ─── Message-thread pump (lets timerCallback fire for MCP/agent/neural) ─────
void pumpMessageThread(int ms)
{
    if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
        mm->runDispatchLoopUntil(ms);
}

// =============================================================================
// Scenario drivers. Each returns a ScenarioResultSet for one buffer size.
// =============================================================================

// S1 — Idle baseline: processor constructed + prepared, no processBlock loop.
ScenarioResultSet scenarioIdle(const Config& cfg, int blockSize)
{
    ScenarioResultSet r{ "S1", "Idle (constructed, no audio)", blockSize, cfg.sampleRate, cfg.passes };
    juce::ScopedJuceInitialiser_GUI juceInit;

    const auto before = takeMemorySnapshot();
    size_t peak = before.peakWorkingSet;

    MorePhiProcessor processor;
    processor.prepareToPlay(static_cast<double>(cfg.sampleRate), blockSize);
    pumpMessageThread(60);   // let any timer setup settle

    // "Idle" = measure the memory footprint of a fully-prepared processor that
    // is NOT processing audio. We sample working set a few times.
    for (int i = 0; i < 5; ++i) { pumpMessageThread(20); peak = std::max(peak, takeMemorySnapshot().peakWorkingSet); }

    const auto after = takeMemorySnapshot();
    processor.releaseResources();
    r.workingSetDeltaBytes = (after.workingSetBytes > before.workingSetBytes)
        ? (after.workingSetBytes - before.workingSetBytes) : 0;
    r.peakWorkingSetBytes  = peak;
    // No per-block timings for idle; record a negligible sustained measurement.
    r.folded = TimingStats{};
    r.bufferTimeUs = static_cast<double>(blockSize) / cfg.sampleRate * 1e6;
    r.cpuPercent = 0.0;
    return r;
}

// Shared driver for S2/S3/S4/S7: a prepared processor + N processBlock passes.
enum class AutomationMode { None, Sweep, Rapid };

ScenarioResultSet scenarioProcess(const Config& cfg, int blockSize, AutomationMode mode,
                                  const std::string& id, const std::string& name,
                                  bool enableSonicMasterCapture, bool startMcpAndAgents)
{
    ScenarioResultSet r{ id, name, blockSize, cfg.sampleRate, cfg.passes };
    juce::ScopedJuceInitialiser_GUI juceInit;

    MorePhiProcessor processor;
    processor.prepareToPlay(static_cast<double>(cfg.sampleRate), blockSize);

    if (enableSonicMasterCapture)
    {
        if (auto* p = processor.getAPVTS().getParameter("SonicMasterAnalysisEnabled"))
            setNormalizedWithGesture(*p, 1.0f);
        processor.getSonicMasterEngine().setActive(true);
    }
    if (startMcpAndAgents)
    {
        processor.startPendingMCPServerForTesting();
        pumpMessageThread(120);   // let startMCPServerIfNeeded / startAgentRuntimeIfNeeded fire
    }

    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    const double bufferUs = static_cast<double>(blockSize) / cfg.sampleRate * 1e6;
    r.bufferTimeUs = bufferUs;

    std::mt19937 rng(0xC0FFEE ^ static_cast<unsigned>(blockSize));
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    const auto memBefore = takeMemorySnapshot();
    size_t peak = memBefore.peakWorkingSet;

    // Reset the in-tree profiler so each scenario's report is self-contained.
    processor.resetProfiler();

    // AUDIT-FIX (M3): collect EVERY per-block timing across ALL passes into one
    // population for p50/p95/p99/stddev. The previous code took percentiles of
    // the 5 pass-averages — p99 of 5 numbers is just the max.
    std::vector<double> allTimings;
    allTimings.reserve(static_cast<size_t>(cfg.passes) * static_cast<size_t>(cfg.blocksPerPass));

    for (int pass = 0; pass < cfg.passes; ++pass)
    {
        std::vector<double> timings;
        timings.reserve(static_cast<size_t>(cfg.blocksPerPass));

        // Warmup block(s) per pass
        fillSineBlock(buffer, 0, cfg.sampleRate);
        processor.processBlock(buffer, midi);

        HighResTimer t;
        for (int b = 0; b < cfg.blocksPerPass; ++b)
        {
            fillSineBlock(buffer, b, cfg.sampleRate);

            if (mode == AutomationMode::Sweep)
            {
                const float phase = static_cast<float>(b) * 0.05f;
                setMorphPos(processor, 0.5f + 0.45f * std::sin(phase),
                                       0.5f + 0.45f * std::cos(phase), 0.5f);
            }
            else if (mode == AutomationMode::Rapid)
            {
                // Several randomized param writes per block via the lock-free queue
                // (simulates aggressive MCP/UI automation bursts).
                if ((b & 1) == 0)
                {
                    setMorphPos(processor, dist(rng), dist(rng), dist(rng));
                    std::vector<MorePhiProcessor::ParamCommand> cmds(8);
                    for (auto& c : cmds)
                    {
                        c.paramIndex = static_cast<int>(dist(rng) * 63);  // null-plugin safe range
                        c.value = dist(rng);
                        c.source = MorePhiProcessor::ParameterEditSource::MCP;
                    }
                    processor.enqueueParameterBatch(cmds);
                }
            }

            t.start();
            processor.processBlock(buffer, midi);
            timings.push_back(t.elapsedUs());

            if (cfg.realtimePacing)
            {
                // Sleep off the difference to wall-clock; faithful CPU% but slower.
                const double spentUs = timings.back();
                if (spentUs < bufferUs)
                    std::this_thread::sleep_for(std::chrono::microseconds(
                        static_cast<long long>(bufferUs - spentUs)));
            }

            // Periodic message-thread pump so deferred work (neural application,
            // agent fan-out, license refresh) actually runs during the loop.
            if ((b & 63) == 0) pumpMessageThread(2);
        }

        // AUDIT-FIX (M3): fold into the cross-pass population before the move.
        allTimings.insert(allTimings.end(), timings.begin(), timings.end());
        TimingStats s = TimingStats::fromSamples(std::move(timings));
        r.perPassAvgUs.push_back(s.avgUs);
        r.folded.avgUs += s.avgUs;
        peak = std::max(peak, takeMemorySnapshot().peakWorkingSet);
    }

    const auto memAfter = takeMemorySnapshot();
    const int N = cfg.passes;
    r.folded.avgUs /= N;

    // AUDIT-FIX (M3): folded percentiles from the full per-block population
    // (all passes × blocksPerPass samples), not from the 5 pass-averages.
    {
        TimingStats allStats = TimingStats::fromSamples(std::move(allTimings));
        r.folded.p50Us = allStats.p50Us;
        r.folded.p95Us = allStats.p95Us;
        r.folded.p99Us = allStats.p99Us;
        r.folded.minUs = allStats.minUs;
        r.folded.maxUs = allStats.maxUs;
        r.folded.samples = allStats.samples;
    }

    r.cpuPercent = (r.folded.avgUs / bufferUs) * 100.0;
    r.workingSetDeltaBytes = (memAfter.workingSetBytes > memBefore.workingSetBytes)
        ? (memAfter.workingSetBytes - memBefore.workingSetBytes) : 0;
    r.peakWorkingSetBytes = peak;

    // Capture per-section profiling from the live processor.
    r.sections = parseSections(processor.getProfilingReport());

    processor.releaseResources();
    pumpMessageThread(40);   // drain timers before ScopedJuceInitialiser dies
    return r;
}

// S5 — Neural isolation: load the ORT model and time runDecision in a loop.
ScenarioResultSet scenarioNeuralIsolation(const Config& cfg, int /*blockSize*/)
{
    ScenarioResultSet r{ "S5", "Neural isolation (ORT runDecision)", 0, cfg.sampleRate, cfg.passes };
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Resolve model + contract next to the canonical staging dir.
    const juce::File modelDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                                    .getParentDirectory();
    // Try a few candidate locations: exe dir, build/sonicmaster, repo build/sonicmaster.
    auto findFile = [&](const std::string& name) -> juce::File {
        std::vector<juce::File> candidates = {
            modelDir.getChildFile(name),
            modelDir.getParentDirectory().getChildFile("sonicmaster").getChildFile(name),
            modelDir.getParentDirectory().getParentDirectory().getChildFile("sonicmaster").getChildFile(name),
            juce::File("G:/More_Phi-vst3-plugin/build/sonicmaster").getChildFile(name)
        };
        for (const auto& c : candidates) if (c.existsAsFile()) return c;
        return {};
    };

    const juce::File model = findFile("masteringbrain_v2_decision.onnx");
    const juce::File contract = findFile("masteringbrain_v2_contract.json");

    if (! model.existsAsFile() || ! contract.existsAsFile())
    {
        r.skipped = true;
        r.skipReason = "ONNX model/contract not found";
        return r;
    }

    SonicMasterDecisionRunner runner;
    if (! runner.loadModel(model.getFullPathName().toStdString(),
                           contract.getFullPathName().toStdString()) ||
        ! runner.isAvailable())
    {
        r.skipped = true;
        r.skipReason = "runner.loadModel failed (ORT not linked?)";
        return r;
    }

    constexpr std::size_t kFrames = kSonicMasterSegmentFrames;     // 262138
    constexpr std::size_t kDecisionWidth = kSonicMasterDecisionWidth;
    std::vector<float> stereo(2 * kFrames);
    std::vector<float> decision(kDecisionWidth);

    // Synthesize a quasi-musical stereo window (1 kHz sine + noise) — deterministic.
    for (std::size_t i = 0; i < kFrames; ++i)
    {
        const double t = static_cast<double>(i) / 44100.0;
        const float s = 0.2f * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * 1000.0 * t));
        stereo[2 * i]     = s;
        stereo[2 * i + 1] = s * 0.8f;
    }

    const auto memBefore = takeMemorySnapshot();
    size_t peak = memBefore.peakWorkingSet;
    std::vector<double> timings;

    // Warmup (session + graph initialization amortization)
    for (int i = 0; i < 2; ++i)
        runner.runDecision(stereo.data(), decision.data(), decision.size());

    HighResTimer t;
    for (int i = 0; i < cfg.inferenceIterations; ++i)
    {
        t.start();
        const bool ok = runner.runDecision(stereo.data(), decision.data(), decision.size());
        const double us = t.elapsedUs();
        timings.push_back(ok ? us : 0.0);
        peak = std::max(peak, takeMemorySnapshot().peakWorkingSet);
    }
    const auto memAfter = takeMemorySnapshot();

    r.folded = TimingStats::fromSamples(std::move(timings));
    // ORT runs on a background analysis thread, not per-block; report per-inference
    // µs and a "per 3s cycle" CPU% (one decision per ~3s window) for context.
    r.bufferTimeUs = 3.0e6;   // nominal analysis interval
    r.cpuPercent = (r.folded.avgUs / r.bufferTimeUs) * 100.0;
    r.workingSetDeltaBytes = (memAfter.workingSetBytes > memBefore.workingSetBytes)
        ? (memAfter.workingSetBytes - memBefore.workingSetBytes) : 0;
    r.peakWorkingSetBytes = peak;
    return r;
}

// S6 — Neural in-context: processor active + capture + one synchronous analysis cycle.
ScenarioResultSet scenarioNeuralInContext(const Config& cfg, int blockSize)
{
    ScenarioResultSet r{ "S6", "Neural in-context (capture + cycle)", blockSize, cfg.sampleRate, cfg.passes };
    juce::ScopedJuceInitialiser_GUI juceInit;

    MorePhiProcessor processor;
    processor.prepareToPlay(static_cast<double>(cfg.sampleRate), blockSize);
    if (auto* p = processor.getAPVTS().getParameter("SonicMasterAnalysisEnabled"))
        setNormalizedWithGesture(*p, 1.0f);
    auto& sm = processor.getSonicMasterEngine();
    sm.setActive(true);

    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;

    // First prime the capture ring with enough audio (~kSonicMasterSegmentFrames).
    const int primeBlocks = static_cast<int>(kSonicMasterSegmentFrames / blockSize) + 8;
    for (int b = 0; b < primeBlocks; ++b)
    {
        fillSineBlock(buffer, b, cfg.sampleRate);
        processor.processBlock(buffer, midi);
    }

    std::vector<double> timings;
    size_t peak = takeMemorySnapshot().peakWorkingSet;
    HighResTimer t;

    // Time runOneCycleForTest (synchronous: drain ring -> resample -> infer -> decode).
    for (int pass = 0; pass < std::min(cfg.passes, 3); ++pass)
    {
        // keep feeding so the ring stays populated between cycles
        for (int b = 0; b < 8; ++b) { fillSineBlock(buffer, b, cfg.sampleRate); processor.processBlock(buffer, midi); }
        t.start();
        const bool applied = sm.runOneCycleForTest();
        timings.push_back(applied ? t.elapsedUs() : t.elapsedUs());
        peak = std::max(peak, takeMemorySnapshot().peakWorkingSet);
    }

    r.folded = TimingStats::fromSamples(std::move(timings));
    r.bufferTimeUs = 3.0e6;
    r.cpuPercent = (r.folded.avgUs / r.bufferTimeUs) * 100.0;
    r.peakWorkingSetBytes = peak;
    processor.releaseResources();
    pumpMessageThread(40);
    return r;
}

// S8 — Combined + Ozone (optional). Tries to load Ozone; skips if absent.
ScenarioResultSet scenarioCombinedOzone(const Config& cfg, int blockSize)
{
    ScenarioResultSet r{ "S8", "Combined + Ozone (hosted)", blockSize, cfg.sampleRate, cfg.passes };
    if (! cfg.runOzonePass) { r.skipped = true; r.skipReason = "disabled via --no-ozone"; return r; }

    const juce::File ozone(R"(C:\Program Files\Common Files\VST3\iZotope\Ozone Pro.vst3)");
    if (! ozone.existsAsFile())
    {
        r.skipped = true;
        r.skipReason = "Ozone not installed at expected VST3 path";
        return r;
    }

    juce::ScopedJuceInitialiser_GUI juceInit;

    // Discover the plugin description (message thread), using the proven
    // findAllTypesForFile pattern from OzoneHeadlessHostMain.cpp.
    juce::PluginDescription desc;
    {
        juce::AudioPluginFormatManager fm;
        fm.addDefaultFormats();
        juce::OwnedArray<juce::PluginDescription> found;
        for (auto* fmt : fm.getFormats())
            fmt->findAllTypesForFile(found, ozone.getFullPathName());
        if (found.isEmpty()) { r.skipped = true; r.skipReason = "VST3 scan returned no types"; return r; }
        desc = *found.getFirst();
    }

    MorePhiProcessor processor;
    processor.prepareToPlay(static_cast<double>(cfg.sampleRate), blockSize);

    // Load THROUGH the processor's host manager so processBlock actually drives
    // the hosted plugin (the hosted_plugin_process profiling section becomes real).
    if (! processor.getHostManager().loadPlugin(desc))
    {
        r.skipped = true;
        r.skipReason = "PluginHostManager::loadPlugin returned false";
        processor.releaseResources();
        pumpMessageThread(40);
        return r;
    }
    pumpMessageThread(150);  // let async prepare settle

    // Treat as combined: enable SonicMaster + MCP + agents + sweep.
    if (auto* p = processor.getAPVTS().getParameter("SonicMasterAnalysisEnabled"))
        setNormalizedWithGesture(*p, 1.0f);
    processor.getSonicMasterEngine().setActive(true);
    processor.startPendingMCPServerForTesting();
    pumpMessageThread(120);

    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    const double bufferUs = static_cast<double>(blockSize) / cfg.sampleRate * 1e6;
    r.bufferTimeUs = bufferUs;

    const auto memBefore = takeMemorySnapshot();
    size_t peak = memBefore.peakWorkingSet;
    processor.resetProfiler();

    // AUDIT-FIX (M3): collect EVERY per-block timing across ALL passes for
    // population-level percentiles (see scenarioProcess for the rationale).
    std::vector<double> allTimings;
    allTimings.reserve(static_cast<size_t>(cfg.passes) * static_cast<size_t>(cfg.blocksPerPass));

    for (int pass = 0; pass < cfg.passes; ++pass)
    {
        std::vector<double> timings;
        timings.reserve(static_cast<size_t>(cfg.blocksPerPass));
        fillSineBlock(buffer, 0, cfg.sampleRate);
        processor.processBlock(buffer, midi);
        HighResTimer t;
        for (int b = 0; b < cfg.blocksPerPass; ++b)
        {
            fillSineBlock(buffer, b, cfg.sampleRate);
            const float phase = static_cast<float>(b) * 0.05f;
            setMorphPos(processor, 0.5f + 0.45f * std::sin(phase),
                                   0.5f + 0.45f * std::cos(phase), 0.5f);
            t.start();
            processor.processBlock(buffer, midi);
            timings.push_back(t.elapsedUs());
            if ((b & 63) == 0) pumpMessageThread(2);
        }
        allTimings.insert(allTimings.end(), timings.begin(), timings.end());  // AUDIT-FIX (M3)
        TimingStats s = TimingStats::fromSamples(std::move(timings));
        r.perPassAvgUs.push_back(s.avgUs);
        r.folded.avgUs += s.avgUs;
        peak = std::max(peak, takeMemorySnapshot().peakWorkingSet);
    }
    r.folded.avgUs /= cfg.passes;
    // AUDIT-FIX (M3): folded percentiles from the full per-block population.
    TimingStats ps = TimingStats::fromSamples(std::move(allTimings));
    r.folded.p50Us = ps.p50Us; r.folded.p95Us = ps.p95Us; r.folded.p99Us = ps.p99Us;
    r.folded.minUs = ps.minUs; r.folded.maxUs = ps.maxUs; r.folded.samples = ps.samples;
    r.cpuPercent = (r.folded.avgUs / bufferUs) * 100.0;
    const auto memAfter = takeMemorySnapshot();
    r.workingSetDeltaBytes = (memAfter.workingSetBytes > memBefore.workingSetBytes)
        ? (memAfter.workingSetBytes - memBefore.workingSetBytes) : 0;
    r.peakWorkingSetBytes = peak;
    r.sections = parseSections(processor.getProfilingReport());
    processor.releaseResources();
    pumpMessageThread(40);
    return r;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
int runAudit(int argc, char** argv)
{
    prepareMeasurementHygiene();  // AUDIT-FIX (M1): FTZ/DAZ + thread affinity before any timing
    const Config cfg = parseArgs(argc, argv);

    std::cout << "\n╔══════════════════════════════════════════════════════════════════════╗\n";
    std::cout <<   "║   MORE-PHI INTEGRATED CPU & MEMORY AUDIT — Real MorePhiProcessor      ║\n";
    std::cout <<   "╚══════════════════════════════════════════════════════════════════════╝\n";
    std::cout << "  passes=" << cfg.passes << " blocks/pass=" << cfg.blocksPerPass
              << " inferenceIters=" << cfg.inferenceIterations
              << " buffers=[";
    for (size_t i = 0; i < cfg.bufferSizes.size(); ++i) std::cout << cfg.bufferSizes[i] << (i + 1 < cfg.bufferSizes.size() ? "," : "");
    std::cout << "] @" << cfg.sampleRate << "Hz"
              << (cfg.realtimePacing ? " [REALTIME PACED]" : " [OFFLINE best-case]") << "\n";
#if defined(_WIN32) && defined(_M_X64)
    std::cout << "  Measurement hygiene: FTZ+DAZ ON, affinity pinned to CPU 2, HIGH_PRIORITY\n";
#else
    std::cout << "  Measurement hygiene: FTZ/DAZ/affinity NOT applied (non-Windows-x64) — expect noise\n";
#endif

    const auto baseline = takeMemorySnapshot();
    if (baseline.supported)
        std::cout << "  Baseline working set: " << std::fixed << std::setprecision(1)
                  << baseline.workingSetMB() << " MB (private " << baseline.privateMB() << " MB)\n";

    std::vector<ScenarioResultSet> all;

    printHeader("S1 — IDLE (no audio)");
    for (int bs : cfg.bufferSizes) { auto r = scenarioIdle(cfg, bs); printResult(r); all.push_back(std::move(r)); }

    printHeader("S2 — ACTIVE, null plugin (engine core steady-state)");
    for (int bs : cfg.bufferSizes) { auto r = scenarioProcess(cfg, bs, AutomationMode::None, "S2", "Active null", false, false); printResult(r); all.push_back(std::move(r)); }

    printHeader("S3 — PARAMETER AUTOMATION (linear sweep)  ← cost-of-automation baseline");
    for (int bs : cfg.bufferSizes) { auto r = scenarioProcess(cfg, bs, AutomationMode::Sweep, "S3", "Sweep", false, false); printResult(r); all.push_back(std::move(r)); }

    printHeader("S4 — PARAMETER AUTOMATION (rapid random bursts)");
    for (int bs : cfg.bufferSizes) { auto r = scenarioProcess(cfg, bs, AutomationMode::Rapid, "S4", "Rapid", false, false); printResult(r); all.push_back(std::move(r)); }

    printHeader("S5 — NEURAL ISOLATION (ORT runDecision loop)");
    { auto r = scenarioNeuralIsolation(cfg, 0); printResult(r); all.push_back(std::move(r)); }

    printHeader("S6 — NEURAL IN-CONTEXT (capture + runOneCycleForTest)");
    { auto r = scenarioNeuralInContext(cfg, 256); printResult(r); all.push_back(std::move(r)); }

    printHeader("S7 — COMBINED (engine + morph + neural + MCP + agents, null plugin)");
    for (int bs : cfg.bufferSizes) { auto r = scenarioProcess(cfg, bs, AutomationMode::Sweep, "S7", "Combined", true, true); printResult(r); all.push_back(std::move(r)); }

    printHeader("S8 — COMBINED + OZONE (optional realistic reference)");
    { auto r = scenarioCombinedOzone(cfg, 256); printResult(r); all.push_back(std::move(r)); }

    printHeader("AUTOMATION DELTA (cost of parameter control)");
    {
        auto findByIdBs = [&](const std::string& id, int bs) -> const ScenarioResultSet* {
            for (const auto& r : all) if (r.id == id && r.blockSize == bs && !r.skipped) return &r;
            return nullptr;
        };
        std::cout << std::left << std::setw(18) << "Buffer"
                  << std::right << std::setw(14) << "S3-S2 avg us"
                  << std::setw(14) << "S3-S2 CPU%"
                  << std::setw(14) << "S4-S2 avg us"
                  << std::setw(14) << "S4-S2 CPU%" << "\n";
        std::cout << std::string(74, '-') << "\n";
        for (int bs : cfg.bufferSizes)
        {
            const auto* s2 = findByIdBs("S2", bs);
            const auto* s3 = findByIdBs("S3", bs);
            const auto* s4 = findByIdBs("S4", bs);
            const double dSweep = (s2 && s3) ? (s3->folded.avgUs - s2->folded.avgUs) : 0.0;
            const double dRapid = (s2 && s4) ? (s4->folded.avgUs - s2->folded.avgUs) : 0.0;
            const double bufUs = static_cast<double>(bs) / cfg.sampleRate * 1e6;
            std::cout << std::left << std::setw(18) << bs
                      << std::right << std::fixed << std::setprecision(3)
                      << std::setw(12) << dSweep << "us"
                      << std::setw(11) << std::setprecision(2) << (dSweep / bufUs * 100.0) << "%"
                      << std::setw(12) << std::setprecision(3) << dRapid << "us"
                      << std::setw(11) << std::setprecision(2) << (dRapid / bufUs * 100.0) << "%\n";
        }
    }

    printHeader("NEURAL: ISOLATION vs IN-CONTEXT vs COMBINED");
    {
        const auto* s5 = [&]() -> const ScenarioResultSet* { for (const auto& r : all) if (r.id == "S5") return &r; return nullptr; }();
        const auto* s6 = [&]() -> const ScenarioResultSet* { for (const auto& r : all) if (r.id == "S6") return &r; return nullptr; }();
        const auto* s7 = [&]() -> const ScenarioResultSet* { for (const auto& r : all) if (r.id == "S7" && r.blockSize == 256) return &r; return nullptr; }();
        if (s5) std::cout << "  S5 isolated ORT decision avg: " << std::fixed << std::setprecision(0)
                          << s5->folded.avgUs << " us (" << s5->folded.p99Us << " us p99)\n";
        if (s6) std::cout << "  S6 in-context cycle avg:      " << s6->folded.avgUs << " us ("
                          << "surround cost ≈ " << (s6 && s5 ? (s6->folded.avgUs - s5->folded.avgUs) : 0.0) << " us)\n";
        if (s7) std::cout << "  S7 combined per-block avg:    " << s7->folded.avgUs
                          << " us @256 (" << s7->cpuPercent << "% of one core)\n";
    }

    emitJson(cfg.jsonPath, cfg, all, baseline);

    std::cout << "\n════════════════════════════════════════════════════════════════════════\n";
    std::cout << "AUDIT COMPLETE — " << all.size() << " scenario results, "
              << std::count_if(all.begin(), all.end(), [](const auto& r){ return r.skipped; })
              << " skipped\n";
    std::cout << "════════════════════════════════════════════════════════════════════════\n";
    return 0;
}

} // namespace more_phi::audit

int main(int argc, char** argv)
{
    return more_phi::audit::runAudit(argc, argv);
}
