# More-Phi — AI Mastering Workflow Redesign

> **Status:** Proposed redesign — **verification verdict: `rework_needed`** (5 blockers, 16 adjustments).
> This is a design + audit document, not shipped behavior. New components it names (`master` verb, `SignedPlanCache`, the Cockpit, the confirm gate) **do not exist yet**; pre-existing bugs it uncovered (§8) **do exist** and are independent of the redesign.
> **Method:** produced by a three-phase code-grounded study — (1) map the current workflow + friction across 5 areas, (2) design via a 3-philosophy judge panel, (3) adversarially verify against the real-time/safety/capability constraints. All claims cite `file:line`.

---

## 0. TL;DR

The thesis: **make More-Phi a "show real state, then act" mastering surface, not a "describe intent, then guess" chat.**

- **Default surface = a Mastering Cockpit:** always-on measured-vs-target LUFS/true-peak meters (message-thread Timer, never audio) are the source of truth; genre/character **presets** absorb the jargon; a beginner finishes a master in **≤3 clicks** (pick preset → read meters → Apply).
- **One verb, `master`, with `mode={analyze|preview|apply|render}` + a signed `plan_id`.** `apply` accepts *only* a `plan_id` and replays the cached immutable plan **verbatim** — it never re-runs inference (fixes the verified re-inference defect). This collapses the current ~10 overlapping mastering tool names.
- **One risk-tiered commit model** behind the existing single chokepoint (`dispatchWithAutomationTransaction`): safe-tier moves auto-apply inside the unchanged safety caps; any move outside the caps is forced through a mandatory before/after diff + A/B screen with auto-rollback.
- **One orchestration entry:** the Chat Panel forwards to `AgentRuntime::submitGoal(origin="chat")` — the *same* path external MCP uses — instead of its current disconnected flat tool-loop.
- Complexity is removed **for the musician** (plain adjectives, no `plan_id`/LUFS/Ozone on the default surface) **and deleted from the codebase** (the `mastering.neural_apply` COMMIT door and the chat flat-loop are collapsed, not papered over).

**But:** verification found the design references two load-bearing components that are vaporware (`SignedPlanCache`, the confirm gate), rests the unsafe-tier safety net on an **unfinished** `ABCompareEngine` (2-metric stub), under-specifies the plan fingerprint (silent-corruption risk), and ships with one wording contradiction. It also surfaced **four pre-existing bugs** in shipped code. Those must be resolved before this design can ship. §7 and §8 are the gate.

---

## 1. Current-state friction (what we are removing)

Mapped from source, not assumption. Grouped:

**A. The mastering surface is fragmented and invisible.**
- **~10 overlapping tool names** with inconsistent dry-run/apply semantics: `sonicmaster_decision` (preview-only), `mastering.neural_apply` (apply, but **re-runs inference** — can diverge from the preview), `preview`/`apply_mastering_plan` (raw numeric inputs), `analyze_rule_based_mastering` (the *only* one reading live meters), plus snake_case **and** dotted aliases for each. `MCPToolHandler.cpp:3676-3771`.
- **No mastering UI.** The entire 14-component chain surfaces as a 30px "Neural Master (Preview)" toggle + one status string. No LUFS/true-peak readout, no decoded-plan view, no visual diff. `PluginEditor.cpp:453-456,657-702`.
- **Two inconsistent commit models:** SonicMaster auto-applies every ~3s with **no approval**; agent `apply_mastering_plan` routes to an Approval panel. The user must infer which path applied.

**B. The assistant acts blind.**
- **No pre-action preview/confirm** for routine edits — the system prompt explicitly tells the model "just call the tool, do NOT ask for permission" (`LLMChatClient.cpp:304-306`). The only diff surface renders *after* a workflow ran, as a 4-entry truncated text line (`AIChatPanel.cpp:155-188`).
- **Two disconnected orchestration systems.** The Chat Panel runs `LLMChatClient`'s flat tool-loop (up to 8 round-trips) which **bypasses** `DefaultToolInvoker`'s capability/rate/attribution guards entirely and has **zero references** to `AgentRuntime`/`ConductorAgent`. The documented 7-agent Conductor layer is reachable **only** via external MCP. `CAPABILITIES.md §9/§11` describe them as unified — they are not.
- **Three opaque pre-parse tiers** silently route a message to different engines; failure messages differ per tier. `AIChatPanel.cpp:420-436`.
- **Round-trip latency is invisible:** "Thinking… (iteration N)"; no per-tool progress, no interrupt of an in-flight HTTP call.
- **Heavy jargon leakage:** `sonicmaster_decision`, `masteringbrainv2`, `WinHTTP 12002`, "choose a tool-capable NVIDIA model", raw `WorkflowRun`/`Approval ID`s.

