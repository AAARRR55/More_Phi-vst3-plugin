# Multi-Agent Workflow Optimizations — Design

**Date:** 2026-06-29
**Status:** Approved (O1, O2, O3, O4)
**Branch:** `morph-audit-fixes`

## Problem

The multi-agent orchestration layer has four concrete inefficiencies, identified by
reading the actual code (not hypothetical):

1. **O1 — Agents can't reach the neural mastering path.** `OptimizationAgent::allowedTools()`
   lists `mastering.plan_preview` + `mastering.render_batch` (heuristic) but NOT
   `mastering.neural_apply`. `AnalysisAgent` lacks `sonicmaster_decision`. So when the
   Conductor decomposes a mastering goal, the specialists draft **heuristic** plans and
   never use the embedded ONNX model — the exact blind spot we just fixed for the LLM
   assistant path. This is the highest-impact item.

2. **O2 — `AnalysisAgent` makes 3 serial blocking tool calls** (`analysis.get_summary`,
   `analysis.get_spectrum`, `analysis.get_stereo_field`) with no data dependency between
   them. Each is a full `MCPToolHandler` dispatch (parse + lock + resolve + serialize).
   Running them concurrently cuts AnalysisAgent latency ~3x.

3. **O3 — `escalateStarving()` runs on every worker wake**, doing a FIFO scan of Normal
   and High queues even when both are empty. Called on every single task completion and
   every spurious wake.

4. **O4 — Blackboard pump polls every 50ms unconditionally**, even when idle. Events
   dispatched via the blackboard arrive up to 50ms late. A `condition_variable` notify
   on `publish()` lets the pump wake immediately on publish and sleep longer when idle.

## Design

### O1 — Neural path for agents

**`OptimizationAgent`:**
- Add `mastering.neural_apply` and `sonicmaster_decision` to `allowedTools()`.
- In `execute()`: if the task payload indicates a mastering goal (intent or payload
  `target_lufs`/`master`), prefer the neural path: call `mastering.neural_apply` first
  (one-shot analyze+apply). Fall back to the existing heuristic batch path only if the
  neural call returns `available=false` or `applied=false`.
- Preserve the existing `proposedActions` contract — the neural apply writes directly,
  so when it succeeds, `proposedActions` stays empty (nothing for the Conductor to
  re-dispatch); `findings` reports the apply breakdown.

**`AnalysisAgent`:**
- Add `sonicmaster_decision` to `allowedTools()` so the analysis agent can pull a
  neural decision preview (dry-run) alongside the live measurements. Called only when
  the intent mentions mastering; not on every analysis run (avoids redundant inference).

**Capability gate:** the `DefaultToolInvoker`'s `CapabilityFn` (provided by
`MorePhiProcessor`) must also allow these tools for the Optimization/Analysis roles. The
existing test `FakeInvoker` capability lists must be extended to include the new tools.

### O2 — Parallel analysis reads

**`AnalysisAgent::execute`:** replace the 3 serial `ctx_->tools->invoke(...)` calls with
3 `std::async(std::launch::async, ...)` futures, then collect results. The 3 reads are
independent (no shared mutable state — each returns a fresh JSON). Collect via
`future.get()` with a per-call timeout guard so a hung tool call can't block the worker
indefinitely (consistent with the `masteringNeuralApply` 5s budget pattern).

Threading: `std::async(std::launch::async)` spawns transient threads. This is acceptable
because `execute()` runs on a scheduler worker (message-thread domain, NOT audio thread),
and AnalysisAgent is invoked infrequently (goal-driven, not per-block). The transient
threads are bounded to 3 (one per read).

### O3 — Skip escalateStarving when empty

**`PriorityScheduler::workerLoop`:** guard the `escalateStarving()` call with a cheap
check: only run it when `queues_[Normal]` or `queues_[High]` is non-empty. The check
itself is O(1) (two `.empty()` calls) and is performed under the already-held mutex, so
no additional synchronization. `bumpStarvingBackground()` already has this guard
internally (it returns early if Background is empty) — `escalateStarving()` should match.

### O4 — Event-driven blackboard pump

**`AgentRuntime`:**
- Add a `std::condition_variable blackboardCv_` and `std::mutex blackboardCvMutex_`.
- The pump thread waits on `blackboardCv_.wait_for(lock, 50ms)` instead of
  `Thread::sleep(50)`. The 50ms cap is retained as a safety fallback so a missed
  notify (e.g. publish during teardown) can't stall the pump indefinitely, and the
  `checkPendingDecompositions()` Conductor poll still runs at least every 50ms.
- `BlackboardBridge::publish()` notifies the cv via a callback hook. To avoid coupling
  BlackboardBridge to AgentRuntime, add a `std::function<void()> onPublish_` hook on
  BlackboardBridge that AgentRuntime sets in `start()`. `publish()` calls it after
  `bus_.publish()`.

**Lifecycle:** the hook is cleared in `stop()` before joining the pump thread, and the
pump's `wait_for` is woken by setting `blackboardPumpRunning_=false` + `cv.notify_all()`
(so the pump exits promptly on stop). The hook is only ever called from `publish()`,
which runs on agent worker threads / MCP threads — calling a cv's `notify_one` from
multiple threads is safe.

## Non-Goals

- No changes to the scheduler's lock-free urgents pool (C-3) — it's already optimal.
- No changes to the 2-worker pool size — that's a deployment-tuning concern.
- No changes to the deterministic fallback LLM — it's the offline path.
- No restructuring of the IAgent interface.

## Testing

- **O1:** extend `TestAgentE2E` capability list + add a focused test asserting
  OptimizationAgent's neural-first path is taken when the tool is available, and the
  heuristic fallback when it isn't. Extend `FakeInvoker` to return the
  `mastering.neural_apply` success shape.
- **O2:** existing AnalysisAgent tests still pass (results unchanged, only latency
  improves). Add a timing-insensitive assertion that all 3 findings are populated.
- **O3:** existing scheduler tests (starvation bumps) still pass — escalation still
  fires when queues are non-empty.
- **O4:** existing pump tests still pass; add an assertion that a publish wakes the
  pump in <10ms (vs the old ~50ms floor).

## Risk

- **O1 capability widening:** adding tools to `allowedTools()` is fail-closed by the
  `DefaultToolInvoker` — a tool not in the agent's scope is denied. The widening only
  takes effect for roles we explicitly list. Low risk.
- **O2 std::async:** transient threads. Bounded to 3, message-thread domain only. The
  per-call timeout prevents indefinite blocking. Low risk.
- **O3:** behavior-preserving optimization; escalation still fires identically when
  queues are non-empty. Minimal risk.
- **O4 cv hook:** the `onPublish_` hook adds one indirection on the publish hot path.
  `notify_one` is cheap. The 50ms wait_for fallback preserves the existing worst-case
  latency for Conductor decomposition polling. Low risk.
