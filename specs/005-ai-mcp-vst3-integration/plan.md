# 005 — Optimized AI ↔ MCP ↔ VST3 Integration

**Status:** Specification (target architecture)
**Scope:** More-Phi VST3 plugin — AI assistant, embedded MCP server, realtime audio engine
**Grounding:** This document describes the *optimized target* architecture. It is written against the **existing** More-Phi implementation (`src/AI/`, `src/Plugin/`, `src/Host/`), which already implements the majority of this design. Each section marks what is **[BUILT]** vs **[GAP]** so the spec doubles as a gap analysis and roadmap.

---

## 1. Goals & Design Principles

The AI assistant must feel instantaneous and trustworthy: commands execute on the audio engine with **verified, reversible** results, and the user is told — in plain language — what changed and whether it landed.

| Principle | Meaning |
|-----------|---------|
| **Latency-first** | The hot path (interpret → execute → verify) targets sub-50 ms for single-parameter edits. The LLM is a *fallback* for ambiguous intent, not a tollgate on every command. |
| **Verification-first** | No write is "done" until a structured receipt confirms it. Every mutation returns before/after state and a rollback handle. |
| **Realtime-safe** | The audio thread never blocks, locks, allocates, or does I/O. All AI/MCP writes cross to audio through the existing lock-free queue + atomics. |
| **Reversible** | Mutations are journaled in an automation ledger so any change can be rolled back. |
| **Idempotent where possible** | Replaying a tool call with the same args yields the same state, so retries are safe. |

---

## 2. Existing Baseline (what is already built)

The system is not greenfield. More-Phi already ships:

- **Embedded MCP server** — TCP JSON-RPC 2.0 on a localhost port allocated by `InstanceRegistry` (from 30001), per-instance identity + bearer-token auth, and `TokenOptimizer` rate limiting (`src/AI/MCPServer.cpp`). **[BUILT]**
- **Standalone MCP server** — a separate stdio JSON-RPC binary for host-external Ozone/iZotope workflows (`src/AI/StandaloneMcp/`). **[BUILT]**
- **Tool registry** — 50+ namespaced tools (`more_phi.*`, `hosted_plugin.*`, `analysis.*`, `mastering.*`, `plugin_profile.*`, `automation.*`, `workflow.*`, `izotope_ipc.*`, `ozone.*`) registered in `MCPToolHandler.cpp` (~line 1710). **[BUILT]**
- **AI assistant agent loop** — `AIAssistant` runs an LLM tool-calling loop; tool calls dispatch through a `LocalMcpClientSession` (TCP loopback) **or** fall back to in-process `MCPToolHandler::handle` (`LLMChatClient.cpp` ~line 1089). **[BUILT]**
- **Automation transaction ledger** — write-capable tools are wrapped by `dispatchWithAutomationTransaction`, which journals each mutation with diff-preview and rollback (`automation.history / get_transaction / rollback / diff_preview`). **[BUILT]**
- **Workflow engine** — durable `WorkflowRun` DAGs with steps, dependencies, retries, and `predict_next` (`workflow.*`). **[BUILT]**
- **Realtime-safe handoff** — `LockFreeQueue<ParamCommand, 8192>` + relaxed atomics carry UI/MCP/AI edits to the audio thread; "immediate assistant flush" applies edits without waiting for the next audio block. **[BUILT]**
- **Analysis / metering** — `analysis.get_summary / get_spectrum / get_stereo_field / capture_window / compare_render` provide the "audio state before/after" data that verification needs. **[BUILT]**

### Current blockers / gaps the optimized design must close

- **[GAP-CRITICAL] The AI↔LLM transport link is broken.** The assistant currently fails with `HTTP 400: Expecting ':' delimiter…` against NVIDIA NIM. Root cause under investigation in the request-transport layer (`LLMChatClient` / `LLMConnectionValidator` WinHTTP path): the request body is built correctly by nlohmann/json, so the malformation appears at the HTTP boundary (suspected `Content-Length`/encoding mismatch corrupting the body NVIDIA receives). **Until this link is robust, the entire pipeline is non-functional** — this is prerequisite #1.
- **[GAP] The embedded assistant uses TCP loopback as the primary dispatch path** and only falls back to in-process dispatch. Loopback serialization adds avoidable latency for the most common case (assistant and plugin in the same process).
- **[GAP] No fast local intent pre-parser.** Every command — even `set output gain to -3` — pays a full LLM round-trip.
- **[GAP] Verification receipts are not uniform.** Some tools return rich receipts; others return bare success booleans. The AI has no single contract to validate against.
- **[GAP] No read-side caching** — the agent loop re-reads parameter lists / analysis within a single turn.

