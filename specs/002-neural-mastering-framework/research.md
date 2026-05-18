# Phase 0 Research: Neural Mastering Framework

## Decision 1: Primary Architecture Direction

**Decision**: Use a hybrid deterministic-DSP plus learned low-rate control framework as the primary recommendation. The learned component should be a compact causal Temporal Convolutional Network (TCN) that consumes audio-feature windows and context metadata, then emits bounded mastering targets, parameter deltas, masks, plans, and confidence scores.

**Rationale**: Professional mastering requires predictable latency, bypass safety, CPU headroom, explainable behavior, and artifact control. More-Phi already contains deterministic mastering and audio-domain processors, while its constitution prohibits model inference on the audio callback unless future evidence proves bounded real-time safety. A compact causal TCN gives fixed compute, causal receptive fields, and controllable temporal context without requiring full-band neural audio generation.

**Alternatives considered**:

- **Causal WaveNet-style raw audio model**: Provides causal receptive fields but is too expensive and risky for full-band real-time mastering; useful only as inspiration for dilated residual blocks.
- **Full Transformer/attention model**: Useful for offline reference matching, text intent, genre metadata, and whole-track analysis, but variable and costly for real-time buffer-level processing.
- **U-Net or spectral masking model**: Strong for restoration or offline enhancement, but STFT latency, phase reconstruction risk, and CPU load make it unsuitable as the primary live mastering path.
- **Non-neural deterministic DSP only**: Safest runtime baseline and required fallback, but does not answer the requested neural framework study.
- **Diffusion or autoregressive enhancement**: High research interest but rejected for production real-time use because inference speed and determinism are unsuitable.

## Decision 2: Recommended Architecture Shape

**Decision**: Plan the recommended framework as five layers: feature extraction, compact causal TCN, optional context conditioning, constrained multi-head outputs, and deterministic safety/application layer.

**Rationale**: This shape lets the audit address spectral balance, dynamics, stereo imaging, and harmonic enhancement without requiring opaque neural audio-rate processing. The model can operate on features such as loudness, crest factor, spectral tilt, Bark/mel bands, dynamics envelopes, transient density, stereo correlation, phase width, and harmonic/noise indicators. Outputs can target EQ curves, multiband dynamics settings, stereo width controls, saturation/exciter drive, limiter goals, wet/dry amounts, and confidence gates.

**Alternatives considered**:

- **Single monolithic audio-to-audio model**: Simpler conceptually, but fails explainability and real-time risk requirements.
- **Separate model per mastering task**: Easier to analyze but increases orchestration complexity and can create inconsistent cross-objective decisions.
- **Rule-based assistant only**: Low risk but cannot learn reference-dependent or context-dependent behavior.

## Decision 3: Loss Functions and Objective Evaluation

**Decision**: Recommend a composite objective combining multi-resolution STFT magnitude loss, complex STFT or phase-aware loss, waveform L1/Huber loss, loudness-weighted spectral loss, mid/side stereo image loss, dynamics-envelope loss, identity/transparency regularization, true-peak penalty, and artifact penalties.

**Rationale**: Mastering quality cannot be captured by one metric. Magnitude-only losses can ignore phase and transient detail; waveform-only losses can over-penalize acceptable mastering differences; loudness-only objectives can reward louder but worse outputs. Mid/side and identity terms reduce stereo image damage and unnecessary processing.

**Alternatives considered**:

- **Adversarial-only objective**: Rejected because it can hallucinate artifacts and destabilize loudness.
- **Perceptual metric only**: Rejected because perceptual metrics are incomplete and can be gamed.
- **Mean squared waveform loss only**: Rejected because it poorly reflects transparent mastering quality.

## Decision 4: Training Strategy

**Decision**: Use staged research training: begin with conservative paired mix/master examples, expand with curriculum tasks, diversify genres and source quality, augment gain/EQ/dynamics/stereo conditions carefully, train an offline teacher, then distill to a small student for bounded CPU-friendly preview or control-plane deployment.

**Rationale**: A high-capacity teacher can explore quality ceilings while the student enforces deployment constraints. Curriculum learning reduces early instability and helps isolate whether the model learns useful mastering intent rather than loudness inflation. Distillation provides a measurable bridge from research quality to runtime feasibility.

**Recommended planning ranges**:

- Audio context: 1-8 seconds for student control planning; longer whole-track summaries for offline analysis.
- Learning rate: 1e-5 to 3e-4 with warmup and cosine decay.
- Weight decay: 1e-5 to 1e-3.
- Gradient clipping: 0.5 to 5.0.
- Effective batch: 8-64 audio chunks, adjusted by memory and sample rate.
- Validation split: leakage-resistant by source track, artist/session, plugin chain, genre, and scenario.

**Alternatives considered**:

- **Train only the final runtime model**: Simpler but gives no high-quality reference for distillation.
- **Synthetic augmentation without curated references**: Useful for robustness but insufficient for professional mastering quality.
- **Single random train/test split**: Rejected due to leakage risk across versions, stems, renders, and similar mixes.

## Decision 5: Real-Time Deployment Boundary

**Decision**: Treat direct neural inference in `processBlock()` as prohibited by default. Preferred deployment paths are offline audit, preview render, background analysis, message-thread recommendation, or low-rate control-plan output applied through existing safe handoff mechanisms.

**Rationale**: Neural runtimes can hide allocations, locks, thread scheduling, cache stalls, GPU synchronization, denormal slowdowns, and exceptions. A mastering plugin failure is immediately audible as clicks, dropouts, pumping, overs, or bypass discontinuity. Control-plane neural output allows deterministic DSP to remain the audio-rate source of truth.

**Alternatives considered**:

- **Inline audio-callback model**: Acceptable only after worst-case execution evidence proves fixed topology, fixed memory, no allocation, no locks, no I/O, no exceptions, and completion below the smallest supported buffer deadline.
- **Background model producing audio buffers**: Risky for live use because late buffers require fallback/interpolation and can create discontinuity.
- **Remote inference**: Rejected for real-time use due to latency, availability, privacy, and determinism concerns.

## Decision 6: Signal Integrity and Go/No-Go Gates

**Decision**: The audit must require falsifiable acceptance gates before any production neural deployment. Gates must include objective metrics, blind listening evidence, CPU/latency measurements, and explicit fail-safe behavior.

**Rationale**: Scientific confidence must be proportional to evidence. A model that improves one metric but damages phase, loudness, transient clarity, mono compatibility, bypass behavior, or CPU stability is not acceptable for professional mastering.

**Minimum gates for future proof-of-concept**:

- Identity set: median loudness drift under 0.25 LU and true-peak increase under 0.3 dB.
- Safety: no outputs above the configured true-peak ceiling and no NaN/Inf propagation.
- Stereo: median inter-channel phase/correlation change under the audit-defined threshold for material not requesting width changes.
- Listening: blinded expert review shows student output within the audit-defined tolerance of teacher/reference quality.
- Distillation: student reaches at least 95% of teacher composite validation score before runtime consideration.
- Performance: measured worst-case CPU and latency satisfy the smallest supported DAW buffer target with headroom.
- Bypass: active/bypass transitions remain click-free and equal-latency or explicitly compensated.
- Fallback: missing, late, invalid, or overloaded models hold last safe controls, fade to deterministic DSP, or transparent-bypass without audio interruption.
