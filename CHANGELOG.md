# Changelog

All notable changes to More-Phi are recorded here. This file is maintained
alongside the audit documents in `docs/audits/` and the production-readiness
gates in `docs/validation/`. Format is loosely Keep-a-Changelog; entries are
grouped by date with severity tags.

## 2026-06-19 — AI/MCP re-audit + DSP verification (branch `chore/ponytail-dead-code-cleanup`)

A focused VST3 + AI-MCP-integration re-audit closed the residual isolation,
verification, and documentation gaps left by the 2026-06-18 audit. Full suite
now at **520 test cases / 87,445 assertions** (all green, up from 505/87394).

### Security / Multi-instance isolation
- **[HIGH] ToolResultCache cross-instance read-through** — the process-wide
  shared read-only tool-result cache now namespaces keys by `instanceId`, so
  one plugin instance can no longer read another's cached `get_plugin_info`
  (which embeds `instanceId`/`port`/`morphCode`). `src/AI/ToolResultCache.{h,cpp}`,
  `src/AI/MCPToolHandler.cpp`.
- **[HIGH] AsyncToolExecutor enumerable job IDs** — async job IDs were a bare
  global counter (`async_N`) any MCP client could enumerate to poll another
  instance's jobs. Job IDs are now prefixed with the submitting instance's
  `morphCode`. `src/AI/AsyncToolExecutor.{h,cpp}`, `src/AI/MCPToolHandler.cpp`.
- **[LOW] validateAuth timing** — documented the token-length pre-check leak
  as acceptable (fixed-size public token format) with a note for future
  variable-length tokens. `src/AI/MCPServer.cpp`.

### DSP correctness & verification
- **[HIGH] Latency / PDC** — verified that `setLatencySamples()` already sums
  all four components (hosted plugin + oversampling + FFT window + mastering-
  chain lookahead). Added 3 pinning tests. `tests/Unit/TestDSPQuality.cpp`.
- **LUFSMeter** — K-weighting coefficients verified to match ITU-R BS.1770-4
  Annex 1 Table 1 literally; end-to-end momentary matches analytic prediction
  at 7 frequencies to 0.001 dB; absolute + relative gating tested. New file
  `tests/Unit/TestLUFSMeter.cpp`. Per-instance channel weights added
  (BS.1770-4 Ls/Rs = 1.41; stereo-preserving).
- **AdaptiveEQ** — steady-state gain verified against RBJ cookbook `|H(f)|`
  at filter centres to 0.001 dB (peak/low-shelf/high-shelf/LP/HP). New file
  `tests/Unit/TestAdaptiveEQ.cpp`.
- **TruePeakEstimator** — characterized vs an independent Kaiser-windowed 4×
  reference; refuted the prior "±0.2 dBTP" header claim (12-tap prototype
  under-reads near-Nyquist by ~25 dB); header corrected; behaviour pinned as
  regression guards. `tests/Unit/TestTruePeakEstimator.cpp`,
  `src/Core/TruePeakEstimator.h`.
- **GranularMorphEngine** — H5 normalization re-derived from Hann² mean-square
  (`1/sqrt(0.375·N)`) with the average-vs-instantaneous limitation documented;
  peak-gain-vs-density tests added. `src/Core/GranularMorphEngine.cpp`,
  `tests/Unit/TestGranularEngine.cpp`.
- **EnvelopeFollower / ModulationMatrix** — promoted from inspection-verified
  to tested (block-size-independent time constants; order-independent
  accumulate-then-clamp). New file `tests/Unit/TestModulationAndEnvelope.cpp`.

### Documentation
- **[LOW] iZotope IPC header comment** — removed the stale "29-byte" spec
  reference; recorded the verified 33-byte `<IBddQI` layout with a cross-
  reference to the Python peer. `src/AI/VST3IPCBridge.h`.
- Updated `More-Phi_Technical_Review_Report.md` resolution banner with the
  2026-06-19 fixes.
- Updated `docs/validation/PRODUCTION_READINESS_AUDIT_2026-06-18.md` gate
  table; verdict promoted to **READY** (pending B3 acceptance gates).
- Added this CHANGELOG.

