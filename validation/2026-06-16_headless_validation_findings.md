# Headless Validation & Audit Findings — 2026-06-16

Independent technical audit + empirical validation of the **built** MorePhi v3.3.0
binary (build `windows-msvc-release`), run headless. This complements the earlier
`docs/TECHNICAL_AUDIT_REPORT.md` (2026-02-19) with code-level + measured evidence.

- **Hardware:** 11th Gen Intel Core i7-11370H, 4 cores / 8 threads, 3.30 GHz base
- **OS:** Windows 11
- **Validators:** pluginval 1.0.4; project's `MorePhiBenchmarks.exe`,
  `MorePhiCLI.exe`, `MorePhiMcpServer.exe`, `MorePhiTests.exe`
- **Scope:** Windows/VST3 only (no macOS/AU this session)

---

## 1. VST3 validation — pluginval

| Strictness | Result |
|---|---|
| **5** (project target) | ✅ PASS (exit 0) |
| **10 (MAX)** | ✅ PASS (exit 0) — incl. *Fuzz parameters*, *Parameter thread safety*, *Background thread state* |

Covers: cold/warm open, editor open/close during processing, audio across
{44.1, 48, 96 kHz} × {64…1024}, state save/restore, automation, editor automation,
bus configuration. Reported: 12 programs, surround up to 7.1, sidechain bus,
single-precision, latency **0** (default state).

## 2. Core DSP benchmark — `MorePhiBenchmarks.exe` (10 runs, i7-11370H)

| Subsystem | Avg | Throughput |
|---|---|---|
| Interpolation (256 params) | ~0.07–0.08 µs | ~3.1–3.8e9 **params/s** |
| Elastic physics | ~0.06 µs | ~1.5e7 **updates/s** |
| Drift physics (Perlin) | ~0.13 µs | ~7.0e6 **updates/s** |
| 2D interpolation | ~0.40 µs | ~6e8 **params/s** |
| NeuralMasteringController | ~2.1 µs | ~4.7e5 **calls/s** (tagged *outside callback*) |

- **RT load (Physics+Interp only):** 48 k/256 mean **0.0063 %** (range 0.0048–0.0087);
  44.1 k/64 mean **0.0229 %** (range 0.017–0.031). Single-run figures land at the
  high end of the distribution — cite the mean ± range, not a single sample.
- **Throughput units differ by row** (params/s vs updates/s) — do not compare across rows.
- **Memory (sizeof-based, not runtime):** SnapshotBank inline 16.4 KB + heap slots
  **96.8 KB** + morph buffers 16 KB = **129.2 KB** core (excludes JUCE/MCP/hosted plugin).
- **Note:** the built binary predates in-flight `fix/core-review-criticals` source
  changes (e.g. ENHANCERS-1 exciter oversampling); numbers reflect the tested build.

## 3. MCP server robustness — standalone `MorePhiMcpServer.exe`

**20/20 adversarial checks passed:** spec-compliant error codes (−32700/−32600/−32601/−32602),
notifications correctly silent, **IPC two-key safety gate** (requires *both*
`allow_unsafe_write:true` arg *and* `MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE=1` env),
2 MB single-line request handled (**53 ms** server round-trip; client serialization
excluded; baseline <1 ms), 200-request pipelined burst all answered in order.

Scope: the **standalone** stdio server intentionally has no bearer-auth/rate-limit/
size-cap (it's a local CLI pipe you invoke yourself). Those live in the **embedded**
TCP server (§4).

## 4. Embedded MCP server security — via integration tests

The embedded TCP `MCPServer` does **not** start in headless batch mode (its startup
is a message-thread/DAW-coupled trigger — no listener on 30001–30030, no token in
CLI output). Behavioral validation comes from the project's integration tests, which
instantiate `MCPServer` directly:

| Test | Result |
|---|---|
| #175 "initialize rejects missing bearer token" | ✅ PASS |
| #176 "initialize succeeds with instance auth token" | ✅ PASS |
| #90 "TokenOptimizer rate-limit bookkeeping" | ✅ PASS |
| #84 "safe action rejects locked controls" | ✅ PASS |

**Residual (code-read only — no dedicated tests found):** the 256 KB request cap,
`MAX_CONNECTIONS` limit, and the *constant-time property* of the bearer compare
(functional auth is tested; timing side-channel is not).

## 5. Headless hosting

- `MorePhi.vst3` loads cleanly in a **JUCE host** (`MorePhiCLI` /
  `AudioPluginFormatManager`): 2118 parameters, exit 0.
- `pedalboard` 0.9.16's scanner **fails** to load it (construction-time
  MCP/plugin-scanner/GUI side-effects) — this is **pedalboard-scanner-specific,
  not a hosting defect**. pluginval + a JUCE host + (by design) DAWs all load it.

## 6. Mastering-chain routing — key architectural clarification

The mastering processors are **not in the default `processBlock` audio path**.
Default path: drain queue → MIDI → morph → modulation → `ParameterBridge` →
hosted plugin → output gain; `AutoMasteringEngine::analyzeBlock` runs as
**measurement only**. The processors **apply** audio only when a validated
neural-mastering plan executes (gated by `NeuralMasteringSafetyPolicy`).

- **Empirical:** default render with no hosted inner plugin = **bit-exact silence**
  (wet −240 dB from −14.6 dB pink noise). Mastering-chain audio/latency cannot be
  measured via default render — needs a hosted plugin + a triggered plan
  (DAW/MCP context) or a custom plan-driving harness.
- **Implication:** default-operation CPU is cheap; heavy mastering/oversampling
  DSP engages only on plan execution. (Corrects any reading that treats the
  mastering chain as an always-on audio stage.) Reflected in `docs/ARCHITECTURE.md`.

## 7. Documentation / source corrections applied this session

- `src/Core/SnapshotBank.h`: "~384 KB" → "≈ 96.8 KB" (2 places) — stale from when
  `MAX_PARAMETERS` was larger; verified via `sizeof` in the benchmark memory test.
- `src/Core/HarmonicExciter.h`: Padé tanh "accurate < 0.1 % for |x| < 4"
  → "~2.1 % worst-case over |x| ≤ 4; diverges for |x| > 4" (2 places).
  (Measured: x=1 → 2.13 %, x=2 → 2.09 %, x=3 → 0.49 %, x=4 → 0.65 %.)

## 8. Open / not closed this session

- **Mastering-chain audio A/B + latency-under-plan** — architecturally gated
  behind plan execution; needs a DAW session or a custom plan-driving C++ harness.
- **Direct DAW smoke test** across FL/Reaper/Ableton/Cubase (pluginval S10 is the
  proxy; per-DAW run not done). FL small-stack case already handled per build config.
- **Coverage report** — the 28 % LOC test/src ratio is a proxy, not measured coverage.
- **macOS/AU (`auval`)** — Windows-only session.

## Bottom line

On everything measurable headlessly, MorePhi v3.3.0 clears the production-stability
bar: **pluginval S5 + S10 PASS**, core DSP **~0.006–0.023 % CPU**, MCP robust
(**20/20**), embedded **auth + rate-limit behaviorally tested** (#175/176/90/84).
The mastering chain is an **on-demand, plan-gated application engine**, not a
default audio-path stage — so its audio/latency behavior requires a plan-driving
context to evaluate, which is by design, not a defect.