**C. Parameter exposure is broken/unfinished.**
- **Learn Mode (`isExposed`/`importance`) is bypassed** by the actual AI read/write tools (`get`/`set_parameter`, `describe_plugin_semantic_map`) — only the genetic breeder honors it. `getExposedParameterIndices()` is never called in production.
- **The learning loop is dead:** `recordModification` fires from exactly one explicit MCP tool; `recordAIAdjustment` is never called. Importance scores stay at static baselines.
- **No UI to curate exposure** — only MCP JSON-RPC. `ParameterMapPanel` is a raw 4096-row index/value list.
- **Two parallel AI control vocabularies:** raw `set_parameter` (unitless `[0,1]`, no snap, no clamp) vs semantic `apply_safe_action` (role+dB, safety tiers). Agents taking the simpler path get an unsafe surface.

**D. Capability claims that don't match code (doc drift).**
- `ABCompareEngine` (LUFS/LRA auto-rollback) is **dead code** — included but never instantiated; the A/B button is a plain param-vector restore. `PluginProcessor.cpp:692-720`.
- CAPABILITIES §8/§9/§11/§15/§16/§18 overstate what is wired (details in §10).

---

## 2. Simplified workflow architecture  *(Deliverable A)*

A single pipeline with three tiered commit paths behind **one** chokepoint. Every request — from the Cockpit's Apply button, an outcome card, a free-text chat turn, an external MCP client, or a SonicMaster background cycle — enters the same funnel:

```
Intent ──► AgentRuntime::submitGoal(origin)   (ONE Conductor brain)
        ──► Plan      (signed plan_id, cached immutable, fingerprinted)
        ──► Commit    (Safe auto-apply  |  Unsafe Confirm+A/B)
        ──► enqueueParameterSet(source) ──► LockFreeQueue<ParamCommand,8192> ──► audio drain
```

### 2.1 Stages

| Stage | What | Surface | Real-time constraint |
|------|------|---------|----------------------|
| **1. Intent** | Three equivalent entry vectors: (a) Cockpit preset + Apply; (b) one of 6 plain-adjective outcome cards (Louder/Warmer/Cleaner/Brighter/Wider/Match Reference); (c) free text — *never applied blind*, mapped to a card with a "Did you mean \<card\>?" confirm first. All call `submitGoal(intent, priority, origin)`. | Cockpit (default tab), outcome-card strip, chat as collapsible side panel. | Message-thread only; `submitGoal` schedules onto the 2-worker `PriorityScheduler`. No agent/orchestration code on the audio thread (`TestAgentAudioThreadIsolation` stays green). Conductor fast-paths trivial single-write intents to skip decomposition latency. |
| **2. Plan** | Conductor produces a `NeuralMasteringPlanCandidate`; ONNX path first, deterministic `RuleBasedMasteringResolver` fallback on `model_unavailable`. Candidate runs through `NeuralMasteringSafetyPolicy::validate()` (noexcept). On accept, an immutable plan is stored in `SignedPlanCache` keyed by `plan_id`, carrying a hosted-plugin **fingerprint** (see §7 blocker). | Meters update continuously; a Deviation chip renders the exact `NeuralMasteringValidationIssue` code as a named badge + proposed-vs-current ghost overlay. | Message-thread + SonicMaster's existing analysis thread only. The signed plan/fingerprint/cache are **transient** message-thread-domain state — **must not** be serialized into `getStateInformation`. |
| **3a. Commit — SAFE tier** | If the plan is *by construction* inside the caps (preset-built candidate returns `accepted=true` from the unchanged `validate()`; delta within per-plan cap; confidence ≥ 0.75; no high-risk mask; no AI deviation), it applies **gate-free** with incremental slew (0.6 LU/cycle) so meters converge visibly. | Meters move toward target; one-click Undo always visible. | Runs the existing `applyValidatedPlan()` noexcept path with `enforceDeltaCaps()`; writes cross to audio **only** via `enqueueParameterSet` → `LockFreeQueue`. No new audio-thread entry. |
| **3b. Commit — UNSAFE tier** | Any move outside the safe tier is **forced** through a before/after diff + meters + A/B screen with a single Confirm. `ABCompareEngine` captures slot 11, measures ~2s, auto-rolls back on regression (see §7 blocker — engine is unfinished). | Plan+Commit screen: bounded sorted semantic diff table, before/after meters, named issue badge, Confirm/A-B/Refine. | All message-thread + Timer-driven from already-captured atomic snapshots. |
| **4. Rollback** | `ABCompareEngine::rollbackCandidate()` restores slot 11 at any time, for both tiers. 32-level undo unaffected. RealtimeControlAgent corrections surface retroactively as one-click-reversible Plan-log entries (see §7 — reversal must synthesize a `plan_id`). | Persistent Undo/Reject after every apply; Plan-log panel. | Rollback enqueues via the same `LockFreeQueue`. |

### 2.2 The single commit model

