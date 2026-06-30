// =============================================================================
// tools/headless_mastering_render/headless_render.cpp
//
// T2 Phase-0 headless mastering render harness.
//
// Drives the REAL more_phi::AutoMasteringEngine OFFLINE (no DAW, no audio
// thread, no JUCE message loop) to render a candidate 72-delta mastering vector
// into PCM + meters, callable from Python via ctypes.
//
// Verified-feasible pattern source:
//   - tests/Unit/TestAudioEngine.cpp:381-411  (offline processBlock loop)
//   - tests/Unit/TestAudioEngine.cpp:442-471  (offline applyValidatedPlan)
//
// Determinism contract per candidate:
//   engine.reset()            -> AutoMasteringEngine.cpp:98-120 (flushes all
//                                stage filter states + lastSafeNeuralPlan_)
//   policy.clearLastSafePlan() -> NeuralMasteringSafetyPolicy.cpp:459-463
//                                (project from zero baseline, not running sum)
// =============================================================================
#include "headless_render.h"

#include "Core/AutoMasteringEngine.h"
#include "Core/NeuralMasteringSafetyPolicy.h"
#include "Core/NeuralMasteringTypes.h"
#include "AI/OnnxNeuralMasteringRunner.h"   // buildPlanCandidate, kOnnxOutputDeltaCount

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>   // ScopedJuceInitialiser_GUI

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>

namespace {

// Cross-platform shared-object export macro. Defined in the header (applied to
// declarations there) so decl/def linkage matches under MSVC (C2375 guard).
#ifndef MPHRENDER_EXPORT
#if defined(_WIN32)
    #define MPHRENDER_EXPORT __declspec(dllexport)
#else
    #define MPHRENDER_EXPORT __attribute__((visibility("default")))
#endif
#endif

// Compile-time guards: fail the build loudly if the ABI drifts.
static_assert(more_phi::kOnnxOutputDeltaCount == 72,
              "headless_render: kOnnxOutputDeltaCount must be 72 (eq32+dyn8+stereo8+harm8+lim8+loud8).");

// ---------------------------------------------------------------------------
// Long-lived harness state (one per process; guarded by g_mutex).
// ---------------------------------------------------------------------------
struct HarnessState
{
    // ScopedJuceInitialiser_GUI must outlive the engine (it owns the
    // MessageManager the Timer base class — even stopped — references).
    std::unique_ptr<juce::ScopedJuceInitialiser_GUI> juceInit;
    std::unique_ptr<more_phi::AutoMasteringEngine>   engine;
    more_phi::NeuralMasteringSafetyPolicy            policy;

    double sampleRate  = 48000.0;
    int    blockSize   = 512;
    int    nChannels   = 2;
    bool   normalizerEnabled = false;   // default OFF for CMA-ES determinism
    bool   ready       = false;
};

HarnessState g_state;
std::mutex   g_mutex;
std::atomic<bool> g_initialized { false };

// Pump cadence for the LoudnessNormalizer when enabled: ~10 Hz at the
// configured sample rate / block size (matches AutoMasteringEngine::timerCallback).
int normalizerPumpPeriodBlocks(double sr, int block)
{
    const double targetBlocksPerSecond = sr / static_cast<double>(std::max(1, block));
    const int    period = static_cast<int>(std::round(targetBlocksPerSecond / 10.0));
    return std::max(1, period);
}

} // namespace

// =============================================================================
// extern "C" API — see headless_render.h for the contract.
// =============================================================================