---

## 3. Optimized Target Architecture

### 3.1 Component view

```
                         ┌───────────────────────────────────────────────────────────┐
                         │                       MorePhiProcessor (audio host)         │
                         │                                                           │
   external agents ──TCP──►  Embedded MCP Server (JSON-RPC, port 30001+, bearer auth) │
   (DAW-external,         │         │                                                 │
    standalone MCP,       │         │  dispatch                                       │
    npx mcp clients)      │         ▼                                                 │
                         │  ┌─────────────────────┐    in-process (zero-copy)          │
                         │  │  MCPToolHandler      │◄──────────────────┐               │
                         │  │  + workflow engine   │                   │               │
                         │  │  + automation ledger │                   │               │
                         │  └──────────┬───────────┘                   │               │
                         │             │ write tools                   │ fast path     │
                         │             ▼                               │ (no socket)   │
                         │  ┌─────────────────────┐      ┌──────────────────────────┐  │
                         │  │ AutomationTransaction│◄────►│  AIAssistant (agent loop) │◄─┘
                         │  │  ledger + rollback   │      │  intent → LLM (fallback) │
                         │  └──────────┬───────────┘      └─────────────┬────────────┘  │
                         │             │                                  │               │
                         │             │  ParamCommand                    │ Verification  │
                         │             ▼                                  │ Receipt       │
                         │  ┌─────────────────────┐      ┌───────────────▼────────────┐  │
                         │  │ LockFreeQueue<8192>  │─────►│   Audio thread (processBlock)│ │
                         │  │ + atomics (no locks) │      │  MorphProcessor / engines /  │ │
                         │  └─────────────────────┘      │  ParameterBridge → hosted    │ │
                         │                                └──────────────────────────────┘ │
                         └───────────────────────────────────────────────────────────┘
```

### 3.2 The two dispatch paths

The MCP server is the **middleware** between intent and the audio engine, but there are two consumers and therefore two paths:

1. **External clients** (standalone MCP server, external agents, `npx` MCP clients) → **TCP JSON-RPC** → `MCPServer` → `MCPToolHandler`. This path is required for cross-process clients and must stay. **[BUILT]**
2. **The embedded AI assistant** (assistant + plugin in the same process) → **in-process direct dispatch** → `MCPToolHandler::handle`, **bypassing the TCP loop**. **[OPTIMIZE: promote fallback to primary]**

**Why:** The TCP loopback path pays serialize → socket write → read → deserialize → dispatch → serialize → socket → deserialize on *every* tool call. For the embedded assistant this is pure overhead. The optimized design makes in-process dispatch the primary path for the embedded assistant and keeps TCP only for external clients. The same `MCPToolHandler` serves both, so tool semantics, verification, and the ledger are identical regardless of path.

```
Embedded assistant tool call (optimized):
  AIAssistant ──(in-process)──► MCPToolHandler::handle(method, params)
                                     │
                                     └─► [execute] ─► [verify] ─► VerificationReceipt (struct, no serialization)

External client tool call (unchanged):
  Client ──TCP/JSON-RPC──► MCPServer ─► MCPToolHandler::handle ─► ... ─► JSON receipt over socket
```

### 3.3 Thread domains (realtime contract)