> **Safe Mode defaults ON:** it forces an explicit Confirm for every `apply`/`render` that falls **outside the verified safety caps** (unsafe tier). The **safe-tier preset-built candidate is the single gate-free exception** — precisely because it returns `accepted=true` from the unchanged `validate()` and relies on the `PermissionKernel` autonomy gate + the `enforceDeltaCaps()` cap floor, **not** a bypass of `dispatchWithAutomationTransaction`.
> *(This sentence is stated once, identically, here and in §4 progressive-disclosure — fixing the wording contradiction the verification found.)*

Everything mutating funnels through `MCPToolHandler::handle → dispatchWithAutomationTransaction → PermissionKernel::evaluate()`. The hard confirm gate is enforced **in code inside `DefaultToolInvoker` *before* dispatch** (not a system-prompt line, not the bypassable `prediction_only_no_approval_created` advisory flag at `MCPToolHandler.cpp:979`): no `mode=apply`/`render` dispatches without a `plan_id` that was previewed **and** confirmed this session — identical for chat, MCP, and background cycles.

### 2.3 Tool consolidation

`master` with a `mode` discriminator replaces:

| Today (~10 names) | Becomes |
|---|---|
| `sonicmaster_decision`, `analyze_rule_based_mastering` | `master` `mode=analyze` (heuristic auto-fallback inside analyze) |
| `preview_mastering_plan` / `mastering.plan_preview` | `master` `mode=preview` (populates `SignedPlanCache`) |
| `apply_mastering_plan` / `mastering.apply_plan` | `master` `mode=apply` (takes **only** `plan_id`, replays verbatim) |
| `render_mastering_batch` / `render_status` / `select_candidate` | `master` `mode=render` (returns ranked candidates; selection is a `plan_id` reference — **snapshot, do not mutate** shared candidate state, §7) |
| `mastering.neural_apply` (the COMMIT door) | **Deleted.** Its capability is split: `mode=analyze` (decision) + `mode=apply` (replay). Apply never re-infers. |

Old names survive as **deprecation aliases** (mapping name→mode) for exactly one release, then are removed. `hosted_plugin.set_parameter(s)` / `more_phi.set_parameter(s)` are **kept** as the low-level write primitive that `mode=apply` compiles down to — the commit mechanism, not a parallel mastering verb.

### 2.4 Orchestration unification

`LLMChatClient` becomes a thin **transport + render** layer: it forwards utterances to `AgentRuntime::submitGoal(origin="chat")` — the *same* entry external MCP uses for `agents.run_goal` (verified at `MCPToolHandler.cpp:8040`). Its flat `kMaxToolIterations=8` inline-write loop (`LLMChatClient.cpp:1571-1801`) is removed; writes become `proposedActions` routed through the unified pipeline. A config-flagged flat-loop fallback (`MORE_PHI_LEGACY_CHAT_LOOP`) is kept for one release as an emergency path, then deleted. The Conductor is the single planner for chat, cards, and preset Apply.

---

## 3. Interaction model — examples  *(Deliverable B)*

| Persona | User does/says | What happens | Why it's simple |
|---|---|---|---|
| **Non-technical musician** (knows zero LUFS/EQ/Ozone) | Clicks *Streaming* preset, drags Loudness to ¾, clicks **Apply** | Cockpit measures current LUFS, shows measured-vs-target. Preset pre-builds a candidate guaranteed inside `maxDeltaPerPlan`, so `validate()` → `accepted=true` → safe tier. `master apply` replays `plan_id` through the chokepoint; `PermissionKernel` auto-commits; `applyValidatedPlan()` applies with slew; meters converge. Undo visible throughout. | Preset absorbs LUFS/EQ; meters are the truth; cap floor makes gate-free safe; Undo is the brake. **3 clicks, zero jargon, no chat.** |
| **Producer who wants a vibe** | Clicks **Warmer** card | Routes to `submitGoal(intent="warmer", origin="card")`. ONNX plan nudges low-mid EQ + harmonic. If inside caps → safe auto-apply. If it trips the high-risk harmonic mask → forced to Plan+Commit screen with a "High-impact change" badge, before/after EQ diff, A/B auto-rollback. | Plain adjective entry; the SafetyPolicy's reasoning becomes a visible badge instead of buried in prose. |
| **Power user / mix engineer** | Types: *"Push the limiter ceiling to −1.0 dBTP and widen the high band ~6%, keep low end mono"* | `LLMChatClient` (transport-only) maps to cards (Wider + custom limiter nudge), shows "Did you mean…?" confirm. Limiter move is high-risk → mandatory Confirm screen **regardless of autonomy** (the hard gate is in code). Diff table shows the <20 changing semantic params; A/B measures; on ≥2-metric regression auto-rolls back. Exact mapped Ozone param names (`mapping_status: enqueued/skipped/unmapped`) in Advanced. | Free text never applies blind; the chokepoint gate can't be bypassed by autonomy tier. |
| **External MCP / automation** | JSON-RPC `master.apply {plan_id:"abc123"}` | Identical path to chat/cockpit. Hard gate checks `plan_id` was previewed+confirmed this session; on mismatch refuses with "re-run `mode=preview`". Fingerprint checked; on plugin-state change refuses with "re-run preview". Old `mastering.neural_apply` works one release as a deprecation alias that logs a warning. | One verb, one gate, one chokepoint for every origin. Apply = deterministic replay of exactly what was previewed. |
| **Reactive safety** (`RealtimeControlAgent`) | (DAW playback clips; LUFS breach detected) | Correction runs **synchronously on the blackboard pump thread** (NOT via `callAsync` — see §7/§8) → `DefaultToolInvoker` → `MCPToolHandler::handle` (same chokepoint), subject to `PermissionKernel` + `SafetyPolicy` + `QualitySafety` veto + `maxCorrectionsPerParamPerSecond=4` + `correctionBudgetPerRun=16`. Surfaces retroactively as a one-click-reversible Plan-log entry. | The one sanctioned non-interactive exception is named explicitly, audited like every other write, and reversible via `master.apply` (synthesized `plan_id`). |

