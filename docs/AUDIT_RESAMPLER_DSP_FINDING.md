# SonicMaster Resampler DSP Defects — Investigation & Remediation Plan

**Status:** Investigated and **confirmed** (2026-06-30). Fix **deferred** — the
correct rewrite is architecturally coupled to performance and NaN-semantics
decisions that ripple through the SonicMaster test suite and the shipping
inference path. This document records the proof and the agreed remediation so
the work can be picked up as a focused task without re-deriving the root cause.

## Scope

`more_phi::resamplePolyphase()` in `src/AI/SonicMasterAnalysisEngine.cpp`
resamples the captured host-rate waveform down to the model's 44.1 kHz / 262138-
frame input on **every analysis cycle and every on-demand `requestDecisionNow`**,
except when the host already runs at exactly 44.1 kHz (a `std::copy` fast-path
bypasses it). At 48 kHz — the most common production rate — every inference runs
through this function (ratio ≈ 1.088).

## The two confirmed defects

Both were verified by reproducing the exact C++ filter design + resampling loop
in Python against the shipped implementation.

### Defect A — Non-unity DC gain (flatness bug)

A constant (DC) input of amplitude A does **not** emerge at amplitude A:

| Conversion (ratio)        | Measured DC gain |
|----------------------------|------------------|
| 44.1k → 44.1k (1.000)     | 0.387            |
| 48k → 44.1k   (1.088)     | 0.006            |
| 96k → 44.1k   (2.177)     | 0.006            |

Mechanism: the implementation is a 128-phase × **8-tap** polyphase table. Each
output sample exercises only a single 8-tap phase branch (sum ≈ 0.387); it never
sums enough of the 1024-tap prototype to reconstruct DC. The downstream
`peak-normalize to -1 dBFS` masks the absolute-level error, but the **spectral
envelope** the model relies on is distorted. Increasing `kTapsPerPhase` does
**not** help — the DC gain stays ≈ 0.385 regardless (verified up to 128
taps/phase), confirming the indexing convention itself cannot deliver unity gain.

### Defect B — Ratio-independent cutoff (bandlimiting bug)

The Kaiser prototype is designed **once** with a fixed `cutoff = 0.42`. The
correct lowpass cutoff for ratio-R conversion is `min(0.5, 1/(2·R))` (in
input-normalized units), so the output is bandlimited to whichever Nyquist is
lower. Because the cutoff does not track the ratio:

- Downsampling 96k → 44.1k is **not** bandlimited to the 22.05 kHz output
  Nyquist → aliasing into the model input.
- The doc comment claims a "Nyquist-anchored cutoff at the OUTPUT sample rate,"
  but the code delivers a fixed 0.42 regardless of `srcLen/dstLen`.

### Train/serve skew

The ONNX model was trained on `librosa.resample` / `soxr` preprocessing (see
`scripts/neural-mastering/control/build_manifest.py`,
`reference_score.py`), which is unity-gain at DC and correctly bandlimited. The
C++ resampler violates both properties → the production model input is
out-of-distribution at every non-44.1k rate.

## Why the fix was deferred

A correct rewrite (direct-form per-sample windowed-sinc, Kaiser β=8.6, ±48
input taps, per-output DC normalization) was implemented, unit-tested, and
verified to achieve unity DC gain and correct ratio-aware bandlimiting. It was
then **reverted** because it introduced regressions in the wider SonicMaster
suite:

1. **Performance.** The direct-form evaluates a sinc + Kaiser-window (24-term
   I₀ series) per output sample: ≈ 25 M transcendental ops per channel at the
   262138-frame model segment. The static table lookup it replaces is ≈ 2 M
   ops. The ~50–100× slowdown trips the analysis engine's 10 s staleness guard
   and races the background `analysisLoop` thread, causing 4–6
   `TestSonicMasterAnalysisEngine` / `TestGenreLoudnessPrior` cases to fail by
   timing, not by value.
2. **NaN semantics.** The direct-form's per-output normalization means a single
   NaN input taints the entire output window (peak → NaN → `SilentInput` gate →
   inference skipped). The table implementation only taints the few outputs
   that reference the NaN sample. `TestSonicMasterAnalysisEngine.cpp:470`
   asserts inference *does* run on NaN-poisoned audio, so the rewrite changes
   the documented robustness contract.

These are not bugs in the rewrite — they are **architectural couplings** that a
resampler change forces a position on. Per the project's debugging discipline
(three+ attempted approaches each surfacing a new shared concern), the right
move is a dedicated task rather than a drive-by patch on a shipping inference
path.

## Remediation plan

1. **Expose `resamplePolyphase` for direct testing.** Move it out of the
   anonymous namespace into `more_phi` scope (declare in
   `SonicMasterAnalysisEngine.h`). Today it is file-static, so the only
   coverage is indirect — which is exactly why Defects A/B survived. (Drafted
   during this investigation; reverted together with the rewrite.)
2. **Add DSP-correctness tests** pinning (a) unity DC gain at ratios {1.0, 1.088,
   2.177, 0.919} and (b) ratio-aware bandlimiting (passband flat to 15 kHz,
   stopband < 0.05 above output Nyquist, on 96k → 44.1k).
3. **Implement a fast + correct design.** Precompute a per-ratio polyphase
   table **and rebuild it only when the ratio changes** (i.e. on
   `prepareToPlay` sample-rate change — rare). The table must use enough taps
   per phase to reconstruct DC at every phase branch; verify with the tests
   from step 2. This keeps the steady-state cost at O(dstLen · taps) table
   lookups (fast) while fixing both defects.
4. **Decide the NaN contract explicitly.** Either (a) keep "NaN taints only
   local outputs" by skipping NaN inputs in the accumulation, or (b) update
   `TestSonicMasterAnalysisEngine.cpp:470` to assert the whole-window reject.
   Pick one and document it on the function.
5. **Re-benchmark the cycle.** Confirm a full `runCycle` at 96 kHz stays well
   under the 10 s staleness budget with margin for the background-thread
   cadence.

## Evidence artifacts

- `/tmp/probe_resample3.py` — DC-gain measurement across ratios (Defect A).
- `/tmp/probe_resample2.py` — full-prototype magnitude response + per-phase
  energy (Defect B mechanism).
- `/tmp/probe_cutoff.py` — derivation of the correct `min(0.5, 1/(2·ratio))`
  cutoff.
- `/tmp/probe_resample7.py` — direct-form rewrite achieving unity DC gain and
  correct 22.05 kHz bandlimit on 96k → 44.1k.

(Scripts are reproducible from the filter constants in
`SonicMasterAnalysisEngine.cpp`; they are not checked in.)