| Domain | What runs here | Rules |
|--------|----------------|-------|
| **Audio thread** | `processBlock`, morph/physics/modulation, parameter application | No locks, no allocation, no I/O, no throwing. Reads via seqlock/atomics; writes only via the lock-free queue. |
| **Message thread** | UI, deferred hosted-plugin load, full-state recall | Owns hosted-plugin parameter reads (message-thread-only, per H10 fix). |
| **MCP/connection threads** | JSON-RPC handling, auth, tool dispatch | Never touch audio state directly — enqueue `ParamCommand`s and read atomics. |
| **Agent thread** | `AIAssistant` LLM loop, intent parsing, receipt validation | Network I/O allowed; communicates to audio only through the queue + the ledger. |
| **Background workers** | `ThreadPool`, `ChainPlanExecutor`, offline renders | Long ops (mastering renders, dataset) run here and report job IDs. |

**Contract:** any component may *enqueue* a write; only the audio thread *applies* it. Verification reads back the resulting atomic/seqlock state (never probes the audio thread synchronously).

---

## 4. Tool Mapping & Prioritization

All capabilities are exposed as discrete tools with a JSON-Schema input contract and a structured output. Tools are grouped into **domains** and assigned a **priority tier** by execution frequency × user impact. Highest-tier tools receive the most optimization attention (caching, batching, fast-path dispatch).

### 4.1 Tool taxonomy (representative; full registry in `MCPToolHandler.cpp`)

| Domain | Representative tools | Input shape | Output shape | RT-safe write | Tier |
|--------|----------------------|-------------|--------------|---------------|------|
| **Hosted-plugin params** | `set_parameter`, `set_parameters_batch`, `get_parameter`, `list_parameters`, `hosted_plugin.*` | `{stableId\|index\|name, value}` | `{ok, applied:{id,value}, transaction_id, rollback_id}` | queue | **T0** |
| **More-Phi controls** | `more_phi.set_parameter(s)`, `more_phi.get_parameter`, `more_phi.parameters` | `{parameter_id, value}` | `{ok, value_before, value_after, transaction_id}` | queue | **T0** |
| **Morph / navigation** | `set_morph_position`, `get_morph_state` | `{x,y,fader,source}` | `{ok, x,y,fader, occupied_slots}` | atomics | **T0** |
| **Snapshots** | `capture_snapshot`, `recall_snapshot`, `hosted_plugin.capture_state` | `{slot, mode}` | `{ok, slot, occupied:[…]}` | queue | **T1** |
| **Analysis (read)** | `analysis.get_summary/get_spectrum/get_stereo_field/capture_window/compare_render` | `{resolution, window_seconds}` | compact meter/spectrum object | read-only | **T1** (cacheable) |
| **Mastering** | `mastering.plan_preview/apply_plan`, `apply_mastering_plan`, `render_batch/status/select_candidate` | `{genre_index, dynamic_range, …}` | plan + applied diff; render → `{job_id}` | queue + async | **T2** |
| **Semantic profiles** | `plugin_profile.describe_semantic_map/apply_safe_action/restore_safe_snapshot` | `{action, allow_caution, dry_run}` | semantic controls; `{rollback_id}` | queue | **T1** |
| **Verification/ledger** | `automation.history/get_transaction/rollback/diff_preview` | `{transaction_id}` | ledger entry / diff | n/a | **T0** (cross-cutting) |
| **Workflow orchestration** | `workflow.create/submit/execute/get/list/predict_next/cancel` | `{user_intent, steps[]}` | `{workflow_run_id, status}` | async | **T2** |
| **Diagnostics** | `diagnose_parameter_pipeline`, `run_self_test` | `{…}` | health report | read-only | **T2** |
| **Hosted lifecycle** | `hosted_plugin.scan/load`, `ozone.audit_parameters` | `{path}` | plugin description | message thread | **T3** |
| **iZotope IPC** | `izotope_ipc_attach/detach/status/snapshot/capture`, `ozone_run_assistant` | `{segment_name, …}` | IPC probe / assistant result | external | **T3** |

**Priority tiers**
- **T0** — highest frequency + highest impact (parameter writes, morph, verification). Optimize first: in-process dispatch, batching, uniform receipts.
- **T1** — frequent reads + structural changes (snapshots, analysis, semantic actions). Optimize: caching, before/after capture.
- **T2** — heavy/async (mastering, workflows). Optimize: async job model, retries, predict_next.
- **T3** — rare/external (plugin loading, IPC). Correctness over latency.

### 4.2 Canonical tool contract (input → output → states)