---

## 4. UI/UX changes — cutting cognitive load  *(Deliverable C)*

### 4.1 The Mastering Cockpit (default landing tab, via the existing `setVisibleTabs` mask)

Top-to-bottom:
1. **Always-on measured-vs-target meter pair** (LUFS-I + true-peak dBTP) — message-thread Timer feeding from `SonicMasterAnalysisEngine`/`AutoMasteringEngine`.
2. **Genre/character preset row** (Streaming / EDM / Hip-Hop / Folk / …) backed by `kGenreMasteringProfiles` + `kMasteringTargetCurves`, each showing its resolved target LUFS in plain numbers.
3. **Loudness** + **Character** knobs (semantic, not raw EQ indices).
4. **Single Apply button.**
5. **6 outcome cards** as the secondary entry affordance.
6. **Deviation chip** (appears only when a Plan exists) — renders the `NeuralMasteringValidationIssue` code as a named badge + proposed-vs-current ghost overlay.
7. **Persistent Undo/Reject.**
8. Chat becomes a **collapsible side panel** (escape hatch), not the primary surface.

### 4.2 One unified status model

Every plan/agentic action is exactly one of: `Idle · Measuring · Planning · Preview-Ready · Applying-Safe · Awaiting-Confirm · A/B-Measuring · Applied · Rolled-Back`. The 4-level autonomy matrix is **removed from per-action UI**; replaced by three plain-English segments.

### 4.3 Terminology map (technical → plain)