extern "C" {

// ---------------------------------------------------------------------------
// morephi_headless_init — call ONCE after ctypes.CDLL.
//
//   sampleRate       e.g. 48000.0
//   maxBlockSize     must be >= the chunk size you feed to render() (512).
//   normalizerMode   0 = disable LoudnessNormalizer (gain=1.0, deterministic,
//                        loudness delta is setpoint-only no-op — RECOMMENDED for
//                        EQ/dynamics/stereo search)
//                    1 = pump updateCorrectionGain() at ~10 Hz inside render()
//                        (matches live runtime; required to search the loudness
//                        axis against an actual LUFS target)
//
// Returns 0 on success, nonzero on error.
// ---------------------------------------------------------------------------
MPHRENDER_EXPORT int morephi_headless_init(double sampleRate,
                                           int    maxBlockSize,
                                           int    normalizerMode)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_initialized.load())
    {
        // Idempotent re-init: reconfigure in-place.
        g_state.engine->setActive(false);
    }

    if (sampleRate <= 0.0 || maxBlockSize <= 0)
        return 1;

    try
    {
        // JUCE MessageManager must exist before any juce::Timer subclass is
        // constructed. ScopedJuceInitialiser_GUI creates it on THIS thread
        // (the ctypes caller thread) — fine since the teacher is single-threaded.
        if (!g_state.juceInit)
            g_state.juceInit = std::make_unique<juce::ScopedJuceInitialiser_GUI>();

        if (!g_state.engine)
            g_state.engine = std::make_unique<more_phi::AutoMasteringEngine>();

        // CRITICAL 3rd arg = false: no 10 Hz timer, no GenreClassifier thread,
        // no heuristic compressor defaults. Pure DSP shell.
        g_state.engine->prepare(sampleRate, maxBlockSize, /*startIntelligence=*/false);
        g_state.engine->setActive(true);
        // Option 2: open the internal-DSP write gate (Fix 8) for eval WITHOUT
        // starting prepare(true)'s intelligence subsystems (timer/genre/heuristics).
        g_state.engine->setInternalChainActive(true);

        g_state.sampleRate       = sampleRate;
        g_state.blockSize        = maxBlockSize;
        g_state.nChannels        = 2;
        g_state.normalizerEnabled = (normalizerMode == 1);
        g_state.engine->getLoudnessNormalizer().setEnabled(g_state.normalizerEnabled);

        g_state.ready = true;
        g_initialized.store(true);
        return 0;
    }
    catch (...)
    {
        g_state.ready = false;
        return 2;
    }
}

// ---------------------------------------------------------------------------
// morephi_headless_shutdown — optional; releases the engine + JUCE.
// Safe to call multiple times. After this, init() must be called again.
// ---------------------------------------------------------------------------
MPHRENDER_EXPORT void morephi_headless_shutdown(void)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state.engine) g_state.engine->setActive(false);
    g_state.engine.reset();
    // NOTE: juceInit is destroyed LAST (after engine) so the MessageManager
    // outlives the Timer base. Order in struct destruction handles this.
    g_state.juceInit.reset();
    g_state.ready = false;
    g_initialized.store(false);
}

