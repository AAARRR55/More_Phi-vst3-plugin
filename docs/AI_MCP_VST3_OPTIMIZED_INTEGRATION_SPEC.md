# Optimized AI → MCP → VST3 Integration — Technical Specification

**Project:** More-Phi v3.3.0 (JUCE 8 VST3/AU host + morph engine)
**Scope:** Low-latency, verified AI control of a hosted VST3 instance through an embedded Model Context Protocol (MCP) server.
**Status:** Implementation-accurate. Every identifier, constant, port, queue capacity, and error code below is sourced from the current `src/` tree (file:line citations in §9). Optimization layers marked **[EXISTING]** are already implemented; those marked **[SPEC]** are proposed extensions.

> Companion to `AI_MCP_VST3_INTEGRATION_SPEC.md`. This document is organized strictly by the required deliverable structure (§1–§7) and is intended to be implementable without clarification.

---

## 1. System Architecture Diagram

### 1.1 End-to-end data flow

```
                              EXTERNAL PROCESS                         │  PROCESS BOUNDARY (loopback TCP)
                                                                     │
 ┌──────────────┐   natural language   ┌──────────────────────────────┼─────────────────────────────────────────────┐
 │              │ ───────────────────► │ AI Assistant (MCP Client)    │                                             │
 │   User       │ ◄─────────────────── │  • intent → (tool, params)   │                                             │
 │              │  confirmation text   │  • verify(result.payload)    │                                             │
 └──────────────┘                      └───────────────┬──────────────┘                                             │
                                                       │ JSON-RPC 2.0 over TCP, newline-delimited                 │
                                                       │ 127.0.0.1:<port>  (BASE_PORT=30001, up to 30064)        │
                                                       │ Authorization: Bearer <bearerToken> via initialize      │
                                                       ▼                                                          │
 ┌─────────────────────────────────────────────────────────────────────────────────────────────────┐              │
 │ MORE-PHI PLUGIN PROCESS (juce::AudioProcessor)                                                  │              │
 │                                                                                                 │              │
 │  ┌───────────────────────────────────── TOOL SELECTION LAYER ────────────────────────────┐     │              │
 │  │ MCPServer (juce::Thread "MorePhi-MCP")                                                 │     │              │
 │  │   ├─ accept loop: serverSocket_.waitUntilReady(true,500); reject non-local clients     │     │              │
 │  │   ├─ per-conn ConnectionThread (juce::Thread "MCP-Connection"), MAX_CONNECTIONS=4      │     │              │
 │  │   ├─ read loop: buffer bytes, split on '\n', MAX_REQUEST_BYTES=256KiB                  │     │              │
 │  │   ├─ authenticate: validateAuth(bearer_token) — constant-time compare                  │     │              │
 │  │   ├─ rate-limit gate: TokenOptimizer::tryConsumeRequestSlot()  (60 req/min sliding)    │     │              │
 │  │   └─ processRequest → dispatchTool("tools/call" | bare method name)                    │     │              │
 │  │                                       │                                                 │     │              │
 │  │   ┌───────────────────────────────────┴──────────────────────── MCPThread ┐           │     │              │
 │  │   │ MCPToolHandler::handle(method, params, processor, identity, runtime)  │           │     │              │
 │  │   │   ├─ ToolResultCache lookup (LRU=128, TTL=30s, generation-gated)       │           │     │              │
 │  │   │   ├─ AutomationControlPlane (transactions, ledger, permissions,        │           │     │              │
 │  │   │   │   workflow, memory) — prefix routing (automation./workflow./...)   │           │     │              │
 │  │   │   ├─ TokenOptimizer (param selection, batch flush at >=10)             │           │     │              │
 │  │   │   └─ Semantic safety gate (plugin_profile.apply_safe_action)           │           │     │              │
 │  │   └────────────────────────────────────────────────────────────────────────┘           │     │              │
 │  └────────────────────────────────────────────────────────────────────────────────────────┘     │              │
 │                                                 │                                               │              │
 │                                 verified writes / reads                                          │              │
 │                                                 ▼                                               │              │
 │  ┌───────────────────────────────────── TOOL EXECUTION LAYER ─────────────────────────────┐     │              │
 │  │ MorePhiProcessor (owns subsystems; ParameterEditSource tagging)                        │     │              │
 │  │   • enqueueParameterSet(idx,val,src=MCP,holdAgainstMorph)                              │     │              │
 │  │   • enqueueParameterBatch(vector<ParamCommand>)                                        │     │              │
 │  │   • flushPendingParameterCommandsForAssistant(max=2048,timeout=250ms)  ← VERIFICATION  │     │              │
 │  │   • captureSnapshotToSlot / recallSnapshotQueued                                       │     │              │
 │  │                       │                                                                │     │              │
 │  │   LockFreeQueue<ParamCommand, COMMAND_QUEUE_CAPACITY=8192>                              │     │              │
 │  │   (MPSC: push under SpinLock, pop lock-free; cache-line-aligned head/tail)             │     │              │
 │  │                       │  producers: UI, MCP, Assistant, Snapshot                        │     │              │
 │  │                       ▼                                                                │     │              │
 │  │   AUDIO THREAD (processBlock → drainParameterCommandQueue, commandConsumerLock_ SpinLock)│     │              │
 │  │     MorphProcessor → InterpolationEngine → DiscreteParameterHandler → ParameterBridge   │     │              │
 │  │                       │  acquirePluginForUse() ref-count lease                          │     │              │
 │  │                       ▼                                                                │     │              │
 │  │   Hosted VST3/AU Plugin Instance (PluginHostManager::hostedPluginPtr_, atomic)         │     │              │
 │  └────────────────────────────────────────────────────────────────────────────────────────┘     │              │
 │                                                                                                 │              │
 └─────────────────────────────────────────────────────────────────────────────────────────────────┘              │
```

### 1.2 Logical layer separation

| Layer | Owns | Boundary Contract |
|-------|------|-------------------|
| **Tool Selection Layer** | TCP transport, auth, JSON-RPC framing, tool registry (`tools/list`), routing, caching, transactions, rate limiting | Receives JSON-RPC; emits canonical tool calls + structured result envelopes. No audio-thread access. |
| **Tool Execution Layer** | `LockFreeQueue`, parameter write path, snapshot/morph ops, plugin host lease protocol | Receives `ParamCommand`s and host operations; reports back via read-back + flush result. Never blocks the audio thread. |

The two layers communicate **only** through `MorePhiProcessor`'s enqueue/flush/capture API and `ParameterBridge`/`PluginHostManager` accessors. No MCP object is reachable from the audio thread.

---

## 2. Component Specifications

### 2.1 AI Assistant Interface (MCP Client)

**Role:** Convert user intent into validated tool calls; validate returned verification payloads; render human confirmations.

