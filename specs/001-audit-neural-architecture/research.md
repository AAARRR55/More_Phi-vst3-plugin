# Research: Neural Architecture Audit

## Decision: Treat More-Phi's primary ML task as control/parameter recommendation, not raw audio generation

**Rationale**: More-Phi's core product value is morphing hosted-plugin parameter snapshots with physics, interpolation, MCP/AI control, and dataset generation. Existing project constraints emphasize normalized hosted-plugin parameter vectors, snapshot state, audio-feature extraction, and safe control handoff. Raw audio generation would be higher risk, harder to validate in a DAW, and unnecessary for the requested success criteria unless future evidence proves the product goal has changed.

**Alternatives considered**:
- Raw audio generation: rejected as primary because it is computationally heavy, difficult to make real-time safe, and not required for parameter morphing success.
- Pure text-to-preset generation: insufficient because it ignores audio features and plugin-specific parameter constraints.
- Pure audio-to-parameter inversion: useful but incomplete because More-Phi also needs intent, metadata, snapshots, and temporal control context.

## Decision: Use multimodal control representations

**Rationale**: The audit should evaluate scalar audio descriptors, optional mel-spectrogram summaries, normalized parameter vectors, parameter masks/types, time-varying automation, plugin metadata, text intent, and validation labels. Codebase evidence includes 31-float audio feature vectors, mel spectrogram tensors shaped around 128 bands by hundreds of frames in prototype assets, normalized parameter arrays up to 2048 entries, 12 snapshot slots, and MCP/LLM control-plane data.

**Alternatives considered**:
- Scalar audio features only: efficient but likely too weak for complex perceptual recommendations.
- Mel spectrograms only: richer but heavier and missing parameter semantics.
- Parameter vectors only: ignores the audio result and user intent.

## Decision: Recommend a compact hybrid multimodal temporal Transformer as the primary architecture

**Rationale**: A small attention-based model can combine heterogeneous context: audio-feature embeddings, plugin metadata, text intent embeddings, snapshot/parameter tokens, parameter masks, and short temporal windows. It can model dependencies between parameters and time-varying control decisions better than independent regressors while still being deployable off the audio thread as a low-rate control-plane model. The audit should recommend a shallow audio encoder feeding a compact Transformer policy/decoder that predicts bounded parameter targets, deltas, or automation trajectories.

**Alternatives considered**:
- CNN-only: strong for local spectral/audio patterns but weak as the full multimodal decision model.
- RNN/LSTM/GRU: easier to bound for streaming control but weaker for heterogeneous parameter dependencies and long-range context.
- GAN: not recommended as primary because training stability, controllability, and safety constraints are poor fits for parameter automation.
- Diffusion: useful for offline variation generation but too expensive and iterative for live control recommendations.
- MLP-only: efficient and useful as a baseline but less expressive for multimodal temporal context.

## Decision: Use retrieval plus constrained regression as the fallback baseline

**Rationale**: A nearest-example retrieval layer over audio features, plugin metadata, and text-intent embeddings can provide interpretable recommendations. A constrained regressor can predict bounded normalized parameter deltas while respecting masks, high-risk controls, and confidence thresholds. This baseline is easier to validate and should be the comparison point for any Transformer improvement.

**Alternatives considered**:
- Random forest or gradient boosting only: useful for tabular features but less suitable for variable-length temporal contexts.
- Rule-only heuristic planner: safe but likely insufficient for target usefulness.
- Full deep model only: too risky without an interpretable baseline and fallback.

## Decision: Keep all inference off the audio callback until separately proven safe

**Rationale**: More-Phi's constitution and architecture require the audio thread to avoid allocation, locks, I/O, blocking, exceptions, and model inference. Neural recommendations must be produced in background, MCP, message-thread, offline, or dataset contexts, then passed as bounded control data through existing queues, smoothing, and parameter-safety mechanisms.

**Alternatives considered**:
- Audio-thread inference: rejected for this audit because bounded non-allocating proof, runtime selection, and worst-case host validation are not established.
- Network-hosted inference for live control: rejected for real-time paths because network calls cannot satisfy audio-plugin determinism.
- Offline-only models: safe but may not satisfy future live-assistance goals; retained for larger model families and dataset generation.

## Decision: Recommended hyperparameter and output ranges

**Rationale**: The audit should propose ranges rather than single values until dataset size and target quality are measured. Recommended initial ranges are: 10-50 Hz control-rate features; 1-30 second context windows; 128-512 model width; 2-8 Transformer layers; 4-8 heads; 32-128 parameter/type embeddings; dropout 0.05-0.2; learning rate 1e-4 to 3e-4 with warmup and cosine decay; weight decay 1e-5 to 1e-3; batch size 32-256 depending on sequence length.

**Alternatives considered**:
- Larger generative models: deferred to offline variation generation due to latency and validation burden.
- Tiny MLP-only models: retained as fallback but not expressive enough for the main multimodal task.

## Decision: Use multi-objective losses and safety constraints

**Rationale**: More-Phi outputs mix continuous controls, discrete/binary/enumeration controls, masked parameters, safety-sensitive controls, and perceptual audio outcomes. The audit should recommend Huber or MSE for normalized continuous values/deltas, cross-entropy or focal loss for discrete choices, masked losses for unavailable controls, derivative/smoothness penalties for trajectories, constraint penalties for unsafe deltas, perceptual feature losses, and ranking/preference losses when human or expert comparisons exist.

**Alternatives considered**:
- Single MSE loss: rejected because it would mishandle discrete controls, masks, safety constraints, and perceptual quality.
- Pure perceptual loss: rejected because it does not enforce parameter safety or controllability.

## Decision: Evaluate by generalization, safety, latency, and usefulness

**Rationale**: Success must be measured with plugin-relevant outcomes: held-out plugin/source/scenario splits, parameter MAE/RMSE, discrete accuracy, trajectory derivative error, constraint violation rate, p50/p95/p99 inference latency outside the audio callback, memory/CPU budget, expert-reference agreement, pairwise preference or usefulness ratings, and audio-feature target error.

**Alternatives considered**:
- Accuracy-only evaluation: rejected because user trust depends on safety, latency, and perceptual usefulness.
- Listening tests only: valuable but too subjective without repeatable quantitative metrics.