// ---------------------------------------------------------------------------
// render — render one candidate 72-delta vector through the mastering chain.
//
// Returns:
//    0  success
//    1  null pointer / invalid arg
//    2  not initialized (call morephi_headless_init first)
//    3  candidate rejected by safety policy (plan.valid == false)
//    4  applyValidatedPlan returned false (plan.valid/fallbackMode mismatch)
//    5  internal exception
//
// Buffers:
//   unmastered_pcm   interleaved stereo [n_samples * 2]  (L,R,L,R,...)
//   delta72          exactly 72 floats in order:
//                       eq[0..31], dynamics[0..7], stereo[0..7],
//                       harmonic[0..7], limiter[0..7], loudness[0..7]
//   out_rendered     interleaved stereo [n_samples * 2]  (caller-allocated)
//   out_lufs         &float  (integrated LUFS, post-chain; <= -200 if <3s audio)
//   out_dbtp         &float  (true-peak dBTP, post-chain)
//   out_limiter_gr   &float  (limiter gain reduction dB)
// ---------------------------------------------------------------------------
MPHRENDER_EXPORT int render(const float*  unmastered_pcm,
                            int           n_samples,
                            double        sample_rate,
                            const float*  delta72,
                            float*        out_rendered,
                            float*        out_lufs,
                            float*        out_dbtp,
                            float*        out_limiter_gr)
{
    if (unmastered_pcm == nullptr || delta72 == nullptr ||
        out_rendered == nullptr || out_lufs == nullptr ||
        out_dbtp == nullptr || out_limiter_gr == nullptr)
        return 1;
    if (n_samples <= 0)
    {
        *out_lufs = -std::numeric_limits<float>::infinity();
        *out_dbtp = -std::numeric_limits<float>::infinity();
        *out_limiter_gr = 0.0f;
        return 0;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_state.ready || !g_state.engine)
        return 2;

    // The engine was prepared for g_state.sampleRate; if the caller passes a
    // different rate we re-prepare (rare; CMA-ES typically pins one rate).
    if (sample_rate != g_state.sampleRate && sample_rate > 0.0)
    {
        g_state.engine->setActive(false);
        g_state.engine->prepare(sample_rate, g_state.blockSize, /*startIntelligence=*/false);
        g_state.engine->setActive(true);
        g_state.engine->setInternalChainActive(true);
        g_state.engine->getLoudnessNormalizer().setEnabled(g_state.normalizerEnabled);
        g_state.sampleRate = sample_rate;
    }

    try
    {
        auto& engine = *g_state.engine;
        const int   nch        = g_state.nChannels;   // 2
        const int   block      = g_state.blockSize;   // 512
        const double sr        = g_state.sampleRate;

        // ── 1. Per-candidate determinism: reset ALL state ────────────────────
        //    AutoMasteringEngine.cpp:98-120 flushes splitter/dynamics/eq/
        //    stereo/exciter/limiter/lufs/normalizer/meterWindow/spectrum/
        //    stereoField AND calls clearLastSafeNeuralMasteringPlan().
        engine.reset();
        engine.setActive(true);
        engine.getLoudnessNormalizer().setEnabled(g_state.normalizerEnabled);

        // ── 2. delta72 -> ValidatedNeuralMasteringPlan (EXACT runtime path) ─
        more_phi::NeuralMasteringFeatureFrame frame;
        frame.sampleRate   = sr;
        frame.channelCount = nch;
        frame.blockSize    = block;
        frame.frameIndex   = 0;   // offline: no frame counter

        // Full-authority mask: allow the teacher to move ALL six control groups
        // (incl. high-risk harmonic + limiter). The safety policy below is
        // configured to ACCEPT high-risk masks.
        more_phi::MasteringControlMask mask;
        mask.eq       = true;
        mask.dynamics = true;
        mask.stereo   = true;
        mask.harmonic = true;
        mask.limiter  = true;
        mask.loudness = true;

        // buildPlanCandidate() also calls sanitizePlanCandidate() internally
        // (clamps deltas to [-1,1], coerces non-finite to 0).
        auto candidate = more_phi::buildPlanCandidate(
            delta72,
            more_phi::kOnnxOutputDeltaCount,   // 72
            frame,
            /*confidence=*/1.0f,
            more_phi::NeuralMasteringEvidenceLevel::PrototypeMeasured,
            mask);

        // Safety-policy config tuned for a full-axis teacher:
        //   - highRiskControls cleared (default {harmonic,limiter} -> HighRiskMask reject)
        //   - maxDeltaPerPlan = ±1.0 (default 0.05-0.15 clamps 85% of the range)
        //   - minTargets = -1, maxTargets = +1 (the formulas clamp internally anyway)
        //   - minConfidence = 0 (we pass confidence=1.0 regardless)
        auto cfg = more_phi::NeuralMasteringSafetyPolicy::defaultConfig();
        cfg.highRiskControls = more_phi::MasteringControlMask{};
        cfg.minConfidence    = 0.0f;
        cfg.maxDeltaPerPlan.eq.fill(1.0f);
        cfg.maxDeltaPerPlan.dynamics.fill(1.0f);
        cfg.maxDeltaPerPlan.stereo.fill(1.0f);
        cfg.maxDeltaPerPlan.harmonic.fill(1.0f);
        cfg.maxDeltaPerPlan.limiter.fill(1.0f);
        cfg.maxDeltaPerPlan.loudness.fill(1.0f);
        cfg.minTargets.eq.fill(-1.0f);       cfg.maxTargets.eq.fill(1.0f);
        cfg.minTargets.dynamics.fill(-1.0f); cfg.maxTargets.dynamics.fill(1.0f);
        cfg.minTargets.stereo.fill(-1.0f);   cfg.maxTargets.stereo.fill(1.0f);
        cfg.minTargets.harmonic.fill(-1.0f); cfg.maxTargets.harmonic.fill(1.0f);
        cfg.minTargets.limiter.fill(-1.0f);  cfg.maxTargets.limiter.fill(1.0f);
        cfg.minTargets.loudness.fill(-1.0f); cfg.maxTargets.loudness.fill(1.0f);

        // Reconfigure the policy in-place (cheaper than reconstructing each call).
        g_state.policy = more_phi::NeuralMasteringSafetyPolicy(cfg);
        // CRITICAL: project from ZERO baseline, not the previous candidate's
        // accepted target (NeuralMasteringSafetyPolicy.cpp:435-437 uses
        // lastSafePlan_ as `previous`). Without this, deltas compound.
        g_state.policy.clearLastSafePlan();

        more_phi::NeuralMasteringRuntimeState rs;
        rs.currentFrame = frame.frameIndex;   // 0 == producedAtFrame -> never stale
        rs.sampleRate   = sr;
        rs.channelCount = nch;
        rs.layout       = more_phi::NeuralMasteringLayout::Stereo;
        rs.overload     = false;

        auto result = g_state.policy.validate(candidate, rs);
        if (!result.accepted || !result.plan.valid)
            return 3;

        // ── 3. plan -> DSP params (6 formulas, AutoMasteringEngine.cpp:347-409)
        if (!engine.applyValidatedPlan(result.plan))
            return 4;

        // ── 4. Stream through 512-sample chunks ──────────────────────────────
        //    processBlock mutates buf in-place; runs on THIS thread (no audio
        //    thread affinity). bandBuffers_ are pre-sized to maxBlockSize, so
        //    never feed a chunk larger than `block`.
        juce::AudioBuffer<float> buf(nch, block);
        const int pumpPeriod = normalizerPumpPeriodBlocks(sr, block);
        int pumped = 0;

        for (int off = 0; off < n_samples; off += block)
        {
            const int ns = std::min(block, n_samples - off);

            // Interleaved -> planar into buf; zero the tail of partial chunks so
            // stale samples don't leak into the limiter / LUFS meters.
            for (int ch = 0; ch < nch; ++ch)
            {
                float* dst = buf.getWritePointer(ch);
                for (int i = 0; i < ns; ++i)
                    dst[i] = unmastered_pcm[(static_cast<std::size_t>(off + i) * nch)
                                            + static_cast<std::size_t>(ch)];
                for (int i = ns; i < block; ++i)
                    dst[i] = 0.0f;
            }

            engine.processBlock(buf);

            // Planar -> interleaved out (only the valid ns samples).
            for (int ch = 0; ch < nch; ++ch)
            {
                const float* src = buf.getReadPointer(ch);
                for (int i = 0; i < ns; ++i)
                    out_rendered[(static_cast<std::size_t>(off + i) * nch)
                                 + static_cast<std::size_t>(ch)] = src[i];
            }

            // Pump the LoudnessNormalizer correction gain at ~10 Hz to match the
            // live timerCallback cadence (AutoMasteringEngine.cpp:300). No-op
            // when the normalizer is disabled (updateCorrectionGain early-returns).
            if (g_state.normalizerEnabled && ++pumped >= pumpPeriod)
            {
                engine.getLoudnessNormalizer().updateCorrectionGain();
                pumped = 0;
            }
        }

        // ── 5. Extract meters (post-chain, atomic getters, any thread) ───────
        *out_lufs       = engine.getLUFSIntegrated();
        *out_dbtp       = engine.getTruePeak_dBTP();
        *out_limiter_gr = engine.getLimiterGainReductionDB();
        return 0;
    }
    catch (...)
    {
        return 5;
    }
}

// ---------------------------------------------------------------------------
// Utility: report the mastering-chain latency (limiter lookahead + exciter OS
// delay when enabled) in samples at the current sample rate. Callers that need
// edge-accurate PCM should feed `latency` trailing silence and discard the
// first `latency` rendered samples.
// ---------------------------------------------------------------------------
MPHRENDER_EXPORT int morephi_headless_chain_latency(void)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_state.ready || !g_state.engine) return 0;
    return g_state.engine->getMasteringChainLatency();
}

} // extern "C"
