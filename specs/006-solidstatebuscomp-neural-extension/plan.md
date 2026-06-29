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
| **P0** | Streaming loader + song-level + param-level split manifest; provenance/leakage audit (reuse 004 tooling) | Manifest passes split-leakage check |
| **P1** | F2 gray-box differentiable compressor baseline; per-param monotonicity evidence | Beats naive (gain-only) baseline on GR-EMD |
| **P2** | `HybridMasteringNet` + FiLM conditioning (black-box F1); inverse + forward heads; 8 + 3 losses | SI-SDR ≥ F2 on held-out songs **and** held-out params |
| **P3** | EQ augmentation (§1.1) baked into the loader | Robustness: held-out-song metric holds under ±6 dB EQ perturbation |
| **P4** | EQ as co-conditioning axis (§1.2); synthetic-pair generation via `AdaptiveEQ` | Predictable output response to `e` conditioning sweep |
| **P5** | Multi-move control plan (full processor vocabulary) on license-clear data; Safety-Layer integration; model card | 003 §3.4 release gates |

P0–P2 use SolidStateBusComp (CC-BY-NC, research only). P5 migrates to license-clear material for anything that could ship.

---

## 7. Open questions (decide before P2)

1. Does the team have **license-clear** EQ'd/mastered pairs for P5, or is P5 blocked on data acquisition? (003 professional-paired mode.)
2. FiLM vs AdaLN — confirm via the T1 search rather than assume; CMA-ES already in-house.
3. Is the goal a **shippable** mastering model (needs license-clear data) or a **research/baseline** artifact (SolidStateBusComp suffices)? This determines whether P5 is in-scope now or deferred.
