# Implementation Plan: SolidStateBusComp Neural Extension (EQ + Mastering Moves)

**Branch (proposed)**: `006-solidstatebuscomp-neural-extension` | **Date**: 2026-06-29
**Parent spec**: [003-neural-mastering-roadmap](../003-neural-mastering-roadmap/plan.md) (extends `HybridMasteringNet`, not a parallel path)
**Runtime posture**: offline training / validation only. No model inference, I/O, allocation, locks, or learned audio in `processBlock()`.

---

## 0. Reframe: what the dataset is, and what that implies

`amphion/SolidStateBusComp` = the **Diff-SSL-G-Comp** dataset (*"Towards a Large-Scale and Diverse Dataset for Virtual Analog Modeling"*, Gu et al., arXiv:2504.04589).

| Property | Verified value |
|---|---|
| Input | 175 real *unmastered* songs (Cambridge Multitrack Library) |
| Parameter space | **220 combinations** of `(threshold, attack, release, ratio)` |
| Volume | **2,528 hours** |
| Annotated controls | **threshold, attack, release, ratio** only — device-labels, not DAW-labels |
| Layout | `processed_ground_truth/threshold_X_attack_Y_release_Z_ratio_W/<song>-exported.wav` |
| License | **CC-BY-NC** + Cambridge Multitrack constraints; gated access (must accept terms) |
| Native task | Controllable virtual-analog **bus-compressor** modeling |

**Three consequences that drive every decision below:**

1. **EQ is not in the dataset.** "EQ changes to the dataset" must be *injected* (augmentation, conditioning axis, or rendered target). Treating EQ as something to "extract" is ill-posed.
2. **"Mastering moves" reduce, at first, to one move: bus compression** with a 4-parameter control vector. Broader mastering (EQ, multiband, imaging, limiting) must be *synthesized* via More-Phi's existing DSP chain, not mined from this corpus.
3. **The 4 compressor params are already a fixed-size control plan** — a near-perfect fit for 003's `NeuralMasteringPlan` schema (control vector + mask + confidence + evidence). This is a *parameter-conditioned* model, which is better-posed and more controllable than the end-to-end "unmastered→mastered" target in 003 §1.1.

**Net positioning:** extend `HybridMasteringNet` with a **parameter-conditioning head** (FiLM) to turn it into a controllable virtual-analog compressor; layer **EQ as augmentation first, then as a co-conditioning axis** via synthetic pairs; and define **mastering moves** against the existing `AutoMasteringEngine` processor vocabulary. Reuse `generate_mastering_dataset.py` / `train_neural_mastering.py`; do not fork them.

---

## 1. EQ Integration Strategy

The question "augmentation vs. input-space vs. learned transformation" has a correct ordering: **augmentation first (cheap, always-on), then conditioning axis (paired), never free-standing learned EQ (no target).**

### 1.1 EQ as data augmentation (Phase A — do this immediately)
Random smooth EQ curves applied to the *input* waveform only, with the ground-truth output left untouched relative to the original input. Purpose: **invariance/robustness**, not new targets.

- Implementation: reuse the spectral-balance perturbation already in `generate_mastering_dataset.py` §2.4 ("smooth low/mid/high EQ curves in the frequency domain"). Wrap it as a training-time transform with bounded magnitude (±6 dB at anchor points, Q-controlled, minimum-phase to avoid pre-ring).
- Must be **only on the input stem**. The (input, target) pair stays causally consistent only if the target is re-derived through the *same* augmentation before the compressor — see 1.2. Pure input-only EQ turns the task into "denoise the EQ," which is the wrong inductive bias for a compressor model. Resolve this in 1.2.

### 1.2 EQ as a co-conditioning axis (Phase B — the real extension)
Promote EQ from background noise to a **first-class control** by rendering it into the target through a synthetic chain:

