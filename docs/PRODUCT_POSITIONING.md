# Product Positioning & Pricing Rationale

**Status:** Recommendation with cited rationale, **not** a ratified commercial decision.
**Date:** 2026-06-19
**Author:** Derived from a two-round technical + market scrutiny of More-Phi v3.4.1
(`feature/multi-agent-layer` branch, 95+ test cases green).

> **Read this first.** The numbers below are **recommendations**, built from
> facts verified against the codebase and primary sources in this session.
> They are **not** a set price. If a product owner exists, the tier table and
> copy must be ratified by them before any storefront goes live. The purpose of
> this document is to make the *evidence* durable so the pricing decision can be
> made once, with the facts on the table, rather than re-derived every quarter.

---

## 1. Verified facts the positioning rests on

Every figure below was checked against the codebase or a primary source on
2026-06-19. They are reproducible; the commands are in the CHANGELOG entry of
the same date.

### Codebase composition (LOC, cpp+h)

| Layer | LOC | Share | Notes |
|-------|-----|-------|-------|
| `src/AI/` (total) | 40,582 | 54% | Includes offline tooling, not just runtime |
| └─ `src/AI/Dataset/` | 15,245 | 38% of AI | **Offline** synthetic-audio dataset generator; not in the shipped plugin's runtime AI path |
| └─ `src/AI/StandaloneMcp/` | 3,532 | 9% of AI | Standalone CLI MCP server; not the plugin's embedded server |
| └─ **Runtime AI** (MCP server + tool handlers + mastering heuristics) | **21,805** | **~22%** | The AI surface the *shipped binary* actually runs |
| `src/Core/` | 16,167 | 22% | Morph computation, DSP, physics |
| `src/UI/` | 8,593 | 11% | |
| `src/Plugin/` | 3,966 | 5% | |
| `src/Host/` | 1,767 | 2% | VST3/AU hosting |
| `src/Licensing/` | 1,491 | 2% | |
| `src/Preset/` | 1,427 | 2% | |
| `src/MIDI/` | 267 | <1% | |

**Key correction:** the "over half the codebase is AI" framing overstates the
*runtime* AI surface. After stripping the offline Dataset generator and the
Standalone CLI, the AI the shipped plugin actually executes is **~22K LOC** —
large for a boutique tool, but defensible for a 40+-tool MCP surface, not
sprawling.

### Licensing reality (JUCE 8, verified against the official EULA)

| Tier | Annual revenue/funding limit | Cost |
|------|------------------------------|------|
| **Personal** | ≤ $20,000 | **Free** (commercial use allowed) |
| **Starter** | ≤ $300,000 | $800 perpetual |
| **Indie** | No limit | $3,500 perpetual |
| **Pro** | No limit | Higher |

**Implication:** there is **no cost wall** forcing the price floor down. At a
$149 list selling ~200 copies/yr (~$30K gross), the product crosses the $20K
Personal ceiling and lands in **Starter → $800 perpetual** — a ~2.7% COGS line
item against gross. The earlier "$1,200–4,000/yr must be absorbed" framing was
a Pro-tier number that does not apply at launch volumes.

### DSP / concurrency verification (this session, test-backed)

Not asserted — proven:
- **LUFSMeter** K-weighting coefficients match ITU-R BS.1770-4 Annex 1 Table 1
  literally; end-to-end momentary matches analytic prediction at 7 frequencies
  to 0.001 dB; absolute + relative gating tested.
- **AdaptiveEQ** steady-state gain matches the RBJ cookbook `|H(f)|` at filter
  centres to 0.001 dB (peak/shelf/LP/HP).
- **TruePeakEstimator** characterized against an independent reference; prior
  "±0.2 dBTP" claim refuted and corrected. The old 12-tap prototype under-reads
  near-Nyquist by ~25 dB. The current implementation uses a 4-phase × 64-tap
  polyphase FIR upsampler (256-tap linear-phase Kaiser β=8.6 prototype, ~80 dB
  stopband), tracking the reference to within ~0.02 dBTP (R3 fix, 2026-07-16).
- **Real-time path:** seqlock + `atomic_thread_fence`, SPSC queue with
  `alignas(64)` indices, `noexcept` `processBlock` with `ScopedNoDenormals`.
- **Latency/PDC:** all four components (hosted plugin + oversampling + FFT
  window + mastering-chain lookahead) correctly summed to `setLatencySamples`;
  pinned by tests.

---

## 2. Competitive position

**Niche:** plugin-host + parameter/snapshot morphing — the category NI Kore 2
created and vacated in 2011. More-Phi is the only modern tool that hosts
arbitrary VST3/AU plugins and morphs between their parameter states.

| Competitor | Does it host arbitrary plugins? | Does it morph snapshots? | Notes |
|-----------|:-------------------------------:|:-----------------------:|-------|
| NI Kore 2 (legacy, 2011) | ✓ | ✓ (8-point A–H) | Dead; no modern plugin compat |
| Melda MXXX / MXXXCore | ✗ (Melda-internal only) | ✓ (XY) | Mature, expensive |
| Unfiltered Audio TRIAD/BYOME | ✗ | ✗ (modulation, not snapshot morph) | Polished, indie-beloved |
| Krotos Studio Pro | ✗ | partial (param linking) | Sound-design niche, subscription |
| Output Portal | ✗ | ✗ (granular XY) | AAA boutique polish |
| **More-Phi** | **✓** | **✓ (12-slot)** | **+ MCP AI server (unmatched)** |

