# Multi-Agent Orchestration Layer — Design

**Status:** Approved design (brainstorming complete). Awaiting spec review, then implementation plan.
**Date:** 2026-06-21
**Scope:** A new multi-agent orchestration layer that sits *above* the existing `MCPToolHandler` / `WorkflowOrchestrator` / `AutomationControlPlane`, reusing their permission/ledger/memory/event subsystems. No duplication of existing infrastructure.
**Out of scope:** Production code. This is a design deliverable; implementation follows via the `writing-plans` skill.

---

## 0. Context — what already exists (do not rebuild)

The More-Phi repo already contains a substantial AI/MCP/automation stack. This design is **additive** and must be read against that baseline.

| Capability | Existing asset (file:line where relevant) |
|---|---|
| Embedded TCP MCP server (JSON-RPC 2.0, bearer auth, thread-per-conn, port from `InstanceRegistry` starting at 30001) | `src/AI/MCPServer.h`, `src/AI/MCPServer.cpp` |
| Tool dispatch chokepoint — every mutating tool becomes an audited `AutomationTransaction` | `src/AI/MCPToolHandler.h:21` (`MCPToolHandler::handle`), `src/AI/MCPToolHandler.cpp` (`dispatchWithAutomationTransaction`) |
| Standalone stdio MCP server (separate executable, same protocol) | `src/AI/StandaloneMcp/StandaloneMcpServer.h` |
| Python MCP server (official SDK) + named-pipe/Unix-socket bridge to the plugin | `scripts/vst3-mcp-server/server.py`, `src/AI/VST3IPCBridge.h` |
| Automation control plane: ledger, permissions, events, workflows, memory | `src/AI/AutomationControlPlane.h` (`AutomationRuntime` composes all five) |
| `WorkflowOrchestrator` — single-DAG workflow runs with approval/retry/rollback | `AutomationControlPlane.h:361` |
| `PermissionKernel` — autonomy levels (Manual/Assist/CoPilot/Autopilot) + per-tool risk classifier | `AutomationControlPlane.h:314`, `AutomationControlPlane.cpp:778` (`classifyTool`) |
| `IntegrationEventBus` — in-memory event ring with publish/listRecent/exportState | `AutomationControlPlane.h:343` |
| `MemoryStore` — per-scope memory with outcome feedback + scoring | `AutomationControlPlane.h:388` |
| Single-agent LLM workflow loop (the pattern this design generalizes) | `src/AI/AIAssistant.h:50` (`AIAssistant::executeLocalWorkflowPrompt`) |
| Lock-free MCP→audio queue | `src/Core/LockFreeQueue.h`, `ParamCommand` in `src/Plugin/PluginProcessor.h:159` |
| `ParameterEditSource` enum (`Unknown/UI/Assistant/MCP/Snapshot`) — already agent-aware | `src/Plugin/PluginProcessor.h:150` |
| Token budgets + rate limiting | `src/AI/TokenOptimizer.h` (`tryConsumeRequestSlot`) |
| Async tool execution + result caching | `src/AI/AsyncToolExecutor.h`, `src/AI/ToolResultCache.h` |

**The genuine gap this design fills:** Today there is *one* workflow runner executing *one* DAG at a time, driven by *one* LLM loop (`AIAssistant`). There is no concept of multiple specialized agents, agent-to-agent delegation, a priority scheduler across agents, or event-driven reactive coordination between agents. The infrastructure to host all of that (permissions, ledger, memory, events, transactions, queue) is already in place — so this design builds only the coordination layer on top.

---

## 1. Goals & Requirements Coverage

### Goals
1. Enable multiple specialized agents to operate concurrently on the plugin, each with a distinct responsibility.
2. Coordinate them via a Conductor (goal decomposition) and a Blackboard (event-driven reactions), without duplicating the existing single-DAG `WorkflowOrchestrator`.
3. Preserve the project's hard real-time invariant: **no agent code on the audio thread, ever.**
4. Reuse `MCPToolHandler::handle` as the single chokepoint so every agent action inherits permission-gating, audit, memory, and event side-effects.
5. Be extensible: adding a new agent must not modify core infrastructure (open registry).
6. Be observable and testable in isolation.

### Requirements (from the originating request) → how met
- *VST3 plugin as audio interface + UI host + comms bridge* → **existing** `MorePhiProcessor`; the agent runtime is a new message-thread subsystem it owns (§3).
- *Low-latency, thread-safe comms between audio thread and orchestration* → **existing** `LockFreeQueue`; agents write targets into it with `source=Assistant` (§4, §5).
- *Robust orchestration of multiple specialized agents* → new `AgentRuntime` + `Conductor` + 6 specialist agents (§3, §6).
- *Agent communication protocols, state management, decision logic* → `BlackboardBridge` + `IAgent::execute` + `AgentState` (§3, §5).
- *Queue + prioritization by realtime demands* → `PriorityScheduler` with `TaskPriority::RealtimeCritical` (§3). Note: "realtime priority" ≠ audio thread (§5, Non-goal 1).
- *Concurrent requests without blocking* → scheduler worker pool; agents never block the audio thread; Conductor collects results asynchronously (§3, Risk R8).
- *MCP server as standardized protocol* → **existing** TCP + stdio MCP servers carry 7 new `agents.*` tools (§7). No new transport.
- *Message schemas for plugin↔orchestration* → `AgentTask` / `AgentResult` / `IntegrationEvent` payloads (§3).
- *Resource management, session handling, connection pooling* → **existing** `InstanceRegistry` (ports, identities, multi-instance); per-agent budgets via `TokenOptimizer` (§3, §8).
- *Retry logic + fault tolerance* → **existing** `WorkflowOrchestrator` retry/rollback; agent-level graceful degradation (R1).
- *Bidirectional comms* → plugin → `submitGoal`/`submitTask`; orchestration → plugin via `MCPToolHandler`→queue (§5 walkthroughs).
- *Unified config* → single `agent_runtime.json` + shipped default (§9).
- *Logging + monitoring across components* → new scoped `StructuredAgentLogger` + `describeState()` + blackboard as audit channel + existing `ActionLedger` (§9).
- *Security boundaries (validate, sanitize, auth)* → reuse existing auth/rate-limit + new capability scope + semantic gatekeeper (§8).
- *Plugin instantiates orchestration OR connects to running instance* → plugin owns the runtime; external clients connect via existing per-instance MCP port (§7).
- *Extensible for new agents without core changes* → open `AgentRegistry` (§3).
- *Unit tests for critical paths* → §10.

