// =============================================================================
// tools/headless_mastering_render/headless_render.h
//
// T2 Phase-0 headless mastering render harness — C ABI.
//
// Drives the REAL more_phi::AutoMasteringEngine OFFLINE to render a candidate
// 72-delta mastering vector into PCM + meters, callable from Python via ctypes.
//
// VERIFIED-FEASIBLE against source:
//   - tests/Unit/TestAudioEngine.cpp:381-411 (offline processBlock loop)
//   - tests/Unit/TestAudioEngine.cpp:442-471 (offline applyValidatedPlan)
//
// Determinism contract (enforced inside render()):
//   - engine.reset()             per candidate  (flushes all stage filter states)
//   - policy.clearLastSafePlan() per candidate  (project from zero baseline)
//   - 512-sample chunks matching prepare(maxBlockSize=512, startIntelligence=false)
//
// See headless_render.cpp for the full implementation notes.
// =============================================================================
#ifndef MORE_PHI_HEADLESS_RENDER_H
#define MORE_PHI_HEADLESS_RENDER_H

#include <stdint.h>

// Cross-platform shared-object export macro. MUST be applied consistently to
// BOTH the declarations here and the definitions in the .cpp, otherwise MSVC
// rejects the definition as "redefinition; different linkage" (C2375) — the
// header decl (no attribute) and the .cpp def (dllexport) would disagree.
#if defined(_WIN32)
    #define MPHRENDER_EXPORT __declspec(dllexport)
#else
    #define MPHRENDER_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// morephi_headless_init — call ONCE after ctypes.CDLL (idempotent).
//
//   sampleRate     target sample rate in Hz (e.g. 48000.0).
//   maxBlockSize   max chunk size render() will feed (must be >= the chunk size
//                  you use; typically 512). bandBuffers_ are pre-allocated to
//                  this size — never feed a larger buffer.
//   normalizerMode 0 = disable LoudnessNormalizer (gain frozen at 1.0; the
//                     loudness delta becomes setpoint-only — RECOMMENDED for
//                     EQ/dynamics/stereo search, fully deterministic).
//                  1 = pump updateCorrectionGain() at ~10 Hz inside render()
//                     (matches the live AutoMasteringEngine::timerCallback; use
//                     when searching the loudness axis against a real LUFS
//                     target).
//
// Returns 0 on success, nonzero on error.
// ---------------------------------------------------------------------------
MPHRENDER_EXPORT int morephi_headless_init(double sampleRate, int maxBlockSize, int normalizerMode);

// Optional explicit teardown (releases the engine + JUCE MessageManager).
// Safe to call multiple times. After this, init() must be called again before
// any render() call.
MPHRENDER_EXPORT void morephi_headless_shutdown(void);

// ---------------------------------------------------------------------------
// render — render ONE candidate 72-delta vector through the mastering chain.
//
//   unmastered_pcm   interleaved stereo input  [n_samples * 2]  (L,R,L,R,...)
//   n_samples        number of FRAMES (per channel). Must be >= 1.
//   sample_rate      Hz; if it differs from the init rate the engine is
//                    re-prepared (rare; pin one rate for CMA-ES reproducibility).
//   delta72          exactly 72 floats in fixed order:
//                       eq[0..31], dynamics[0..7], stereo[0..7],
//                       harmonic[0..7], limiter[0..7], loudness[0..7]
//                    Values are clamped to [-1,1] internally. Of the 72 slots,
//                    43 are WIRED to DSP (32 eq + 4 dyn + 4 stereo + 1 harm[0]
//                    + 1 lim[0] + 1 loud[0]); the other 29 are DEAD (fixed at 0
//                    in your search vector to avoid wasted dimensions).
//   out_rendered     caller-allocated interleaved stereo [n_samples * 2].
//   out_lufs         receives integrated LUFS (post-chain; <= -200 if <3s audio
//                    because the LUFSMeter relative gate needs ~3s to engage).
//   out_dbtp         receives true-peak dBTP (post-chain).
//   out_limiter_gr   receives limiter gain reduction dB.
//
// Returns:
//    0  success
//    1  null pointer / invalid argument
//    2  not initialized (call morephi_headless_init first)
//    3  candidate rejected by safety policy (plan.valid == false)
//    4  applyValidatedPlan returned false
//    5  internal exception
//
// THREAD SAFETY: render() holds a process-global mutex — safe to call from any
// thread, serialized. For parallel CMA-ES populations, run multiple Python
// processes (each loads its own .so).
// ---------------------------------------------------------------------------
MPHRENDER_EXPORT int render(const float*  unmastered_pcm,
           int           n_samples,
           double        sample_rate,
           const float*  delta72,
           float*        out_rendered,
           float*        out_lufs,
           float*        out_dbtp,
           float*        out_limiter_gr);

// Returns the mastering-chain latency (limiter lookahead + exciter oversampling
// when the harmonic delta enabled it) in samples at the current sample rate.
// Edge-accurate renderers should feed `latency` trailing silence and discard
// the first `latency` rendered samples.
MPHRENDER_EXPORT int morephi_headless_chain_latency(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // MORE_PHI_HEADLESS_RENDER_H
