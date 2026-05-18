# Quickstart: Neural Architecture Audit

Use this guide to validate the future audit deliverable produced from this plan.

## 1. Confirm planning inputs

- Read `specs/001-audit-neural-architecture/spec.md`.
- Read `specs/001-audit-neural-architecture/plan.md`.
- Read `specs/001-audit-neural-architecture/research.md`.
- Confirm the planned output remains a technical audit, not a runtime model implementation.

## 2. Verify required report structure

When `audit-report.md` is produced, compare it with:

- `specs/001-audit-neural-architecture/contracts/audit-report-contract.md`
- `specs/001-audit-neural-architecture/data-model.md`

The report must include the executive summary, modality audit, real-time audit, model-family comparison, recommendation, training plan, evaluation benchmarks, risk register, and next actions.

## 3. Validate model recommendation quality

Confirm the report:

- Selects one primary architecture and one fallback baseline.
- Evaluates Transformer-style, CNN, RNN/LSTM/GRU, GAN, Diffusion, and a simple baseline.
- Explains why the selected model is suitable for More-Phi's parameter/control recommendation task.
- Explains why raw audio generation is rejected, selected, or deferred.
- Defines hyperparameters, losses, training phases, and evaluation metrics.

## 4. Validate More-Phi safety alignment

Confirm the report:

- Keeps neural inference off the audio callback unless future proof is provided.
- Preserves normalized hosted-plugin parameter values and masks.
- Accounts for high-risk controls such as volume, pitch, bypass, and safety-limited parameters.
- Routes any future automation through control-plane handoff rather than direct hosted-plugin mutation.
- Provides graceful fallback guidance when a model is unavailable or uncertain.

## 5. Validate success criteria

The report is ready for planning review when:

- Stakeholders can identify the primary architecture, fallback baseline, and rejected alternatives within 30 minutes.
- All named data modalities are covered.
- At least four model-success thresholds are defined.
- Every proposed usage path is classified as permitted, constrained, or prohibited for real-time plugin use.
- The training plan includes at least three validation metrics and a leakage-resistant split strategy.
- Reviewers rate the recommendation as actionable with an average score of 4 out of 5 or higher.

## 6. Record residual risks

If any criterion cannot be satisfied, document the blocker in the report risk register before moving to implementation tasks.