| Internal / technical | User-facing |
|---|---|
| `plan_id`, `mode` | Hidden (shown only as "This change" on Undo; lives in Advanced) |
| `NeuralMasteringValidationIssue` (HighRiskMask/MaxDeltaProjected/LowConfidence/StalePlan) | "High-impact change" / "Large move" / "Low confidence" / "Stale — re-analyze" badges |
| semantic map (eq/dynamics/stereo/harmonic/limiter/loudness) | "EQ / Dynamics / Stereo / Exciter / Limiter / Loudness" group labels |
| `mastering.neural_apply` / `master.apply` | "Apply" |
| `mapping_status: enqueued/skipped/unmapped` | "Applied" / "Skipped (not found)" / "Not mapped" (Advanced) |
| AutonomyLevel (Manual/Assist/CoPilot/Autopilot) | "Ask every time" / "Suggest, then act on confirm" / "Plan continuously, still confirm writes" |
| LUFS / true-peak dBTP | "Loudness" / "Peak" (**numbers stay** — they're measured state, not jargon to learn) |
| `ABCompareEngine` capture/compareAndDecide/rollback | "Compare" / "Undo" |

### 4.4 Progressive disclosure (3 layers)

- **Layer 1 (default, beginner):** Cockpit — meters + presets + Apply + outcome cards + Undo. **Zero** of `{plan_id, mode, semantic-map, HighRiskMask, neural_apply}` visible.
- **Layer 2 (intermediate, appears on the Plan+Commit screen for the unsafe tier):** before/after semantic diff table (capped to <20 changing params), named issue badges, A/B buttons, autonomy segment selector.
- **Layer 3 (expert, behind a labeled "Advanced / Raw Parameters (debug/expert)" disclosure):** the full `master.*` surface, `mapping_status` per param, the raw up-to-4096 inspector (via `acquirePluginForUse` lease), `plan_id`, legacy aliases.

`OnboardingOverlay` is extended with a 3-screen intro: preset → loudness → meter.

---

## 5. Hosted-plugin parameter exposure / control surface  *(Deliverable D)*

- **Default safe surface = the `NeuralMasteringTargetVector`** (6 semantic groups: `eq[8]`, dynamics, stereo, harmonic, limiter, loudness) + preset `(genreIndex + targetLufs + curveId)`. The mastering diff is rendered against semantic groups, **never raw indices**, on the default surface. Mastering **never** routes through LLM-translated `set_parameters_batch` over 4096 floats.
- **Learn Mode decision (honest):** the `isExposed` flag does **not** gate the neural mastering vector today (it's bypassed — §1). Rather than pretend otherwise, the redesign scopes Learn Mode + `TokenOptimizer` to governing the **Raw Parameters (debug) inspector** and generic hosted-plugin MCP tools; the mastering surface defaults to the TargetVector independent of exposure. *(This is also a CAPABILITIES §15/§16 correction, §10.)*
- **Raw expert escape hatch:** the up-to-4096 surface is reachable **only** behind the Advanced disclosure, using the **same** `acquirePluginForUse()`/`releasePluginFromUse()` lease as the mastering path (no new hosted-plugin access seam). Raw writes still go through `applyValidatedPlan()` via `OzoneParameterMap`/`PluginSemanticMapper`, never by the LLM translating `set_parameters_batch` directly.
- **Discrete handling preserved verbatim:** `DiscreteParameterHandler` (snaps discrete/binary to valid steps during morphing) is kept. In the semantic view, discrete/binary/enumeration params render as toggles/dropdowns/menus, not continuous knobs, and are **excluded** from the mastering diff's float delta-sorting (shown as "Off→On", not deltas).
- **Outcome-card projections** (Warmer→low-mid EQ+harmonic, etc.) are **new data** (`MasteringIntentProfile` table) — a Phase-1 deliverable with mandatory per-card param-mapping tests, and each projection reviewable in the Advanced disclosure so a user can audit what "Warmer" actually moved before it auto-applies.

---

## 6. Hosted-plugin constraints respected  *(constraint table)*

| Constraint | How the redesign honors it |
|---|---|
| Audio path is `noexcept`, allocation/lock/IO/throw-free after `prepareToPlay` | No new audio-thread entry point. Meters/cards/diff render on a message-thread Timer from captured atomic snapshots. `requestDecisionNow` runs on SonicMaster's existing analysis thread. |
| All cross-thread writes via `LockFreeQueue<ParamCommand,8192>` (the single chokepoint) | Unchanged. `master apply` still compiles to `enqueueParameterSet`. `ParameterEditSource` stays coarse at its **6** values (Unknown/UI/Assistant/MCP/Snapshot/Neural) — **not** expanded (the RT-judge's "exactly 3" was wrong; verified `PluginProcessor.h:205-211`). |
| Hosted-plugin access via `acquirePluginForUse()`/`releasePluginForUse()` lease | Semantic TargetVector→raw mapping stays inside existing `applyValidatedPlan`/`OzonePlanApplicator` code that already uses the lease. Raw inspector uses the same seam. |
| `getStateInformation` audio-thread-aware fallback | `SignedPlanCache`, the gate, and `plan_id` are **transient** message-thread state, **not** serialized. The active preset struct layers onto the existing APVTS/snapshot-bank round-trip — no new persistence path. |
| `NeuralMasteringSafetyPolicy` caps + true-peak ceiling (≤ −1.0 dBTP) | Both enforcement points (`validate()` + `applyValidatedPlan`→`enforceDeltaCaps()`) kept. The safe-tier candidate passes `validate()` legitimately because it's inside the caps — **not** exempt. |
| Read-back verification, Ozone name-validated mapping, fail-loud on all-stubs | Reused verbatim. `master apply`'s replay still routes through `OzonePlanApplicator`'s name-checked `enqueueIfMapped`. |
| `PermissionKernel` + `ActionLedger` + self-approval prevention | `master apply`/`render` route through `dispatchWithAutomationTransaction`; the autonomy gate is what makes safe-tier non-interactive. Attribution preserved via `runtime.ledger().record`. |
| UI feedback Timer-deferred (not `callAsync`) | All new surfaces (meters, cards, chip, A/B) render on the message-thread Timer. (The `RealtimeControlAgent` correction is the *one* sanctioned exception — and it does **not** use `callAsync`; see §7/§8.) |

---

## 7. Verification gate — verdict: `rework_needed`

The adversarial verify pass (3 lenses: real-time/safety · capability-preservation · implementability/consistency) did **not** clear the design. Five blockers, five friction-resolution gaps. **These are resolved into the design above where possible; the rest are open gates.**

### 7.1 Blocking issues (must fix before any phase ships)

1. **UB data race on `BlackboardBridge::onPublish_`.** `std::function<void()> onPublish_` (`BlackboardBridge.h:81`) is read+invoked from `publish()` (`BlackboardBridge.cpp:26`) on agent-worker/MCP threads while `setOnPublishHook` writes it on the message thread — **no synchronization**. Torn read → stale/freed lambda → crash; use-after-free window at teardown. The design's "notify_one is safe" defense is a non-sequitur (the race is the `std::function`, not the cv). **Fix:** store as `std::shared_ptr<std::function>` with atomic load/store, mirroring the existing H-3 pattern (`BlackboardBridge.h:37-38`); add a TSAN test; assert no agent worker is mid-execute at `stop()`.
2. **`SignedPlanCache` + the confirm gate do not exist.** grep across `src/ docs/ specs/` = zero hits; `DefaultToolInvoker.cpp:53-100` has no gate. **Fix:** either implement both in Phase 0 **with their own unit tests** (fingerprint invalidation, TTL, corruption-path test) and re-audit, or strike all "confirmed safe" language about them. They cannot be audited against nothing.
3. **`RealtimeControlAgent.h` documents the opposite of the code.** The header (`:14-20`) claims corrections route via `MessageManager::callAsync`; the `.cpp` M5 note (`:122-127`) states "invoked synchronously here, **NOT** via callAsync". Since `callAsync` drops on headless hosts (`CLAUDE.md`), a maintainer "fixing" the code to match the header would silently re-break offline-render output protection. **Fix:** rewrite the header to match the M5 `.cpp`; delete `callAsync` references; add a `TestRealtimeReactive` regression asserting no `callAsync` on the delivery path. *(The redesign's §3/§4 text has been corrected to match the .cpp — synchronous on the pump thread.)*
4. **`ABCompareEngine` is an unfinished 2-metric stub.** `ABCompareEngine.cpp:48` hardcodes `spectralScore=0.f`; `:72-73` excludes it; `compareAndDecide` compares only LUFS+LRA as a 2-of-2 gate. A candidate that preserves loudness but **wrecks spectral balance passes the gate and auto-commits**. No call site exists. **Fix:** before the unsafe-tier Confirm screen ships — implement `SpectralBalanceAnalyser` to populate `spectralScore`, add it as a 3rd metric, add a spectral-only-regression rollback test, wire a call site. The unsafe-tier net is a **dependency on this work**, not "already wired".
5. **The O1 neural-first path has zero test coverage** — the headline behavior change. `TestAgentE2E`'s `FakeInvoker` returns a canned shape with no `available`/`applied` keys, so `OptimizationAgent` always falls through to the heuristic branch; the neural-first branch is never exercised. **Fix:** make the `FakeInvoker` tool-aware (return neural success/failure shapes for `mastering.neural_apply`); assert `findings['path']` in both branches; add a whitelist test or a `quality.target_set` followUp so the `QualitySafety` bypass (neural success empties `proposedActions`) is intentional and checked, not implicit.

### 7.2 Required design adjustments (integrated above / to land per phase)

- **Fingerprint domain expanded** (blocker 2's correctness): fingerprint = `plugin_id` + hash(mapped Ozone params) + **digest of the captured-audio window (or SonicMaster capture-ring generation)** + **hash of ALL upstream signal-path hosted params**, because `requestDecisionNow` re-reads live capture via a null buffer (`MCPToolHandler.cpp:7340`). Reject replay on any mismatch. Corruption-path test required.
- **Safe-Mode wording harmonized** (§2.2 now states the rule once, identically).
- **Phase-0 honesty:** distinguish "zero observable behavior" (true for default-off `master.*` dispatch cases) from "nothing changes" (false — `SignedPlanCache` + gate are new stateful subsystems with invalidation/TTL burden even when flagged off).
- **`master render` semantics:** must **snapshot** (not mutate) the active candidate when keying off `plan_id`, or accept an explicit candidate-index argument; test that two concurrent `render` calls with different `plan_id`s don't clobber shared state.
- **RT-agent retroactive reversal:** must synthesize a `plan_id` (capturing pre-correction `outputGain`) and replay via `master.apply` — **not** a dedicated rollback that creates a second unaudited mutation path.
- **Three added friction resolutions** (fills the 8-of-11 gap, §1/§7.3): (a) "did it help?" depends on finishing `ABCompareEngine`; (b) every legacy-name alias dispatch emits an `ActionLedger` `legacy_alias_used` event during the alias window so divergence is observable, not hidden; (c) every safe-tier auto-apply stamps `IntegrationEvent.source="safe_tier_auto"` (coarse `ParameterEditSource` untouched) so the gate-free path is retroactively auditable.
- **Chat-loop deletion safety:** before Phase 3, migrate NVIDIA empty-200 retry, reasoning-model token-exhaustion recovery, and inline tool-call token parsing (`LLMChatClient.cpp:1571-1801`, tested by `TestLLMChatClient.cpp:622-886`) into the shared transport layer; pin a Conductor fast-path threshold with a latency test; keep `MORE_PHI_LEGACY_CHAT_LOOP` one release.

### 7.3 Coverage matrix (11 frictions → resolution status)

| Friction | Covered? | Note |
|---|---|---|
| Fragmented/invisible mastering surface + ~10 tool names | ✅ | `master` verb collapses the surface; legacy aliases emit `legacy_alias_used` during the window. |
| No pre-action preview/confirm | ✅ | Hard code-level gate in `DefaultToolInvoker`; Cockpit meters as baseline. |
| Two commit models (auto-apply vs Approval) | ✅ | One risk-tiered model (§2.2). |
| Chat flat-loop disconnected from agents | ✅ (Phase-3 gate) | Route through `submitGoal`; migrate transport recovery first. |
| Single mutating chokepoint | ✅ | Verified: `dispatchWithAutomationTransaction` is the only path. |
| Headless-safe reactive delivery | ✅ (doc gate) | Code is correct (synchronous, not `callAsync`); **header + regression test** must land first (blocker 3). |
| RealtimeCritical semantics | ✅ | Verified agents never on audio thread; re-scope latency claim to ~100–200 ms. |
| Safe-Mode vs safe-tier consistency | ✅ | Wording harmonized (§2.2). |
| "Did it help?" auto-rollback | ⚠️ **partial/blocker** | **Blocked** on finishing `ABCompareEngine` (blocker 4). |
| Auditability of gate-free safe-tier auto-apply | ⚠️ **gap** | Requires `IntegrationEvent.source="safe_tier_auto"` stamp. |
| SignedPlanCache replay-integrity / fingerprint | ⚠️ **blocker** | Cache+gate don't exist; fingerprint domain under-specified (blocker 2). |
| `master render` shared-state clobber | ⚠️ **gap** | Snapshot semantics + concurrency test. |
| RT-agent retroactive reversibility | ⚠️ **gap** | Synthesize `plan_id`, replay via `master.apply`. |

---

## 8. Pre-existing bugs discovered by the audit (independent of the redesign — fix-first)

These are in **shipped** code; the redesign neither caused nor depends on them, but they should be fixed before the agent path carries more load.

| Bug | Evidence | Fix |
|---|---|---|
| **`BlackboardBridge::onPublish_` data race** (UB) | `BlackboardBridge.h:81`, `BlackboardBridge.cpp:26` | `shared_ptr<std::function>` atomic + TSAN test (§7.1 #1) |
| **`RealtimeControlAgent.h` lies about `callAsync`** (doc-integrity footgun) | `.h:14-20` vs `.cpp:122-127` | Rewrite header + regression test (§7.1 #3) |
| **O1 neural path untested** | `TestAgentE2E.cpp:63-70` | Tool-aware `FakeInvoker` + path assertions (§7.1 #5) |
| **`AnalysisAgent` leaks `std::async` threads on timeout** | `AnalysisAgent.cpp:48-66` | Cooperative `stop_token` cancellation **or** hard-fail + bounded collector joined at teardown; semaphore cap on concurrent reads |
| **Pump-thread serialization under event bursts** (RT correction runs full transaction synchronously) | `RealtimeControlAgent.cpp:123-135` | Cheaper transaction variant for RealtimeCritical (skip `memory.recordOutcome`, defer ledger) **or** document the burst-latency ceiling; **do not** reintroduce `callAsync` |

---

## 9. Phased implementation sequence (adjusted per verification)

| Phase | Goal | Key changes | Depends on |
|---|---|---|---|
| **0 — Preconditions + additive spine** | Harden concurrency; ship the `master.*` verb + cache/gate dormant behind default-off flags; land O1 fix + its tests. | Fix `onPublish_` race; rewrite `RealtimeControlAgent.h` + regression test; O1 tool-aware tests; add `master.{analyze,preview,apply,render}` dispatch cases delegating to existing handler bodies; add `SignedPlanCache` + the `DefaultToolInvoker` confirm gate **with their own unit tests + the corruption-path test**, default-off; pin the fingerprint domain. | Nothing (genuinely additive for the dispatch cases; cache/gate are new stateful subsystems with their own tests). |
| **1 — Cockpit UI + meters** | Beginner-floor landing surface reusing the most existing machinery. | Cockpit panel via `setVisibleTabs`; measured-vs-target meters (Timer); preset row + Loudness/Character knobs + Apply; Deviation chip; 6 outcome cards (`MasteringIntentProfile` w/ per-card tests); extend `OnboardingOverlay`; bound `AnalysisAgent` async leaks; resolve O1 `target_lufs` default from the genre profile; finish the friction-resolution completeness items (§7.2). | Phase 0 |
| **2 — Orchestration unification + A/B at apply** | Collapse the dual-orchestration split; wire a *finished* `ABCompareEngine` to the apply moment. | **Finish `ABCompareEngine` first** (SpectralBalanceAnalyser + 3rd metric + rollback test + call site); refactor `LLMChatClient::chat()` → `submitGoal(origin="chat")`; Conductor fast-path (pinned threshold + latency test); Plan+Commit screen; "Did you mean \<card\>?" structured confirm; RT-agent reversal via synthesized `plan_id`. | Phase 0 + Phase 1 |
| **3 — Deletion + gate enforcement** | The actual complexity removal. | **Delete** the `mastering.neural_apply` COMMIT door (`MCPToolHandler.cpp:7259-7397`); turn the hard gate ON for all origins; turn `MORE_PHI_UNIFIED_MASTER_VERB` ON by default; remove `MORE_PHI_LEGACY_CHAT_LOOP` after one release; collapse the `~10` dispatch cases; reconcile CAPABILITIES.md (§10). | Phase 2 stable across a release soak |
| **4 — Raw disclosure + discrete polish** | Expert escape hatch correct in the semantic default. | "Raw Parameters (debug/expert)" disclosure via the lease; surface discrete/binary/enum as toggles/dropdowns/menus; final terminology pass (zero internal names on Layer 1). | Phase 3 |

---

## 10. CAPABILITIES.md doc reconciliation (drift → reality)

| Doc claim | Reality | Action |
|---|---|---|
| **§9/§11:** unified 7-agent orchestration; chat uses the agent roster | Chat runs a flat loop with **zero** `submitGoal`/Conductor refs | State `submitGoal` is the single planning entry for chat/cards/cockpit/MCP; `LLMChatClient` is transport+render; flat loop is removed (one-release fallback then deleted). |
| **§15/§16:** Learn Mode `isExposed` "only relevant parameters sent to AI" (enforced gate) | `isExposed`/`TokenOptimizer` bypassed by the mastering path | Mastering defaults to the 6-group TargetVector independent of Learn Mode; Learn Mode governs the Raw Parameters (debug) inspector only. |
| **§18:** A/B "auto-rollback on ≥2 worse metrics" | `ABCompareEngine` is an **unfinished 2-metric stub**, never wired to mastering | Mark as **requires completion**; once finished + wired to `master.apply` (Phase 2), it's the primary driver. |
| **§8:** built-in mastering chain "drives Ozone when active" | `OzonePlanApplicator` path is conditional; built-in chain dormant by default | Document the three apply states (`model_unavailable`/`no_hosted_plugin`/`unmapped`/`applied`) + deterministic fallback. |
| **§7:** `mastering.neural_apply` = "apply validated plan with safety checks" (peer of `sonicmaster_decision`) | It's the COMMIT door that **re-runs inference** at apply time | Replace with the `master` verb; mark `mastering.neural_apply` as a deprecated alias → `master.apply`, then removed. |
| **§9 roster / §22** "19 profiler sections" | CLAUDE.md says **13** | Reconcile the count against `prepareToPlay` and update both docs to match. |

---

## 11. Risk register & open questions (abridged — full register in the verification artifact)

**High severity:** onPublish_ race (existing) · stale-header callAsync reintroduction (existing) · SignedPlanCache silent-corruption replay (design) · ABCompareEngine can't catch spectral regression (existing) · O1 neural path untested (existing).

**Medium:** `std::async` leak under hung analysis tools · pump-thread serialization under clip storms · outcome-card projections map wrong (new data, no source) · chat-loop deletion loses NVIDIA/reasoning-model transport recovery · Conductor fast-path threshold unspecified → chat responsiveness regression · `master.render` shared-state clobber.

**Open questions to pin before Phase 0/2:** (1) `SignedPlanCache` hash algorithm + audio-window + signal-path-param domain; (2) `plan_id` TTL / invalidation signal (plugin-reload? preset-change?); (3) Conductor fast-path threshold rule; (4) preset-struct serialization home (APVTS XML vs snapshot-bank) — avoid a new persistence path; (5) ONNX-unavailable Cockpit fallback indicator ("engine: neural | rule-based"); (6) whether `Autopilot` remains a distinct internal level or merges into the third plain-English segment (the hard gate makes CoPilot/Autopilot behave identically for unsafe writes).

---

## 12. How to read this document

- **§2–§5** is the proposed redesign (the four deliverables). It is **internally consistent** — the verification's contradictions have been resolved into the text above.
- **§7** is the gate: the redesign is **not** shippable as-is. Blockers 1, 3, 5 and the `std::async` leak are **existing-code** fixes that should land regardless; blockers 2 and 4 are design components that must be built (cache/gate) or finished (`ABCompareEngine`) before the phases that depend on them.
- **§8** is independent value: bugs the audit found in shipped code.
- **§9** is the implementation order, adjusted so preconditions precede features and deletion comes last.

*Evidence base: the friction map, the three design proposals, the judge verdicts, and the full adversarial critique + risk register are preserved in the workflow artifacts (understand → design → verify). Every claim above traces to a `file:line` citation in those artifacts.*