**Unmatched USPs:** (1) arbitrary-plugin hosting + morphing (the Kore-2 gap,
unfilled for 14 years); (2) embedded MCP server for AI-agent remote control —
no named competitor ships this; (3) physics-based morph (Elastic/Drift);
(4) genetic preset breeding.

**Shelf position:** boutique indie tier — engineering depth competitive with
Unfiltered Audio / Krotos / u-he; brand, polish, and ecosystem below an
NI/iZotope release. The MCP angle is a genuine differentiator no shelf-mate
offers.

---

## 3. Pricing — milestone-gated **recommendation**

The tier table below is a **recommendation**, structured as a maturity curve
rather than a static point. It pairs each price with the *shipped* capability at
that milestone (see §4 for the executable inventory). Early adopters lock in a
lower price in exchange for being the validation population; the publisher
captures more value as the product earns it.

| Milestone | List (rec.) | Intro/street (rec.) | Gating condition |
|-----------|:-----------:|:-------------------:|------------------|
| **Launch** (current state) | $149 | $99 | Today's binary; B3 gates not yet closed |
| **B3 closed** | $179 | $129 | Multi-platform build (MSVC + macOS Universal + Linux ASan) + `pluginval` strictness-5 + 4-DAW smoke (Ableton/FL/Logic/Reaper) |
| **Learned AI backends ship** | $199 | $149 | Heuristic mastering / genre classifier / VAE stub replaced with real learned models in the shipped binary |

**Rationale for the shape:**
- **Floor ($99–$149):** defensible on hosting + morphing + MCP alone, even with
  the AI surface acknowledged as heuristic. Kore-2's unfilled niche plus the
  MCP USP justify a premium over FX-only morphers (TRIAD street ~$70,
  MXXXCore street ~$108).
- **Ceiling ($199):** gated behind real learned AI, which does not exist yet.
  Pricing there today would inherit the credibility debt described in §4.
- **Headroom exists** because the JUCE cost correction removed the floor
  pressure and the LOC correction removed the "AI bloat" objection.

> ⚠️ **These numbers are not a decision.** Ratify with the product owner before
> any storefront copy is written. The gating *conditions* are durable; the
> *price points* are a synthesis that the owner may adjust up or down.

---

## 4. Capability vs. Roadmap discipline (load-bearing for credibility)

This is the rule that makes the pricing durable, and it is **the same discipline
the codebase already follows.** The README ships an explicit AI-claims-honesty
section that names, for every AI/analysis surface, what the binary *does*
versus what a certified/learned implementation would do:

> - Mastering plans are deterministic `heuristic_rule_engine` recommendations
>   with rule IDs and measured inputs, **not learned model predictions** or
>   calibrated confidence scores.
> - Genre classifier inference is `unavailable`/`default_fallback` unless a
>   real model backend is loaded.
> - Neural compressor inference is `unavailable`/`heuristic_fallback` unless a
>   real inference backend is loaded.
> The latent-space morphing path is not yet implemented in the current release.
> — `README.md`, AI Claims section

**The pricing page must follow the same rule:** each tier's stated capability
must be **executable against the shipped binary**, not the roadmap. No "coming
soon," no asterisks pointing at a tracker. The roadmap lives in the commit log
and CHANGELOG; the pricing page describes the artifact you can download today.

Concretely, at each tier the copy should state:

- **Launch ($149):** "MCP remote control is the real feature. The mastering,
  genre-classification, and VAE-morph surfaces are deterministic heuristics or
  stubs — explicitly, as documented in the README."
- **B3-closed ($179):** same AI surface; the added value is verified
  cross-platform stability and host compatibility, not new AI capability.
- **Learned AI ($199):** only when the heuristic/stub surfaces are replaced by
  real learned models *in the binary*. Until then this tier does not exist on
  the pricing page.

**Why this is a commercial argument, not just a legal one:** the audience that
values this plugin — sound designers and technical producers who read `docs/`
— punishes overclaiming harder than it punishes modesty. A Launch tier that
openly says "the AI is heuristic; the MCP control surface is the product" is a
*stronger* pitch to that audience than a page implying near-AI capability.

---

## 5. What this document is *not*

- **Not a price list.** It is a recommendation with rationale; ratify before use.
- **Not a feature roadmap.** The roadmap is the commit log + CHANGELOG.
- **Not static.** When a tier's gating condition closes, update §3 and §4
  together — never update the price without updating the capability inventory,
  and vice versa.
- **Not a substitute for the audit docs.** Technical risk detail lives in
  `docs/audits/CRITICAL_BUGS_FIXED.md` and
  `docs/validation/PRODUCTION_READINESS_AUDIT_2026-06-18.md`.

---

*Derived from verified facts in the 2026-06-19 scrutiny session. Primary
sources: the codebase (LOC counts, test suite, README AI-claims section), the
JUCE 8 EULA
([juce.com/legal/juce-8-licence](https://juce.com/legal/juce-8-licence/)),
and the audit documents under `docs/audits/` and `docs/validation/`.*