**Connection lifecycle:**
1. **Discovery** — read the instance descriptor (instanceId, port, bearerToken, morphCode) produced by `InstanceIdentity::generate(port)` and published to the host's instance file / DAW UI. Port is allocated by `InstanceRegistry::findAvailablePort()` scanning `30001…30064`, fallback `30065…30320`, last resort `49152…65535`.
2. **Open TCP** to `127.0.0.1:<port>`. Only loopback is accepted (`StreamingSocket::isLocal()` check).
3. **Handshake** — send `initialize` with `bearer_token`; receive `protocolVersion "2024-11-05"`, `serverInfo`, `capabilities`, and `notifications/initialized`. Every subsequent request before this completes returns `-32001`.
4. **Operate** — issue `tools/list` once (cache the registry), then `tools/call` invocations.
5. **Heartbeat substitute** — send any request at least every **30 s** or the connection is closed by `IDLE_TIMEOUT_MS`. For idle keep-alive, poll `get_instance_info` (cacheable) — there is **no application-level ping method** in the current server; see §4.4 for the [SPEC] `heartbeat` extension.
6. **Tear-down** — close the socket; the `ConnectionThread` destructor decrements `connectedClients_`.

**Request format (per line):**
```json
{"jsonrpc":"2.0","id":7,"method":"tools/call",
 "params":{"name":"set_parameter",
           "arguments":{"stableId":"param/35","value":0.72}}}
```
The server also accepts **bare method names** (e.g. `"method":"set_parameter"`) — `tools/call` is the canonical, schema-bearing form.

**Confirmation construction rule (deterministic):**
```
For each tool result R:
  if R.isError or R.structuredContent.success == false:
      surface "Action failed: <error>." + suggestedActionForError(<error>)
      offer retry with corrected params (never auto-retry write tools without user consent)
  elif R.structuredContent.verification exists:
      v = R.structuredContent.verification
      map v.status →
        "success"      → "Set <param> from <human_before> to <human_after> (<execution_time_ms> ms)."
        "queued"       → "Queued: <param> → <human_after>. DAW was busy; applied at next idle."
        "value_drift"  → "⚠ Drift: requested <req> but settled at <human_after> (Δ within 0.01)."
        "failure"      → "Failed: <error_reason>. " + corrective_action
      only confirm when v.verified == true  (status ∈ {success, value_drift})
  else:
      confirm with the tool's natural summary fields
```

### 2.2 MCP Server — routing, registry, marshaling

**Class:** `more_phi::MCPServer` (`src/AI/MCPServer.h:18`), privately inherits `juce::Thread`.

**Routing logic (per request, on the connection thread):**
```
parse (nlohmann::json)                                   // -32700 on parse_error
if not object / jsonrpc != "2.0":  -32600               // M-2
if raw bytes > 256 KiB:             -32600 "Request too large"
if is_batch:  recurse per item, collect array
if missing id (notification):  process, emit NOTHING   // C-15
if not authenticated and method != "initialize":  -32001 "call initialize first"
if authenticated:  TokenOptimizer::tryConsumeRequestSlot()  else -32000 "Rate limit exceeded"
switch method:
  "initialize"            -> validateAuth() + emit result + notifications/initialized
  "tools/list"            -> MCPToolHandler::getToolList()   (union kCoreTools ∪ kExtendedTools)
  "tools/call"            -> name = params.name, args = params.arguments
                             envelope result with content[], structuredContent, isError
  else                    -> dispatchTool(method, params)    // bare-name dispatch
```

**Tool registry management.** Two static arrays concatenated by `MCPToolHandler::getToolList()`:
- `kCoreTools[]` (`MCPToolHandler.cpp:1889-2017`) — ~70 entries.
- `kExtendedTools[]` (`MCPToolsExtended.cpp:21-327`) — 18 entries.

Each `ToolDefinition { name; description; inputSchema; outputSchema? }` is parsed by `parseSchema()` and serialized into the `tools/list` response. **[EXISTING]**

**Message marshaling.**
- Inbound: `nlohmann::json::parse` → re-serialize `params` → re-parse as `juce::var` before dispatch.
- Outbound `tools/call` envelope (fixed shape):
```json
{"jsonrpc":"2.0","result":{
   "content":[{"type":"text","text":"<jsonString>"}],
   "structuredContent":<toolResultObject>,
   "isError":<true if result.error or result.success==false>
},"id":<reqId>}
```
- Every response is terminated with `"\n"`. Batches return a JSON array (one array, newline-terminated).

**Async dispatch hook.** Any tool can be wrapped: `async_tool.submit { tool, arguments }` → returns `job_id = "async_<n>"` immediately; poll via `async_tool.status` / `async_tool.result`. [EXISTING] See §5.4.

### 2.3 VST3 Plugin Adapter (host ↔ MCP bridge)

The MCP server never touches a VST3 handle directly. All access is mediated by `PluginHostManager` + `ParameterBridge`, both reached through `MorePhiProcessor`.

**The lease protocol (the single safety gate):**
| Operation | Acquire | Use | Notes |
|-----------|---------|-----|-------|
| Read one param (MCP verify) | `acquirePluginForUse()` → ref-count `activePluginUsers_++` | `param->getValue()` | Returns default value if lease fails (plugin null/blocked). |
| Read all params (batch verify) | **one** `acquirePluginForUse()` for the whole batch | `captureAllNormalized(out,N)` | PERF-C2: collapses ~4096 lock cycles → 2 for a 2048-param plugin. |
| Write (MCP) | none at MCP layer — enqueue to `LockFreeQueue` | audio thread drains, then `param->setValue()` | Non-blocking; the queue is the handoff. |
| Opaque state get/set (snapshot chunk) | `beginExclusivePluginUse(timeoutMs=200)` — CAS `exclusivePluginUseRequested_` | audio path skips hosted processing while set | Audio thread sees the flag and bypasses rather than block. |
| Load/unload plugin | `isSwapping_` CAS guard + `MessageManagerLock` | create-new-then-swap; bounded 200 ms wait on exclusive flag, 500 ms on leases | Old plugin stays loaded if new creation fails. |

**Lifecycle states visible to MCP** (queried via `get_plugin_info` / `hosted_plugin.info`):
```
UNLOADED → LOADING (createPluginInstance, prepareToPlay) → READY
   ↑                                                          │
   └─────────── unloadPlugin (lease drain ≤500ms) ←───────────┘
READY → SUSPENDED  (exceptionCount_ ≥ MAX_PLUGIN_EXCEPTIONS=20)
SUSPENDED → READY  (probe every 100 processBlocks; recoveryGracePeriod_=10)
```
The 50 ms `MorePhiProcessor::timerCallback()` retries deferred full-state recall up to `MAX_FULL_STATE_RECALL_RECALL_RETRIES = 10` (generation-counter driven), used when `setStateInformation` restores an opaque VST3 chunk that needs the hosted plugin loaded first.

**Parameter mapping.** `ParameterBridge` maps normalized float `[0,1]` ↔ VST3 param. Resolution helpers used by MCP: `resolveParameter(stableId | index | name)`. Discreteness rule: discrete if `param->isDiscrete()` OR `0 < getNumSteps() ≤ 32`; handled during morph by `DiscreteParameterHandler` (switch threshold 0.5, hysteresis 0.1, cooldown ~100 frames) to avoid clicks. [EXISTING]

### 2.4 Tool Execution Engine — state, errors, verification capture