Every tool conforms to one contract so the AI (and tests) can validate uniformly:

```
Input:  JSON object matching the tool's JSON-Schema (see registry).
Output: {
  ok:      bool,                 // execution status
  tool:    string,               // tool name echoed
  data?:   object,               // tool-specific payload (reads)
  receipt?: VerificationReceipt, // present for every WRITE tool (see §5)
  error?:  { code: string, message: string, remedy?: string }  // present iff !ok
}
States:  ok=true (applied + verified) | ok=false with error+remedy | deferred {job_id} (async)
```

---

## 5. Execution Verification Protocol

Verification is the backbone of trust. Every **write** tool returns a `VerificationReceipt`; the AI validates it before telling the user the request succeeded.

### 5.1 VerificationReceipt schema

```
VerificationReceipt {
  transaction_id:  string        // ledger entry (automation.get_transaction)
  status:          "applied" | "partial" | "rejected" | "deferred"
  changes: [                      // per-parameter, what actually changed
    { id, name, before, after, normalized_delta }
  ]
  audio_state: {                  // DSP-level confirmation (from analysis.*)
    before_ref:  object,          // captured pre-write snapshot handle/summary
    after_ref:   object,          // captured post-write snapshot handle/summary
    delta:       object           // analysis.compare_render(before, after)
  }
  rollback_id:     string|null   // handle for automation.rollback
  errors:          [ … ]         // empty on success
  latency_ms:      number        // interpret→execute→verify budget for this call
}
```

### 5.2 Capture sequence (the verify step)

```
                 ┌─────────── pre-snapshot (message/agent thread) ───────────┐
                 │  analysis.capture_window  →  before_ref                     │
                 │  relevant get_parameter   →  value_before                  │
                 └────────────────────────────┬───────────────────────────────┘
                                              │
   execute:  enqueue ParamCommand(s) ─► lock-free queue ─► audio thread applies
                                              │
                 ┌─────────── post-snapshot (after flush settles) ───────────┐
                 │  relevant get_parameter   →  value_after                   │
                 │  analysis.capture_window  →  after_ref                     │
                 │  analysis.compare_render(before_ref, after_ref) → delta    │
                 └────────────────────────────┬───────────────────────────────┘
                                              │
                 build VerificationReceipt, journal in automation ledger ─► return to AI
```

- **`before`/`after` parameter values** come from the realtime-safe read path (atomics/seqlock), never a synchronous audio-thread probe.
- **`audio_state.delta`** uses the existing `analysis.compare_render` so the receipt contains a real DSP-level before/after (e.g., "peak −0.4 dBTP, low-mid energy −1.2 dB"), not just a parameter echo.
- **Atomicity:** all writes inside one user turn commit as a single `AutomationTransaction`, so the receipt describes the whole turn and rolls back atomically.

### 5.3 AI confirmation logic

```
on receipt of VerificationReceipt r:
    if r.status == "applied" and r.errors.empty():
        confirm to user: list r.changes (humanized) + the headline audio_state.delta
        (e.g., "Lowered the 250 Hz region by ~1.2 dB; output peak now −1.0 dBTP.")
    elif r.status == "partial":
        report what applied vs. what didn't, with rollback_id offered
    elif r.status == "rejected":
        surface r.error.message + r.error.remedy; do NOT claim success
    elif r.status == "deferred":
        give user the job_id and a polling/notify promise (workflow/render)
```

### 5.4 Failure handling & corrective suggestions

- Every `error` carries a `remedy` hint (e.g., parameter out of range → clamp suggestion; hosted plugin not loaded → point to `hosted_plugin.load`; queue saturated → retry/backoff).
- Rejected/partial writes are still journaled so `automation.rollback` can restore the pre-turn state.
- Persistent failures are surfaced to `diagnose_parameter_pipeline` so the user (and the assistant) get a single health report (queue health, plugin availability, flush readiness, restore state).

---

## 6. Performance Optimization