### Build
- Removed an accidental duplicate `TestLUFSMeter.cpp` entry in
  `tests/CMakeLists.txt` (would have broken the test build).

### Known limitations / deferred
- **B3** — multi-platform build verification (MSVC + macOS Universal + Linux
  ASan), `pluginval --strictness-level 5`, `vst3_validator`, manual DAW smoke
  (Ableton/FL/Logic/Reaper). Acceptance-gate work, not code.
- **N1** — `MCPToolHandler.cpp` remains ~6,100 lines (monolithic). A per-
  category split is tracked as a maintainability follow-up.
- True-peak estimator is accurate for DC/low-frequency ISP but under-reads
  near-Nyquist (documented; would need a wider prototype FIR to reach
  reference-grade accuracy).

### Architectural facts & corrections (factual record, no pricing)
Recorded so future readers don't re-derive them from stale framings. Pricing
rationale lives separately in `docs/PRODUCT_POSITIONING.md`.

- **JUCE 8 commercial cost is $0 at launch volumes.** Verified against the
  official EULA ([juce.com/legal/juce-8-licence](https://juce.com/legal/juce-8-licence/)).
  Tiers: **Personal** (≤ $20K/yr revenue *or funding*, free, commercial use
  allowed), **Starter** (≤ $300K/yr, $800 perpetual), **Indie** (no limit,
  $3,500 perpetual), **Pro** (no limit, higher). There is **no cost cliff**
  forcing a price floor up: at $149 list × ~200 copies/yr (~$30K gross) the
  product lands in Starter at $800 — a ~2.7% COGS line item. The earlier
  "$1,200–4,000/yr must be absorbed" framing was a Pro-tier number that does
  not apply at launch.
- **Runtime AI surface is ~22K LOC, not 40.5K.** `src/AI/` totals 40,582 LOC,
  but the breakdown is:
  - `src/AI/Dataset/` — **15,245 LOC** (offline synthetic-audio dataset
    generator; not in the shipped plugin's runtime path).
  - `src/AI/StandaloneMcp/` — **3,532 LOC** (standalone CLI MCP server; not
    the plugin's embedded server).
  - **Runtime AI (MCP server + tool handlers + mastering heuristics) —
    21,805 LOC** (~22% of the codebase).
  The "over half the codebase is AI infrastructure" framing overstates the
  *shipped* runtime AI surface; once the offline Dataset tool and Standalone
  CLI are stripped, the AI the binary executes is large-but-defensible for a
  40+-tool MCP surface, not sprawling.
- **`find` operator-precedence gotcha (do not re-introduce the 36K figure).**
  An unparenthesized count like
  `find src/AI -name "*.cpp" -o -name "*.h" -not -path "*/Dataset/*"`
  parses as `(all .cpp) OR (non-Dataset .h)` — it counts **all** `.cpp`
  including Dataset's, plus only non-Dataset `.h`, inflating the result by
  ~11K (this produced an erroneous "36,252" figure in an earlier analysis).
  Always parenthesize:
  `find src/AI \( -name "*.cpp" -o -name "*.h" \) -not -path "*/Dataset/*"`.
  The verified total/Dataset/StandaloneMcp/runtime figures above were
  produced with parenthesized commands and reproduce exactly.

---

## 2026-06-18 — Comprehensive production-readiness audit (branch `feat/store-backend`)

47 issues identified (16 Critical, 14 High, 14 Medium, 6 Low) across
threading, memory safety, DSP, serialization, AI/MCP security, and build
stability. All resolved. Full record: `docs/audits/CRITICAL_BUGS_FIXED.md`.

Highlights: ThreadPool RAII deadlock fix (C1), SnapshotBank seqlock (C2),
HostedPluginWindow use-after-free (C3), V1→V2 preset migration (C4),
discrete-parameter step counts (C5), Formant/Spectral/Granular engine
correctness (C6/C7/H5), physics symplectic-Euler stabilization (C8/C9),
real-time `std::pow` removal (C15), PerformanceProfiler audio-thread
allocation (C16), ModulationMatrix accumulate-then-clamp (H3), block-size-
independent sidechain coefficients (H4), MCP instance isolation +
constant-time auth + idle timeout + zombie eviction (C13/C14).