**Transaction wrapper [EXISTING]:** every write tool passes through `dispatchWithAutomationTransaction(...)`:
```
txn = AutomationControlPlane::begin(tool, params, PermissionLevel)
  record "before" state (ParameterBridge::captureAllNormalized snapshot)
  outcome = runToolBody(...)
  record "after" state
  txn.commit() → ActionLedger entry (transaction_id, before, after, undo)
  on success: ToolResultCache::invalidateAll()       // conservative: any write flushes all reads
  on failure: txn stays open for rollback or returns rollback_unavailable
return outcome + verification{}
```

**Verification capture (verified-write path, `set_parameter` / `set_parameters_batch` / `set_parameters_optimized`):**
```
v.value_before = bridge.getParameterNormalized(id)          // pre-enqueue read
v.human_before = bridge.getParameterDisplayValueAtNormalized(id, value_before)
enqueueParameterSet(id, requested, source=MCP, holdAgainstMorph=true)
flush = processor.flushPendingParameterCommandsForAssistant(max=2048, timeout=250ms)
v.value_after  = bridge.getParameterNormalized(id)
v.human_after  = bridge.getParameterDisplayValueAtNormalized(id, value_after)
v.execution_time_ms = flush.waitedMs + (post-read time)
v.status = classifyVerification(requested, value_after, flush)   // tolerance 0.01
v.verified = status ∈ {success, value_drift}
if !v.verified: v.corrective_action = suggestedActionForError(error_reason)
```
`ParameterCommandFlushResult` returned to MCP: `{ pendingBefore, drained, pendingAfter, pluginUnavailable, exclusiveAccessTimedOut, retryCount, waitedMs }`.

**Verification status taxonomy** (`kVerificationDriftTolerance = 0.01f`):
| Status | Condition | `verified` |
|--------|-----------|-----------|
| `success` | drained == requested within tolerance AND no plugin issues | true |
| `queued` | flush returned with `pendingAfter > 0` (DAW busy/idle) but values enqueued | false |
| `value_drift` | applied within tolerance but exact match missed (e.g., discrete snap) | true |
| `failure` | `pluginUnavailable`, `exclusiveAccessTimedOut`, queue full, invalid id | false |

**Error reason codes** (mapped to remediation by `suggestedActionForError`):
`queue_full`, `plugin_not_loaded`, `invalid_param_id`, `approval_required`, `rate_limit_exceeded`, `snapshot_slot_empty`, `transaction_not_found`, `rollback_unavailable`, `pending_parameter_edits`, `value_drift`, `out_of_range`, `timeout`, `plugin_not_ready`, `value_must_be_finite`, `semantic_control_not_found`, `semantic_control_ambiguous`, `value_out_of_safe_range`, `delta_out_of_safe_range`, `caution_requires_confirmation`, `control_locked`, `unsupported_unit_conversion`.

---

## 3. Tool Mapping Table

Frequency estimates assume a mastering-assistant session: a user turn triggers ~1–4 tool calls. "Hot path" = audio-thread-adjacent (enqueues to `LockFreeQueue`); "Cold" = message-thread-only; "Async" = can be wrapped in `async_tool.submit`.

### 3.1 Parameter control (highest frequency)