---

## 2. Architecture & Placement

### Layered view

```
┌─────────────────────────────────────────────────────────────────────┐
│                       MESSAGE THREAD (UI, MCP)                       │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │            NEW: AgentRuntime  (src/AI/Agents/)              │    │
│  │                                                             │    │
│  │  AgentRegistry ── Conductor ── BlackboardBridge             │    │
│  │      │              │                │                      │    │
│  │   IAgent(×6)   PriorityScheduler   (wraps IntegrationEventBus)  │
│  └──────┬──────────────┬────────────────┬───────────────────────┘    │
│         │ invokeTool() │                │ publish/subscribe         │
│         ▼              │                ▼                           │
│  ┌─────────────────────┴───────────────────────────┐               │
│  │   EXISTING: MCPToolHandler::handle()            │   ◄─ chokepoint│
│  │   → dispatchWithAutomationTransaction()         │     every tool │
│  │     · PermissionKernel.evaluate()               │     already    │
│  │     · ActionLedger.record()                     │     flows here │
│  │     · IntegrationEventBus.publish()             │                │
│  │     · MemoryStore.recordOutcome()               │                │
│  └──────────────────────┬──────────────────────────┘               │
│                         │ enqueueParameterSet(source=Assistant/MCP) │
│                         ▼                                          │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │   LockFreeQueue<ParamCommand, 8192>   (existing)            │   │
│  └──────────────────────┬──────────────────────────────────────┘   │
└─────────────────────────┼───────────────────────────────────────────┘
                          │ drain (lock-free, no-alloc)
┌─────────────────────────┴───────────────────────────────────────────┐
│              AUDIO THREAD (processBlock — RT-safe)                  │
│   drainQueue → MIDIRouter → MorphProcessor → ParameterBridge         │
│   · Agents NEVER enter this thread                                   │
│   · Agents read realtime state only via atomic snapshots/meters      │
└─────────────────────────────────────────────────────────────────────┘
```

### Three governing rules
1. **One chokepoint, never bypassed.** Every agent action that mutates plugin state goes through `MCPToolHandler::handle()`. No agent gets a back door to parameters. This is what makes permission-gating, audit, memory, and events uniform across all agents for free.
2. **Audio thread is a one-way mailbox.** Agents write *targets* (into `LockFreeQueue` with `source=Assistant`/`MCP`); the audio thread drains them. Agents read realtime state *only* through existing atomic snapshots (meters, LUFS, morph position). No agent thread, allocation, or lock ever touches the audio path.
3. **Reuse before build.** Net-new code is the agent-coordination logic (registry, conductor, scheduler, blackboard bridge, the six agents, config, logging). Everything else is reused as-is.