1. Take `processed_normalized/<song>` as the source.
2. Sample an EQ parameter vector `e = (low_shelf_gain, low_shelf_freq, mid_bell_gain, mid_bell_freq, mid_bell_Q, high_shelf_gain, high_shelf_freq)` from a bounded, musically-plausible distribution.
3. Apply EQ → bus-compress with a sampled `(threshold, attack, release, ratio)` from the dataset's 220 grid → limit to a true-peak ceiling.
4. Store `(eq_conditioning=e, comp_conditioning=c, input=normalized, target=rendered)`.

The model is then **conditioned on both `e` and `c`** via FiLM (§3.1). This turns a compressor-only dataset into a **controllable EQ+comp model** with paired, sample-accurate targets — the only well-posed way to "learn EQ" from a corpus that has none natively. The cost is that the EQ→target mapping is *synthesized* (algorithmic, from More-Phi's `AdaptiveEQ`), so the model learns to imitate our DSP, not a human engineer. That is an acceptable, explicitly-labeled ceiling (mark with `ponytail:` provenance: `synthetic-eq` in the manifest).

### 1.3 Learned EQ *as the model's internal behavior* — defer
"Learned transformations within the architecture" (e.g., a differentiable EQ stage the network discovers) is **deferred**. With no human-EQ'd targets, the network has no signal to discover human intent; it would at best rediscover the synthetic chain in 1.2 opaquely. Revisit only if/when professional EQ'd reference pairs are licensed (see 003 §2.1 professional-paired mode).

**Recommendation:** A (augmentation) now → B (conditioning axis) as the headline extension → C (learned) deferred until paired human data exists.

---

## 2. Parameter Optimization Framework

Full grid/architecture search is infeasible: each run over 2,528 h is days of GPU time, and only 175 songs make overfitting the dominant failure mode. Optimize the *few* parameters that matter, with *multi-fidelity* search.

### 2.1 What actually to tune (ranked by expected payoff)

| Tier | Parameter | Why it matters here |
|---|---|---|
| **T1 — conditioning** | Conditioning injection: FiLM vs AdaLN vs concat | Determines whether the model is genuinely *controllable* or ignores `c`. Highest-payoff for a VA task. |
| **T1** | Conditioning encoding of `(threshold, attack, release, ratio)` | Device-label values have a non-uniform distribution; raw normalize vs. per-param standardize vs. embedding for discrete `ratio`. |
| **T2 — capacity** | Bottleneck depth/width (6 layers / d=384), total downsampling (×1024→256 steps) | Governs long-range gain-reduction memory (compression is *history-dependent* — the envelope follower makes output a function of the past ~100 ms+). |
| **T2** | Residual-delta bound (`tanh` scale) | Compressor gain reduction is bounded; too loose a bound → artifacts, too tight → can't represent deep GR. |
| **T3 — training** | Loss weights (the 8 losses in 003 §1.4), LR + warmup, batch × grad-accum, precision | Standard, but loss-weight balance is dataset-specific. |
| **T3** | Augmentation magnitudes (EQ ±dB, transient, width) | The regularization that actually fights 175-song memorization. |

### 2.2 Technique — and why each

- **Bayesian optimization (TPE / CMA-ES) via Optuna, with ASHA/Hyperband early-stopping** — *not* grid search. Rationale: each config is expensive; multi-fidelity BO runs many configs at low fidelity (1–2 epochs on a song subset) and only promotes survivors. CMA-ES is already proven in-house (T2 teacher, per memory). Use it.
- **A small, hand-designed search space, not NAS.** Full NAS over a waveform U-Net+Transformer is unjustifiably expensive. Search around the conditioning head + bottleneck width only; fix the rest to the 003 baseline.
- **Curriculum as a fidelity ladder:** fidelity-0 = 8 songs × 1 param setting (smoke); fidelity-1 = 40 songs × 20 settings; fidelity-2 = full. Hyperband's successive-halving maps directly onto this.
- **Population-based training (PBT)** as an optional second pass on T3 (LR, loss weights, augmentation) once T1/T2 are fixed — it tunes *during* training rather than restarting.

### 2.3 Decisions you do **not** search
Fix these from 003 to avoid wasted runs: mid/side input stem, multi-resolution STFT + waveform L1 backbone, segment length 262,144, residual prediction, no `processBlock()` inference.

---

## 3. Mastering Moves Learning Approach

### 3.1 Definition
A **mastering move** = one bounded, parameterized operation in the More-Phi chain, drawn from the existing processor vocabulary (003 research.md Decision 4): bus-compression `(threshold, ratio, attack, release)`, `AdaptiveEQ` band gains, `MultibandDynamicsProcessor` thresholds/ratios, `StereoImager` width, `HarmonicExciter` drive, `BrickwallLimiter` ceiling, `LoudnessNormalizer` LUFS target. A **move sequence** = an ordered application. Each move is a fixed-size slice of the `NeuralMasteringPlan` schema (control vector + mask + confidence), so the Safety Layer (003) projects it before any audio-rate use.

### 3.2 Two formulations — pick by task, run both as an ablation

**F1 — Control-plan regression (black-box, primary).** Model ingests audio features → predicts a bounded control plan `p = f(audio)`; the *existing deterministic DSP* applies it. This is 003's native posture (planner outside the callback, Data Plane applies). For SolidStateBusComp specifically, the "plan" is the 4-vector `c`, and the target is the device's rendered output — so F1 trains the *control head* against an audio-reconstruction loss through a frozen differentiable proxy of the compressor. Generalizes to full mastering moves once §1.2 EQ conditioning and the other processors are wired in.

**F2 — Differentiable gray-box compressor (ablation / strong baseline).** Because the dataset is *literally* a hardware compressor's input→output, model it as a differentiable envelope-follower + gain-computer (a handful of learnable params: time constants, knee, ratio curve) instead of a 2M-param U-Net. Far fewer parameters, fully interpretable, trivially real-time, and an excellent **lower bound** for F1 to beat. Limitation: models compression only, can't absorb EQ/imaging. Use it to (a) sanity-check that black-box gains aren't from rediscovering a compressor, and (b) ship a zero-risk deterministic fallback.

**Recommendation:** F1 as the production path; F2 as the always-on baseline and the constitution-compliant fallback. Never ship a learned move without F2 as the floor and the Safety Layer as the ceiling.

### 3.3 What "learning the move" means concretely
Given an input, the model should recover the `(threshold, attack, release, ratio)` that the device used, *and* predict the resulting audio. Train both heads jointly: an **inverse** head (audio → param regression, MSE on normalized params) and a **forward** head (audio + param → audio, multi-res STFT). The inverse head is a cheap, strong auxiliary signal and gives you a directly-evaluable "did it identify the compression?" metric.

---

## 4. Training Methodology

### 4.1 Data preparation — the dual-axis split (the critical domain-specific step)
With 175 songs × 220 settings, leakage is severe if segments of the same song appear in train and test. **Split on song identity**, and additionally carve a **second axis**:

- **Song split (generalization to new material):** 140 train / 18 val / 17 test songs. All 220 settings for a song stay in that song's split.
- **Parameter split (generalization in control space):** within the val/test songs, hold out specific param combinations entirely (e.g., extreme `ratio` × slow `attack` never seen in training) to measure **extrapolation**, not just interpolation.
- Report both axes separately. A model that interpolates seen params on seen songs but fails on held-out songs has memorized; one that fails on held-out params isn't truly controllable.

Practical access: 2,528 h (~2.6 TB @ 48 k stereo) is not pullable wholesale. Use the `datasets` library in **streaming mode** with `${HF_TOKEN}` (gated CC-BY-NC) and a per-song index; curriculum stages (§2.2) consume subsets on demand.

### 4.2 Conditioning normalization
`(threshold, attack, release, ratio)` are device-labels with skewed ranges (release spans 0.1–∞ ms, ratio is near-discrete). Encode as: continuous params → per-param standardization on the 220-grid empirical mean/std; `ratio` → embedding (it takes few discrete values). Inject via **FiLM** (γ/β per encoder stage) — the standard for controllable audio synthesis; AdaLN is the heavier alternative if FiLM underperforms in the T1 search.

### 4.3 Losses
Reuse the 003 §1.4 suite (multi-res STFT convergence + log-mag L1, waveform L1, mid/side L1, transient derivative L1, loudness proxy, stereo correlation, VGG feature match) and add three compressor-specific terms:

- **Gain-reduction-matching loss:** compare the model's implicit GR curve to the device's (envelope of `target/input` in dB). This is the compression-specific signal that pure STFT loss under-weights.
- **Identity loss:** when conditioning = bypass, output ≈ input (penalize coloration at null settings). Cheap regularizer, big trust payoff.
- **Inverse-head param MSE** (§3.3) as an auxiliary.

### 4.4 Validation strategy
- Held-out **songs** (F1 must generalize) + held-out **param combos** (must extrapolate).
- Metrics: SI-SDR, multi-res STFT, log-spectral distance, loudness error, **GR-curve EMD**, plus external PEAQ/VISQOL on a small subset (003 §1.5 templates).
- A **per-parameter ablation sweep**: vary one of `threshold/attack/release/ratio` at inference, plot output loudness + GR vs. the device's — the curves must be monotonic and track the device. This is the single most convincing "it learned the move" evidence.
- Constitution gate: no checkpoint promotes to runtime behavior until 003 §3.4 release gates pass (provenance, leakage audit, objective + perceptual + listening).

---

## 5. Implementation Considerations

### 5.1 Compute
- **Storage/I/O:** streaming dataset (§4.1), on-the-fly decode, no full-materialization. Cache decoded segments to a local scratch only for the curriculum subset.
- **GPU memory:** 262,144-sample stereo segments × U-Net with d=384 activations are large → **gradient checkpointing** on encoder stages + the transformer bottleneck; batch via grad-accum. BF16 autocast (003 §3.1).
- **Fidelity budget:** bound total BO trials by wall-clock, not by count; ASHA kills losers at fidelity-1 so most cost concentrates on survivors.

### 5.2 Overfitting (the dominant risk — only 175 songs)
Mitigations, in priority order: (1) **song-level split** (non-negotiable); (2) **augmentation** (§1.1 EQ + transient + width); (3) held-out param combos as a true generalization probe; (4) dropout + weight decay in the transformer; (5) early-stop on held-out-song SI-SDR; (6) the F2 gray-box prior (§3.2) as an inductive-bias floor.

### 5.3 Generalization to new audio
The virtual-analog framing generalizes *better* than end-to-end mastering because it's a well-defined input→output *function* of explicit controls, not a stylistic transfer. Validate on out-of-distribution songs (different genres than Cambridge) before any quality claim.

### 5.4 License & governance (hard constraint)
- **CC-BY-NC + Cambridge Multitrack**: research/evaluation use only. **Trained weights derived from this corpus cannot ship in a commercial More-Phi build** without separate licensing. The corpus is for *research, ablation, and baseline evidence*; production mastering models must train on license-clear material (see 004-dataset-curation split-leakage policy). Record this in the model card.
- **Token governance:** the dataset is gated; authenticate via `${HF_TOKEN}` read from the environment at runtime. **Never hardcode the token; never commit it.** (A token was shared in plaintext during planning — treat as compromised, rotate it.)

### 5.5 Real-time / constitution alignment
All learning stays offline. Learned output enters the plugin only as a bounded `NeuralMasteringPlan` that passes the Safety Layer projection; the audio thread consumes only deterministic processor state. No new `processBlock()` behavior is introduced by this spec.

---

## 6. Phased milestones

| Phase | Deliverable | Gate to next |
|---|---|---|
| **P0** ✅ | Manifest builder (`build_solidstatebuscomp_manifest.py`) — dual-axis split + G10 audit reuse + HF staging (`--materialize`) + `run_training_waveform.sh`; selfcheck green | Manifest passes split-leakage check — DONE |
| **P1** ✅ | F2 gray-box differentiable compressor (`graybox_compressor.py`, 5 params) + trainer (`train_graybox_compressor.py`) + `run_training_graybox.sh`; selfcheck + trainer smoke green (monotone threshold/ratio, exact identity at ratio=1, stereo-linked, finite loss) | Beats naive (gain-only) baseline on GR-EMD — training runs on Lightning |
| **P2** ✅ | FiLM-conditioned `HybridMasteringNet` (`conditioned_mastering_net.py`, 15.5M params) + inverse param-inference head + trainer (`train_conditioned_master.py`: 8-loss suite + GR-curve + inverse-MSE); val reports SI-SDR split by `holdoutAxis` (interp vs extrap); selfcheck + smoke green | SI-SDR ≥ F2 on held-out songs **and** held-out params — training runs on Lightning |
| **P3** ✅ | EQ augmentation (§1.1) baked into the loader: `eq_augment.py` (3-section RBJ min-phase biquad cascade via `torchaudio.functional.lfilter`, primed, identity@0 dB, selfcheck green) + input-only aug in F1 `CondDataset` + `validate_under_eq` probe (fixed ±6 dB grid, SI-SDR split by holdoutAxis, separate metric keys — never `vl['loss']`) + optional eval-only F2-proxy behind `--eq-proxy`; runner fixed (`train_conditioned_master.py`→`…mastering.py` P2 typo) + `EQ_AUGMENT`/`EQ_AUGMENT_DB`/`EQ_PROXY` envs; F2 unaugmented by design. smoke + probe/proxy exercise green | Robustness: held-out-song metric holds under ±6 dB EQ perturbation — training runs on Lightning (compare augmented vs un-augmented `si_sdr_robust_drop`) |
| **P4** ✅ | EQ as co-conditioning axis (§1.2): synthetic POST-DEVICE target `peak_normalize(min_phase_eq(device_target; e), 0.98)` rendered inline in `CondDataset` (no new DSP, no manifest change); `cond` 4→8-dim `[c, e]` (net `cond_dim` default 4→8, fans to FiLM + inverse head); `compute_stats` analytical e-stats (q mean **1.0**, not 0 — the load-bearing invariant); `validate_eq_sweep` P4 gate (sweep `e`, unperturbed input, SI-SDR vs the synthetic target + band-energy monotonicity + `flat_check_drop` continuity catcher); inverse head masked to c-only. net selfcheck + smoke + sweep/probe exercise green (15.456M, `flat_check_drop≈0`) | Predictable output response to `e` conditioning sweep — training runs on Lightning (gate: high per-`e` SI-SDR, `mono_*_up_frac→1`, `flat_check_drop≈0`) |
| **P5** | Multi-move control plan (full processor vocabulary) on license-clear data; Safety-Layer integration; model card | 003 §3.4 release gates |

P0–P2 use SolidStateBusComp (CC-BY-NC, research only). P5 migrates to license-clear material for anything that could ship.

---

## 6.1 Lightning training & line reconciliation

**Two model lines coexist; this spec owns the waveform line (they are complementary, not duplicates).**

| Line | Trainer | Manifest | SolidStateBusComp role | Status |
|---|---|---|---|---|
| Control regressor (F1 control-plan variant) | `control/train.py` | `control/build_manifest.py` → `train.jsonl` (feature→delta) | eval **feature input** (`manifest_ssl_eval`) | trained on Lightning (Model A) |
| Waveform forward model (F1 black-box, this spec) | `train_conditioned_master.py` (FiLM-conditioned `conditioned_mastering_net.py`) | `build_solidstatebuscomp_manifest.py` → paired `inputPath/targetPath` + per-item controls | **paired ground truth** (input → 220 hardware outs) | P2 done |

**Lightning staging model** (mirrors the established `control/run_training_t2.sh`): audio is fetched from the gated HF dataset into `data/` on the studio **before** training; `${HF_TOKEN}` is injected as a Lightning secret (never committed); `train_neural_mastering.py` then loads local files via `torchaudio.load` unmodified. `run_training_waveform.sh` wires build → stage → train. The full corpus is ~2.6 TB, so `--max-songs` caps a curriculum subset.

## 6.2 P3 outcome — §1.1 tension resolved

A design workflow (4 readers → 3 proposals → 3 judges → synthesis) converged unanimously, verified line-by-line against the code, on this resolution:

- **Augmentation = input-only minimum-phase EQ, target left as the clean device output.** `generate_mastering_dataset.py::smooth_eq` is a *zero-phase* FFT-multiply (`rfft × real gain → irfft`, confirmed at lines 118–134) and was **rejected** — it pre-rings, and a pre-ring transient is itself a memorizable tag for a 15.5M-param net. Its replacement is a 3-section RBJ biquad cascade (low-shelf @180 Hz, peaking bell @1 kHz — the only true Q knob, high-shelf @6 kHz), minimum-phase by construction (stable poles inside the unit circle), anchor centres lifted from `smooth_eq`'s log-freq grid.
- **The §1.1 tension ("input-only EQ = denoise-the-EQ") is accepted, not resolved.** The compressor D and spectral shaping A do not commute (D's detector reads per-sample |x|), so `(A(x), D(x))` is not one coherent device map; P3 regularizes toward *local EQ invariance* — a robustness proxy, not robustness itself. The causally-correct fix `D(A(x))` is **uncomputable** here (the SolidStateBusComp hardware is unreachable) and is deferred to **P4** (synthetic chain). Minimum-phase was chosen partly to keep this bias in-distribution (analog-style coloration) rather than adding zero-phase pre-ring that looks like a foreign device artifact.
- **Implementation choice:** `torchaudio.functional.lfilter` (existing dep, fast causal IIR) over a hand-rolled Python DF1 loop — the loop is O(T) per section in Python (~0.8 s/segment), unusable in a DataLoader worker. One real bug was caught and fixed en route: `lfilter` clamps output to [−1,1] by default (param `clamp`, renamed from `clamp_ct` across versions), which saturates any boost whose impulse exceeds 1.0 — `eq_augment.py` disables it version-robustly.
- **F2 is unaugmented by design** (no EQ parameters in its topology; input-only EQ would spike its loss from topology mismatch). The P3 gate is F1-only.
- **Gate** (`validate_under_eq`, separate `eq_probe` keys, never `vl['loss']`): held-out-song SI-SDR under a fixed ±6 dB grid, split by `holdoutAxis`. `si_sdr_robust_drop` is meaningful only compared against the **un-augmented baseline's** floor — good `robust_drop` can coexist with the model having learned to invert EQ (measurement ceiling). Optional eval-only **F2-proxy** (`--eq-proxy`) bounds how device-relevant (GR-coupled) the EQ delta is vs "denoise-the-EQ" waste — never a training target.

## 6.3 P4 outcome — EQ promoted to a co-conditioning axis; §1.1 tension dissolved (half)

A design workflow (4 readers → 3 proposals → 3 judges → synthesis) confronted an **irreducible trilemma**: no topology achieves all of {dissolves the plan's *pre*-compressor EQ causality, continuous at e=identity, no new compressor DSP}. The device is unreachable, so the causally-correct target `device(EQ(input; e); c)` is uncomputable. The three candidates:

- **POST-EQ/DEVICE** (chosen): `target = peak_normalize(min_phase_eq(device_target; e), 0.98)` — apply the EQ to the *existing real device target*. Zero new DSP. Continuous at e=identity by construction (min_phase_eq is passthrough at 0 dB). The compressor in the chain *is* the real device.
- **PRE-EQ/REFCOMP** (rejected): `target = limit(ref_comp(EQ(input; e); c))` — matches the plan's literal §1.2 step-3 ordering, but **discontinuous at e=identity** (`ref_comp(input; c) ≠ device(input; c)`; the reference compressor cannot be device-calibrated). This would force FiLM to spend capacity bridging two compressor identities at the anchor rather than learning the EQ axis — structurally fatal for the sweep gate. Its only reuse path (`bus_compress`) is a verified trap: 2-param (threshold/ratio) with attack/release frozen into an `avg_pool1d` window — it would silently drop 2 of the 4 conditioning axes.
- **PRE-EQ/F2** (rejected): blocked — F2 is untrained (P1 not launched) and §3.2 warns against F2 as an F1 target (black-box collapse).

**Chosen: POST-EQ/DEVICE**, because *e-continuity at identity* is the empirically-verifiable, load-bearing property for both the P4 sweep gate and downstream coherence, and it costs zero new DSP. It **half-dissolves** the §1.1 tension, honestly: it kills the label↔target *incoherence* (the same single `e`-draw renders the target AND labels the cond, so the objective flips from EQ-*invariance* to EQ-*responsiveness* — the model is told to PRODUCE `EQ(device_target; e)`), but it does not achieve the plan's preferred *pre*-compressor ordering. The residual ceiling is **EQ/comp non-commutativity** (post-comp EQ ≠ pre-comp EQ: a post-comp +6 dB shelf provokes no gain reduction a pre-comp shelf would), labeled provenance `synthetic-eq`. POST-EQ's residual is strictly preferable to PRE-EQ's e=0 discontinuity given the unreachable device.

**Deviation from §1.2's "via `AdaptiveEQ`":** the synthetic EQ uses `eq_augment.min_phase_eq` (the P3 Python torch EQ), NOT the C++ `AdaptiveEQ` (juce::dsp, unreachable at Python train time). The limiter in §1.2 step 3 is dropped (it would destroy e=identity continuity; `peak_normalize(0.98)` bounds extreme-e peaks instead — a labeled secondary ceiling).

**Implementation notes (the load-bearing subtleties):**
- `cond` is 8-dim `[c(4), e(4)]`, **c-first** (pinned; reordering would feed EQ gains to the F2 proxy as compressor params). `cond_dim` is the single point of truth in the net (fans to FiLM + inverse head).
- **e-stats are analytical** (e is drawn per-epoch, never stored): `mean_e=[0,0,0,1.0]`, `std_e=[max_db/√3]×3 + [1/√12]`. **q mean is 1.0, NOT 0** — `sample_eq_gains` draws `q~U(0.5,1.5)`, so the identity `q=1.0` is the distribution centre; mean_q=0 would break the `e=identity ↔ cond_norm=0` anchor that FiLM identity-init and the gate's `flat_check_drop` rely on.
- **Inverse head masked to c-only** (`inv_loss = mse(inv[:, :4], cond[:, :4])`): recovering the EQ tilt (a deconvolution) from the synthetic target is harder/less stable than recovering c; promote to full 8-dim supervision only if e-recovery proves stable on the first run.
- **Checkpoint-format break** (4→8-dim): one-way; P4 starts fresh. Downstream readers of `cond_stats.json` that rebuild a `[B,4]` cond will break (flagged; out of scope here). To reproduce a strict P2/P3 run, `git checkout` the pre-P4 commit.
- **`validate_eq_sweep`** (P4 gate, sibling of `validate_under_eq`): fix `c`, sweep `e` over `eq_probe_grid`, unperturbed input, SI-SDR vs the matching synthetic target + band-energy monotonicity (`mono_*_up_frac`) + `flat_check_drop≈0` wiring catcher. The P3 `validate_under_eq` probe is kept side-by-side (input-robustness at e=identity).

**Forward path to close the residual ceiling:** if P5 licenses a real device render OR a device-calibrated reference compressor becomes available, re-derive via PRE-EQ/REFCOMP to convert the half-dissolution into full pre-compressor causality — but only then; today the e=0 discontinuity of PRE-EQ is worse than POST-EQ's ordering-relabeled bias.

## 7. Open questions (decide before P2)

1. Does the team have **license-clear** EQ'd/mastered pairs for P5, or is P5 blocked on data acquisition? (003 professional-paired mode.)
2. FiLM vs AdaLN — confirm via the T1 search rather than assume; CMA-ES already in-house.
3. Is the goal a **shippable** mastering model (needs license-clear data) or a **research/baseline** artifact (SolidStateBusComp suffices)? This determines whether P5 is in-scope now or deferred.