| Strategy | Mechanism | Status |
|----------|-----------|--------|
| **Intent pre-parsing** | A fast local classifier maps high-frequency, unambiguous commands (`"set X to N"`, `"recall snapshot 3"`, `"bypass"`) to a structured intent → direct tool call, **skipping the LLM entirely**. Ambiguous/complex intents fall through to the LLM. | **[GAP — build]** |
| **In-process fast path** | Embedded assistant dispatches tools directly to `MCPToolHandler::handle`, no TCP serialization. | **[BUILT as fallback → promote to primary]** |
| **Tool result caching** | Read-only tools (`more_phi.parameters`, `analysis.get_summary/get_spectrum`, `plugin_profile.describe_semantic_map`) cached with short TTL + **write invalidation** (any `AutomationTransaction` commit evicts affected cache keys). | **[GAP — build]** |
| **Batching** | `set_parameters_batch` / `more_phi.set_parameters` coalesce many edits into one queue drain and one transaction; the assistant is encouraged to batch within a turn. | **[BUILT]** |
| **Coalescing** | Rapid repeated writes to the same parameter within a turn collapse to the final value (debounce on the queue). | **[PARTIAL — formalize]** |
| **Async execution** | Long ops (`mastering.render_batch`, `workflow.execute`) return a `job_id` immediately; results arrive via poll (`render_status`) or callback. Never blocks the agent loop or audio. | **[BUILT]** |
| **Retries / idempotency** | Workflow steps declare `maxRetries`; tools are idempotent on args so retries are safe. | **[BUILT]** |
| **Token discipline** | `TokenOptimizer` caps request size; semantic maps (`plugin_profile.describe_semantic_map`) expose LLM-safe controls instead of raw parameter dumps, shrinking context. | **[BUILT]** |

### 6.1 Intent pre-parser (the biggest single latency win)

```
parseIntent(userText):
    # Tier-1: deterministic patterns, no LLM
    m = match(r"set\s+(.+?)\s+to\s+(-?\d+(?:\.\d+)?)", userText)
    if m: return {intent:"set_parameter", target:m[1], value:float(m[2]), confidence:high}

    m = match(r"recall\s+(?:snapshot\s+)?(\d+)", userText)
    if m: return {intent:"recall_snapshot", slot:int(m[1]), confidence:high}

    # ... a handful of high-frequency patterns ...

    # Tier-2: ambiguous/compound → LLM with full tool set
    return {intent:"llm", confidence:low}
```

- Tier-1 hits resolve in **<5 ms** with no network and no token cost.
- Every Tier-1 resolution still goes through the **same execute → verify** pipeline, so correctness/verification is identical to the LLM path.

### 6.2 Latency budget (single parameter edit, optimized)

```
interpret (intent pre-parser)   ≤  5 ms
dispatch (in-process)           ≤  1 ms
enqueue + audio apply (1 block) ≤  3–10 ms  (depends on block size)
verify (read-back + compare)    ≤  5 ms
─────────────────────────────────────────
total, hot path                 ≤ ~20 ms     (target: feels instantaneous)
LLM-fallback path               +  300–3000 ms (NVIDIA NIM, cold start up to kTimeoutMsNvidia)
```

---

## 7. End-to-End Request Flow

### 7.1 Happy path (deterministic intent)

```
User: "set output gain to -3"
  │
  ▼
AIAssistant.parseIntent  ──► {intent:set_parameter, target:"output gain", value:-3}   [local, <5ms]
  │
  ▼
MCPToolHandler.handle("more_phi.set_parameter", {parameter_id:"output_gain", value:0.7})  [in-process]
  │   ┌─ pre: get_parameter → 0.85 ; analysis.capture_window → before_ref
  │   ├─ enqueue ParamCommand(output_gain, 0.7)  ─► lock-free queue ─► audio applies
  │   ├─ post: get_parameter → 0.70 ; analysis.capture_window → after_ref
  │   └─ diff = analysis.compare_render(before_ref, after_ref)
  ▼
VerificationReceipt { status:"applied",
                      changes:[{id:"output_gain", before:0.85, after:0.70}],
                      audio_state:{delta:{peak_db:-0.6}},
                      transaction_id:"txn_…", rollback_id:"rb_…" }
  │
  ▼
AIAssistant validates receipt → confirms:
  "Set output gain to −3 dB (0.70). Output peak dropped ~0.6 dB. Done."
```