### New vs. reused
| New (in `src/AI/Agents/`) | Reused unchanged |
|---|---|
| `IAgent`, `AgentContext`, `AgentTask`, `AgentResult` | `MCPToolHandler::handle` (chokepoint) |
| `AgentRegistry` | `AutomationRuntime` (ledger/permissions/events/workflows/memory) |
| `Conductor` (generalizes `AIAssistant`'s loop) | `LockFreeQueue<ParamCommand>` |
| `PriorityScheduler` | `PermissionKernel` (autonomy + risk) |
| `BlackboardBridge` (typed pub/sub over `IntegrationEventBus`) | `IntegrationEventBus` |
| 6 agent implementations | `AsyncToolExecutor`, `TokenOptimizer`, `ToolResultCache` |
| `AgentRuntimeConfig`, `StructuredAgentLogger` | APVTS, `InstanceRegistry`, MCP servers (TCP + stdio) |

---

## 3. Core Contracts

### Enums and value types

```cpp
// src/AI/Agents/IAgent.h
namespace more_phi::agents {

enum class AgentRole {
    Conductor, Analysis, Optimization, Creative,
    RealtimeControl, QualitySafety, Memory, Custom
};

enum class AgentState {
    Unregistered, Idle, Busy, Draining, Stopped, Failed
};

enum class TaskPriority {
    Background,   // bookkeeping, memory compaction
    Normal,       // typical analysis/optimization
    High,         // user-initiated goal subtasks
    RealtimeCritical  // reactive correction — jumps the AGENT queue only
};

struct AgentTask {
    juce::String id;
    juce::String runId;            // originating conductor workflow run
    AgentRole   targetRole;
    juce::String intent;           // NL or structured
    nlohmann::json payload;
    TaskPriority priority = TaskPriority::Normal;
    juce::Time   deadline;         // soft deadline
    juce::String origin;           // "user" | "conductor" | <agentId> | "mcp"
};

struct AgentResult {
    juce::String taskId;
    bool success = false;
    juce::String errorCode;
    nlohmann::json findings;                          // structured observations
    std::vector<nlohmann::json> proposedActions;      // tool calls for conductor to re-dispatch
    std::vector<IntegrationEvent> emitEvents;         // blackboard posts
    std::vector<AgentTask>        followUps;          // ONLY honored if returned by Conductor
    nlohmann::json telemetry;                         // tokens, latencyMs, toolsCalled[]
};

} // namespace
```

### The agent contract

```cpp
class IAgent {
public:
    virtual ~IAgent() = default;
    virtual AgentRole    role() const noexcept = 0;
    virtual juce::String id() const noexcept = 0;
    virtual std::vector<juce::String> allowedTools() const = 0;     // capability scope
    virtual std::vector<juce::String> subscribedEventTypes() const { return {}; }
    virtual void prepare(const AgentContext&) = 0;                  // wire dependencies
    virtual AgentResult execute(const AgentTask& task) = 0;         // sync; runs on a scheduler worker
    virtual void onEvent(const IntegrationEvent&) {}                // blackboard callback
    virtual AgentState state() const noexcept = 0;
    virtual void stop() = 0;                                        // cooperative cancel
};
```

Agents are deliberately simple to write: implement `execute(task) → result`, declare capability scope and event interests, own no threading.

### Injection seams

```cpp
// src/AI/Agents/AgentContext.h
class IToolInvoker {
public:
    virtual nlohmann::json invoke(const juce::String& toolName,
                                  const nlohmann::json& params,
                                  const juce::String& agentId) = 0;
};

class IAgentLogger {
public:
    virtual void log(const juce::String& agentId, const juce::String& level,
                     const juce::String& message, const nlohmann::json& fields = {}) = 0;
};

struct AgentContext {
    MorePhiProcessor*       processor = nullptr;
    const InstanceIdentity* identity  = nullptr;
    AutomationRuntime*      runtime   = nullptr;   // ledger/permissions/events/workflows/memory
    IToolInvoker*           tools     = nullptr;   // the chokepoint wrapper
    BlackboardBridge*       blackboard= nullptr;
    IAgentLogger*           logger    = nullptr;
};
```

### The container

```cpp
// src/AI/Agents/AgentRuntime.h
class AgentRuntime {
public:
    AgentRuntime(MorePhiProcessor&, const InstanceIdentity&, AutomationRuntime&);
    ~AgentRuntime();

    void start();
    void stop();

    // Open registry — adding an agent never touches core.
    void registerAgent(std::unique_ptr<IAgent>);
    IAgent* find(AgentRole) const;
    std::vector<AgentRole> registeredRoles() const;

    // Top-level entry: user goal → Conductor decomposes → specialists execute.
    juce::String submitGoal(const juce::String& userIntent,
                            TaskPriority p = TaskPriority::High,
                            const juce::String& origin = "user");

    // Direct entry (event-driven, or MCP `agents.run_task`).
    juce::String submitTask(AgentTask);

    // Observability.
    nlohmann::json describeState() const;
    std::optional<AgentResult> peekResult(const juce::String& taskId) const;
    BlackboardBridge& blackboard() noexcept;

private:
    // ... registry, scheduler, blackboard, result store, config
};
```

### Reactive layer + execution layer

```cpp
// src/AI/Agents/Blackboard/BlackboardBridge.h
class BlackboardBridge {
public:
    explicit BlackboardBridge(IntegrationEventBus&);
    IntegrationEvent publish(const juce::String& source, const juce::String& type,
                             nlohmann::json payload, const juce::String& runId = {});
    void subscribe(const juce::String& agentId,
                   const std::vector<juce::String>& eventTypes,
                   std::function<void(const IntegrationEvent&)>);
    void poll();   // scheduler pumps: reads events since lastRevision_, fans out
};

// src/AI/Agents/Scheduler/PriorityScheduler.h
class PriorityScheduler {
public:
    void start(unsigned numWorkers = 2);
    void stop();
    void submit(std::function<void()>, TaskPriority);
    nlohmann::json stats() const;   // depth per lane, starvation counters
};
```

### The four load-bearing decisions

**D1 — `IToolInvoker` is the chokepoint wrapper and the home of per-agent guardrails.**
The default implementation (`DefaultToolInvoker`) calls `MCPToolHandler::handle()` (so every agent action inherits permission-gating, audit, memory, events for free) and additionally enforces: (a) the agent's `allowedTools()` capability scope (e.g. CreativeAgent cannot call `restore_safe_plugin_snapshot`); (b) per-agent token/rate budget via the existing `TokenOptimizer`; (c) attribution — stamps `agentId` into the workflow run and event `source`. Defense in depth: global `PermissionKernel` risk classification **plus** per-agent capability scope. Mockable in tests.

**D2 — `RealtimeCritical` priority is NOT the audio thread.**
It means "jump ahead in the agent queue." The scheduler runs entirely on message-thread-domain worker threads — never the audio thread. When a RealtimeCritical agent decides to act, it still writes its target through the existing lock-free `enqueueParameterSet(source=Assistant)`. The audio contract (no agent code, no allocation, no lock on the audio path) is preserved exactly. This is the single most important safety property in the design.

**D3 — Per-agent attribution via the ledger, not the audio queue.**
The existing `ParameterEditSource` enum stays coarse (`Assistant`/`MCP`) — the audio thread only needs "agent-like vs UI vs snapshot" for touch-detection. Which specific agent drove an edit is recorded in the `ActionLedger` (`toolName` + `workflowRunId` → Conductor run → agent) and in `IntegrationEvent.source` (free-form string). This avoids bloating an RT-critical enum while giving full traceability off the audio path.

**D4 — The Conductor is just an `IAgent` with `role() == Conductor`.**
No special-casing in the runtime. Its `execute()` reads a user goal, builds a `WorkflowRun` via the existing `WorkflowOrchestrator::createRun`, decomposes into subtasks for specialist agents, submits them, collects results, iterates. This is `AIAssistant::executeLocalWorkflowPrompt` generalized from "one pre-planned DAG" to "decompose across N agents" — the cleanest possible migration story (§12).

---

## 4. Agent Coordination Model

**Hybrid: Conductor + Blackboard.**

- **Conductor** owns top-level goal decomposition and delegation. It is the *only* agent that may submit tasks to other agents (enforced by the runtime — see D-isolation below). This prevents the coordination graph from developing cycles.
- **Blackboard** (`BlackboardBridge` over `IntegrationEventBus`) carries event-driven reactions between agents at the working layer. e.g. AnalysisAgent publishes `analysis.finding`; OptimizationAgent and CreativeAgent may react; QualitySafetyAgent observes proposals. This enables emergence (analyst detects clipping → mastering agent reacts → creative agent suggests alternative) without a new peer-to-peer messaging protocol.

**D-isolation (no-direct-submission rule):** When any non-Conductor *agent* returns `followUps` targeting another specialist, the runtime *drops them and emits a warning event* (it does not enqueue them). Only the Conductor's `followUps` are honored. This is tested explicitly (`TestAgentIsolation`).

> **Scope of the isolation rule (disambiguation):** D-isolation governs *agent-to-agent* delegation only. It does **not** restrict external MCP clients: the `agents.run_task` tool (§7) is an intentional escape hatch allowing a human, external LLM host, or the UI to submit a task directly to a named specialist. That is a *user* submitting to an agent (origin = `"mcp"`/`"user"`), not an agent submitting to another agent, so it does not violate isolation. The Conductor's exclusivity is over *automated* intra-system delegation.

**Acyclicity argument:** With delegation restricted to the Conductor and reactive coordination restricted to events (one-way publishes, never synchronous request/reply between specialists), the coordination graph is a tree rooted at the Conductor plus a DAG of event flows. Feedback-loop risk is bounded by rate caps and budgets (§6, R2) rather than by topology.

---

## 5. Realtime Safety Contract

This section is normative — implementations and reviews must verify it.

| Property | How guaranteed |
|---|---|
| No agent code runs on the audio thread | Agents execute only on `PriorityScheduler` worker threads (message-thread domain). The audio thread (`processBlock`) never calls into `AgentRuntime`. Verified by `TestAgentAudioThreadIsolation`. |
| No allocation on the audio path from agent code | Agents write via `enqueueParameterSet`, whose queue entry is the existing trivially-copyable `ParamCommand` (`PluginProcessor.h:159`). No agent-owned heap reaches the queue. |
| No locks on the audio path from agent code | The queue is lock-free on the consumer side (`LockFreeQueue.h`). Agents never hold a lock the audio thread needs. |
| Agents observe realtime state read-only | Via existing atomic meter snapshots / `get_mastering_state`-style tools queried off the audio thread. Agents never read raw audio-thread internals. |
| "Realtime" latency ceiling | Reactive correction latency ≈ analysis cadence (~100ms, configurable) + queue drain (1–2 audio blocks). This is the maximum real-time-ness achievable safely. Improving it requires reducing analysis cadence, **not** moving agents onto the audio thread. |

**Non-goal 1 (normative):** *Agents on the audio thread.* Any future request to "make agents faster by running them on the audio thread" must be rejected; it breaks the project's foundational RT-safety invariant.

---

## 6. The Six Specialist Agents (+ Conductor)

Each agent has a distinct, non-overlapping responsibility.

| Agent | Purpose | Allowed tools (capability scope) | Subscribes (in) | Emits (out) |
|---|---|---|---|---|
| **Conductor** | Decompose goal → delegate → iterate | `workflow.*`, `hosted_plugin.info`, read-only analysis | `*.failed`, `*.needs_decision` | `conductor.plan`, `conductor.complete` |
| **AnalysisAgent** | Read-only measurement & diagnosis | `analysis.*`, `ozone.track.analyze`, `get_mastering_state`, spectrum/stereo/level reads | `audio.transport_changed` | `analysis.finding`, `analysis.clipping_detected`, `analysis.lufs_breach` |
| **OptimizationAgent** | Parameter optimization toward a target metric | `set_parameter*`, `set_more_phi_parameter*`, `mastering.plan_preview`, `mastering.render_batch` | `analysis.finding`, `quality.target_set` | `optimization.proposal`, `quality.measure_request` |
| **CreativeAgent** | Suggest artistic/alternative directions (human-in-the-loop) | `suggest_*`, `find_related_parameters`, `suggest_intermediate_snapshots`, `capture_snapshot` | `analysis.finding`, `optimization.proposal` | `creative.suggestion` |
| **RealtimeControlAgent** | Sub-100ms reactive corrections via existing queue | `set_morph_position`, `set_parameter` (capped set), `more_phi.bypass*` | `analysis.clipping_detected`, `analysis.lufs_breach` | `realtime.correction_applied` |
| **QualitySafetyAgent** | Verify proposed actions; enforce policy before apply | `get_mastering_state`, `audit_plugin_profile`, `restore_safe_plugin_snapshot`, `compare_analysis` | `optimization.proposal`, `creative.suggestion`, `realtime.*` | `quality.verdict`, `quality.target_set` |
| **MemoryAgent** | Persist outcomes; surface relevant memory for decisions | `automation.memory_*`, `get_usage_stats`, `learn_from_adjustment` | `conductor.complete`, `quality.verdict` | `memory.recall_ready` |

### Per-agent behavior

**Conductor (infrastructure role).** Turns a user goal ("master this for streaming, keep it warm") into a coordinated multi-agent plan. Builds a `WorkflowRun` via `WorkflowOrchestrator::createRun`, decomposes into specialist subtasks, submits them via `AgentRuntime::submitTask`, collects `AgentResult`s, may re-plan on `*.failed`. Honors a per-goal token budget (`TokenOptimizer`) and a wall-clock deadline. Emits `conductor.complete` with a final report; the workflow run is the auditable record. It is the only agent that submits subtasks *to other agents*.

**AnalysisAgent (read-only — the system's eyes).** Measures current state. Never mutates. Calls read-only `analysis.*` / `ozone.track.analyze` / `get_mastering_state`. Publishes structured findings (peak, LUFS-I, DR, spectral tilt, stereo width, clipping flags). `allowedTools()` is purely read-only → `DefaultToolInvoker` rejects any write tool. Runs a periodic meter-poll (configurable cadence, default 100ms) on a scheduler timer, *not* the audio thread.

**OptimizationAgent (drives parameters toward a metric).** Given a target metric (e.g. "LUFS-I = −14, no true-peak over −1.0"), proposes parameter changes. Uses `mastering.plan_preview` to draft, `mastering.render_batch` to evaluate N candidates offline, picks best by metric, returns as `proposedActions` (tool calls the Conductor re-dispatches through `MCPToolHandler` → permission-gated). Does not apply directly — QualitySafety must sign off for anything above autonomy threshold. The existing `OnnxNeuralMasteringRunner` / `AutoMasteringEngine` can be surfaced as a *tool* the OptimizationAgent calls (via a new `mastering.neural_plan` tool) — integrating the neural path as one option among several, not a special case.

**CreativeAgent (artistic alternatives, never auto-applied at low autonomy).** Suggests alternative directions ("try a warmer EQ curve", "breed two snapshots for a hybrid"). Calls `suggest_*` / `find_related_parameters` / `suggest_intermediate_snapshots`. May `capture_snapshot` of a suggestion for the user to A/B. `proposedActions` are tagged elevated risk → always require approval unless autonomy is `Autopilot` *and* QualitySafety approves. Carries a per-agent `requireApprovalRegardlessOfAutonomy` flag (default true) — Creative stays human-in-the-loop even at Autopilot.

**RealtimeControlAgent (reactive, sub-100ms intent, still queue-bound).** Reacts to fast-changing signals (clipping, LUFS breach) by enqueuing a corrective target through the existing safe channel. Computes a small corrective target (e.g. nudge output trim −2 dB, engage limit), writes via `enqueueParameterSet(source=Assistant)`. Per D2, "RealtimeCritical" priority means "jump the agent queue," *not* "touch the audio thread." Hard rate cap (`maxCorrectionsPerParamPerSecond`, default 4) and a per-run correction budget (default 16) prevent oscillation; QualitySafety can veto.

**QualitySafetyAgent (the gatekeeper).** Independently verifies any proposed mutation before it is applied (above autonomy threshold) and enforces `NeuralMasteringSafetyPolicy` + streaming targets. Re-measures predicted state (`compare_analysis`), checks against safety policy + targets, returns a `quality.verdict` (approve / approve-with-modifications / reject / needs-user-approval). Sets `quality.target_set` which OptimizationAgent reacts to. May invoke `restore_safe_plugin_snapshot` as a last resort (high-risk → always approval-gated). *Why separate from `PermissionKernel`:* `PermissionKernel` is *mechanical* risk classification + autonomy state (global, per-tool). QualitySafetyAgent is *semantic* verification against audio-specific targets and policy (per-proposal, content-aware). They compose — a proposal must pass both. This is defense in depth.

**MemoryAgent (institutional memory).** Consolidates *workflow-level* outcomes and surfaces relevant prior memory when a decision is being made. Reads `MemoryStore::search`/`intentContext` to inject prior outcomes into Conductor's planning; on `conductor.complete`, writes one *workflow-level* `ActionOutcome` (with intent correlation + user-feedback scoring) and triggers `updateOutcomeFeedback` when the user later accepts/rejects. **Granularity note (avoids double-recording):** the existing `dispatchWithAutomationTransaction` already auto-records *transaction-level* outcomes (one per parameter write, as evidence); MemoryAgent does *not* duplicate those — it writes only the higher-level workflow outcome that ties the transactions to the user intent and feedback score. This division is tested in `TestMemoryAgent` (assert: N transaction records from the ledger + exactly 1 workflow record from MemoryAgent per run). Closes the learning loop the existing `MemoryStore` was built for but no agent populates systematically today.

### Three properties this cast guarantees
1. **No agent-to-agent direct submission except via Conductor.** Prevents cycles; makes the dependency graph a tree rooted at the Conductor. Reactive hand-offs happen through the blackboard, not direct calls.
2. **Read/write separation is structural.** AnalysisAgent physically cannot write (capability scope). The only mutating agents are Optimization, Creative, RealtimeControl — and all three route proposed mutations through `MCPToolHandler` (permission-gated) with QualitySafety as semantic gatekeeper. Analysis + Memory are read-only; QualitySafety is verify-then-allow.
3. **Every agent is independently testable.** Each takes an `AgentContext` with mockable seams (`IToolInvoker`, `BlackboardBridge`, `IAgentLogger`, `AutomationRuntime` with test-dir override).

---

## 7. MCP Surface & Startup

### New MCP tools (additive; registered in `MCPToolHandler`)
No new transport — the existing TCP + stdio servers carry these.

| Tool | Purpose | Risk class |
|---|---|---|
| `agents.list` | List registered agents: roles, states, capability scopes | ReadOnly |
| `agents.run_goal` | Submit a natural-language goal to the Conductor; returns runId | LowWrite (real risk evaluated per-step by `PermissionKernel`) |
| `agents.run_task` | Submit a task directly to a named agent (bypass Conductor) | MediumWrite |
| `agents.run_status` | Poll a run/task: state, subtask progress, latest findings | ReadOnly |
| `agents.run_cancel` | Cooperative cancel (drain → stop) | LowWrite |
| `agents.blackboard.recent` | Read recent blackboard events (namespaced) | ReadOnly |
| `agents.set_autonomy` | Wrapper over `PermissionKernel::setAutonomyLevel` for the agent domain | HighImpact |

Risk classifications for `agents.*` must be added to `PermissionKernel::classifyTool` (the static map at `AutomationControlPlane.cpp:778`). This is a named integration task in the implementation plan.

### Startup sequence

```
MorePhiProcessor constructor (existing):
  · APVTS, morph/core/host subsystems, LockFreeQueue          [existing]
  · automationRuntime_ = AutomationRuntime()                   [existing]
  · aiAssistant_       = AIAssistant(*this)                    [existing]

startMCPServerIfNeeded() (existing, extended):
  · InstanceRegistry::registerInstance() → identity/port      [existing]
  · mcpServer_.startServer(port)                                [existing TCP server]
  · vst3IpcBridge_.start()                                      [existing stdio bridge]
+ agentRuntime_ = AgentRuntime(*this, identity_, automationRuntime_)   [NEW]
+ agentRuntime_.registerAgent(make<Conductor>())
+ agentRuntime_.registerAgent(make<AnalysisAgent>())
+ ... (6 specialists)
+ agentRuntime_.start()   // spawns PriorityScheduler workers, starts blackboard pump

External client (Claude Desktop / Python scaffold / UI):
  · TCP connect 127.0.0.1:<port>, initialize{bearer_token}     [existing]
  · tools/call "agents.run_goal" { intent, successCriteria }   [NEW tool]
  · poll "agents.run_status" { runId } until complete
```

The plugin **instantiates and owns** the runtime. Connecting to an *already-running* instance is handled by the existing `InstanceRegistry` multi-instance model — external clients target a specific instance's port. The runtime is per-instance, consistent with `AutomationRuntime`/`MCPServer` today.

### Walkthrough A — User goal, end-to-end (proactive path)
Goal: *"Master this track for streaming, keep the low end warm."*

```
1. User → UI "AI Goal" field (or MCP agents.run_goal)
   └─ AgentRuntime::submitGoal(intent, priority=High, origin="user")
       └─ Conductor.execute(task):
           · WorkflowOrchestrator::createRun(intent, context)   [reused]
           · decompose → 3 subtasks: Analysis, Memory-recall, then Optimization
           · AgentRuntime::submitTask(Analysis), submitTask(Memory)   [parallel]

2. AnalysisAgent.execute():
   └─ IToolInvoker.invoke("analysis.get_summary")        → read-only, cached
   └─ IToolInvoker.invoke("analysis.get_spectrum")
   └─ emit analysis.finding{ lufs=-9.2, tilt=+2dB, lowEnergy=high, peak=-0.3dBF }

3. MemoryAgent.execute():
   └─ runtime.memory().intentContext(session)            [reused]
   └─ emit memory.recall_ready{ "warm+jazz prior: −1 @150Hz worked, score 0.8" }

4. Conductor collects A+M → submits Optimization with target:
       { lufs=-14, tpMax=-1.0, lowShelfKeep=true }

5. OptimizationAgent.execute():
   └─ invoke("mastering.plan_preview", {...})             [drafts]
   └─ invoke("mastering.render_batch", n=4)               [offline eval]
   └─ pick best by metric → proposedActions=[set_parameters_batch{...}]
   └─ emit optimization.proposal{ diff, predictedLufs=-13.9 }

6. QualitySafetyAgent.onEvent(optimization.proposal):     [reactive, blackboard]
   └─ invoke("compare_analysis", before/after)
   └─ check NeuralMasteringSafetyPolicy + streaming targets
   └─ emit quality.verdict{ approve } | quality.target_set{}

7. Conductor sees verdict=approve:
   └─ re-dispatch proposedActions via IToolInvoker → MCPToolHandler::handle
       └─ dispatchWithAutomationTransaction:               [reused chokepoint]
           · PermissionKernel.evaluate("set_parameters_batch", risk=MediumWrite)
               · autonomy=Assist → requireApproval? No (MediumWrite + Assist = auto)
               · autonomy=Manual → approval_required event to UI
           · execute → enqueueParameterSet(source=Assistant)   [LOCK-FREE QUEUE]
           · ActionLedger.record(tx) · events.publish(tx.completed) · memory.recordOutcome

8. Audio thread (next processBlock): drains queue → ParameterBridge → hosted plugin
   └─ Agents never entered this thread. Zero alloc, zero lock from agent code.

9. Conductor: successCriteria met? → workflow.run.complete
   └─ MemoryAgent.onEvent(conductor.complete): recordOutcome(score)
```

### Walkthrough B — Reactive clip correction (event-driven path)

```
1. AnalysisAgent on a 100ms Timer (NOT audio thread):
   └─ reads atomic meter snapshot (peak held since last poll)
   └─ peak > -0.1 dBF sustained 3 polls → invoke("analysis.get_summary") confirms
   └─ emit analysis.clipping_detected{ peak=+0.2dBF, channel=R, since }

2. RealtimeControlAgent.onEvent(clipping_detected):       [priority=RealtimeCritical]
   └─ compute correction: output trim −1.5 dB
   └─ invoke("set_parameter", {output_gain, -1.5dB})      [capped tool set]
       └─ MCPToolHandler → PermissionKernel (LowWrite, Assist=auto)
       └─ enqueueParameterSet(source=Assistant)           [LOCK-FREE QUEUE]
   └─ emit realtime.correction_applied{ param, delta }

3. QualitySafetyAgent.onEvent(realtime.*):                 [semantic watchdog]
   └─ if correction would exceed per-run budget OR violate policy →
       emit quality.verdict{ reject, reason } → RealtimeControl backs off
   └─ else silent approve

4. Audio thread: drains the −1.5dB target within ~1-2 blocks.
   └─ Total agent-side latency: ~100ms (analysis cadence) + queue drain.
   └─ "Realtime" = analysis cadence + queue priority, NOT audio-thread execution.
```

Even the fastest reactive path is two queue-hops from the audio thread (agent → lock-free queue → audio drain). There is no third hop and no agent code on the audio thread.

---

## 8. Security Boundaries

1. **Inbound validation, reused.** Every MCP request already passes through `MCPServer` bearer-token auth + rate limiting (`TokenOptimizer::tryConsumeRequestSlot`). The new `agents.*` tools inherit this — no unauthenticated agent access.
2. **Capability scope (new, in `DefaultToolInvoker`).** An agent can only invoke tools in its declared `allowedTools()`. Defense in depth below `PermissionKernel`. `IToolInvoker` checks `allowedTools()` on the top-level tool name only; nested tool-calls are not a concept in `MCPToolHandler` (tools don't call tools), so there is no bypass vector (R7).
3. **Per-agent budget (new, via existing `TokenOptimizer`).** Each agent gets a token/rate slice; exceeding it raises a `budget_exceeded` event and the agent is paused (not failed). Agents that exceed budget do not starve the existing MCP path.
4. **Semantic gatekeeper (new).** QualitySafetyAgent independently verifies mutations against `NeuralMasteringSafetyPolicy` + targets before they apply above autonomy threshold. Composes with (does not replace) the mechanical `PermissionKernel`.
5. **Data sanitization, reused.** Agents consume tool *results* (already-shaped JSON from `MCPToolHandler`); they never receive raw external input directly into a tool call — all params flow through `PermissionKernel.evaluate` and the existing param-write sanitization in `ParameterBridge`.
6. **No new network surface.** The agent runtime is in-process and talks to the audio thread via a lock-free queue. The only external boundary remains the existing loopback-only MCP server.

---

## 9. Configuration, Logging, Observability

### Configuration — single file, reusing existing patterns

```
<userAppData>/More-Phi/agents/agent_runtime.json      ← user-overridable
config/agents/agent_runtime.default.json              ← shipped defaults
```

```jsonc
// config/agents/agent_runtime.default.json
{
  "schemaVersion": 1,
  "enabled": true,                          // global kill-switch; runtime no-ops if false
  "scheduler": {
    "workerThreads": 2,                     // message-thread domain only
    "maxQueueDepth": 256,
    "starvationGuardMs": 5000               // bump a starving lane after this
  },
  "blackboard": {
    "pumpIntervalMs": 50,
    "maxEventHistory": 256                  // matches IntegrationEventBus default
  },
  "realtime": {
    "analysisCadenceMs": 100,               // AnalysisAgent meter-poll interval
    "maxCorrectionsPerParamPerSecond": 4,   // anti-oscillation guard
    "correctionBudgetPerRun": 16            // hard cap before QualitySafety veto
  },
  "agents": {
    "conductor":    { "enabled": true, "tokenBudgetPerGoal": 8000, "deadlineMs": 30000 },
    "analysis":     { "enabled": true, "cacheTtlMs": 500 },
    "optimization": { "enabled": true, "renderBatchSize": 4 },
    "creative":     { "enabled": true, "maxSuggestions": 3 },
    "realtime":     { "enabled": true },
    "quality":      { "enabled": true },
    "memory":       { "enabled": true }
  },
  "logging": {
    "level": "info",                        // error|warn|info|debug|trace
    "file": "<userAppData>/More-Phi/agents/agent_runtime.log",
    "rotateBytes": 5242880
  }
}
```

Loaded by a small `AgentRuntimeConfig` struct (mirrors `DatasetConfig`'s load+validate pattern), validated against a schema at load, with the shipped default as fallback. No new dependency.

**Unified config story:** APVTS owns *audio/realtime* knobs (morph, physics, output) — these stay on the audio path. `agent_runtime.json` owns *orchestration* knobs. They do not overlap by design. `InstanceRegistry` identity/port stays runtime-generated as today.

### Logging — new structured logger, scoped to the agent layer

The repo currently has only ad-hoc `DBG(...)` / `juce::Logger::writeToLog` outside Dataset V3's `GenerationLogger`. The agent layer introduces a small structured logger because agents are where unstructured `DBG` sprawl becomes unreadable fast.

```cpp
// src/AI/Agents/Logging/IAgentLogger.h  (the seam from §3)
class IAgentLogger {
public:
    virtual void log(const juce::String& agentId, const juce::String& level,
                     const juce::String& message, const nlohmann::json& fields = {}) = 0;
};
// Concrete: StructuredAgentLogger — writes one JSON line per event to agent_runtime.log
// { "ts":"2026-06-21T14:03:11Z", "agent":"optimization-1", "level":"info",
//   "msg":"proposal emitted", "fields":{ "runId":"...", "predictedLufs":-13.9 } }
```

Deliberately scoped to the agent layer only. It does **not** retrofit logging across the whole codebase (unrelated refactoring, out of scope). A `NullAgentLogger` exists for tests.

### Observability — three surfaces, all additive
1. **`AgentRuntime::describeState()`** → JSON snapshot of all agents (role, state, current task, queue depth, tokens consumed, last error). Surfaced via `agents.list` MCP tool and a future UI panel.
2. **Blackboard as the audit channel.** Every `IntegrationEvent` has `source`/`type`/`payload`/`timestamp`; the existing `IntegrationEventBus::listRecent` already serves them. The new `agents.blackboard.recent` tool exposes them — every agent interaction is observable through existing event infra.
3. **Existing `ActionLedger` covers mutations.** Every parameter write becomes an `AutomationTransaction` with before/after/rollback, persisted to `action_ledger.json`. Combined with blackboard events: *goal → conductor plan → agent findings → proposal → quality verdict → audited transaction → outcome memory record* — full provenance.

**Not built:** no metrics dashboard, no Prometheus, no OpenTelemetry. The three surfaces above are the right weight for an audio plugin.

---

## 10. Test Plan

Catch2 v3, under `tests/Unit/`, integrated into the existing `MorePhiTests` target in `tests/CMakeLists.txt`. Mocking uses abstract seams (`IToolInvoker`, `BlackboardBridge`, `IAgentLogger`, `AutomationRuntime` with `setStoreDirectoryOverrideForTests`) and the `MockV2Interfaces.h` pattern.

| Layer | Test file | What it proves |
|---|---|---|
| **Core contracts** | `TestAgentRuntimeCore.cpp` | `AgentRegistry` register/lookup/lifecycle; `PriorityScheduler` ordering + starvation guard; `BlackboardBridge` pub/sub fan-out + isolation; `DefaultToolInvoker` enforces capability scope (agent blocked from disallowed tool) + per-agent budget |
| **Conductor** | `TestConductorAgent.cpp` | Goal → subtask decomposition; collects results; re-plans on `*.failed`; honors deadline/budget; only agent that submits to other agents |
| **Per-agent unit tests** | `TestAnalysisAgent.cpp` … `TestMemoryAgent.cpp` (6 files) | Each agent with a fake `IToolInvoker` returning canned tool results: Analysis is read-only & refuses writes; Optimization picks best candidate by metric; RealtimeControl respects rate cap + budget (anti-oscillation); QualitySafety approves/rejects per policy; Memory writes `ActionOutcome` + recalls by intent |
| **No-direct-submission rule** | `TestAgentIsolation.cpp` | A specialist that attempts `submitTask` to another specialist is rejected at the runtime; only Conductor's `followUps` are honored |
| **RT-safety invariant** | `TestAgentAudioThreadIsolation.cpp` | Instrumented: no agent thread id == audio thread id across a full goal run; no allocations on agent path reach the queue (queue entry is trivially-copyable `ParamCommand` as today) |
| **End-to-end (integration)** | `TestAgentE2E.cpp` | Full goal: submitGoal → conductor → analysis+memory → optimization → quality verdict → permission-gated apply → ledger record → outcome memory. Drives the real `AutomationRuntime` (test-dir override), a stub `MorePhiProcessor`, fake `IToolInvoker` |
| **MCP surface** | extend `TestMCPServerUnit.cpp` | `agents.run_goal` / `agents.run_status` / `agents.list` dispatch + risk classification in `PermissionKernel` |
| **Reactive path** | `TestRealtimeReactive.cpp` | Inject `analysis.clipping_detected` → RealtimeControl enqueues correction → QualitySafety veto path → rate-cap enforcement |

**Testing principles:** (a) each agent tested in isolation with mocked tools (fast, deterministic); (b) the chokepoint (`MCPToolHandler`) is *not* re-tested through agents — it already has coverage; agents are tested against a fake `IToolInvoker`, then one E2E test proves the real wiring; (c) the audio-thread-isolation invariant gets its own test because it is the most important safety property and must not regress.

---

## 11. Risks & Mitigations

| # | Risk | L / I | Mitigation |
|---|---|---|---|
| **R1** | **LLM transport broken** (HTTP 400 vs NVIDIA NIM, spec 005 GAP-CRITICAL). LLM-backed agents (Conductor, Creative, Memory-recall) can't call an LLM end-to-end until fixed. | High / High | (a) Design all agents so the LLM-call is one injectable seam, testable with a fake LLM; (b) call out the fix as a **prerequisite** in the implementation plan, not part of this design's scope; (c) Conductor/Optimization have deterministic fallbacks (reuse `AIAssistant::planLocalWorkflowPrompt`'s pattern) so the system degrades gracefully without an LLM. |
| **R2** | **Feedback loops on the blackboard.** Analysis → RealtimeControl → correction changes audio → Analysis re-finds issue → loop. | Med / Med | (a) Per-param rate cap (`maxCorrectionsPerParamPerSecond`); (b) per-run correction budget with QualitySafety veto; (c) event types carry a `generation` counter; QualitySafety suppresses above threshold. `TestRealtimeReactive` targets this. |
| **R3** | **PriorityScheduler starvation.** Background tasks never run under sustained High/RealtimeCritical load. | Med / Low | `starvationGuardMs` bumps the oldest Background task to Normal after timeout. Tested in `TestAgentRuntimeCore`. |
| **R4** | **Token/rate budget exhaustion under multi-agent load.** Six agents each consuming `TokenOptimizer` slots could starve the existing MCP path. | Med / Med | Per-agent budget slices (not a shared pool); agents that exceed budget emit `budget_exceeded` and pause, not fail. `TokenOptimizer` already supports rate scaling; we extend, not replace. |
| **R5** | **"Realtime" expectation mismatch.** A reader expects audio-thread-grade latency from "RealtimeControlAgent". | Med / Med | Name + doc the cadence ceiling (~100ms) explicitly in the spec, the agent's header, and the config. Non-goal 1 is the backstop. |
| **R6** | **Regression in existing `AIAssistant`/LLM path** during Peer→Supersede migration. | Low / Med | Phase 1 (Peer) touches nothing existing. Migration is a later, separately-tested phase behind `MORE_PHI_AGENT_CONDUCTOR_MIGRATION`. |
| **R7** | **Capability-scope bypass** via nested tool calls. | Low / High | `IToolInvoker` checks `allowedTools()` on the top-level tool name only (matches how `PermissionKernel` classifies today); nested tool calls aren't a concept in `MCPToolHandler` (tools don't call tools), so there is no bypass vector. Documented as a security note. |
| **R8** | **Deadlock** if Conductor blocks waiting for a subtask whose result needs the same worker. | Low / High | Scheduler workers never block on a *specific* task's result; Conductor collects results asynchronously via the blackboard / result store, not by blocking. Tested in `TestConductorAgent` under N=1 worker. |

---

## 12. Migration Path: Peer (Phase 1) → Supersede (Phase 2, later)

**Phase 1 — Peer (this design's deliverable).** `AgentRuntime` is additive. `AIAssistant` continues unchanged. An optional `AiAssistantAdapterAgent` (wrapping `AIAssistant::executeLocalWorkflowPrompt`) can register as a peer so existing behavior participates in the new world. Zero regression risk. Ships the full multi-agent pattern.

**Phase 2 — Supersede (future, separately specced).** Conductor absorbs `AIAssistant`'s planning logic; `AIAssistant` becomes a thin shim or is removed. Gated behind `MORE_PHI_AGENT_CONDUCTOR_MIGRATION`. The LLM transport (R1) must be fixed first. This is a documented target, not built now.

---

## 13. Open Questions (resolved by default)

1. **Default autonomy for agent-driven goals:** `Assist` (mutations above MediumWrite need explicit approval). Conservative by default; users opt up to `CoPilot`/`Autopilot`.
2. **Should CreativeAgent auto-apply at `Autopilot`?** No — Creative stays human-in-the-loop even at Autopilot. Coded as per-agent `requireApprovalRegardlessOfAutonomy` (default true).
3. **Analysis cadence (100ms):** fixed in config, not adaptive in v1. Adaptive cadence is a v2 enhancement, out of scope.
4. **Logger format:** JSON-lines to a file. No structured streaming/OTel.

---

## 14. File Layout

All new files under `src/AI/Agents/`:

```
src/AI/Agents/
  IAgent.h                         — contract: AgentRole/State/Task/Result/IAgent
  AgentContext.h                   — AgentContext, IToolInvoker, IAgentLogger seams
  AgentRuntimeConfig.h/.cpp        — load + validate agent_runtime.json
  AgentRuntime.h/.cpp              — registry + submitGoal/submitTask + lifecycle
  AgentRegistry.h/.cpp             — register/find/lifecycle (owned by AgentRuntime)
  Conductor/
    ConductorAgent.h/.cpp          — goal decomposition (generalizes AIAssistant loop)
  Adapters/
    AiAssistantAdapterAgent.h/.cpp — optional Phase-1 bridge wrapping AIAssistant (§12)
  Scheduler/
    PriorityScheduler.h/.cpp       — priority queue + worker pool (message-thread domain)
    TaskPriority.h
  Blackboard/
    BlackboardBridge.h/.cpp        — typed pub/sub over IntegrationEventBus
  Tooling/
    DefaultToolInvoker.h/.cpp      — wraps MCPToolHandler::handle + capability/budget/attribution
    AgentToolError.h
  Logging/
    IAgentLogger.h
    StructuredAgentLogger.h/.cpp   — JSON-lines file logger
    NullAgentLogger.h
  Agents/
    AnalysisAgent.h/.cpp
    OptimizationAgent.h/.cpp
    CreativeAgent.h/.cpp
    RealtimeControlAgent.h/.cpp
    QualitySafetyAgent.h/.cpp
    MemoryAgent.h/.cpp

config/agents/
  agent_runtime.default.json       — shipped defaults

tests/Unit/
  TestAgentRuntimeCore.cpp
  TestConductorAgent.cpp
  TestAnalysisAgent.cpp
  TestOptimizationAgent.cpp
  TestCreativeAgent.cpp
  TestRealtimeControlAgent.cpp
  TestQualitySafetyAgent.cpp
  TestMemoryAgent.cpp
  TestAgentIsolation.cpp
  TestAgentAudioThreadIsolation.cpp
  TestAgentE2E.cpp
  TestRealtimeReactive.cpp
```

**Touched (minimally), not new:**
- `src/Plugin/PluginProcessor.{h,cpp}` — own an `AgentRuntime` member; start/stop in `startMCPServerIfNeeded()`/destructor.
- `src/AI/MCPToolHandler.cpp` — add the 7 `agents.*` tool cases to the dispatch table + `agents.*` entries to `getToolList()`.
- `src/AI/AutomationControlPlane.cpp` — add `agents.*` risk classifications to `PermissionKernel::classifyTool`.
- `CMakeLists.txt` — add the new sources to `MORE_PHI_AI_SOURCES`.
- `tests/CMakeLists.txt` — add the new test files to `MorePhiTests`.

---

## 15. Requirement Traceability Summary

| Originating requirement | Section |
|---|---|
| VST3 plugin as audio interface + UI host + comms bridge | §2, §7 (existing `MorePhiProcessor`) |
| Low-latency, thread-safe comms audio↔orchestration | §5, §7 (existing `LockFreeQueue`) |
| Multiple specialized agents with distinct responsibilities | §6 |
| Agent communication protocols, state, decision logic | §3, §4 |
| Queue + prioritization by realtime demands | §3 (`PriorityScheduler`); §5 (cadence ceiling) |
| Concurrent requests without blocking | §3, R8 |
| MCP server as standardized protocol | §7 (existing TCP + stdio carry new tools) |
| Message schemas plugin↔orchestration | §3 (`AgentTask`/`AgentResult`/`IntegrationEvent`) |
| Resource mgmt, session handling, connection pooling | §7 (existing `InstanceRegistry`); §3 (per-agent budgets) |
| Retry logic + fault tolerance | §2 (existing `WorkflowOrchestrator`); R1 |
| Bidirectional comms | §7 walkthroughs A & B |
| Unified config | §9 |
| Logging + monitoring across components | §9 |
| Security boundaries | §8 |
| Plugin instantiates OR connects to running instance | §7 |
| Extensible for new agents without core changes | §3 (open registry) |
| Unit tests for critical paths | §10 |
| Industry-standard patterns, no anti-patterns | §5 (RT-safety), §4 (acyclicity), §6 (read/write separation) |