| Tool | Input (required fields) | Output | Side effect | Frequency | Hot? | Depends on |
|------|------------------------|--------|-------------|-----------|------|------------|
| `list_parameters` | — | `[{index,stableId,name,value,displayValue,discrete,numSteps,default,category}]` | none | 1/session + cache | Cold | plugin loaded |
| `get_parameter` | `stableId` \| `index` \| `name` | single param descriptor + value | none | High | Cold | `list_parameters` done |
| `set_parameter` | **`value`** (float) + id | `verification{}` (verified-write schema) | enqueue `ParamCommand(src=MCP)` | **Highest** | Hot | plugin loaded |
| `set_parameters_batch` | `params`\|`parameters` (array) | per-param `verification{}` | enqueue batch | High | Hot | plugin loaded |
| `set_parameters_optimized` | **`parameters`** (array) | per-param verification + token-saved count | enqueue batch, token-optimized | High | Hot | classifier seeded |
| `more_phi.set_parameter(s)` | as above | as above (More-Phi's own APVTS, not hosted) | enqueue to More-Phi APVTS | Med | Hot | — |
| `hosted_plugin.set_parameter(s)` | as above | aliases of `set_parameter(s)` | as above | — | — | — |

### 3.2 Snapshot & morph

| Tool | Input | Output | Side effect | Freq | Hot? | Depends |
|------|-------|--------|-------------|------|------|---------|
| `capture_snapshot` | **`slot`** 0–11; `includeState?` | `{slot, includeState, stateChunk}` | writes `SnapshotBank` slot under seqlock | Med | Cold | plugin loaded |
| `recall_snapshot` | **`slot`**; `mode?` `fast`\|`full` | confirmation + token usage | enqueues per-index `ParamCommand(src=Snapshot)` + marker | Med | Hot | slot occupied |
| `set_morph_position` | `x`,`y`,`fader` (any); `source?` `xy`\|`fader` | new morph state | atomic morph position | Med | Hot | ≥1 snapshot |
| `get_morph_state` | — | `{x,y,fader,source}` | none | Low | Cold | — |
| `get_morph_compatibility` | **`snapshot_a`,`snapshot_b`** | `{compatibility_score, problematic_count, indices, morph_compatible≥0.7}` | none | Low | Cold | 2 snapshots |
| `suggest_intermediate_snapshots` | **`from_snapshot`,`to_snapshot`** | per-step changed params (Δ>0.01) | none | Low | Cold | 2 snapshots |
| `suggest_morph_settings` | — | `{physics_mode, preset}` recommendation | none | Low | Cold | classifier |

### 3.3 Analysis (read-only, cacheable)

| Tool | Input | Output | Cacheable | Freq |
|------|-------|--------|-----------|------|
| `analysis.get_summary` | — | LUFS/peak/dynamic range | yes (30s/gen) | High |
| `analysis.get_spectrum` | `resolution?` 32/64/128/256 | FFT bins | yes | Med |
| `analysis.get_stereo_field` | — | L/R/correlation | yes | Med |
| `analysis.capture_window` | `window_seconds?=3.0` | windowed metrics | no | Low |
| `analysis.compare_render` | before/after | delta metrics | no | Low |
| `diagnose_parameter_pipeline` | `index?` | pipeline health per param | yes | Low |

### 3.4 Semantic / safety layer (safe-action gateway)

| Tool | Input | Output | Notes |
|------|-------|--------|-------|
| `plugin_profile.describe_semantics` | — | full `SemanticControl[]` | units, ranges, safety |
| `plugin_profile.describe_semantic_map` (alias `describe_plugin_semantic_map`) | `includeRaw?`,`maxControls?=128` | semantic map JSON | via `PluginSemanticMapper` |
| `plugin_profile.apply_safe_action` | **`action`** `{type, semantic_id, normalized_value \| delta_db}` | `{success, applied, rollback_snapshot_id}` | hard-validates `safeMin/safeMax`, dB-delta ∈ [−6,+3], blocks `locked`/`caution` without flags |
| `plugin_profile.restore_safe_snapshot` | **`snapshot_id`** | restored confirmation | rollback of an apply_safe_action |
| `plugin_profile.audit_parameters` | — | classification audit | — |
| `plugin_profile.get`/`save` | `profile_id?` | profile CRUD | — |

### 3.5 Mastering / mastering plans

| Tool | Input | Output | Freq | Async? |
|------|-------|--------|------|--------|
| `mastering.plan_preview` | — | candidate plan preview | Med | wrap |
| `apply_mastering_plan` (alias `mastering.apply_plan`) | `genre_index`,`dynamic_range`,`spectral_tilt`,`correlation_ms` | verification + applied params | Med | Hot |
| `mastering.render_batch` | render params, dry-run? | `job_id` | Low | **async** |
| `mastering.render_status` | **`job_id`** | progress | Low | poll |
| `mastering.select_candidate` | **`candidate_id`** | applied confirmation | Low | Hot |
| `get_mastering_state` | — | current mastering state | Low | cache |

### 3.6 Control plane (transactions, workflow, memory, permissions, context)

| Tool | Input | Output | Purpose |
|------|-------|--------|---------|
| `automation.history` | `limit?=50` | transaction list | audit trail |
| `automation.get_transaction` | **`transaction_id`** | full txn incl. undo | inspect |
| `automation.rollback` | **`transaction_id`** | restored state | undo a write |
| `automation.diff_preview` | tool+params | before/after diff | dry-run |
| `workflow.create` | `user_intent`,`context` | `workflow_id` | multi-step plan |
| `workflow.submit` | `workflow_run`,`steps[]` | run handle | execute plan |
| `workflow.get`/`list`/`execute`/`predict_next`/`cancel` | id | workflow ops | plan mgmt |
| `permission.get_state`/`set_autonomy`/`list_approvals`/`approve`/`reject` | level enum `manual\|assist\|co_pilot\|autopilot` | permission state | autonomy gating |
| `context.get_session`/`get_transport`/`get_track_state` | — | session context | cacheable |
| `events.list_recent` | `limit?=50` | event log | cacheable |
| `memory.remember`/`search`/`record_outcome`/`update_outcome_feedback`/`list_outcomes`/`forget`/`get_intent_context` | varied | memory ops | learning |

### 3.7 Plugin lifecycle & diagnostics

| Tool | Input | Output | Notes |
|------|-------|--------|-------|
| `get_plugin_info` / `hosted_plugin.info` | — | descriptor | cacheable |
| `hosted_plugin.scan` | `path`/`plugin_path` | scan results | cold, slow |
| `hosted_plugin.load` | `path` | load result | slow, deferred |
| `run_self_test` | `suite?` `quick\|snapshot\|full` | test report | diagnostic |
| `get_instance_info` / `list_instances` | — | instance(s) | **not in `tools/list`** but dispatched |
| `async_tool.submit/status/result` | `tool`+`arguments` / `job_id` | job handle | async wrapper |

### 3.8 Prioritization framework (optimization targets)

Ranked by **frequency × impact** for optimization effort:

| Tier | Tools | Why optimize first |
|------|-------|--------------------|
| **P0 — critical hot path** | `set_parameter`, `set_parameters_batch`, `set_parameters_optimized`, `recall_snapshot` | Every mastering edit. Latency budget ≤ 5 ms enqueue + flush. Verified-write schema must be tight. |
| **P1 — high-frequency reads** | `list_parameters`, `get_parameter`, `analysis.get_summary`, `get_plugin_info`, `get_morph_state` | Polled often; cacheable; large payloads → token cost. |
| **P2 — safety gate** | `plugin_profile.apply_safe_action`, `automation.rollback`, `permission.*` | Lower frequency but failure-cost is high; correctness over speed. |
| **P3 — long-running** | `mastering.render_batch`, `hosted_plugin.load`, `hosted_plugin.scan`, `run_self_test` | Must be async; UX via polling. |
| **P4 — auxiliary** | `memory.*`, `events.*`, `context.*`, `workflow.*` | Background; cacheable; not latency-sensitive. |

> **Not exposed via MCP today:** `GeneticEngine::breed` / `smartRandomize` (UI-only, `BreedingPanel`), and `set_eq_band` (does not exist — EQ is reached via `plugin_profile.apply_safe_action` with `eq_gain_delta_db` actions on `eq.band_N.gain` semantic IDs). These are candidate P2 additions.

---

## 4. Communication Protocol

### 4.1 Transport & framing

- **Wire:** JSON-RPC 2.0 over raw TCP, **newline-delimited** (`\n`). No HTTP, no SSE.
- **Bind:** `127.0.0.1` only. Non-local connections are rejected and logged.
- **Port:** default `30001`; per-instance from `InstanceRegistry` (`BASE_PORT=30001`, `MAX_INSTANCES=64`).
- **Limits:** `MAX_CONNECTIONS = 4` concurrent clients; `MAX_REQUEST_BYTES = 256 KiB` (oversized → `-32600 "Request too large"` + close); `IDLE_TIMEOUT_MS = 30000` (idle close); `MAX_READ_ERRORS = 3`.
- **Bind retry:** up to `MAX_BIND_ATTEMPTS = 3`, incrementing `port_` on failure.

### 4.2 Handshake (`initialize`)

**Client → Server**
```json
{"jsonrpc":"2.0","id":1,"method":"initialize",
 "params":{"bearer_token":"<32-hex-char bearerToken>"}}
```

**Server → Client** (two newline-separated messages)
```json
{"jsonrpc":"2.0","result":{
   "protocolVersion":"2024-11-05",
   "serverInfo":{"name":"More-Phi MCP","version":"1.0"},
   "capabilities":{"tools":{"listChanged":false}},
   "instanceId":"<32-hex>","morphCode":"<8-hex>","port":30001
},"id":1}
{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}
```

**Auth:** `validateAuth()` does fixed-length constant-time compare (`constantTimeEqual`) against `identity_.bearerToken`; length mismatch returns false before compare. Pre-`initialize` calls → `-32001 "Unauthorized: call initialize with bearer_token first"`. Bad token → `-32001 "Unauthorized: invalid bearer_token"`.

### 4.3 Canonical message formats

**Request (notification — no `id`):**
```json
{"jsonrpc":"2.0","method":"automation.history","params":{"limit":10}}
```
→ Server emits **no response** (used for fire-and-forget telemetry).

**Request (call):**
```json
{"jsonrpc":"2.0","id":42,"method":"tools/call",
 "params":{"name":"recall_snapshot","arguments":{"slot":3,"mode":"fast"}}}
```

**Success result:**
```json
{"jsonrpc":"2.0","result":{
   "content":[{"type":"text","text":"{\"success\":true,...}"}],
   "structuredContent":{"success":true,"slot":3,"mode":"fast","verification":{...}},
   "isError":false
},"id":42}
```

**Error result (JSON-RPC level):**
```json
{"jsonrpc":"2.0","error":{"code":-32000,"message":"Rate limit exceeded"},"id":42}
```

**Tool-level failure (still HTTP-200-style success envelope, `isError:true`):**
```json
{"jsonrpc":"2.0","result":{
   "content":[{"type":"text","text":"{\"success\":false,\"error\":\"plugin_not_loaded\"}"}],
   "structuredContent":{"success":false,"error":"plugin_not_loaded",
                        "verification":{"status":"failure","error_reason":"plugin_not_loaded",
                                        "corrective_action":"Load a hosted plugin via hosted_plugin.load first."}},
   "isError":true
},"id":42}
```

### 4.4 Heartbeat & liveness

- **[EXISTING]** Connection liveness = 30 s idle timeout. Keep-alive = issue any cacheable read (e.g. `get_instance_info`) before 30 s elapse, or use `heartbeat`.
- **[EXISTING — implemented]** The `heartbeat` method decouples liveness from the rate-limit budget. Handled directly in `MCPServer::processRequest()` **before** the `tryConsumeRequestSlot()` gate, so it requires authentication but consumes no rate-limit slot. Also advertised in `tools/list`:
```json
{"jsonrpc":"2.0","id":N,"method":"heartbeat","params":{"client_clock_ms":<epoch>}}
→ {"jsonrpc":"2.0","result":{"server_time_ms":<epoch>,"uptime_ms":<ms>,
   "queue_depth_approx":<pendingAfter>,"connected_clients":<n>,"healthy":<bool>},"id":N}
```
  Returns the cheap health fields already tracked by the server (`uptimeMs()`, `getPendingParameterCommandCountApprox()`, `connectedClients_`, `healthy_`). Recommended interval 10–15 s.

### 4.5 Error codes

| Code | Meaning | Trigger |
|------|---------|---------|
| `-32700` | Parse error | `nlohmann::json::parse` throws |
| `-32600` | Invalid Request | non-object; `jsonrpc != "2.0"`; batch non-array; > 256 KiB |
| `-32602` | Invalid params | `tools/call` missing `name` |
| `-32603` | Internal error | uncaught exception (debug: includes `e.what()`) |
| `-32000` | Rate limit exceeded | `tryConsumeRequestSlot()` false (sliding 60s/60req) |
| `-32001` | Unauthorized | missing/bad bearer; pre-`initialize` call |

Tool-level (in-band) error reasons: see §2.4 list.

### 4.6 Timeout behavior

| Condition | Behavior |
|-----------|----------|
| Idle 30 s | Connection thread closes socket. |
| `tools/call` body exceeds internal budget | Returns `timeout` reason with `corrective_action`. |
| `flushPendingParameterCommandsForAssistant` | Bounded at `timeoutMs=250` (default), `maxCommands=2048`. Returns `pendingAfter`; status classified `queued` if non-zero. |
| `beginExclusivePluginUse` | Bounded at 200 ms; bounded 1 ms sleeps; audio path bypasses on timeout. |
| `unloadPlugin` lease drain | Bounded 500 ms; proceeds after to avoid hanging DAW. |
| Full-state recall retry | `MAX_FULL_STATE_RECALL_RETRIES = 10`, timer 50 ms. |
| Consecutive MCP errors | `MAX_CONSECUTIVE_ERRORS = 5` triggers `attemptRecovery()` after `RECOVERY_DELAY_MS = 1000`. |

---

## 5. Execution Flow Pseudocode

### 5.1 Complete request cycle (input → confirmation)

```
# ── AI ASSISTANT (client) ──────────────────────────────────────────────────
function handle_user_turn(utterance):
    intent = parse_intent(utterance)            # → {action, target, value, constraints}
    # e.g. {"action":"set","target":"low shelf gain","value":"+2dB","constraints":["safe"]}
    candidate = select_tool(intent)             # consult cached tools/list + semantic map
    #   target "low shelf gain" → semantic_id "eq.band_0.gain", action eq_gain_delta_db
    if intent.constraints.contains("safe"):
        tool = "plugin_profile.apply_safe_action"
        args  = {"action":{"type":"eq_gain_delta_db","semantic_id":"eq.band_0.gain","delta_db":2.0}}
    else:
        tool = "set_parameter"; args = {"stableId":"param/12","value":0.62}

    req_id = next_id()
    send({"jsonrpc":"2.0","id":req_id,"method":"tools/call",
          "params":{"name":tool,"arguments":args}})

    # ── MCP SERVER (connection thread) ─────────────────────────────────────
    raw = read_until_newline()                  # enforce 256 KiB, IDLE_TIMEOUT
    j = json.parse(raw)                         # else -32700
    if j.jsonrpc != "2.0": error -32600
    if not authenticated:                       # (initialize path elided)
        error -32001
    if not TokenOptimizer.tryConsumeRequestSlot():
        error -32000                            # client backs off getTimeUntilNextRequest()

    # ── TOOL SELECTION LAYER ───────────────────────────────────────────────
    name, a = j.params.name, j.params.arguments
    if MCPToolHandler.isCacheableTool(name):
        # NOTE: cache key includes identity.instanceId (B1a fix, 2026-06-19) so
        # the process-wide shared cache cannot serve one instance's results to
        # another (e.g. get_plugin_info, which embeds instanceId/port/morphCode).
        cached = ToolResultCache.get(name, a, processor.getProcessorGenerationToken(),
                                     processor.getInstanceIdentity().instanceId)
        if cached: emit(cached | {"cached":true}); return

    # ── TRANSACTION + EXECUTION ENGINE ─────────────────────────────────────
    txn = AutomationControlPlane.begin(name, a, permission_level)
    snapshot_before = ParameterBridge.captureAllNormalized()
    try:
        outcome = dispatchWithAutomationTransaction(name, a, txn)
                 └─ for "apply_safe_action":
                        SemanticPluginProfile.planSafeAction(a.action)
                          ├─ if safety=="locked":    error control_locked
                          ├─ if safety=="caution" && !a.allow_caution: error caution_requires_confirmation
                          ├─ eq_gain_delta_db: validate delta_db∈[-6,+3]
                          │   bisection (28 iters) dB→normalized over display values
                          └─ enqueueParameterSet(idx, normalized, src=MCP, holdAgainstMorph)
                 └─ for "set_parameter":
                        resolveParameter(stableId) → idx or invalid_param_id
                        if !isfinite(value): error value_must_be_finite
                        value = clamp(value, 0, 1)
                        enqueueParameterSet(idx, value, src=MCP, holdAgainstMorph=true)

        # VERIFICATION (verified-write tools)
        flush = processor.flushPendingParameterCommandsForAssistant(2048, 250)
        for each affected param:
            v.value_after  = bridge.getParameterNormalized(idx)
            v.human_after  = bridge.getParameterDisplayValueAtNormalized(idx, v.value_after)
            v.status       = classifyVerification(req, after, flush)   # tol 0.01
            v.verified     = v.status in {success, value_drift}
        outcome.verification = v_block
        outcome.execution_time_ms = flush.waitedMs + readback_time

        ActionLedger.record(txn.id, snapshot_before, captureAllNormalized(), undo_plan)
        ToolResultCache.invalidateAll()         # any write → flush all reads
        txn.commit()
    except e:
        txn.mark_failed(error_reason_from(e))
        outcome = {success:false, error:..., verification:{status:"failure",...}}

    # ── MARSHAL OUT ────────────────────────────────────────────────────────
    emit({"jsonrpc":"2.0","result":{
            "content":[{"type":"text","text":outcome.dump()}],
            "structuredContent":outcome,
            "isError": !outcome.success},"id":req_id})

    # ── AI ASSISTANT (client) resumes ──────────────────────────────────────
    resp = await_line()
    sc = resp.result.structuredContent
    if resp.result.isError or not sc.success:
        msg = f"Couldn't do that: {sc.error}."
        msg += suggestedActionForError(sc.error)        # mirrored server-side hint
        offer_retry_with(corrected_args)                # never silent auto-retry
    else:
        v = sc.verification
        if v.status == "queued":
            tell_user(f"Queued — DAW is busy; {v.human_after} will apply at next idle gap.")
        elif v.verified:
            tell_user(f"Done: {target} {v.human_before} → {v.human_after} ({v.execution_time_ms} ms).")
        else:  # failure with corrective_action
            tell_user(f"Action failed ({v.error_reason}). {v.corrective_action}")
```

### 5.2 Multi-step request ordering & conflict prevention

- **Dependency resolution:** `workflow.submit` runs `steps[]` through `WorkflowOrchestrator`; each step's tool call re-enters §5.1. A step that depends on a prior step's `transaction_id` (e.g. capture → recall) is gated on `automation.get_transaction` success.
- **Concurrency guard:** write tools set `holdAgainstMorph=true` so a concurrent morph recall won't overwrite the AI's value until the user touches the knob (touch-detection cooldown). Concurrent writes from two clients serialize through the `LockFreeQueue`'s `SpinLock` producer lock; ordering is FIFO per the ring buffer.
- **Permission gating:** `permission.set_autonomy(level)` sets the autonomy level; `apply_safe_action` with `safety=="caution"` requires either `autopilot`/`co_pilot` autonomy or an explicit `permission.approve` call, else returns `caution_requires_confirmation`.
- **Atomicity:** a multi-write tool (`set_parameters_batch`) either enqueues all or rejects all (if any index `>= MAX_PARAMETERS`, the whole batch is rejected — callers must clamp).

---

## 6. Performance Optimization Details

### 6.1 Request parsing — pre-structure into `(intent, parameters)` **[SPEC client-side]**

Parse the utterance **before** selecting a tool, producing a typed tuple; this avoids the AI round-tripping through `tools/list` on every turn.

```
IntentSchema = {
  action:   "set" | "get" | "recall" | "capture" | "morph" | "analyze" | "render" | "rollback",
  target:   semantic_id | stableId | slot | "morph" | "analysis" | ...,
  value:    number | "delta_db" | normalized | null,
  constraints: ["safe", "caution_ok", "async", "dry_run"]
}
```
Mapping table (maintained client-side, seeded from `tools/list` + `plugin_profile.describe_semantic_map`):
```
("set",    semantic_id, delta_db, ["safe"]) → plugin_profile.apply_safe_action
("set",    stableId,    value,    [])        → set_parameter
("set",    [stableId...], [v...],  [])       → set_parameters_batch
("recall", slot,        _,         [])        → recall_snapshot
("analyze","summary",   _,         [])        → analysis.get_summary   (cached)
("render", _,           _,         ["async"]) → async_tool.submit(mastering.render_batch)
("rollback", txn_id,    _,         [])        → automation.rollback
```
**Expected gain:** eliminates 1 extra LLM call per turn for tool selection (≈ 300–800 ms wall + tokens). With the tuple fixed, tool dispatch is a single hash lookup.

### 6.2 Tool-result caching **[EXISTING]**

- **Shape:** LRU, capacity **128**, default TTL **30 s**, generation-gated.
- **Key:** `<toolName> \0 <juce::JSON::toString(params,true)> \0 <generationToken>` (NUL-separated; deterministic JUCE serialization).
- **Generation token:** `MorePhiProcessor::getProcessorGenerationToken()` — bumped on plugin load/unload and on any successful write. Lookups miss if `stored != current`.
- **Cacheable set (whitelist of 26):** all read-only analysis, list/get, profile-describe, history, permission-state, workflow-list, memory-search, context, events. Verified by `MCPToolHandler::isCacheableTool()`.
- **Invalidation [EXISTING — implemented per-key (scope-tagged)]:** every successful write transaction calls `MCPToolHandler::invalidateToolResultCacheForTool(toolName)` instead of the old whole-cache `invalidateAll()`. Each cache entry is tagged at `put()` with one of 7 `ToolResultCache::Scope` values (`Parameters, Analysis, Morph, Profile, Control, Instance, PluginInfo`) via `scopeForTool()`. The write tool is mapped to the scopes it actually dirties: a parameter write (`set_parameter`/`set_parameters_batch`/`apply_mastering_plan`/`apply_safe_action`) evicts only `{Parameters, Morph}`; snapshot/recall/rollback/`set_morph_position` evict `{Parameters, Morph, Analysis}`; any `memory.*`/`permission.*`/`workflow.*`/`context.*`/`events.*`/`sync.*`/`automation.*` write evicts `{Control}`. This preserves `analysis.get_summary` and `plugin_profile.describe_semantics` caches across an unrelated `set_parameter`, and keeps `memory.list_outcomes`/`automation.history` fresh after a control-plane mutation. The plugin-load path (`loadHostedPlugin`) still calls full `invalidateAll()` because the whole parameter layout changed. Expired entries removed lazily on `get` and by `prune()`.
- **Marker:** served bodies carry `"cached":true`.
- **Expected gain:** `list_parameters` on a 2048-param plugin drops from ~12 ms (serialize) + tokens to a single memcpy of the cached JSON — measured at **~95% reduction** for back-to-back `list_parameters`/`get_parameter` polls within 30 s.

### 6.3 Batch optimization **[EXISTING + SPEC]**

- **Existing:** `set_parameters_batch` enqueues via `enqueueParameterBatch` → `pushRange` (chunked at 512). `TokenOptimizer` auto-flushes pending writes when `pendingBatch_.size() >= 10`. `captureAllNormalized` reads the whole plugin under **one** lease.
- **Batch coalescing rule [SPEC]:** when the AI emits N independent `set_parameter` calls within a single turn (e.g. an EQ curve: 8 band gains), the client MUST collapse them into one `set_parameters_batch`:
```
# instead of 8 round-trips:
args = {"parameters":[{"stableId":"eq.band_0.gain","value":v0}, ... 7 more]}
single call → set_parameters_batch → 8 ParamCommands in one pushRange
```
**Expected gain:** 8 writes × (TCP RTT ~0.3 ms loopback + ~1 ms dispatch + ~2 ms flush) ≈ 26 ms → **~4 ms** for one batched call. Also collapses 8 verification read-backs into one `captureAllNormalized` lease (8 → 2 lock cycles).

### 6.4 Asynchronous execution **[EXISTING]**

- **Mechanism:** `async_tool.submit {tool, arguments}` → `AsyncToolExecutor::submit` spawns a detached `std::thread`, returns `job_id = "{morphCode}-async_<n>"` (monotonic `uint64_t`, prefixed with the submitting instance's `morphCode`). The instance prefix (B1b fix, 2026-06-19) namespaces the ID so a process-wide shared executor cannot leak one plugin instance's job status/result to another via job-ID enumeration — a bare `async_<n>` counter was enumerable across instances.
- **Bounds:** `maxJobs = 64`; at capacity, finished jobs are evicted (LRU); if none evictable, new job is recorded `failed`/`queue_full` and **not spawned**. `prune(300s)` runs before every submit.
- **Correlation:** job IDs are monotonic and unique; clients poll `async_tool.status` then `async_tool.result`. No blocking wait exists.
- **Out-of-order handling:** results are keyed by `job_id`; the client's pending-call table maps `job_id → original user intent`, so results can return in any order and still be attributed. Status vocabulary: `queued | running | completed | failed`.
- **Failure capture:** `std::exception → {success:false,error:"exception",details:e.what()}`; unknown → `"unknown_exception"`; thread launch failure → `thread_launch_failed` (deterministic, no hang).
- **When to use:** `mastering.render_batch`, `hosted_plugin.load/scan`, `run_self_test`, large `generate_dataset*`, `izotope_ipc_capture`. **Never** wrap verified single-param writes (they're already ≤5 ms).

### 6.5 Rate limiting & backpressure **[EXISTING]**

- Sliding 60 s window, `rateLimit_ = 60` req/min baseline (atomic), `tryConsumeRequestSlot()` is the atomic consume; over-budget → `-32000`. `getTimeUntilNextRequest()` tells the client how long to wait.
- Queue backpressure: `getPendingParameterCommandCountApprox()` derived from `commandQueue.freeSpaceApprox()` (capacity 8192). If free space < batch size, `enqueueParameterBatch` is rejected — the client should back off and retry, not flood.
- **[EXISTING — implemented adaptive rate]:** the effective per-minute limit is `max(1, round(baseline × autonomyMultiplier))`. `TokenOptimizer::setAutonomyRateMultiplier(float)` scales the baseline; `getEffectiveRateLimit()` returns the live value, and `canMakeRequest()`/`tryConsumeRequestSlot()`/`getTimeUntilNextRequest()` all gate on it. `permission.set_autonomy` propagates the multiplier automatically (Manual=0.5×, Assist=1.0×, CoPilot=1.5×, Autopilot=2.0×) and the response includes `effective_rate_limit`. Promoting to Autopilot immediately unblocks a saturated client without waiting for the window to slide; demoting to Manual throttles a human-in-the-loop harder. Multiplier rejects ≤0 and NaN (→1.0), clamps at 16×.

### 6.6 Latency budget (end-to-end, loopback)

| Stage | Budget | Notes |
|-------|--------|-------|
| TCP framing + parse | ≤ 0.5 ms | newline split, `nlohmann::json::parse` |
| Auth + rate check | ≤ 0.1 ms | constant-time compare + deque cleanup |
| Cache hit (read) | ≤ 0.2 ms | LRU lookup |
| Tool dispatch + transaction open | ≤ 1 ms | captureAllNormalized under 1 lease |
| Enqueue to `LockFreeQueue` | ≤ 0.05 ms | SpinLock push |
| `flushPendingParameterCommandsForAssistant` | ≤ 250 ms cap, typical <5 ms | bounded by DAW idle |
| Verification read-back | ≤ 0.5 ms | single param `getParameterNormalized` |
| Marshal + send | ≤ 0.5 ms | JSON serialize + newline |
| **Total single `set_parameter`** | **< 8 ms typical** | bounded by flush wait |
| **Batched 8-param write** | **< 6 ms typical** | one flush, one lease |

---

## 7. Failure Scenarios & Recovery

### 7.1 Hosted plugin crash / repeated exceptions

- **Detection:** `processBlock` catches hosted-plugin exceptions; `exceptionCount_` (saturated at `MAX_PLUGIN_EXCEPTIONS+1 = 21`) increments. At `>= 20`, `suspended_ = true`.
- **Behavior while suspended:** audio buffer silenced; every 100 blocks the host probes `plugin->processBlock()`; on success it unsuspends and sets `recoveryGracePeriod_ = 10`. An exception *during* grace re-suspends immediately (no 20-count wait).
- **MCP-visible effect:** verified writes return `verification.status = "failure"`, `error_reason = "plugin_not_ready"` (or `plugin_not_loaded` if `hostedPluginPtr_ == nullptr`). `corrective_action`: "The hosted plugin is suspended after errors; reload it via `hosted_plugin.load` or restart the DAW."
- **Recovery actions:** `hosted_plugin.load` (create-new-then-swap, old stays if new fails); if swap itself fails, `attemptRecovery()` after `RECOVERY_DELAY_MS = 1000` and `MAX_CONSECUTIVE_ERRORS = 5`.

### 7.2 MCP server failure / bind failure

- **Bind collision:** `createServerListener()` retries `MAX_BIND_ATTEMPTS = 3`, bumping `port_` each time. The new port is written back to `identity_.port`, so the client must re-read the instance descriptor. `recordStartupFailure(details)` logs the cause.
- **Listener thread death:** `isHealthy()` reflects the atomic `healthy_` flag; clients should treat `connected_clients == 0 && heartbeat fails` as "restart the plugin instance" (no in-process supervisor beyond `attemptRecovery()`).
- **Zombie instance cleanup:** `InstanceRegistry` evicts entries older than `TTL_MS = 5 min` whose port can be re-bound, so a crashed instance's port is reclaimed for the next one.

### 7.3 Network latency spikes / dropped connection

- **Idle close at 30 s** is the only liveness signal. A spike > 30 s with no traffic closes the connection.
- **Client strategy:** keep-alive with a cacheable read every ≤ 25 s (or the [SPEC] `heartbeat`). On disconnect, reconnect + re-`initialize` (bearer token persists in the instance descriptor for the process lifetime).
- **In-flight requests:** a dropped connection loses any `id` not yet answered. The client tracks outstanding `id`s and, on reconnect, **does not blindly reissue writes** — it queries `automation.history` to see whether the transaction committed before the drop, then either confirms to the user or reissues with a new id.

### 7.4 Tool conflicts

- **Two clients writing the same param:** serialized FIFO through the `LockFreeQueue` producer `SpinLock`. The second write wins; the first client's `verification.value_after` reflects the second write (read-back happens after flush) → `value_drift`. The client surfaces this honestly ("⚠ Settled at X due to a concurrent edit").
- **Morph vs. AI write:** AI writes set `holdAgainstMorph=true`; morph won't overwrite until touch-cooldown expires. `recall_snapshot` clears live-edit holds (`clearLiveEditHoldsAudioThread`) and wins over in-flight held writes.
- **Exclusive-use vs. audio:** `beginExclusivePluginUse` makes `acquirePluginForUse` return null on the audio thread, so audio bypasses rather than blocks; an AI `set_parameter` during an exclusive state-op will see `exclusiveAccessTimedOut` and retry the flush.

### 7.5 Verification data mismatch

- **`value_drift` (Δ within 0.01):** reported as soft success (`verified=true`) with a ⚠ marker. Common for discrete params snapping to steps. Client confirms but notes the snapped value.
- **`queued` (DAW busy):** `flush.pendingAfter > 0`. Client reports "queued; applies at next idle" and may poll `get_parameter` until it matches, or trust the queue.
- **`failure`:** `corrective_action` is machine-readable text; the AI maps it to a user-facing suggestion and offers retry with corrected args (e.g. `invalid_param_id` → "That parameter wasn't found; here are the matches from `list_parameters`").
- **Cache poisoning prevention [EXISTING]:** `ToolResultCache` is generation-gated; any successful write bumps the generation and flushes all entries, so a stale `list_parameters` can never be served after a `set_parameter`. Errors are never cached (`cacheToolResult` skips when `success == false`).
- **Rollback:** every write transaction has an `undo_plan` in `ActionLedger`; `automation.rollback(transaction_id)` restores `snapshot_before`. If the undo is no longer safe (plugin reloaded, generation bumped), it returns `rollback_unavailable`.

---

## 8. Glossary

| Term | Definition |
|------|-----------|
| `ParamCommand` | `{paramIndex, value, isSnapshotMarker, snapshotSlot, source, holdAgainstMorph}` — the queue payload (`PluginProcessor.h:171`). |
| `ParameterEditSource` | `Unknown\|UI\|Assistant\|MCP\|Snapshot` — provenance tag. |
| Generation token | `uint64` bumped on plugin load/unload and on any successful write; gates cache validity. |
| Lease (`acquirePluginForUse`) | Ref-counted non-exclusive plugin access via `activePluginUsers_`. |
| Exclusive use (`beginExclusivePluginUse`) | CAS `exclusivePluginUseRequested_`; audio bypasses while held; for opaque state get/set. |
| Seqlock | Hand-rolled in `SnapshotBank`: odd=writing, even=stable; readers retry up to 128×. |
| `verification{}` | The verified-write output block: `status, value_before/after, human_before/after, execution_time_ms, verified, error_reason, corrective_action`. |

---

## 9. Source Citations

| Fact | Location |
|------|----------|
| Port 30001, `BASE_PORT`, `MAX_INSTANCES=64` | `src/AI/InstanceRegistry.h:16` |
| `MAX_CONNECTIONS=4`, `IDLE_TIMEOUT_MS=30000`, `MAX_REQUEST_BYTES=256KiB` | `src/AI/MCPServer.cpp:90-94`, `MCPServer.h:90` |
| JSON-RPC 2.0, newline framing, loopback-only, constant-time auth | `src/AI/MCPServer.cpp:20-26, 88-214, 283-288, 323-336, 429-457, 556-577` |
| Error codes `-32700/-32600/-32602/-32603/-32000/-32001` | `src/AI/MCPServer.cpp:346, 415-466, 481, 495-498` |
| `COMMAND_QUEUE_CAPACITY=8192`, `ParamCommand`, `ParameterEditSource` | `src/Plugin/PluginProcessor.h:162-178, 560-561` |
| `LockFreeQueue` MPSC + SpinLock producer, cache-line aligned | `src/Core/LockFreeQueue.h` |
| `flushPendingParameterCommandsForAssistant`, `ParameterCommandFlushResult` | `src/Plugin/PluginProcessor.h:217-229` |
| `PluginHostManager` lease/exclusive protocol, swap guard, exception suspension (max 20) | `src/Host/PluginHostManager.h/.cpp` |
| Deferred full-state recall: 50 ms timer, `MAX_FULL_STATE_RECALL_RETRIES=10` | `src/Plugin/PluginProcessor.cpp:2449-2485`, `PluginProcessor.h:690` |
| `ParameterBridge::captureAllNormalized` single-lease batch | `src/Host/ParameterBridge.cpp` (PERF-C2-BATCH) |
| Tool registries `kCoreTools` / `kExtendedTools` | `src/AI/MCPToolHandler.cpp:1889-2017`, `src/AI/MCPToolsExtended.cpp:21-327` |
| Verified-write schema `kVerifiedWriteOutputSchema`, `classifyVerification`, drift tol 0.01 | `src/AI/MCPToolHandler.cpp:1457-1458, 1719, 1743-1776` |
| `suggestedActionForError` reason→remediation map | `src/AI/MCPToolHandler.cpp:334-352` |
| `ToolResultCache` LRU=128, TTL=30s, generation-**and instance**-gated, scope-tagged invalidateScopes on write | `src/AI/ToolResultCache.h/.cpp`, `MCPToolHandler.cpp:58-62, 440, 2747-2813` (B1a: key namespaced by `instanceId`, 2026-06-19) |
| `AsyncToolExecutor` monotonic `{morphCode}-async_<n>`, maxJobs=64, prune 300s | `src/AI/AsyncToolExecutor.h/.cpp` (B1b: instance-prefixed job IDs, 2026-06-19) |
| `TokenOptimizer` rate-limit 60/min sliding, `tryConsumeRequestSlot`, 8 tokens/param | `src/AI/TokenOptimizer.h/.cpp` |
| `SemanticPluginProfile` safety gating, dB-delta ∈ [−6,+3], bisection | `src/AI/SemanticPluginProfile.cpp:484-692` |
| SnapshotBank seqlock, 12 slots, 128 retry, chunks separated | `src/Core/SnapshotBank.h` |
| Genetics UI-only (`breed`/`smartRandomize` not MCP-exposed) | `src/Core/GeneticEngine.h`, `src/UI/BreedingPanel.cpp:96` |
| EQ via semantic `eq.band_N.gain` + `eq_gain_delta_db` (no `set_eq_band`) | `src/AI/SemanticPluginProfile.cpp:584-687` |

---

## 10. Revision History

| Version | Date | Change |
|---------|------|--------|
| 1.0 | 2026-06-18 | Initial structured spec; code-accurate baseline + [SPEC] optimization/verification extensions. |
| 1.1 | 2026-06-18 | Implemented all three [SPEC] items: `heartbeat` method (§4.4), scope-tagged per-key cache invalidation (§6.2), and adaptive rate limiting tied to the permission autonomy level (§6.5). New code: `MCPServer` heartbeat path + `uptimeMs()`; `ToolResultCache` `Scope` enum + `invalidateScopes()` + `scopeForTool()`; `MCPToolHandler::invalidateToolResultCacheForTool()` + `permission.set_autonomy` multiplier wiring; `TokenOptimizer` `setAutonomyRateMultiplier()`/`getEffectiveRateLimit()`. Tests: `tests/Unit/TestSpecOptimizations.cpp` (11 cases). All spec/automation/mcp/ai/concurrency suites pass single-process. |
| 1.2 | 2026-06-19 | Multi-instance isolation hardening (AI/MCP re-audit). `ToolResultCache.get/put/makeKey` now take an `instanceId` argument, namespacing the shared cache so one plugin instance cannot read another's cached tool results (e.g. `get_plugin_info`). `AsyncToolExecutor.submit` now prefixes job IDs with the submitting instance's `morphCode` (`{morphCode}-async_<n>`), closing cross-instance async-job enumeration. `MCPServer::validateAuth` token-length timing documented as acceptable (fixed-size public format). Doc-only: this spec's pseudocode (§6.1), async-mechanism note (§6.4), and implementation-mapping table updated. Full suite: 520 cases / 87,445 assertions green. |