### 7.2 Compound / ambiguous path (LLM, with tool loop)

```
User: "make the low end tighter and brighter overall"
  │
  ▼
parseIntent → confidence:low → LLM (NVIDIA) with tool set + semantic map
  │
  ▼  (agent loop, kMaxToolIterations)
  ┌─► LLM proposes: workflow.submit(steps:[ low_shelf_cut, high_shelf_boost, limiter_ceiling ])
  │   execute each step via MCPToolHandler (batched transaction)
  │   each step returns a VerificationReceipt → AI validates
  └─◄ if receipts confirm target metrics → stop; else iterate
  │
  ▼
AIAssistant summarizes applied changes + before/after audio delta + offers rollback
```

### 7.3 Async path (mastering render)

```
User: "render 3 mastering candidates"
  → mastering.render_batch{candidate_count:3} → returns {job_id} immediately
  → background ThreadPool renders offline (no audio-thread involvement)
  → AI: "Started 3 candidates (job q1w2). I'll report when they're ready."
  → on completion: mastering.select_candidate{id} → verified apply
```

### 7.4 Verification-aware agent loop (pseudocode)

```
function runAgent(userMessage):
    intent = parseIntent(userMessage)
    plan = intent.confidence == high
             ? [toolCallFromIntent(intent)]
             : llmPlan(userMessage, tools, semanticMap)     # may be multi-step

    txn = automation.beginTransaction(userMessage)
    beforeAudio = analysis.captureWindow()

    for step in plan:
        result = MCPToolHandler.handle(step.tool, step.params)   # in-process
        if not result.ok:
            logFailure(step, result.error)
            suggest(result.error.remedy)
            automation.rollback(txn)            # restore pre-turn state
            return "Couldn't apply: {remedy}. Reverted."
        # result.receipt already contains per-step before/after

    afterAudio = analysis.captureWindow()
    delta = analysis.compareRender(beforeAudio, afterAudio)
    receipt = txn.commit(changes=collected, audio={beforeAudio, afterAudio, delta})

    return confirmToUser(receipt)               # humanized changes + audio delta
```

---

## 8. Gap Analysis → Phased Roadmap

| Phase | Work | Outcome | Status |
|-------|------|---------|--------|
| **P0 — Unblock** | Fix the AI↔LLM transport (`HTTP 400` body corruption in the WinHTTP request path; verify `Content-Type`/`Content-Length`/body-encoding). Without this the assistant cannot reach the model at all. | Assistant can complete a chat round-trip. | **[GAP-CRITICAL]** |
| **P1 — Fast path** | Promote in-process dispatch to the primary path for the embedded assistant; keep TCP for external clients only. | ~10–20 ms cut per tool call. | **[mostly built]** |
| **P1 — Intent pre-parser** | Add deterministic intent classifier for T0 command patterns; route misses to LLM. | Sub-10 ms response for common edits; lower token cost. | **[GAP]** |
| **P2 — Uniform receipts** | Make `VerificationReceipt` the mandatory return for every write tool; wire `analysis.compare_render` into the audio_state delta. | Trustworthy, consistent confirmation. | **[partial]** |
| **P2 — Read caching** | Cache read-only tools with write invalidation on transaction commit. | Fewer redundant reads in agent loops. | **[GAP]** |
| **P3 — Coalescing** | Debounce repeated same-parameter writes within a turn. | Less queue churn. | **[partial]** |
| **P3 — Observability** | Expose per-tool latency/receipt stats via `diagnose_parameter_pipeline`. | Tunable latency budget. | **[partial]** |

---

## 9. Summary

The optimized architecture is, in essence: **keep the mature MCP middleware, but (a) repair the LLM transport link, (b) give the embedded assistant a zero-serialization in-process fast path, (c) front it with a deterministic intent pre-parser, (d) make every write return a uniform `VerificationReceipt` with real DSP before/after, and (e) cache reads and coalesce writes.** The result is a pipeline that resolves common commands in tens of milliseconds, verifies every change against the actual audio output, and can roll anything back — exactly the "execute with verified results, then confirm" contract requested.
