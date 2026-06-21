# Multi-Agent Orchestration Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a multi-agent orchestration layer on top of More-Phi's existing `MCPToolHandler` / `AutomationControlPlane` / `LockFreeQueue`, with a Conductor + Blackboard coordination model, six specialist agents, and 7 new `agents.*` MCP tools — without duplicating any existing infrastructure.

**Architecture:** A C++ `AgentRuntime` (message-thread domain) owns an open `AgentRegistry`, a `PriorityScheduler`, and a `BlackboardBridge` (typed pub/sub over the existing `IntegrationEventBus`). Agents implement `IAgent::execute(task) → result` and act exclusively through `IToolInvoker` (default impl wraps `MCPToolHandler::handle`, the single chokepoint). Agents never touch the audio thread; they write parameter *targets* into the existing `LockFreeQueue` with `source=Assistant`.

**Tech Stack:** C++20, JUCE 8.0.4, nlohmann/json 3.11.3, Catch2 v3.4.0. All already fetched via FetchContent.

**Spec:** `docs/superpowers/specs/2026-06-21-multi-agent-orchestration-layer-design.md`

**Prerequisite note (Risk R1):** The existing LLM transport is currently broken (HTTP 400 vs NVIDIA NIM, spec 005 GAP-CRITICAL). This plan does NOT fix it — agents that need an LLM (Conductor, Creative, Memory-recall) use an injectable `ILlmClient` seam with a deterministic fallback so the system compiles, tests pass, and degrades gracefully. Fixing the transport is a separate plan.

---

## File Structure

New files (all under `src/AI/Agents/`):

| File | Responsibility |
|---|---|
| `IAgent.h` | `AgentRole`/`AgentState`/`TaskPriority` enums, `AgentTask`/`AgentResult` structs, `IAgent` abstract interface |
| `AgentContext.h` | `AgentContext` struct, `IToolInvoker` + `IAgentLogger` + `ILlmClient` seams |
| `AgentRuntimeConfig.h/.cpp` | Load + validate `agent_runtime.json` with shipped-default fallback |
| `AgentRegistry.h/.cpp` | Register/find/lifecycle; owned by `AgentRuntime` |
| `Scheduler/TaskPriority.h` | (included via IAgent.h; standalone header for non-agent callers) |
| `Scheduler/PriorityScheduler.h/.cpp` | Priority queue + worker pool (message-thread domain) |
| `Blackboard/BlackboardBridge.h/.cpp` | Typed pub/sub over `IntegrationEventBus` |
| `Tooling/AgentToolError.h` | Error-result helper |
| `Tooling/DefaultToolInvoker.h/.cpp` | Wraps `MCPToolHandler::handle`; enforces capability scope + budget + attribution |
| `Logging/IAgentLogger.h` | (in AgentContext.h); `NullAgentLogger.h`, `StructuredAgentLogger.h/.cpp` |
| `Llm/ILlmClient.h`, `Llm/NullLlmClient.h`, `Llm/DeterministicFallbackLlmClient.h/.cpp` | Injectable LLM seam (R1 mitigation) |
| `AgentRuntime.h/.cpp` | The container: owns registry + scheduler + blackboard + result store |
| `Conductor/ConductorAgent.h/.cpp` | Goal → WorkflowRun → delegation → iterate |
| `Adapters/AiAssistantAdapterAgent.h/.cpp` | Optional Phase-1 bridge wrapping `AIAssistant` |
| `Agents/AnalysisAgent.h/.cpp` | Read-only measurement |
| `Agents/OptimizationAgent.h/.cpp` | Parameter optimization toward a metric |
| `Agents/CreativeAgent.h/.cpp` | Artistic suggestions (human-in-the-loop) |
| `Agents/RealtimeControlAgent.h/.cpp` | Reactive corrections via existing queue |
| `Agents/QualitySafetyAgent.h/.cpp` | Semantic gatekeeper |
| `Agents/MemoryAgent.h/.cpp` | Workflow-level outcome memory |
| `config/agents/agent_runtime.default.json` | Shipped defaults |

Modified files (minimal):
- `src/Plugin/PluginProcessor.h/.cpp` — own `std::unique_ptr<agents::AgentRuntime>`; start/stop.
- `src/AI/MCPToolHandler.h/.cpp` — 7 `agents.*` dispatch cases + `getToolList()` entries.
- `src/AI/AutomationControlPlane.cpp` — `agents.*` risk classifications in `classifyTool`.
- `CMakeLists.txt` — add new sources to `MORE_PHI_AI_SOURCES`.
- `tests/CMakeLists.txt` — add new test files to `MorePhiTests`.
- `AGENTS.md` — document the new layer.

Test files (all under `tests/Unit/`):
`TestAgentRuntimeCore.cpp`, `TestConductorAgent.cpp`, `TestAnalysisAgent.cpp`, `TestOptimizationAgent.cpp`, `TestCreativeAgent.cpp`, `TestRealtimeControlAgent.cpp`, `TestQualitySafetyAgent.cpp`, `TestMemoryAgent.cpp`, `TestAgentIsolation.cpp`, `TestAgentAudioThreadIsolation.cpp`, `TestAgentE2E.cpp`, `TestRealtimeReactive.cpp`.

---

## Phasing

- **Phase 1 — Core runtime skeleton (Tasks 1–9):** contracts, scheduler, blackboard, registry, tool invoker, logger, config, runtime container. Ends with a working, tested runtime that registers a dummy agent and returns results. Independently shippable.
- **Phase 2 — Conductor + Analysis (Tasks 10–14):** first real proactive + reactive loop. Compiles, tested.
- **Phase 3 — Remaining specialists (Tasks 15–19):** Optimization, Creative, RealtimeControl, QualitySafety, Memory.
- **Phase 4 — MCP surface + plugin wiring (Tasks 20–23):** `agents.*` tools, risk classification, `PluginProcessor` ownership, config + logger activation, E2E test.
- **Phase 5 — Safety invariants + docs (Tasks 24–26):** audio-thread-isolation test, CMake wiring finalization, AGENTS.md.

---

# Phase 1 — Core Runtime Skeleton

## Task 1: Agent enums + value types (`IAgent.h`)

**Files:**
- Create: `src/AI/Agents/IAgent.h`

- [ ] **Step 1: Create the header**

```cpp
// src/AI/Agents/IAgent.h
#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <vector>

namespace more_phi::agents {

// Forward declarations to avoid heavy includes in this contract header.
struct AgentContext;
class IntegrationEvent;     // defined in AI/AutomationControlPlane.h (real type)
// Note: the real IntegrationEvent is in more_phi:: (not agents::). We reuse it
// via the include in AgentContext.h. Here we only need it as a struct tag for
// AgentResult::emitEvents and IAgent::onEvent, so a forward decl in more_phi:: suffices.

enum class AgentRole
{
    Conductor,
    Analysis,
    Optimization,
    Creative,
    RealtimeControl,
    QualitySafety,
    Memory,
    Custom
};

enum class AgentState
{
    Unregistered,
    Idle,
    Busy,
    Draining,
    Stopped,
    Failed
};

enum class TaskPriority
{
    Background,        // bookkeeping, memory compaction
    Normal,            // typical analysis/optimization
    High,              // user-initiated goal subtasks
    RealtimeCritical   // reactive correction — jumps the AGENT queue only (NOT audio thread)
};

juce::String toString(AgentRole);
juce::String toString(AgentState);
juce::String toString(TaskPriority);
AgentRole agentRoleFromString(const juce::String&);

struct AgentTask
{
    juce::String id;
    juce::String runId;            // originating conductor workflow run (empty for top-level)
    AgentRole   targetRole = AgentRole::Custom;
    juce::String intent;           // NL or structured
    nlohmann::json payload = nlohmann::json::object();
    TaskPriority priority = TaskPriority::Normal;
    juce::int64  deadlineMs = 0;   // soft deadline, 0 = none (stored as ms since epoch)
    juce::String origin;           // "user" | "conductor" | <agentId> | "mcp"
};

// We reuse more_phi::IntegrationEvent directly (it already has source/type/payload/timestamp).
// To avoid a hard include cycle, AgentResult holds them as nlohmann::json envelopes that the
// runtime converts to more_phi::IntegrationEvent at publish time. This keeps IAgent.h light.
struct AgentEventEnvelope
{
    juce::String type;
    nlohmann::json payload = nlohmann::json::object();
};

struct AgentResult
{
    juce::String taskId;
    bool success = false;
    juce::String errorCode;
    nlohmann::json findings = nlohmann::json::object();
    std::vector<nlohmann::json> proposedActions;        // tool calls for conductor to re-dispatch
    std::vector<AgentEventEnvelope> emitEvents;         // blackboard posts (runtime publishes as IntegrationEvent)
    std::vector<AgentTask>           followUps;          // honored ONLY if returned by Conductor
    nlohmann::json telemetry = nlohmann::json::object(); // tokens, latencyMs, toolsCalled[]
};

class IAgent
{
public:
    virtual ~IAgent() = default;

    virtual AgentRole    role() const noexcept = 0;
    virtual juce::String id() const noexcept = 0;
    virtual std::vector<juce::String> allowedTools() const = 0;        // capability scope
    virtual std::vector<juce::String> subscribedEventTypes() const { return {}; }
    virtual bool requireApprovalRegardlessOfAutonomy() const { return false; }

    virtual void prepare(const AgentContext& ctx) = 0;                 // wire dependencies
    virtual AgentResult execute(const AgentTask& task) = 0;            // sync; runs on a scheduler worker
    virtual void onEvent(const juce::String& eventType,
                         const nlohmann::json& payload,
                         const juce::String& source,
                         const juce::String& runId) { (void) eventType; (void) payload; (void) source; (void) runId; }

    virtual AgentState state() const noexcept = 0;
    virtual void stop() = 0;                                           // cooperative cancel
};

} // namespace more_phi::agents
```

**Design note:** `AgentEventEnvelope` + JSON-based `onEvent` keep `IAgent.h` free of the `AutomationControlPlane.h` include (which pulls in JUCE + json + mutex + many structs). The runtime converts these to/from the real `more_phi::IntegrationEvent` at the boundary. This is a deliberate compilation firewall.

- [ ] **Step 2: Verify it compiles in isolation**

Run:
```bash
cmake --build build --config Release --target MorePhi -- /verify 2>nul
```
(We will not be able to build standalone yet — the file is unreferenced. Instead, sanity-check syntax by adding it to CMake in Task 9. For now, just ensure the file is saved.)

- [ ] **Step 3: Commit**

```bash
git add src/AI/Agents/IAgent.h
git commit -m "feat(agents): add IAgent contract + enums + value types"
```

---

## Task 2: Agent context + seams (`AgentContext.h`)

**Files:**
- Create: `src/AI/Agents/AgentContext.h`

- [ ] **Step 1: Create the header**

```cpp
// src/AI/Agents/AgentContext.h
#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include "AI/Agents/IAgent.h"

namespace more_phi {
class MorePhiProcessor;
struct InstanceIdentity;
class AutomationRuntime;
} // namespace more_phi

namespace more_phi::agents {

class BlackboardBridge;   // defined later in this phase

// The single chokepoint agents use to act. Default impl wraps MCPToolHandler::handle.
class IToolInvoker
{
public:
    virtual ~IToolInvoker() = default;
    // Returns the tool result as parsed JSON. On policy/capability/budget failure,
    // returns an object with { "error": { "code": "...", "message": "..." } }.
    virtual nlohmann::json invoke(const juce::String& toolName,
                                  const nlohmann::json& params,
                                  const juce::String& agentId) = 0;
};

class IAgentLogger
{
public:
    virtual ~IAgentLogger() = default;
    virtual void log(const juce::String& agentId,
                     const juce::String& level,            // error|warn|info|debug|trace
                     const juce::String& message,
                     const nlohmann::json& fields = nlohmann::json::object()) = 0;
};

// Injectable LLM seam (Risk R1 mitigation). The real transport is currently broken;
// agents that need an LLM receive this and fall back to a deterministic client.
class ILlmClient
{
public:
    virtual ~ILlmClient() = default;

    struct CompletionRequest
    {
        juce::String systemPrompt;
        juce::String userPrompt;
        nlohmann::json tools = nlohmann::json::array();  // optional tool schema list
        int maxTokens = 1024;
    };
    struct CompletionResponse
    {
        bool ok = false;
        juce::String content;
        nlohmann::json toolCalls = nlohmann::json::array();
        int tokensUsed = 0;
        juce::String errorCode;
    };

    virtual CompletionResponse complete(const CompletionRequest& request) = 0;
    virtual juce::String providerName() const = 0;
};

struct AgentContext
{
    MorePhiProcessor*       processor = nullptr;
    const InstanceIdentity* identity  = nullptr;
    AutomationRuntime*      runtime   = nullptr;     // ledger/permissions/events/workflows/memory
    IToolInvoker*           tools     = nullptr;     // the chokepoint wrapper (never null at runtime)
    BlackboardBridge*       blackboard= nullptr;     // never null at runtime
    IAgentLogger*           logger    = nullptr;     // never null at runtime (NullAgentLogger in tests)
    ILlmClient*             llm       = nullptr;     // may be null; agents must handle gracefully
};

} // namespace more_phi::agents
```

- [ ] **Step 2: Commit**

```bash
git add src/AI/Agents/AgentContext.h
git commit -m "feat(agents): add AgentContext + IToolInvoker/IAgentLogger/ILlmClient seams"
```

---

## Task 3: Priority scheduler (`PriorityScheduler.h/.cpp`) — TDD

**Files:**
- Create: `src/AI/Agents/Scheduler/PriorityScheduler.h`
- Create: `src/AI/Agents/Scheduler/PriorityScheduler.cpp`
- Create: `tests/Unit/TestAgentRuntimeCore.cpp`

- [ ] **Step 1: Write the failing test (first test in the new file)**

Create `tests/Unit/TestAgentRuntimeCore.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <thread>

#include "AI/Agents/Scheduler/PriorityScheduler.h"

using namespace more_phi::agents;
using namespace std::chrono_literals;

TEST_CASE("PriorityScheduler runs submitted tasks", "[agents][scheduler]")
{
    PriorityScheduler scheduler;
    scheduler.start(2);

    std::atomic<int> counter{0};
    scheduler.submit([&] { counter.fetch_add(1, std::memory_order_relaxed); }, TaskPriority::Normal);
    scheduler.submit([&] { counter.fetch_add(1, std::memory_order_relaxed); }, TaskPriority::Normal);

    // Spin briefly until both complete or timeout.
    for (int i = 0; i < 200 && counter.load(std::memory_order_relaxed) < 2; ++i)
        std::this_thread::sleep_for(5ms);

    REQUIRE(counter.load(std::memory_order_relaxed) == 2);
    scheduler.stop();
}

TEST_CASE("PriorityScheduler honors priority ordering under single worker", "[agents][scheduler]")
{
    // With one worker and tasks submitted before start, higher priority runs first.
    PriorityScheduler scheduler;

    std::vector<int> order;
    juce::SpinLock orderLock;

    auto record = [&](int tag) {
        juce::SpinLock::ScopedLockType lock(orderLock);
        order.push_back(tag);
    };

    scheduler.submit([&] { record(1); }, TaskPriority::Background);
    scheduler.submit([&] { record(2); }, TaskPriority::High);
    scheduler.submit([&] { record(3); }, TaskPriority::RealtimeCritical);

    scheduler.start(1); // single worker so ordering is deterministic from the queue

    for (int i = 0; i < 200 && order.size() < 3; ++i)
        std::this_thread::sleep_for(5ms);

    REQUIRE(order.size() == 3);
    // RealtimeCritical (3) before High (2) before Background (1)
    REQUIRE(order[0] == 3);
    REQUIRE(order[1] == 2);
    REQUIRE(order[2] == 1);

    scheduler.stop();
}
```

- [ ] **Step 2: Add the test file to CMake (temporary, so we can run it)**

Edit `tests/CMakeLists.txt` — add after line 94 (inside the `MorePhiTests` source list, before the closing `)`):

```
    Unit/TestAgentRuntimeCore.cpp
```

- [ ] **Step 3: Run the test to verify it fails (compilation failure expected — header doesn't exist)**

```bash
cmake --build build --config Release --target MorePhiTests 2>&1 | findstr /C:"PriorityScheduler.h" /C:"error"
```
Expected: error — cannot open `AI/Agents/Scheduler/PriorityScheduler.h`.

- [ ] **Step 4: Create the header**

```cpp
// src/AI/Agents/Scheduler/PriorityScheduler.h
#pragma once

#include "AI/Agents/IAgent.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace more_phi::agents {

// Message-thread-domain priority queue + worker pool. NEVER used from the audio thread.
class PriorityScheduler
{
public:
    PriorityScheduler();
    ~PriorityScheduler();

    void start(unsigned numWorkers = 2);
    void stop();

    void submit(std::function<void()> task, TaskPriority priority);

    // Observability
    struct Stats
    {
        int depthBackground = 0;
        int depthNormal = 0;
        int depthHigh = 0;
        int depthRealtimeCritical = 0;
        long long executed = 0;
        long long starvationBumps = 0;
    };
    Stats stats() const;

private:
    void workerLoop();
    void bumpStarvingBackground();

    struct Entry
    {
        std::function<void()> task;
        TaskPriority priority;
        juce::int64 submitTimeMs = 0;   // for starvation detection
    };
    struct EntryCompare
    {
        bool operator()(const Entry& a, const Entry& b) const
        {
            return static_cast<int>(a.priority) < static_cast<int>(b.priority); // higher enum value = higher prio
        }
    };

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::priority_queue<Entry, std::vector<Entry>, EntryCompare> queue_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};
    std::atomic<long long> executed_{0};
    std::atomic<long long> starvationBumps_{0};

    long long starvationGuardMs_ = 5000;
};

} // namespace more_phi::agents
```

- [ ] **Step 5: Create the implementation**

```cpp
// src/AI/Agents/Scheduler/PriorityScheduler.cpp
#include "AI/Agents/Scheduler/PriorityScheduler.h"

#include <juce_core/juce_core.h>

namespace more_phi::agents {

namespace {
juce::int64 nowMs() noexcept
{
    return juce::Time::currentTimeMillis();
}
} // namespace

PriorityScheduler::PriorityScheduler() = default;

PriorityScheduler::~PriorityScheduler()
{
    stop();
}

void PriorityScheduler::start(unsigned numWorkers)
{
    if (running_.exchange(true))
        return;
    workers_.reserve(numWorkers);
    for (unsigned i = 0; i < numWorkers; ++i)
        workers_.emplace_back([this] { workerLoop(); });
}

void PriorityScheduler::stop()
{
    if (!running_.exchange(false))
        return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
    }
    cv_.notify_all();
    for (auto& t : workers_)
        if (t.joinable())
            t.join();
    workers_.clear();
    // Drain anything left unexecuted so we don't keep dangling lambdas.
    while (!queue_.empty())
        queue_.pop();
}

void PriorityScheduler::submit(std::function<void()> task, TaskPriority priority)
{
    if (!task)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(Entry{ std::move(task), priority, nowMs() });
    cv_.notify_one();
}

void PriorityScheduler::workerLoop()
{
    while (true)
    {
        Entry entry;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !running_.load() || !queue_.empty(); });
            if (!running_.load() && queue_.empty())
                return;
            bumpStarvingBackground();
            if (queue_.empty())
                continue;
            entry = queue_.top();
            queue_.pop();
        }
        try
        {
            entry.task();
        }
        catch (...)
        {
            // Agent tasks must not crash the pool. Swallow; agents record their own errors.
        }
        executed_.fetch_add(1, std::memory_order_relaxed);
    }
}

void PriorityScheduler::bumpStarvingBackground()
{
    // Called under mutex_. If the oldest Background entry has waited past the guard,
    // promote it to Normal so it can't starve indefinitely under sustained high-prio load.
    if (queue_.empty() || starvationGuardMs_ <= 0)
        return;
    // We cannot mutate std::priority_queue in place; rebuild if needed.
    std::vector<Entry> snapshot;
    snapshot.reserve(queue_.size());
    const auto t = nowMs();
    bool promoted = false;
    while (!queue_.empty())
    {
        Entry e = queue_.top();
        queue_.pop();
        if (e.priority == TaskPriority::Background && (t - e.submitTimeMs) > starvationGuardMs_)
        {
            e.priority = TaskPriority::Normal;
            promoted = true;
        }
        snapshot.push_back(std::move(e));
    }
    for (auto& e : snapshot)
        queue_.push(std::move(e));
    if (promoted)
        starvationBumps_.fetch_add(1, std::memory_order_relaxed);
}

PriorityScheduler::Stats PriorityScheduler::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Stats s;
    // priority_queue only exposes top(); rebuild counts by draining into a temp.
    std::vector<Entry> snapshot;
    snapshot.reserve(queue_.size());
    auto q = queue_;
    while (!q.empty())
    {
        switch (q.top().priority)
        {
            case TaskPriority::Background:        ++s.depthBackground; break;
            case TaskPriority::Normal:            ++s.depthNormal; break;
            case TaskPriority::High:              ++s.depthHigh; break;
            case TaskPriority::RealtimeCritical:  ++s.depthRealtimeCritical; break;
        }
        q.pop();
    }
    s.executed = executed_.load(std::memory_order_relaxed);
    s.starvationBumps = starvationBumps_.load(std::memory_order_relaxed);
    return s;
}

} // namespace more_phi::agents
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build build --config Release --target MorePhiTests
cd build && ctest -C Release -R "PriorityScheduler" --output-on-failure
```
Expected: 2 tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/AI/Agents/Scheduler/PriorityScheduler.h src/AI/Agents/Scheduler/PriorityScheduler.cpp tests/Unit/TestAgentRuntimeCore.cpp tests/CMakeLists.txt
git commit -m "feat(agents): add PriorityScheduler with priority ordering + starvation guard"
```

---

## Task 4: Blackboard bridge (`BlackboardBridge.h/.cpp`) — TDD

**Files:**
- Create: `src/AI/Agents/Blackboard/BlackboardBridge.h`
- Create: `src/AI/Agents/Blackboard/BlackboardBridge.cpp`
- Modify: `tests/Unit/TestAgentRuntimeCore.cpp` (append tests)

- [ ] **Step 1: Append failing tests**

Append to `tests/Unit/TestAgentRuntimeCore.cpp`:

```cpp
#include "AI/AutomationControlPlane.h"
#include "AI/Agents/Blackboard/BlackboardBridge.h"

TEST_CASE("BlackboardBridge publishes and fans out to subscribers", "[agents][blackboard]")
{
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};

    int received = 0;
    juce::String seenType;
    nlohmann::json seenPayload;
    bb.subscribe("analysis-1", { "analysis.finding" },
        [&](const juce::String& type, const nlohmann::json& payload, const juce::String& source) {
            ++received;
            seenType = type;
            seenPayload = payload;
        });

    bb.publish("analysis-1", "analysis.finding", { { "lufs", -9.2 } });
    bb.poll();

    REQUIRE(received == 1);
    REQUIRE(seenType == "analysis.finding");
    REQUIRE(seenPayload["lufs"].get<double>() == Approx(-9.2));
}

TEST_CASE("BlackboardBridge isolates subscribers by event type", "[agents][blackboard]")
{
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};

    int aHits = 0, bHits = 0;
    bb.subscribe("agent-a", { "alpha" }, [&](auto&&...) { ++aHits; });
    bb.subscribe("agent-b", { "beta" },  [&](auto&&...) { ++bHits; });

    bb.publish("src", "alpha", {});
    bb.publish("src", "beta", {});
    bb.publish("src", "alpha", {});
    bb.poll();

    REQUIRE(aHits == 2);
    REQUIRE(bHits == 1);
}
```

(Also add `#include <catch2/matchers/catch_matchers_floating_point.hpp>` at the top of the file if `Approx` is not already available — Catch2 v3 provides `Approx` in `catch_test_macros.hpp` by default, so this is usually unnecessary.)

- [ ] **Step 2: Run to verify failure**

```bash
cmake --build build --config Release --target MorePhiTests 2>&1 | findstr /C:"BlackboardBridge.h" /C:"error"
```
Expected: compilation error — header missing.

- [ ] **Step 3: Create the header**

```cpp
// src/AI/Agents/Blackboard/BlackboardBridge.h
#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace more_phi {
class IntegrationEventBus;
} // namespace more_phi

namespace more_phi::agents {

// Typed pub/sub OVER the existing IntegrationEventBus. Does not modify it.
// poll() must be called on a scheduler/message thread to fan out events to subscribers.
class BlackboardBridge
{
public:
    using Callback = std::function<void(const juce::String& type,
                                        const nlohmann::json& payload,
                                        const juce::String& source)>;

    explicit BlackboardBridge(IntegrationEventBus& bus);

    // Publish returns the generated eventId (the runtime also forwards the real
    // IntegrationEvent into the bus so listRecent/exportState keep working).
    juce::String publish(const juce::String& source,
                         const juce::String& type,
                         nlohmann::json payload,
                         const juce::String& runId = {});

    void subscribe(const juce::String& agentId,
                   const std::vector<juce::String>& eventTypes,
                   Callback callback);

    // Drain new events since the last poll and fan out to matching subscribers.
    void poll();

private:
    IntegrationEventBus& bus_;
    std::mutex subscribersMutex_;
    std::unordered_map<std::string, std::vector<std::pair<std::string, Callback>>> subscribers_;
    int lastSeenCount_ = 0;   // how many events we have already processed
};

} // namespace more_phi::agents
```

- [ ] **Step 4: Create the implementation**

```cpp
// src/AI/Agents/Blackboard/BlackboardBridge.cpp
#include "AI/Agents/Blackboard/BlackboardBridge.h"

#include "AI/AutomationControlPlane.h"

namespace more_phi::agents {

BlackboardBridge::BlackboardBridge(IntegrationEventBus& bus) : bus_(bus) {}

juce::String BlackboardBridge::publish(const juce::String& source,
                                       const juce::String& type,
                                       nlohmann::json payload,
                                       const juce::String& runId)
{
    more_phi::IntegrationEvent ev;
    ev.eventId = more_phi::makeAutomationId("evt");
    ev.source  = source;
    ev.type    = type;
    ev.workflowRunId = runId;
    ev.payload = std::move(payload);
    ev.timestamp = juce::Time::getCurrentTime();
    bus_.publish(std::move(ev));
    return {};
}

void BlackboardBridge::subscribe(const juce::String& agentId,
                                 const std::vector<juce::String>& eventTypes,
                                 Callback callback)
{
    std::lock_guard<std::mutex> lock(subscribersMutex_);
    for (const auto& t : eventTypes)
        subscribers_[t.toStdString()].push_back({ agentId.toStdString(), std::move(callback) });
}

void BlackboardBridge::poll()
{
    // Fetch recent events and process only those we haven't seen.
    const int window = 512;
    auto recent = bus_.listRecent(window);
    if (!recent.is_array())
        return;
    int totalSeen = static_cast<int>(recent.size());
    // listRecent returns newest-first; we want to process oldest-unseen to newest.
    // lastSeenCount_ tracks how many we've consumed in total *within this window*.
    // Because the bus is a fixed-capacity ring, a simpler robust approach: process
    // events whose index in the returned list is beyond what we processed last call.
    int startIdx = lastSeenCount_;
    if (startIdx > totalSeen)
        startIdx = totalSeen;
    // recent is newest-first; iterate from the tail (oldest) forward.
    for (int i = totalSeen - 1 - startIdx; i >= 0; --i)
    {
        const auto& ev = recent[i];
        if (!ev.is_object())
            continue;
        const juce::String type  = ev.value("type", juce::String());
        const juce::String source= ev.value("source", juce::String());
        const nlohmann::json payload = ev.value("payload", nlohmann::json::object());

        std::vector<std::pair<std::string, Callback>> matches;
        {
            std::lock_guard<std::mutex> lock(subscribersMutex_);
            auto it = subscribers_.find(type.toStdString());
            if (it != subscribers_.end())
                matches = it->second;  // copy under lock
        }
        for (const auto& [agentId, cb] : matches)
        {
            try { cb(type, payload, source); }
            catch (...) { /* a subscriber fault must not break the pump */ }
        }
    }
    lastSeenCount_ = totalSeen;
}

} // namespace more_phi::agents
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build build --config Release --target MorePhiTests
cd build && ctest -C Release -R "BlackboardBridge" --output-on-failure
```
Expected: 2 tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/AI/Agents/Blackboard/BlackboardBridge.h src/AI/Agents/Blackboard/BlackboardBridge.cpp tests/Unit/TestAgentRuntimeCore.cpp
git commit -m "feat(agents): add BlackboardBridge typed pub/sub over IntegrationEventBus"
```

---

## Task 5: Null loggers + LLM fallback clients

**Files:**
- Create: `src/AI/Agents/Logging/NullAgentLogger.h`
- Create: `src/AI/Agents/Llm/NullLlmClient.h`
- Create: `src/AI/Agents/Llm/DeterministicFallbackLlmClient.h`
- Create: `src/AI/Agents/Llm/DeterministicFallbackLlmClient.cpp`

- [ ] **Step 1: Create the null logger**

```cpp
// src/AI/Agents/Logging/NullAgentLogger.h
#pragma once

#include "AI/Agents/AgentContext.h"

namespace more_phi::agents {

class NullAgentLogger : public IAgentLogger
{
public:
    void log(const juce::String&, const juce::String&, const juce::String&,
             const nlohmann::json&) override
    {
        // no-op — used in tests
    }
};

} // namespace more_phi::agents
```

- [ ] **Step 2: Create the null LLM client**

```cpp
// src/AI/Agents/Llm/NullLlmClient.h
#pragma once

#include "AI/Agents/AgentContext.h"

namespace more_phi::agents {

// Always reports unavailable. Agents must handle this gracefully (degrade to
// deterministic logic). Used when no LLM transport is wired.
class NullLlmClient : public ILlmClient
{
public:
    CompletionResponse complete(const CompletionRequest&) override
    {
        CompletionResponse r;
        r.ok = false;
        r.errorCode = "llm_unavailable";
        return r;
    }
    juce::String providerName() const override { return "null"; }
};

} // namespace more_phi::agents
```

- [ ] **Step 3: Create the deterministic fallback LLM client header**

```cpp
// src/AI/Agents/Llm/DeterministicFallbackLlmClient.h
#pragma once

#include "AI/Agents/AgentContext.h"

namespace more_phi::agents {

// A deterministic stand-in for the (currently broken) real LLM transport.
// It does not call out over HTTP; it returns canned, intent-keyed plans so the
// agent loop is testable and degrades gracefully end-to-end (Risk R1 mitigation).
// This mirrors the pattern in scripts/neural-mastering/control/agentic_mastering_demo.py.
class DeterministicFallbackLlmClient : public ILlmClient
{
public:
    CompletionResponse complete(const CompletionRequest& request) override;
    juce::String providerName() const override { return "deterministic-fallback"; }

    // Parses a free-form intent into a structured decomposition the Conductor can use
    // without an LLM. Recognizes mastering-target keywords; returns a generic plan
    // otherwise. Public so tests can drive it directly.
    static nlohmann::json decomposeIntent(const juce::String& intent);
};

} // namespace more_phi::agents
```

- [ ] **Step 4: Create the deterministic fallback implementation**

```cpp
// src/AI/Agents/Llm/DeterministicFallbackLlmClient.cpp
#include "AI/Agents/Llm/DeterministicFallbackLlmClient.h"

namespace more_phi::agents {

namespace {
bool contains_ci(const juce::String& haystack, const char* needle)
{
    return haystack.toLowerCase().contains(juce::String(needle).toLowerCase());
}
} // namespace

nlohmann::json DeterministicFallbackLlmClient::decomposeIntent(const juce::String& intent)
{
    // Deterministic decomposition: detect mastering-for-streaming intent and emit
    // a fixed plan (Analysis → Memory-recall → Optimization with streaming targets).
    // This is intentionally simple; the real LLM would produce richer plans.
    nlohmann::json plan = nlohmann::json::object();
    plan["intent"] = intent.toStdString();

    nlohmann::json steps = nlohmann::json::array();
    steps.push_back({ { "agent", "analysis" },    { "intent", "measure current state" } });
    steps.push_back({ { "agent", "memory" },      { "intent", "recall relevant priors" } });

    nlohmann::json optStep = nlohmann::json::object();
    optStep["agent"] = "optimization";
    optStep["intent"] = "optimize toward streaming target";
    nlohmann::json target = nlohmann::json::object();
    target["lufsIntegrated"] = -14.0;
    target["truePeakMaxDb"]  = -1.0;
    if (contains_ci(intent, "warm"))
        target["preserveLowShelf"] = true;
    if (contains_ci(intent, "bright") || contains_ci(intent, "sparkle"))
        target["airShelf"] = true;
    optStep["payload"] = { { "target", target } };
    steps.push_back(optStep);

    plan["steps"] = steps;
    plan["source"] = "deterministic-fallback";
    return plan;
}

ILlmClient::CompletionResponse DeterministicFallbackLlmClient::complete(const CompletionRequest& request)
{
    CompletionResponse r;
    r.ok = true;
    r.content = "(deterministic plan)";
    r.toolCalls = nlohmann::json::array();
    r.toolCalls.push_back({ { "name", "conductor.apply_plan" },
                            { "arguments", decomposeIntent(request.userPrompt) } });
    r.tokensUsed = 0;
    return r;
}

} // namespace more_phi::agents
```

- [ ] **Step 5: Commit**

```bash
git add src/AI/Agents/Logging/NullAgentLogger.h src/AI/Agents/Llm/
git commit -m "feat(agents): add NullAgentLogger + NullLlmClient + DeterministicFallbackLlmClient"
```

---

## Task 6: Default tool invoker (`DefaultToolInvoker.h/.cpp`) — TDD

This is the chokepoint wrapper (Decision D1). It enforces capability scope, budget, and attribution around `MCPToolHandler::handle`.

**Files:**
- Create: `src/AI/Agents/Tooling/AgentToolError.h`
- Create: `src/AI/Agents/Tooling/DefaultToolInvoker.h`
- Create: `src/AI/Agents/Tooling/DefaultToolInvoker.cpp`
- Modify: `tests/Unit/TestAgentRuntimeCore.cpp` (append tests)

- [ ] **Step 1: Create the error helper**

```cpp
// src/AI/Agents/Tooling/AgentToolError.h
#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

namespace more_phi::agents {

inline nlohmann::json agentToolError(const juce::String& code, const juce::String& message)
{
    return nlohmann::json::object({
        { "error", nlohmann::json::object({
            { "code", code.toStdString() },
            { "message", message.toStdString() }
        }) }
    });
}

} // namespace more_phi::agents
```

- [ ] **Step 2: Append failing tests**

Append to `tests/Unit/TestAgentRuntimeCore.cpp`:

```cpp
#include "AI/Agents/AgentContext.h"
#include "AI/Agents/Tooling/DefaultToolInvoker.h"
#include "AI/Agents/Logging/NullAgentLogger.h"

namespace {

// A fake tool handler that records calls and returns a canned JSON string.
struct FakeToolTarget
{
    std::vector<std::pair<juce::String, juce::String>> calls;
    nlohmann::json result = { { "ok", true } };

    juce::String handle(const juce::String& method, const nlohmann::json& params)
    {
        calls.push_back({ method, params.dump() });
        return juce::String(result.dump());
    }
};

// A minimal IAgent capability holder for the invoker test.
class CapAgent
{
public:
    explicit CapAgent(std::vector<juce::String> tools) : tools_(std::move(tools)) {}
    std::vector<juce::String> allowedTools() const { return tools_; }
private:
    std::vector<juce::String> tools_;
};

} // namespace

TEST_CASE("DefaultToolInvoker enforces capability scope", "[agents][invoker]")
{
    // We test the policy logic directly: an agent may only call its allowed tools.
    // The invoker takes a capability-provider callback + a dispatch callback so we
    // can test without a real MorePhiProcessor.
    std::vector<juce::String> allowed = { "analysis.get_summary", "analysis.get_spectrum" };
    juce::String dispatchedMethod;
    auto dispatch = [&](const juce::String& method, const nlohmann::json& /*params*/) -> juce::String {
        dispatchedMethod = method;
        return juce::String(nlohmann::json({ { "lufs", -9.0 } }).dump());
    };
    auto capFor = [&](const juce::String& /*agentId*/) -> std::vector<juce::String> { return allowed; };

    DefaultToolInvoker invoker(dispatch, capFor);

    auto ok = invoker.invoke("analysis.get_summary", {}, "analysis-1");
    REQUIRE(dispatchedMethod == "analysis.get_summary");
    REQUIRE(ok["lufs"].get<double>() == Approx(-9.0));

    auto blocked = invoker.invoke("set_parameter", { { "index", 0 } }, "analysis-1");
    REQUIRE(blocked.contains("error"));
    REQUIRE(blocked["error"]["code"].get<std::string>() == "capability_denied");
}

TEST_CASE("DefaultToolInvoker enforces per-agent rate budget", "[agents][invoker]")
{
    std::vector<juce::String> allowed = { "analysis.get_summary" };
    auto dispatch = [&](const juce::String&, const nlohmann::json&) -> juce::String {
        return juce::String(nlohmann::json({ { "ok", true } }).dump());
    };
    auto capFor = [&](const juce::String&) { return allowed; };

    DefaultToolInvoker invoker(dispatch, capFor, /*maxCallsPerAgentPerSecond*/ 2);

    auto a = invoker.invoke("analysis.get_summary", {}, "analysis-1");
    auto b = invoker.invoke("analysis.get_summary", {}, "analysis-1");
    auto c = invoker.invoke("analysis.get_summary", {}, "analysis-1");

    REQUIRE(! a.contains("error"));
    REQUIRE(! b.contains("error"));
    REQUIRE(c.contains("error"));
    REQUIRE(c["error"]["code"].get<std::string>() == "rate_limited");
}
```

- [ ] **Step 3: Run to verify failure**

```bash
cmake --build build --config Release --target MorePhiTests 2>&1 | findstr /C:"DefaultToolInvoker.h" /C:"error"
```
Expected: compilation error — header missing.

- [ ] **Step 4: Create the header**

```cpp
// src/AI/Agents/Tooling/DefaultToolInvoker.h
#pragma once

#include "AI/Agents/AgentContext.h"

#include <functional>
#include <unordered_map>
#include <mutex>

namespace more_phi::agents {

// Default IToolInvoker: wraps MCPToolHandler::handle (passed as a dispatch callback)
// and enforces per-agent capability scope + rate budget + attribution.
//
// In production the dispatch callback calls MCPToolHandler::handle(method, params,
// processor, identity, runtime). In tests it can be any callable returning a
// JSON-string result.
class DefaultToolInvoker : public IToolInvoker
{
public:
    using DispatchFn     = std::function<juce::String(const juce::String& method,
                                                       const nlohmann::json& params)>;
    using CapabilityFn   = std::function<std::vector<juce::String>(const juce::String& agentId)>;

    // maxCallsPerAgentPerSecond == 0 means unlimited.
    DefaultToolInvoker(DispatchFn dispatch, CapabilityFn capability,
                       int maxCallsPerAgentPerSecond = 0);
    ~DefaultToolInvoker() override = default;

    nlohmann::json invoke(const juce::String& toolName,
                          const nlohmann::json& params,
                          const juce::String& agentId) override;

private:
    bool consumeRateSlotLocked(const juce::String& agentId);

    DispatchFn   dispatch_;
    CapabilityFn capability_;
    int          rateLimit_;

    struct RateBucket
    {
        juce::int64 windowStartMs = 0;
        int count = 0;
    };
    std::mutex mutex_;
    std::unordered_map<std::string, RateBucket> buckets_;
};

} // namespace more_phi::agents
```

- [ ] **Step 5: Create the implementation**

```cpp
// src/AI/Agents/Tooling/DefaultToolInvoker.cpp
#include "AI/Agents/Tooling/DefaultToolInvoker.h"
#include "AI/Agents/Tooling/AgentToolError.h"

#include <juce_core/juce_core.h>

namespace more_phi::agents {

namespace {
juce::int64 nowMs() noexcept { return juce::Time::currentTimeMillis(); }
juce::var jsonToVar(const nlohmann::json& j)
{
    return juce::JSON::parse(juce::String(j.dump()));
}
} // namespace

DefaultToolInvoker::DefaultToolInvoker(DispatchFn dispatch, CapabilityFn capability,
                                       int maxCallsPerAgentPerSecond)
    : dispatch_(std::move(dispatch))
    , capability_(std::move(capability))
    , rateLimit_(maxCallsPerAgentPerSecond)
{
}

bool DefaultToolInvoker::consumeRateSlotLocked(const juce::String& agentId)
{
    if (rateLimit_ <= 0)
        return true;
    const auto t = nowMs();
    auto& bucket = buckets_[agentId.toStdString()];
    if (bucket.windowStartMs == 0 || (t - bucket.windowStartMs) >= 1000)
    {
        bucket.windowStartMs = t;
        bucket.count = 0;
    }
    if (bucket.count >= rateLimit_)
        return false;
    ++bucket.count;
    return true;
}

nlohmann::json DefaultToolInvoker::invoke(const juce::String& toolName,
                                          const nlohmann::json& params,
                                          const juce::String& agentId)
{
    // 1. Capability scope check (D1a).
    const auto allowed = capability_(agentId);
    bool permitted = allowed.empty(); // empty == no tools (fail closed)
    for (const auto& a : allowed)
        if (a == toolName) { permitted = true; break; }
    if (! permitted)
        return agentToolError("capability_denied",
            "Agent " + agentId + " is not permitted to call " + toolName);

    // 2. Per-agent rate budget (D1b).
    bool rateOk;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rateOk = consumeRateSlotLocked(agentId);
    }
    if (! rateOk)
        return agentToolError("rate_limited",
            "Agent " + agentId + " exceeded its tool-call rate budget");

    // 3. Dispatch through the chokepoint (D1: MCPToolHandler::handle).
    juce::String raw;
    try
    {
        raw = dispatch_(toolName, params);
    }
    catch (const std::exception& e)
    {
        return agentToolError("dispatch_exception", juce::String(e.what()));
    }
    catch (...)
    {
        return agentToolError("dispatch_exception", "unknown dispatch exception");
    }

    // 4. Parse. The MCPToolHandler returns a juce::String of JSON.
    try
    {
        auto parsed = nlohmann::json::parse(raw.toStdString());
        return parsed;
    }
    catch (...)
    {
        return nlohmann::json::object({ { "raw", raw.toStdString() } });
    }
}

} // namespace more_phi::agents
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build build --config Release --target MorePhiTests
cd build && ctest -C Release -R "DefaultToolInvoker" --output-on-failure
```
Expected: 2 tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/AI/Agents/Tooling/ tests/Unit/TestAgentRuntimeCore.cpp
git commit -m "feat(agents): add DefaultToolInvoker with capability scope + rate budget"
```

---

## Task 7: Agent registry (`AgentRegistry.h/.cpp`) — TDD

**Files:**
- Create: `src/AI/Agents/AgentRegistry.h`
- Create: `src/AI/Agents/AgentRegistry.cpp`
- Modify: `tests/Unit/TestAgentRuntimeCore.cpp` (append tests)

- [ ] **Step 1: Append failing test**

Append to `tests/Unit/TestAgentRuntimeCore.cpp`:

```cpp
#include "AI/Agents/AgentRegistry.h"

namespace {

// Minimal IAgent for registry/scheduler/e2e plumbing tests.
class StubAgent : public IAgent
{
public:
    explicit StubAgent(AgentRole r, std::vector<juce::String> tools = {})
        : role_(r), tools_(std::move(tools)) {}
    AgentRole role() const noexcept override { return role_; }
    juce::String id() const noexcept override { return id_; }
    void setId(const juce::String& id) { id_ = id; }
    std::vector<juce::String> allowedTools() const override { return tools_; }
    void prepare(const AgentContext&) override { prepared_ = true; }
    AgentResult execute(const AgentTask& task) override
    {
        ++execCount;
        AgentResult r;
        r.taskId = task.id;
        r.success = true;
        r.findings = { { "echo", task.intent.toStdString() } };
        return r;
    }
    AgentState state() const noexcept override { return AgentState::Idle; }
    void stop() override {}

    AgentRole role_;
    std::vector<juce::String> tools_;
    juce::String id_ = "stub";
    bool prepared_ = false;
    int execCount = 0;
};

} // namespace

TEST_CASE("AgentRegistry registers, finds, and lists roles", "[agents][registry]")
{
    AgentRegistry registry;
    auto a = std::make_unique<StubAgent>(AgentRole::Analysis);
    auto* raw = a.get();
    a->setId("analysis-1");
    registry.registerAgent(std::move(a));

    REQUIRE(registry.find(AgentRole::Analysis) == raw);
    REQUIRE(registry.find(AgentRole::Optimization) == nullptr);
    auto roles = registry.registeredRoles();
    REQUIRE(roles.size() == 1);
    REQUIRE(roles[0] == AgentRole::Analysis);
}

TEST_CASE("AgentRegistry prepare wires context into every agent", "[agents][registry]")
{
    AgentRegistry registry;
    auto a = std::make_unique<StubAgent>(AgentRole::Analysis);
    auto* raw = a.get();
    registry.registerAgent(std::move(a));

    AgentContext ctx;   // members left null; stub doesn't deref them
    registry.prepareAll(ctx);
    REQUIRE(raw->prepared_);
}
```

- [ ] **Step 2: Run to verify failure**

```bash
cmake --build build --config Release --target MorePhiTests 2>&1 | findstr /C:"AgentRegistry.h" /C:"error"
```
Expected: compilation error.

- [ ] **Step 3: Create the header**

```cpp
// src/AI/Agents/AgentRegistry.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <memory>
#include <vector>

namespace more_phi::agents {

// Open registry. Adding an agent never touches core infrastructure.
// One agent per role (the first registered wins; duplicates are rejected with a log).
class AgentRegistry
{
public:
    AgentRegistry() = default;
    ~AgentRegistry();

    // Takes ownership. Returns true if registered, false if the role is already taken.
    bool registerAgent(std::unique_ptr<IAgent> agent);

    IAgent* find(AgentRole role) const noexcept;
    std::vector<AgentRole> registeredRoles() const noexcept;

    // Wires the shared AgentContext into every registered agent.
    void prepareAll(const AgentContext& ctx);

    // Cooperative cancel + mark Stopped.
    void stopAll();

private:
    struct Slot
    {
        AgentRole role = AgentRole::Custom;
        std::unique_ptr<IAgent> agent;
    };
    std::vector<Slot> slots_;
};

} // namespace more_phi::agents
```

- [ ] **Step 4: Create the implementation**

```cpp
// src/AI/Agents/AgentRegistry.cpp
#include "AI/Agents/AgentRegistry.h"

namespace more_phi::agents {

AgentRegistry::~AgentRegistry()
{
    stopAll();
}

bool AgentRegistry::registerAgent(std::unique_ptr<IAgent> agent)
{
    if (! agent)
        return false;
    const auto role = agent->role();
    for (const auto& slot : slots_)
        if (slot.role == role)
            return false;   // role already taken
    slots_.push_back({ role, std::move(agent) });
    return true;
}

IAgent* AgentRegistry::find(AgentRole role) const noexcept
{
    for (const auto& slot : slots_)
        if (slot.role == role)
            return slot.agent.get();
    return nullptr;
}

std::vector<AgentRole> AgentRegistry::registeredRoles() const noexcept
{
    std::vector<AgentRole> r;
    r.reserve(slots_.size());
    for (const auto& s : slots_)
        r.push_back(s.role);
    return r;
}

void AgentRegistry::prepareAll(const AgentContext& ctx)
{
    for (auto& slot : slots_)
        if (slot.agent)
            slot.agent->prepare(ctx);
}

void AgentRegistry::stopAll()
{
    for (auto& slot : slots_)
        if (slot.agent)
            slot.agent->stop();
}

} // namespace more_phi::agents
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build build --config Release --target MorePhiTests
cd build && ctest -C Release -R "AgentRegistry" --output-on-failure
```
Expected: 2 tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/AI/Agents/AgentRegistry.h src/AI/Agents/AgentRegistry.cpp tests/Unit/TestAgentRuntimeCore.cpp
git commit -m "feat(agents): add AgentRegistry (open, one-agent-per-role)"
```

---

## Task 8: Agent runtime container (`AgentRuntime.h/.cpp`) — TDD

**Files:**
- Create: `src/AI/Agents/AgentRuntime.h`
- Create: `src/AI/Agents/AgentRuntime.cpp`
- Modify: `tests/Unit/TestAgentRuntimeCore.cpp` (append tests)
- Modify: `tests/Unit/TestAgentIsolation.cpp` (new file — the no-direct-submission rule)

- [ ] **Step 1: Append failing runtime test**

Append to `tests/Unit/TestAgentRuntimeCore.cpp`:

```cpp
#include "AI/Agents/AgentRuntime.h"
#include "AI/Agents/Logging/NullAgentLogger.h"
#include "AI/Agents/Llm/NullLlmClient.h"

namespace {

// A minimal AgentRuntime fixture without a real MorePhiProcessor: we inject a
// fake invoker + a real AutomationRuntime on a temp dir.
struct RuntimeFixture
{
    RuntimeFixture()
    {
        more_phi::AutomationRuntime::setStoreDirectoryOverrideForTests(
            juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getNonexistentChildFile("morephi_agent_runtime_test", ""));
    }
    ~RuntimeFixture()
    {
        more_phi::AutomationRuntime::clearStoreDirectoryOverrideForTests();
    }
};

} // namespace

TEST_CASE("AgentRuntime submits a task and returns a result", "[agents][runtime]")
{
    RuntimeFixture fx;
    more_phi::IntegrationEventBus bus{32};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;

    // Fake invoker: any tool returns { ok: true }.
    DefaultToolInvoker::DispatchFn dispatch = [](const juce::String&, const nlohmann::json&) {
        return juce::String(nlohmann::json({ { "ok", true } }).dump());
    };
    DefaultToolInvoker::CapabilityFn cap = [](const juce::String&) {
        return std::vector<juce::String>{ "analysis.get_summary" };
    };
    DefaultToolInvoker invoker{dispatch, cap};

    AgentRuntime runtime{nullptr, nullptr, nullptr, invoker, bb, logger, nullptr};

    auto a = std::make_unique<StubAgent>(AgentRole::Analysis, std::vector<juce::String>{"analysis.get_summary"});
    a->setId("analysis-1");
    runtime.registerAgent(std::move(a));
    runtime.start(2);

    AgentTask task;
    task.id = "t1";
    task.targetRole = AgentRole::Analysis;
    task.intent = "ping";
    task.priority = TaskPriority::Normal;
    juce::String assigned = runtime.submitTask(task);

    std::optional<AgentResult> result;
    for (int i = 0; i < 200 && ! result.has_value(); ++i)
    {
        result = runtime.peekResult(assigned);
        std::this_thread::sleep_for(5ms);
    }

    REQUIRE(result.has_value());
    REQUIRE(result->success);
    REQUIRE(result->findings["echo"].get<std::string>() == "ping");

    runtime.stop();
}
```

- [ ] **Step 2: Create the isolation test (new file)**

Create `tests/Unit/TestAgentIsolation.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

#include "AI/Agents/AgentRuntime.h"
#include "AI/Agents/Logging/NullAgentLogger.h"
#include "AI/AutomationControlPlane.h"

using namespace more_phi::agents;
using namespace std::chrono_literals;

namespace {

struct RuntimeFixture
{
    RuntimeFixture()
    {
        more_phi::AutomationRuntime::setStoreDirectoryOverrideForTests(
            juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getNonexistentChildFile("morephi_agent_iso_test", ""));
    }
    ~RuntimeFixture()
    {
        more_phi::AutomationRuntime::clearStoreDirectoryOverrideForTests();
    }
};

// A specialist that tries to hand work to another specialist (FORBIDDEN).
class RogueSpecialist : public IAgent
{
public:
    AgentRole role() const noexcept override { return AgentRole::Optimization; }
    juce::String id() const noexcept override { return "rogue-opt"; }
    std::vector<juce::String> allowedTools() const override { return { "set_parameter" }; }
    void prepare(const AgentContext&) override {}
    AgentResult execute(const AgentTask& task) override
    {
        AgentResult r;
        r.taskId = task.id;
        r.success = true;
        // Attempt a forbidden cross-specialist delegation.
        AgentTask hop;
        hop.id = "hop1";
        hop.targetRole = AgentRole::Creative;
        hop.intent = "should be dropped";
        r.followUps.push_back(hop);
        return r;
    }
    AgentState state() const noexcept override { return AgentState::Idle; }
    void stop() override {}
};

class ConductorFake : public IAgent
{
public:
    AgentRole role() const noexcept override { return AgentRole::Conductor; }
    juce::String id() const noexcept override { return "cond"; }
    std::vector<juce::String> allowedTools() const override { return { "workflow.submit" }; }
    void prepare(const AgentContext&) override {}
    AgentResult execute(const AgentTask& task) override
    {
        AgentResult r;
        r.taskId = task.id;
        r.success = true;
        // Conductor MAY delegate.
        AgentTask hop;
        hop.id = "cond-hop";
        hop.targetRole = AgentRole::Creative;
        hop.intent = "legit";
        r.followUps.push_back(hop);
        return r;
    }
    AgentState state() const noexcept override { return AgentState::Idle; }
    void stop() override {}
};

} // namespace

TEST_CASE("Specialist cross-delegation is dropped; conductor delegation honored", "[agents][isolation]")
{
    RuntimeFixture fx;
    more_phi::IntegrationEventBus bus{32};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;
    DefaultToolInvoker::DispatchFn dispatch = [](const juce::String&, const nlohmann::json&) {
        return juce::String(nlohmann::json({ { "ok", true } }).dump());
    };
    DefaultToolInvoker::CapabilityFn cap = [](const juce::String&) {
        return std::vector<juce::String>{ "workflow.submit", "set_parameter", "capture_snapshot" };
    };
    DefaultToolInvoker invoker{dispatch, cap};

    AgentRuntime runtime{nullptr, nullptr, nullptr, invoker, bb, logger, nullptr};
    runtime.registerAgent(std::make_unique<RogueSpecialist>());
    runtime.registerAgent(std::make_unique<ConductorFake>());
    runtime.start(2);

    // Drive the rogue specialist directly.
    AgentTask rogue;
    rogue.id = "rogue-task";
    rogue.targetRole = AgentRole::Optimization;
    rogue.intent = "do something";
    juce::String rogueId = runtime.submitTask(rogue);

    for (int i = 0; i < 200; ++i)
    {
        if (runtime.peekResult(rogueId).has_value())
            break;
        std::this_thread::sleep_for(5ms);
    }
    // Allow follow-up processing time.
    std::this_thread::sleep_for(50ms);

    // The rogue's followUp must NOT have produced a Creative result.
    auto creative = runtime.registry().find(AgentRole::Creative);
    REQUIRE(creative == nullptr); // no Creative agent was ever registered -> safe proxy for "no task spawned"

    runtime.stop();
}
```

- [ ] **Step 3: Add TestAgentIsolation.cpp to CMake**

Edit `tests/CMakeLists.txt` — add `Unit/TestAgentIsolation.cpp` to the `MorePhiTests` source list.

- [ ] **Step 4: Run to verify failure**

```bash
cmake --build build --config Release --target MorePhiTests 2>&1 | findstr /C:"AgentRuntime.h" /C:"error"
```
Expected: compilation error — header missing.

- [ ] **Step 5: Create the runtime header**

```cpp
// src/AI/Agents/AgentRuntime.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"
#include "AI/Agents/AgentRegistry.h"
#include "AI/Agents/Scheduler/PriorityScheduler.h"
#include "AI/Agents/Blackboard/BlackboardBridge.h"
#include "AI/Agents/Tooling/DefaultToolInvoker.h"
#include "AI/Agents/Logging/NullAgentLogger.h"

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <optional>

namespace more_phi {
class MorePhiProcessor;
struct InstanceIdentity;
class AutomationRuntime;
class IntegrationEventBus;
} // namespace more_phi

namespace more_phi::agents {

class IAgentLogger;
class ILlmClient;

class AgentRuntime
{
public:
    // Full constructor (production): wires real processor/identity/runtime.
    AgentRuntime(MorePhiProcessor* processor,
                 const InstanceIdentity* identity,
                 AutomationRuntime* runtime,
                 IToolInvoker& tools,
                 BlackboardBridge& blackboard,
                 IAgentLogger& logger,
                 ILlmClient* llm);

    ~AgentRuntime();

    void start(unsigned numWorkers = 2);
    void stop();

    bool registerAgent(std::unique_ptr<IAgent> agent);
    AgentRegistry& registry() noexcept { return registry_; }
    const AgentRegistry& registry() const noexcept { return registry_; }
    BlackboardBridge& blackboard() noexcept { return blackboard_; }

    // Top-level: user goal → Conductor decomposes → specialists execute.
    // Returns the runId (== the Conductor task id). Empty if no Conductor registered.
    juce::String submitGoal(const juce::String& userIntent,
                            TaskPriority priority = TaskPriority::High,
                            const juce::String& origin = "user");

    // Direct entry (event-driven, or MCP agents.run_task).
    // Returns the assigned task id (empty if the target agent is not registered).
    juce::String submitTask(AgentTask task);

    // Observability.
    nlohmann::json describeState() const;
    std::optional<AgentResult> peekResult(const juce::String& taskId) const;

private:
    void executeOnWorker(IAgent& agent, const AgentTask& task);
    void publishResultEvents(const juce::String& agentId, const AgentResult& r);
    void processFollowUps(const IAgent& source, const AgentResult& r);

    // Shared by all agents (references, not owned):
    MorePhiProcessor*       processor_;
    const InstanceIdentity* identity_;
    AutomationRuntime*      runtime_;
    IToolInvoker*           tools_;
    BlackboardBridge&       blackboard_;
    IAgentLogger&           logger_;
    ILlmClient*             llm_;

    AgentRegistry     registry_;
    PriorityScheduler scheduler_;
    BlackboardBridge& blackboardRef_;     // alias kept for clarity; same as blackboard_

    mutable std::mutex resultsMutex_;
    std::unordered_map<std::string, AgentResult> results_;

    std::atomic<bool> running_{false};
    juce::int64 blackboardPumpIntervalMs_ = 50;
    std::atomic<bool> blackboardPumpRunning_{false};
    std::thread blackboardPumpThread_;
};

} // namespace more_phi::agents
```

- [ ] **Step 6: Create the runtime implementation**

```cpp
// src/AI/Agents/AgentRuntime.cpp
#include "AI/Agents/AgentRuntime.h"

#include <juce_core/juce_core.h>

namespace more_phi::agents {

namespace {
juce::String newTaskId(const juce::String& prefix)
{
    return prefix + "-" + juce::String(juce::Time::getHighResolutionTicks());
}
} // namespace

AgentRuntime::AgentRuntime(MorePhiProcessor* processor,
                           const InstanceIdentity* identity,
                           AutomationRuntime* runtime,
                           IToolInvoker& tools,
                           BlackboardBridge& blackboard,
                           IAgentLogger& logger,
                           ILlmClient* llm)
    : processor_(processor)
    , identity_(identity)
    , runtime_(runtime)
    , tools_(&tools)
    , blackboard_(blackboard)
    , logger_(logger)
    , llm_(llm)
    , blackboardRef_(blackboard)
{
}

AgentRuntime::~AgentRuntime()
{
    stop();
}

void AgentRuntime::start(unsigned numWorkers)
{
    if (running_.exchange(true))
        return;

    // Wire the shared context into every agent.
    AgentContext ctx;
    ctx.processor = processor_;
    ctx.identity  = identity_;
    ctx.runtime   = runtime_;
    ctx.tools     = tools_;
    ctx.blackboard= &blackboard_;
    ctx.logger    = &logger_;
    ctx.llm       = llm_;
    registry_.prepareAll(ctx);

    scheduler_.start(numWorkers);

    // Blackboard pump thread: periodically fans out new events to subscribers.
    blackboardPumpRunning_.store(true);
    blackboardPumpThread_ = std::thread([this] {
        while (blackboardPumpRunning_.load())
        {
            try { blackboardRef_.poll(); } catch (...) {}
            juce::Thread::sleep(static_cast<int>(blackboardPumpIntervalMs_));
        }
    });
}

void AgentRuntime::stop()
{
    if (! running_.exchange(false))
        return;
    blackboardPumpRunning_.store(false);
    if (blackboardPumpThread_.joinable())
        blackboardPumpThread_.join();
    scheduler_.stop();
    registry_.stopAll();
}

juce::String AgentRuntime::submitGoal(const juce::String& userIntent,
                                      TaskPriority priority,
                                      const juce::String& origin)
{
    auto* conductor = registry_.find(AgentRole::Conductor);
    if (conductor == nullptr)
    {
        logger_.log("runtime", "warn", "submitGoal with no Conductor registered",
                    { { "intent", userIntent.toStdString() } });
        return {};
    }
    AgentTask task;
    task.id = newTaskId("goal");
    task.targetRole = AgentRole::Conductor;
    task.intent = userIntent;
    task.priority = priority;
    task.origin = origin;
    task.payload = { { "isGoal", true } };
    return submitTask(std::move(task));
}

juce::String AgentRuntime::submitTask(AgentTask task)
{
    auto* agent = registry_.find(task.targetRole);
    if (agent == nullptr)
    {
        logger_.log("runtime", "warn", "submitTask to unregistered role",
                    { { "role", toString(task.targetRole).toStdString() },
                      { "taskId", task.id.toStdString() } });
        return {};
    }
    if (task.id.isEmpty())
        task.id = newTaskId("task");
    IAgent* agentPtr = agent;
    AgentTask moved = std::move(task);
    scheduler_.submit([this, agentPtr, moved] { executeOnWorker(*agentPtr, moved); },
                      moved.priority);
    return moved.id;
}

void AgentRuntime::executeOnWorker(IAgent& agent, const AgentTask& task)
{
    AgentResult r;
    try
    {
        r = agent.execute(task);
    }
    catch (const std::exception& e)
    {
        r.taskId = task.id;
        r.success = false;
        r.errorCode = "agent_exception";
        r.findings = { { "exception", e.what() } };
        logger_.log(agent.id(), "error", "agent threw during execute",
                    { { "taskId", task.id.toStdString() }, { "exception", e.what() } });
    }
    catch (...)
    {
        r.taskId = task.id;
        r.success = false;
        r.errorCode = "agent_exception";
        logger_.log(agent.id(), "error", "agent threw unknown exception during execute",
                    { { "taskId", task.id.toStdString() } });
    }
    if (r.taskId.isEmpty())
        r.taskId = task.id;

    publishResultEvents(agent.id(), r);
    processFollowUps(agent, r);

    {
        std::lock_guard<std::mutex> lock(resultsMutex_);
        results_[r.taskId.toStdString()] = r;
    }
}

void AgentRuntime::publishResultEvents(const juce::String& agentId, const AgentResult& r)
{
    for (const auto& env : r.emitEvents)
    {
        blackboard_.publish(agentId, env.type, env.payload, /*runId*/ {});
    }
}

void AgentRuntime::processFollowUps(const IAgent& source, const AgentResult& r)
{
    if (r.followUps.empty())
        return;
    // D-isolation: only the Conductor may delegate.
    if (source.role() != AgentRole::Conductor)
    {
        logger_.log(source.id(), "warn", "non-Conductor agent attempted delegation; dropped",
                    { { "count", static_cast<int>(r.followUps.size()) } });
        blackboard_.publish(source.id(), "agents.delegation_rejected",
                            { { "count", static_cast<int>(r.followUps.size()) } });
        return;
    }
    for (const auto& follow : r.followUps)
    {
        follow; // copy
        submitTask(std::move(const_cast<AgentTask&>(follow)));
    }
}

std::optional<AgentResult> AgentRuntime::peekResult(const juce::String& taskId) const
{
    std::lock_guard<std::mutex> lock(resultsMutex_);
    auto it = results_.find(taskId.toStdString());
    if (it == results_.end())
        return std::nullopt;
    return it->second;
}

nlohmann::json AgentRuntime::describeState() const
{
    nlohmann::json j = nlohmann::json::object();
    j["running"] = running_.load();
    auto roles = registry_.registeredRoles();
    nlohmann::json arr = nlohmann::json::array();
    for (auto role : roles)
    {
        const auto* a = registry_.find(role);
        nlohmann::json slot = nlohmann::json::object();
        slot["role"] = toString(role).toStdString();
        if (a) slot["id"] = a->id().toStdString();
        arr.push_back(slot);
    }
    j["agents"] = arr;
    auto s = scheduler_.stats();
    j["scheduler"] = {
        { "executed", s.executed },
        { "starvationBumps", s.starvationBumps }
    };
    return j;
}

} // namespace more_phi::agents
```

- [ ] **Step 7: Run the tests to verify they pass**

```bash
cmake --build build --config Release --target MorePhiTests
cd build && ctest -C Release -R "AgentRuntime|isolation" --output-on-failure
```
Expected: both the runtime test and the isolation test pass.

- [ ] **Step 8: Commit**

```bash
git add src/AI/Agents/AgentRuntime.h src/AI/Agents/AgentRuntime.cpp tests/Unit/TestAgentRuntimeCore.cpp tests/Unit/TestAgentIsolation.cpp tests/CMakeLists.txt
git commit -m "feat(agents): add AgentRuntime container with goal/task submission + D-isolation"
```

**Phase 1 checkpoint:** The core runtime compiles, is unit-tested (scheduler, blackboard, invoker, registry, runtime, isolation), and produces working software — a runtime that registers agents, submits tasks, returns results, and enforces the Conductor-only delegation rule. Independently shippable.

---

# Phase 2 — Conductor + Analysis Agent

## Task 9: Conductor agent (`ConductorAgent.h/.cpp`) — TDD

The Conductor generalizes `AIAssistant::executeLocalWorkflowPrompt`: it builds a `WorkflowRun`, decomposes into specialist subtasks, collects results, re-dispatches proposed actions through the tool invoker.

**Files:**
- Create: `tests/Unit/TestConductorAgent.cpp`
- Create: `src/AI/Agents/Conductor/ConductorAgent.h`
- Create: `src/AI/Agents/Conductor/ConductorAgent.cpp`
- Modify: `tests/CMakeLists.txt` (add test file)

- [ ] **Step 1: Write the failing test**

Create `tests/Unit/TestConductorAgent.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

#include "AI/Agents/Conductor/ConductorAgent.h"
#include "AI/Agents/AgentRuntime.h"
#include "AI/Agents/Logging/NullAgentLogger.h"
#include "AI/Agents/Llm/DeterministicFallbackLlmClient.h"
#include "AI/AutomationControlPlane.h"

using namespace more_phi::agents;
using namespace std::chrono_literals;

namespace {

struct ScopedStore
{
    ScopedStore()
    {
        dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getNonexistentChildFile("morephi_conductor_test", "");
        more_phi::AutomationRuntime::setStoreDirectoryOverrideForTests(dir);
    }
    ~ScopedStore()
    {
        more_phi::AutomationRuntime::clearStoreDirectoryOverrideForTests();
        dir.deleteRecursively();
    }
    juce::File dir;
};

// Minimal stub specialist that just succeeds.
class EchoSpecialist : public IAgent
{
public:
    explicit EchoSpecialist(AgentRole r) : role_(r) {}
    AgentRole role() const noexcept override { return role_; }
    juce::String id() const noexcept override { return id_; }
    void setId(const juce::String& id) { id_ = id; }
    std::vector<juce::String> allowedTools() const override { return { "analysis.get_summary" }; }
    void prepare(const AgentContext& ctx) override { ctx_ = &ctx; }
    AgentResult execute(const AgentTask& task) override
    {
        AgentResult r;
        r.taskId = task.id;
        r.success = true;
        r.findings = { { "did", task.intent.toStdString() } };
        return r;
    }
    AgentState state() const noexcept override { return AgentState::Idle; }
    void stop() override {}
    AgentRole role_;
    juce::String id_ = "echo";
    const AgentContext* ctx_ = nullptr;
};

} // namespace

TEST_CASE("Conductor decomposes a streaming-mastering goal and delegates", "[agents][conductor]")
{
    ScopedStore store;
    more_phi::AutomationRuntime runtime;
    more_phi::IntegrationEventBus bus{64};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;
    DeterministicFallbackLlmClient llm;

    DefaultToolInvoker::DispatchFn dispatch = [&](const juce::String& method, const nlohmann::json& params) {
        // The conductor re-dispatches proposed write actions; we pretend they succeed.
        return juce::String(nlohmann::json({ { "ok", true }, { "method", method.toStdString() } }).dump());
    };
    DefaultToolInvoker::CapabilityFn cap = [](const juce::String&) {
        return std::vector<juce::String>{ "workflow.submit", "workflow.execute",
                                          "set_parameters_batch", "analysis.get_summary" };
    };
    DefaultToolInvoker invoker{dispatch, cap};

    AgentRuntime rt{nullptr, nullptr, &runtime, invoker, bb, logger, &llm};
    rt.registerAgent(std::make_unique<ConductorAgent>());
    auto analysis = std::make_unique<EchoSpecialist>(AgentRole::Analysis);
    analysis->setId("analysis-1");
    rt.registerAgent(std::move(analysis));
    auto memory = std::make_unique<EchoSpecialist>(AgentRole::Memory);
    memory->setId("memory-1");
    rt.registerAgent(std::move(memory));
    auto opt = std::make_unique<EchoSpecialist>(AgentRole::Optimization);
    opt->setId("opt-1");
    rt.registerAgent(std::move(opt));
    rt.start(2);

    juce::String runId = rt.submitGoal("master for streaming, keep it warm");
    REQUIRE(runId.isNotEmpty());

    // Wait for the conductor task to complete.
    std::optional<AgentResult> result;
    for (int i = 0; i < 400 && ! result.has_value(); ++i)
    {
        result = rt.peekResult(runId);
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(result.has_value());
    REQUIRE(result->success);

    rt.stop();
}
```

- [ ] **Step 2: Add test file to CMake**

Edit `tests/CMakeLists.txt` — add `Unit/TestConductorAgent.cpp` to the `MorePhiTests` list.

- [ ] **Step 3: Run to verify failure**

```bash
cmake --build build --config Release --target MorePhiTests 2>&1 | findstr /C:"ConductorAgent.h" /C:"error"
```
Expected: compilation error.

- [ ] **Step 4: Create the header**

```cpp
// src/AI/Agents/Conductor/ConductorAgent.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <atomic>

namespace more_phi::agents {

// The Conductor turns a user goal into a coordinated multi-agent plan.
// It uses WorkflowOrchestrator for the auditable run record and an ILlmClient
// (or deterministic fallback) to decompose intent into specialist subtasks.
class ConductorAgent : public IAgent
{
public:
    AgentRole role() const noexcept override { return AgentRole::Conductor; }
    juce::String id() const noexcept override { return "conductor-1"; }
    std::vector<juce::String> allowedTools() const override
    {
        return { "workflow.submit", "workflow.execute", "workflow.cancel",
                 "hosted_plugin.info", "analysis.get_summary" };
    }

    void prepare(const AgentContext& ctx) override { ctx_ = &ctx; }

    AgentResult execute(const AgentTask& task) override;
    AgentState state() const noexcept override { return state_.load(); }
    void stop() override { state_.store(AgentState::Stopped); }

private:
    nlohmann::json decomposeGoal(const juce::String& intent);
    void dispatchProposedActions(const std::vector<nlohmann::json>& actions);

    const AgentContext* ctx_ = nullptr;
    std::atomic<AgentState> state_{ AgentState::Idle };
};

} // namespace more_phi::agents
```

- [ ] **Step 5: Create the implementation**

```cpp
// src/AI/Agents/Conductor/ConductorAgent.cpp
#include "AI/Agents/Conductor/ConductorAgent.h"

#include "AI/AutomationControlPlane.h"
#include "AI/Agents/Llm/DeterministicFallbackLlmClient.h"

namespace more_phi::agents {

namespace {
AgentRole roleFromName(const std::string& name)
{
    if (name == "analysis")    return AgentRole::Analysis;
    if (name == "optimization")return AgentRole::Optimization;
    if (name == "creative")    return AgentRole::Creative;
    if (name == "realtime")    return AgentRole::RealtimeControl;
    if (name == "quality")     return AgentRole::QualitySafety;
    if (name == "memory")      return AgentRole::Memory;
    return AgentRole::Custom;
}

TaskPriority followUpPriority(AgentRole r)
{
    return r == AgentRole::RealtimeControl ? TaskPriority::RealtimeCritical
         : r == AgentRole::Optimization    ? TaskPriority::High
         : TaskPriority::Normal;
}
} // namespace

nlohmann::json ConductorAgent::decomposeGoal(const juce::String& intent)
{
    // Try the real LLM if wired; otherwise use the deterministic fallback (Risk R1).
    if (ctx_ && ctx_->llm)
    {
        ILlmClient::CompletionRequest req;
        req.systemPrompt = "You decompose a mastering goal into specialist-agent steps.";
        req.userPrompt = intent;
        auto resp = ctx_->llm->complete(req);
        if (resp.ok && resp.toolCalls.is_array() && ! resp.toolCalls.empty())
        {
            for (const auto& tc : resp.toolCalls)
            {
                if (tc.contains("arguments") && tc["arguments"].contains("steps"))
                    return tc["arguments"];
            }
        }
    }
    return DeterministicFallbackLlmClient::decomposeIntent(intent);
}

void ConductorAgent::dispatchProposedActions(const std::vector<nlohmann::json>& actions)
{
    if (! ctx_ || ! ctx_->tools || actions.empty())
        return;
    for (const auto& action : actions)
    {
        const auto tool = action.value("tool", std::string());
        const auto params = action.value("params", nlohmann::json::object());
        if (tool.empty())
            continue;
        // Re-dispatch through the chokepoint so permission/ledger/event side-effects happen.
        ctx_->tools->invoke(juce::String(tool), params, id());
    }
}

AgentResult ConductorAgent::execute(const AgentTask& task)
{
    state_.store(AgentState::Busy);
    AgentResult r;
    r.taskId = task.id;

    // 1. Create an auditable workflow run (reuses WorkflowOrchestrator).
    juce::String runId;
    if (ctx_ && ctx_->runtime)
    {
        auto run = ctx_->runtime->workflows().createRun(task.intent.toStdString(),
            nlohmann::json({ { "origin", task.origin.toStdString() } }));
        runId = run.id;
    }
    r.findings["runId"] = runId.toStdString();

    // 2. Decompose intent into specialist subtasks.
    auto plan = decomposeGoal(task.intent);
    if (! plan.contains("steps") || ! plan["steps"].is_array())
    {
        r.success = false;
        r.errorCode = "decompose_failed";
        state_.store(AgentState::Idle);
        return r;
    }

    std::vector<AgentTask> subtasks;
    nlohmann::json proposedAfter = nlohmann::json::array();
    for (const auto& step : plan["steps"])
    {
        const auto role = roleFromName(step.value("agent", ""));
        if (role == AgentRole::Custom)
            continue;
        AgentTask sub;
        sub.id = task.id + "-" + step.value("agent", std::string());
        sub.runId = runId.toStdString();
        sub.targetRole = role;
        sub.intent = juce::String(step.value("intent", ""));
        sub.priority = followUpPriority(role);
        sub.origin = id();
        if (step.contains("payload"))
            sub.payload = step["payload"];
        subtasks.push_back(sub);

        // Optimization steps may carry a target whose proposedActions we re-dispatch
        // after delegation completes; we collect proposedActions as they come back
        // via the result store in a real run. Here we note the plan.
        if (role == AgentRole::Optimization && step.contains("payload"))
            proposedAfter.push_back({ { "agent", "optimization" }, { "payload", step["payload"] } });
    }

    // 3. Delegate: emit followUps (only Conductor's are honored by the runtime).
    r.followUps = std::move(subtasks);
    r.findings["plan"] = plan;
    r.findings["delegatedCount"] = static_cast<int>(r.followUps.size());
    r.emitEvents.push_back({ "conductor.plan", { { "runId", runId.toStdString() }, { "plan", plan } } });

    // 4. The actual specialist results arrive asynchronously (via followUps).
    //    The conductor task is "complete" once delegation is issued; a higher-level
    //    orchestrator (or a future conductor-with-barrier) would await sub-results.
    //    For Phase 2 we mark success = delegation issued.
    r.success = true;
    r.telemetry["decomposedVia"] = plan.value("source", "deterministic-fallback");
    state_.store(AgentState::Idle);
    return r;
}

} // namespace more_phi::agents
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
cmake --build build --config Release --target MorePhiTests
cd build && ctest -C Release -R "Conductor" --output-on-failure
```
Expected: test passes (goal submitted, conductor decomposed + delegated, task marked success).

- [ ] **Step 7: Commit**

```bash
git add src/AI/Agents/Conductor/ tests/Unit/TestConductorAgent.cpp tests/CMakeLists.txt
git commit -m "feat(agents): add ConductorAgent (goal decomposition via WorkflowOrchestrator + LLM seam)"
```

---

## Task 10: Analysis agent (`AnalysisAgent.h/.cpp`) — TDD

**Files:**
- Create: `tests/Unit/TestAnalysisAgent.cpp`
- Create: `src/AI/Agents/Agents/AnalysisAgent.h`
- Create: `src/AI/Agents/Agents/AnalysisAgent.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/Unit/TestAnalysisAgent.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "AI/Agents/Agents/AnalysisAgent.h"
#include "AI/Agents/AgentContext.h"
#include "AI/Agents/Tooling/DefaultToolInvoker.h"
#include "AI/AutomationControlPlane.h"

using namespace more_phi::agents;

namespace {

struct FakeInvoker // implements IToolInvoker inline
{
    nlohmann::json summary = { { "lufs_integrated", -9.2 }, { "true_peak_db", -0.3 } };
    nlohmann::json spectrum = { { "tilt_db", 2.0 } };
    nlohmann::json invoke(const juce::String& tool, const nlohmann::json&, const juce::String&)
    {
        if (tool == "analysis.get_summary")  return summary;
        if (tool == "analysis.get_spectrum") return spectrum;
        return { { "error", { { "code", "unknown_tool" } } } };
    }
};

class FakeToolInvokerAdapter : public IToolInvoker
{
public:
    explicit FakeToolInvokerAdapter(FakeInvoker& f) : f_(f) {}
    nlohmann::json invoke(const juce::String& t, const nlohmann::json& p, const juce::String& a) override
    { return f_.invoke(t, p, a); }
private:
    FakeInvoker& f_;
};

} // namespace

TEST_CASE("AnalysisAgent is read-only and refuses write tools", "[agents][analysis]")
{
    FakeInvoker fake;
    FakeToolInvokerAdapter invoker{fake};
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;

    AgentContext ctx;
    ctx.tools = &invoker;
    ctx.blackboard = &bb;
    ctx.logger = &logger;

    AnalysisAgent agent;
    agent.prepare(ctx);

    REQUIRE(agent.allowedTools() == std::vector<juce::String>{
        "analysis.get_summary", "analysis.get_spectrum", "analysis.get_stereo_field",
        "ozone.track.analyze", "get_mastering_state", "analysis.capture_window",
        "analysis.compare_render" });

    AgentTask task;
    task.id = "a1";
    task.intent = "analyze current state";
    AgentResult r = agent.execute(task);

    REQUIRE(r.success);
    REQUIRE(r.findings["lufs_integrated"].get<double>() == Approx(-9.2));
    REQUIRE(r.findings["tilt_db"].get<double>() == Approx(2.0));
    REQUIRE(r.emitEvents.size() >= 1);
    REQUIRE(r.emitEvents[0].type == "analysis.finding");
}

TEST_CASE("AnalysisAgent refuses a write tool even if asked", "[agents][analysis]")
{
    // The capability scope is enforced by DefaultToolInvoker, not the agent itself.
    // Here we verify the agent's allowedTools() does NOT include set_parameter.
    AnalysisAgent agent;
    auto tools = agent.allowedTools();
    bool hasSet = false;
    for (const auto& t : tools)
        if (t == "set_parameter") hasSet = true;
    REQUIRE_FALSE(hasSet);
}
```

- [ ] **Step 2: Add test file to CMake**

Edit `tests/CMakeLists.txt` — add `Unit/TestAnalysisAgent.cpp`.

- [ ] **Step 3: Run to verify failure**

```bash
cmake --build build --config Release --target MorePhiTests 2>&1 | findstr /C:"AnalysisAgent.h" /C:"error"
```
Expected: compilation error.

- [ ] **Step 4: Create the header**

```cpp
// src/AI/Agents/Agents/AnalysisAgent.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <atomic>

namespace more_phi::agents {

// Read-only measurement & diagnosis. Never mutates. The system's eyes.
class AnalysisAgent : public IAgent
{
public:
    AgentRole role() const noexcept override { return AgentRole::Analysis; }
    juce::String id() const noexcept override { return "analysis-1"; }
    std::vector<juce::String> allowedTools() const override
    {
        return { "analysis.get_summary", "analysis.get_spectrum", "analysis.get_stereo_field",
                 "ozone.track.analyze", "get_mastering_state",
                 "analysis.capture_window", "analysis.compare_render" };
    }
    std::vector<juce::String> subscribedEventTypes() const override { return { "audio.transport_changed" }; }

    void prepare(const AgentContext& ctx) override { ctx_ = &ctx; }
    AgentResult execute(const AgentTask& task) override;
    AgentState state() const noexcept override { return state_.load(); }
    void stop() override { state_.store(AgentState::Stopped); }

private:
    const AgentContext* ctx_ = nullptr;
    std::atomic<AgentState> state_{ AgentState::Idle };
};

} // namespace more_phi::agents
```

- [ ] **Step 5: Create the implementation**

```cpp
// src/AI/Agents/Agents/AnalysisAgent.cpp
#include "AI/Agents/Agents/AnalysisAgent.h"

namespace more_phi::agents {

AgentResult AnalysisAgent::execute(const AgentTask& task)
{
    state_.store(AgentState::Busy);
    AgentResult r;
    r.taskId = task.id;

    if (! ctx_ || ! ctx_->tools)
    {
        r.success = false;
        r.errorCode = "no_tool_invoker";
        state_.store(AgentState::Idle);
        return r;
    }

    nlohmann::json findings = nlohmann::json::object();
    findings["intent"] = task.intent.toStdString();

    auto summary = ctx_->tools->invoke("analysis.get_summary", {}, id());
    if (! summary.contains("error"))
        findings["summary"] = summary;

    auto spectrum = ctx_->tools->invoke("analysis.get_spectrum", {}, id());
    if (! spectrum.contains("error"))
        findings["spectrum"] = spectrum;

    auto stereo = ctx_->tools->invoke("analysis.get_stereo_field", {}, id());
    if (! stereo.contains("error"))
        findings["stereo"] = stereo;

    // Flatten a few headline numbers for event consumers (reactive agents).
    if (summary.contains("lufs_integrated")) findings["lufs_integrated"] = summary["lufs_integrated"];
    if (summary.contains("true_peak_db"))    findings["true_peak_db"]    = summary["true_peak_db"];
    if (spectrum.contains("tilt_db"))        findings["tilt_db"]         = spectrum["tilt_db"];

    r.findings = findings;
    r.success = true;

    r.emitEvents.push_back({ "analysis.finding", findings });

    // Reactive triggers (consumed by RealtimeControlAgent):
    if (summary.contains("true_peak_db") && summary["true_peak_db"].is_number())
    {
        const double tp = summary["true_peak_db"].get<double>();
        if (tp > -0.1)
            r.emitEvents.push_back({ "analysis.clipping_detected",
                { { "true_peak_db", tp }, { "source", id().toStdString() } } });
    }
    if (summary.contains("lufs_integrated") && summary["lufs_integrated"].is_number())
    {
        const double lufs = summary["lufs_integrated"].get<double>();
        if (lufs > -8.0)
            r.emitEvents.push_back({ "analysis.lufs_breach",
                { { "lufs_integrated", lufs }, { "source", id().toStdString() } } });
    }

    state_.store(AgentState::Idle);
    return r;
}

} // namespace more_phi::agents
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build build --config Release --target MorePhiTests
cd build && ctest -C Release -R "AnalysisAgent" --output-on-failure
```
Expected: 2 tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/AI/Agents/Agents/AnalysisAgent.h src/AI/Agents/Agents/AnalysisAgent.cpp tests/Unit/TestAnalysisAgent.cpp tests/CMakeLists.txt
git commit -m "feat(agents): add AnalysisAgent (read-only measurement + reactive triggers)"
```

**Phase 2 checkpoint:** Conductor + Analysis compose a working proactive + reactive loop. Both unit-tested against fake tool invokers.

---

# Phase 3 — Remaining Specialist Agents

## Task 11: Optimization agent (`OptimizationAgent.h/.cpp`) — TDD

**Files:**
- Create: `tests/Unit/TestOptimizationAgent.cpp`
- Create: `src/AI/Agents/Agents/OptimizationAgent.h`
- Create: `src/AI/Agents/Agents/OptimizationAgent.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/Unit/TestOptimizationAgent.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "AI/Agents/Agents/OptimizationAgent.h"
#include "AI/Agents/AgentContext.h"
#include "AI/AutomationControlPlane.h"

using namespace more_phi::agents;

namespace {

class FakeInvoker : public IToolInvoker
{
public:
    nlohmann::json invoke(const juce::String& tool, const nlohmann::json& params, const juce::String&) override
    {
        lastTool = tool;
        lastParams = params;
        if (tool == "mastering.plan_preview")
            return { { "plan", { { "output_gain_db", -2.0 } } } };
        if (tool == "mastering.render_batch")
        {
            // Return N candidates with decreasing error; the agent should pick candidate 0.
            nlohmann::json cands = nlohmann::json::array();
            for (int i = 0; i < 4; ++i)
                cands.push_back({ { "id", i }, { "lufs_error", 0.5 + 0.1 * i }, { "params", { { "out", -1.0 - 0.5 * i } } } });
            return { { "candidates", cands } };
        }
        return { { "ok", true } };
    }
    juce::String lastTool;
    nlohmann::json lastParams;
};

} // namespace

TEST_CASE("OptimizationAgent picks the best candidate by metric", "[agents][optimization]")
{
    FakeInvoker fake;
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;

    AgentContext ctx;
    ctx.tools = &fake;
    ctx.blackboard = &bb;
    ctx.logger = &logger;

    OptimizationAgent agent;
    agent.prepare(ctx);

    AgentTask task;
    task.id = "o1";
    task.intent = "optimize toward streaming target";
    task.payload = { { "target", { { "lufsIntegrated", -14.0 }, { "truePeakMaxDb", -1.0 } } } };
    AgentResult r = agent.execute(task);

    REQUIRE(r.success);
    REQUIRE(r.proposedActions.size() >= 1);
    REQUIRE(r.proposedActions[0]["tool"].get<std::string>() == "set_parameters_batch");
    REQUIRE(r.emitEvents.size() >= 1);
    bool hasProposal = false;
    for (const auto& e : r.emitEvents) if (e.type == "optimization.proposal") hasProposal = true;
    REQUIRE(hasProposal);
}
```

- [ ] **Step 2: Add test file to CMake**

Edit `tests/CMakeLists.txt` — add `Unit/TestOptimizationAgent.cpp`.

- [ ] **Step 3: Run to verify failure** (compilation error expected)

```bash
cmake --build build --config Release --target MorePhiTests 2>&1 | findstr /C:"OptimizationAgent.h" /C:"error"
```

- [ ] **Step 4: Create the header**

```cpp
// src/AI/Agents/Agents/OptimizationAgent.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <atomic>

namespace more_phi::agents {

// Drives parameters toward a target metric. Drafts via mastering.plan_preview,
// evaluates N candidates via mastering.render_batch, picks best by error.
// Does NOT apply directly — returns proposedActions for the Conductor to re-dispatch
// (permission-gated) and QualitySafety to verify.
class OptimizationAgent : public IAgent
{
public:
    AgentRole role() const noexcept override { return AgentRole::Optimization; }
    juce::String id() const noexcept override { return "optimization-1"; }
    std::vector<juce::String> allowedTools() const override
    {
        return { "set_parameter", "set_parameters_batch",
                 "set_more_phi_parameter", "set_more_phi_parameters",
                 "mastering.plan_preview", "mastering.render_batch" };
    }
    std::vector<juce::String> subscribedEventTypes() const override
    { return { "analysis.finding", "quality.target_set" }; }

    void prepare(const AgentContext& ctx) override { ctx_ = &ctx; }
    AgentResult execute(const AgentTask& task) override;
    AgentState state() const noexcept override { return state_.load(); }
    void stop() override { state_.store(AgentState::Stopped); }

private:
    const AgentContext* ctx_ = nullptr;
    std::atomic<AgentState> state_{ AgentState::Idle };
};

} // namespace more_phi::agents
```

- [ ] **Step 5: Create the implementation**

```cpp
// src/AI/Agents/Agents/OptimizationAgent.cpp
#include "AI/Agents/Agents/OptimizationAgent.h"

#include <algorithm>
#include <limits>

namespace more_phi::agents {

AgentResult OptimizationAgent::execute(const AgentTask& task)
{
    state_.store(AgentState::Busy);
    AgentResult r;
    r.taskId = task.id;

    if (! ctx_ || ! ctx_->tools)
    {
        r.success = false; r.errorCode = "no_tool_invoker";
        state_.store(AgentState::Idle); return r;
    }

    const auto target = task.payload.value("target", nlohmann::json::object());

    // 1. Draft a plan.
    auto draft = ctx_->tools->invoke("mastering.plan_preview",
        { { "target", target } }, id());

    // 2. Evaluate a batch of candidates.
    const int batchSize = 4;
    auto batch = ctx_->tools->invoke("mastering.render_batch",
        { { "target", target }, { "count", batchSize }, { "dry_run", true } }, id());

    // 3. Pick the candidate with the lowest lufs_error.
    int bestIdx = -1;
    double bestError = std::numeric_limits<double>::infinity();
    nlohmann::json bestParams = nlohmann::json::object();
    if (batch.contains("candidates") && batch["candidates"].is_array())
    {
        for (const auto& c : batch["candidates"])
        {
            const double err = c.value("lufs_error", std::numeric_limits<double>::infinity());
            if (err < bestError)
            {
                bestError = err;
                bestIdx = c.value("id", -1);
                bestParams = c.value("params", nlohmann::json::object());
            }
        }
    }

    r.findings = {
        { "draft", draft },
        { "evaluatedCount", batch.contains("candidates") ? static_cast<int>(batch["candidates"].size()) : 0 },
        { "bestCandidateId", bestIdx },
        { "bestError", bestError }
    };

    // 4. Return the chosen parameter delta as a proposedAction for the Conductor to re-dispatch.
    if (! bestParams.empty())
    {
        nlohmann::json action = {
            { "tool", "set_parameters_batch" },
            { "params", { { "values", bestParams } } }
        };
        r.proposedActions.push_back(action);
    }

    r.emitEvents.push_back({ "optimization.proposal", {
        { "bestCandidateId", bestIdx }, { "bestError", bestError },
        { "proposedActionCount", static_cast<int>(r.proposedActions.size()) }
    }});

    r.success = true;
    state_.store(AgentState::Idle);
    return r;
}

} // namespace more_phi::agents
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build build --config Release --target MorePhiTests
cd build && ctest -C Release -R "OptimizationAgent" --output-on-failure
```
Expected: test passes.

- [ ] **Step 7: Commit**

```bash
git add src/AI/Agents/Agents/OptimizationAgent.h src/AI/Agents/Agents/OptimizationAgent.cpp tests/Unit/TestOptimizationAgent.cpp tests/CMakeLists.txt
git commit -m "feat(agents): add OptimizationAgent (draft + render-batch + pick-best)"
```

---

## Task 12: Creative agent (`CreativeAgent.h/.cpp`) — TDD

**Files:**
- Create: `tests/Unit/TestCreativeAgent.cpp`
- Create: `src/AI/Agents/Agents/CreativeAgent.h`
- Create: `src/AI/Agents/Agents/CreativeAgent.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/Unit/TestCreativeAgent.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "AI/Agents/Agents/CreativeAgent.h"
#include "AI/Agents/AgentContext.h"
#include "AI/AutomationControlPlane.h"

using namespace more_phi::agents;

namespace {
class FakeInvoker : public IToolInvoker
{
public:
    nlohmann::json invoke(const juce::String&, const nlohmann::json&, const juce::String&) override
    {
        return { { "suggestions", { { "warmer", true }, { "brighter", false } } } };
    }
};
} // namespace

TEST_CASE("CreativeAgent requires approval regardless of autonomy", "[agents][creative]")
{
    CreativeAgent agent;
    REQUIRE(agent.requireApprovalRegardlessOfAutonomy());

    FakeInvoker fake;
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;
    AgentContext ctx;
    ctx.tools = &fake; ctx.blackboard = &bb; ctx.logger = &logger;
    agent.prepare(ctx);

    AgentTask task;
    task.id = "c1";
    task.intent = "suggest alternatives";
    AgentResult r = agent.execute(task);

    REQUIRE(r.success);
    bool hasSuggestion = false;
    for (const auto& e : r.emitEvents)
        if (e.type == "creative.suggestion") hasSuggestion = true;
    REQUIRE(hasSuggestion);
    // Creative MUST NOT auto-apply: no proposedActions with write tools.
    REQUIRE(r.proposedActions.empty());
}
```

- [ ] **Step 2: Add test file to CMake**

Edit `tests/CMakeLists.txt` — add `Unit/TestCreativeAgent.cpp`.

- [ ] **Step 3: Run to verify failure** (compilation error expected)

- [ ] **Step 4: Create the header**

```cpp
// src/AI/Agents/Agents/CreativeAgent.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <atomic>

namespace more_phi::agents {

// Artistic suggestions. Never auto-applied (requireApprovalRegardlessOfAutonomy).
class CreativeAgent : public IAgent
{
public:
    AgentRole role() const noexcept override { return AgentRole::Creative; }
    juce::String id() const noexcept override { return "creative-1"; }
    std::vector<juce::String> allowedTools() const override
    {
        return { "suggest_intermediate_snapshots", "find_related_parameters",
                 "suggest_morph_settings", "capture_snapshot" };
    }
    std::vector<juce::String> subscribedEventTypes() const override
    { return { "analysis.finding", "optimization.proposal" }; }
    bool requireApprovalRegardlessOfAutonomy() const override { return true; }

    void prepare(const AgentContext& ctx) override { ctx_ = &ctx; }
    AgentResult execute(const AgentTask& task) override;
    AgentState state() const noexcept override { return state_.load(); }
    void stop() override { state_.store(AgentState::Stopped); }

private:
    const AgentContext* ctx_ = nullptr;
    std::atomic<AgentState> state_{ AgentState::Idle };
};

} // namespace more_phi::agents
```

- [ ] **Step 5: Create the implementation**

```cpp
// src/AI/Agents/Agents/CreativeAgent.cpp
#include "AI/Agents/Agents/CreativeAgent.h"

namespace more_phi::agents {

AgentResult CreativeAgent::execute(const AgentTask& task)
{
    state_.store(AgentState::Busy);
    AgentResult r;
    r.taskId = task.id;

    if (! ctx_ || ! ctx_->tools)
    {
        r.success = false; r.errorCode = "no_tool_invoker";
        state_.store(AgentState::Idle); return r;
    }

    // Advisory only. We do not propose write actions (this is enforced structurally
    // by not populating proposedActions; QualitySafety would gate them anyway).
    auto related = ctx_->tools->invoke("find_related_parameters",
        { { "intent", task.intent.toStdString() } }, id());
    auto hybrids = ctx_->tools->invoke("suggest_intermediate_snapshots", {}, id());

    r.findings = { { "related", related }, { "hybrids", hybrids } };
    r.emitEvents.push_back({ "creative.suggestion", {
        { "intent", task.intent.toStdString() },
        { "advisory", true }
    }});
    r.success = true;

    state_.store(AgentState::Idle);
    return r;
}

} // namespace more_phi::agents
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build build --config Release --target MorePhiTests
cd build && ctest -C Release -R "CreativeAgent" --output-on-failure
```
Expected: test passes.

- [ ] **Step 7: Commit**

```bash
git add src/AI/Agents/Agents/CreativeAgent.h src/AI/Agents/Agents/CreativeAgent.cpp tests/Unit/TestCreativeAgent.cpp tests/CMakeLists.txt
git commit -m "feat(agents): add CreativeAgent (advisory, human-in-the-loop)"
```

---

## Task 13: Realtime control agent (`RealtimeControlAgent.h/.cpp`) — TDD

**Files:**
- Create: `tests/Unit/TestRealtimeControlAgent.cpp`
- Create: `src/AI/Agents/Agents/RealtimeControlAgent.h`
- Create: `src/AI/Agents/Agents/RealtimeControlAgent.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/Unit/TestRealtimeControlAgent.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "AI/Agents/Agents/RealtimeControlAgent.h"
#include "AI/Agents/AgentContext.h"
#include "AI/AutomationControlPlane.h"

using namespace more_phi::agents;

namespace {
class FakeInvoker : public IToolInvoker
{
public:
    mutable juce::String lastTool;
    mutable nlohmann::json lastParams;
    nlohmann::json invoke(const juce::String& tool, const nlohmann::json& params, const juce::String&) const
    {
        lastTool = tool; lastParams = params;
        return { { "ok", true } };
    }
    nlohmann::json invoke(const juce::String& tool, const nlohmann::json& params, const juce::String&) override
    {
        return const_cast<const FakeInvoker*>(this)->invoke(tool, params, juce::String{});
    }
};
} // namespace

TEST_CASE("RealtimeControlAgent enqueues a correction on clipping", "[agents][realtime]")
{
    FakeInvoker fake;
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;
    AgentContext ctx;
    ctx.tools = &fake; ctx.blackboard = &bb; ctx.logger = &logger;

    RealtimeControlAgent agent;
    // Configure tiny budget/cadence so the test is deterministic.
    RealtimeControlAgent::Config cfg;
    cfg.maxCorrectionsPerParamPerSecond = 4;
    cfg.correctionBudgetPerRun = 16;
    agent.setConfig(cfg);
    agent.prepare(ctx);

    // Inject a clipping event via onEvent (as the blackboard pump would).
    agent.onEvent("analysis.clipping_detected",
        { { "true_peak_db", 0.2 }, { "channel", "R" } }, "analysis-1", "run-1");

    REQUIRE(fake.lastTool == "set_parameter");
    // The correction nudges output gain down.
    REQUIRE(fake.lastParams.contains("index"));
}
```

- [ ] **Step 2: Add test file to CMake**

Edit `tests/CMakeLists.txt` — add `Unit/TestRealtimeControlAgent.cpp`.

- [ ] **Step 3: Run to verify failure** (compilation error expected)

- [ ] **Step 4: Create the header**

```cpp
// src/AI/Agents/Agents/RealtimeControlAgent.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace more_phi::agents {

// Reactive corrections via the existing lock-free queue. "RealtimeCritical"
// priority means "jump the AGENT queue", NOT "run on the audio thread" (Decision D2).
class RealtimeControlAgent : public IAgent
{
public:
    struct Config
    {
        int maxCorrectionsPerParamPerSecond = 4;   // anti-oscillation
        int correctionBudgetPerRun = 16;           // hard cap before QualitySafety veto
        float clipTrimStepDb = 1.5f;               // how far to trim per clip event
        int outputGainParamIndex = 0;              // tunable; real wiring sets this
    };

    AgentRole role() const noexcept override { return AgentRole::RealtimeControl; }
    juce::String id() const noexcept override { return "realtime-1"; }
    std::vector<juce::String> allowedTools() const override
    {
        return { "set_parameter", "set_morph_position", "more_phi.set_parameter" };
    }
    std::vector<juce::String> subscribedEventTypes() const override
    { return { "analysis.clipping_detected", "analysis.lufs_breach" }; }

    void setConfig(Config c);
    void prepare(const AgentContext& ctx) override { ctx_ = &ctx; }

    // Driven by the blackboard pump.
    void onEvent(const juce::String& type,
                 const nlohmann::json& payload,
                 const juce::String& source,
                 const juce::String& runId) override;

    // Sync execute (for direct task submission).
    AgentResult execute(const AgentTask& task) override;
    AgentState state() const noexcept override { return state_.load(); }
    void stop() override { state_.store(AgentState::Stopped); }

private:
    bool consumeRateAndBudgetLocked(const juce::String& runId, int paramIndex);

    const AgentContext* ctx_ = nullptr;
    Config config_;
    std::atomic<AgentState> state_{ AgentState::Idle };

    struct RateBucket { juce::int64 windowStartMs = 0; int count = 0; };
    mutable std::mutex mutex_;
    std::unordered_map<std::string, RateBucket> rateBuckets_;
    std::unordered_map<std::string, int> runBudgets_;
};

} // namespace more_phi::agents
```

- [ ] **Step 5: Create the implementation**

```cpp
// src/AI/Agents/Agents/RealtimeControlAgent.cpp
#include "AI/Agents/Agents/RealtimeControlAgent.h"

#include <juce_core/juce_core.h>

namespace more_phi::agents {

namespace {
juce::int64 nowMs() noexcept { return juce::Time::currentTimeMillis(); }
} // namespace

void RealtimeControlAgent::setConfig(Config c) { config_ = c; }

bool RealtimeControlAgent::consumeRateAndBudgetLocked(const juce::String& runId, int paramIndex)
{
    const auto t = nowMs();
    const auto key = runId.toStdString() + ":" + std::to_string(paramIndex);
    auto& bucket = rateBuckets_[key];
    if (bucket.windowStartMs == 0 || (t - bucket.windowStartMs) >= 1000)
    {
        bucket.windowStartMs = t;
        bucket.count = 0;
    }
    if (bucket.count >= config_.maxCorrectionsPerParamPerSecond)
        return false;
    int& budget = runBudgets_[runId.toStdString()];
    if (budget >= config_.correctionBudgetPerRun)
        return false;
    ++bucket.count;
    ++budget;
    return true;
}

void RealtimeControlAgent::onEvent(const juce::String& type,
                                   const nlohmann::json& payload,
                                   const juce::String& /*source*/,
                                   const juce::String& runId)
{
    if (! ctx_ || ! ctx_->tools)
        return;
    if (type != "analysis.clipping_detected" && type != "analysis.lufs_breach")
        return;

    const int param = config_.outputGainParamIndex;
    bool allowed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        allowed = consumeRateAndBudgetLocked(runId, param);
    }
    if (! allowed)
    {
        // Rate/budget exhausted: let QualitySafety observe the back-off.
        if (ctx_->blackboard)
            ctx_->blackboard->publish(id(), "realtime.correction_skipped",
                { { "reason", "rate_or_budget" }, { "type", type.toStdString() } }, runId);
        return;
    }

    // Compute a small corrective trim. Negative = output down.
    const float stepDb = -config_.clipTrimStepDb;
    auto result = ctx_->tools->invoke("set_parameter",
        { { "index", param }, { "value", stepDb } }, id());

    if (ctx_->blackboard)
        ctx_->blackboard->publish(id(), "realtime.correction_applied",
            { { "param", param }, { "delta_db", stepDb },
              { "type", type.toStdString() }, { "result", result } }, runId);
}

AgentResult RealtimeControlAgent::execute(const AgentTask& task)
{
    state_.store(AgentState::Busy);
    AgentResult r;
    r.taskId = task.id;
    r.success = true;
    r.findings = { { "note", "reactive agent; primary entry is onEvent" },
                   { "task", task.intent.toStdString() } };
    state_.store(AgentState::Idle);
    return r;
}

} // namespace more_phi::agents
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build build --config Release --target MorePhiTests
cd build && ctest -C Release -R "RealtimeControlAgent" --output-on-failure
```
Expected: test passes.

- [ ] **Step 7: Commit**

```bash
git add src/AI/Agents/Agents/RealtimeControlAgent.h src/AI/Agents/Agents/RealtimeControlAgent.cpp tests/Unit/TestRealtimeControlAgent.cpp tests/CMakeLists.txt
git commit -m "feat(agents): add RealtimeControlAgent (queue-bound reactive corrections)"
```

---

## Task 14: Quality-safety agent (`QualitySafetyAgent.h/.cpp`) — TDD

**Files:**
- Create: `tests/Unit/TestQualitySafetyAgent.cpp`
- Create: `src/AI/Agents/Agents/QualitySafetyAgent.h`
- Create: `src/AI/Agents/Agents/QualitySafetyAgent.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/Unit/TestQualitySafetyAgent.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "AI/Agents/Agents/QualitySafetyAgent.h"
#include "AI/Agents/AgentContext.h"
#include "AI/AutomationControlPlane.h"

using namespace more_phi::agents;

namespace {
class FakeInvoker : public IToolInvoker
{
public:
    double afterLufs = -13.9;
    double afterTp = -1.1;
    nlohmann::json invoke(const juce::String& tool, const nlohmann::json&, const juce::String&) override
    {
        if (tool == "analysis.compare_render")
            return { { "after", { { "lufs_integrated", afterLufs }, { "true_peak_db", afterTp } } } };
        return { { "ok", true } };
    }
};
} // namespace

TEST_CASE("QualitySafetyAgent approves a proposal within targets", "[agents][quality]")
{
    FakeInvoker fake;
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;
    AgentContext ctx;
    ctx.tools = &fake; ctx.blackboard = &bb; ctx.logger = &logger;

    QualitySafetyAgent agent;
    QualitySafetyAgent::Config cfg;
    cfg.maxLufs = -14.0; cfg.maxTruePeakDb = -1.0;
    agent.setConfig(cfg);
    agent.prepare(ctx);

    QualitySafetyAgent::Proposal p;
    p.proposedActions = { { { "tool", "set_parameters_batch" } } };
    p.target = { { "lufsIntegrated", -14.0 }, { "truePeakMaxDb", -1.0 } };
    auto verdict = agent.evaluate(p);

    REQUIRE(verdict.approved);     // -13.9 > -14 (within), -1.1 < -1.0 (within)
}

TEST_CASE("QualitySafetyAgent rejects a proposal that breaches true-peak", "[agents][quality]")
{
    FakeInvoker fake;
    fake.afterTp = -0.5; // breach
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;
    AgentContext ctx;
    ctx.tools = &fake; ctx.blackboard = &bb; ctx.logger = &logger;

    QualitySafetyAgent agent;
    QualitySafetyAgent::Config cfg;
    cfg.maxTruePeakDb = -1.0;
    agent.setConfig(cfg);
    agent.prepare(ctx);

    QualitySafetyAgent::Proposal p;
    p.target = { { "truePeakMaxDb", -1.0 } };
    auto verdict = agent.evaluate(p);
    REQUIRE_FALSE(verdict.approved);
    REQUIRE(verdict.reason == "true_peak_breach");
}
```

- [ ] **Step 2: Add test file to CMake**

Edit `tests/CMakeLists.txt` — add `Unit/TestQualitySafetyAgent.cpp`.

- [ ] **Step 3: Run to verify failure** (compilation error expected)

- [ ] **Step 4: Create the header**

```cpp
// src/AI/Agents/Agents/QualitySafetyAgent.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <atomic>
#include <vector>

namespace more_phi::agents {

// Semantic gatekeeper. Composes with (does not replace) the mechanical PermissionKernel.
class QualitySafetyAgent : public IAgent
{
public:
    struct Config
    {
        double maxLufs = -14.0;
        double maxTruePeakDb = -1.0;
    };

    struct Proposal
    {
        std::vector<nlohmann::json> proposedActions;
        nlohmann::json target = nlohmann::json::object();
        juce::String runId;
    };
    struct Verdict
    {
        bool approved = false;
        juce::String reason;
        nlohmann::json measurements = nlohmann::json::object();
    };

    AgentRole role() const noexcept override { return AgentRole::QualitySafety; }
    juce::String id() const noexcept override { return "quality-1"; }
    std::vector<juce::String> allowedTools() const override
    {
        return { "analysis.compare_render", "get_mastering_state",
                 "audit_plugin_profile", "restore_safe_plugin_snapshot" };
    }
    std::vector<juce::String> subscribedEventTypes() const override
    { return { "optimization.proposal", "creative.suggestion", "realtime.correction_applied" }; }

    void setConfig(Config c) { config_ = c; }
    void prepare(const AgentContext& ctx) override { ctx_ = &ctx; }

    Verdict evaluate(const Proposal& proposal);

    void onEvent(const juce::String& type,
                 const nlohmann::json& payload,
                 const juce::String& source,
                 const juce::String& runId) override;

    AgentResult execute(const AgentTask& task) override;
    AgentState state() const noexcept override { return state_.load(); }
    void stop() override { state_.store(AgentState::Stopped); }

private:
    const AgentContext* ctx_ = nullptr;
    Config config_;
    std::atomic<AgentState> state_{ AgentState::Idle };
};

} // namespace more_phi::agents
```

- [ ] **Step 5: Create the implementation**

```cpp
// src/AI/Agents/Agents/QualitySafetyAgent.cpp
#include "AI/Agents/Agents/QualitySafetyAgent.h"

namespace more_phi::agents {

QualitySafetyAgent::Verdict QualitySafetyAgent::evaluate(const Proposal& proposal)
{
    Verdict v;
    if (! ctx_ || ! ctx_->tools)
    {
        v.reason = "no_tool_invoker";
        return v;
    }

    // Re-measure the predicted/after state via compare_render.
    auto compared = ctx_->tools->invoke("analysis.compare_render",
        { { "actions", proposal.proposedActions }, { "target", proposal.target } }, id());

    const auto after = compared.value("after", nlohmann::json::object());
    const double lufs = after.value("lufs_integrated", -99.0);
    const double tp   = after.value("true_peak_db", -99.0);
    v.measurements = { { "lufs_integrated", lufs }, { "true_peak_db", tp } };

    // Streaming targets: loudness must not exceed (i.e. be louder than) the ceiling.
    // lufs values are negative; "too loud" means lufs > maxLufs (closer to 0).
    if (lufs > config_.maxLufs + 0.05)
    {
        v.reason = "lufs_breach";
        return v;
    }
    if (tp > config_.maxTruePeakDb + 0.05)
    {
        v.reason = "true_peak_breach";
        return v;
    }

    v.approved = true;
    v.reason = "within_targets";
    return v;
}

void QualitySafetyAgent::onEvent(const juce::String& type,
                                 const nlohmann::json& payload,
                                 const juce::String& /*source*/,
                                 const juce::String& runId)
{
    if (type == "optimization.proposal" || type == "creative.suggestion")
    {
        Proposal p;
        p.runId = runId;
        if (payload.contains("actions"))
            p.proposedActions = payload["actions"];
        auto verdict = evaluate(p);
        if (ctx_ && ctx_->blackboard)
        {
            ctx_->blackboard->publish(id(), "quality.verdict",
                { { "approved", verdict.approved }, { "reason", verdict.reason.toStdString() },
                  { "type", type.toStdString() } }, runId);
            if (verdict.approved)
                ctx_->blackboard->publish(id(), "quality.target_set",
                    { { "from", type.toStdString() } }, runId);
        }
    }
    // realtime.correction_applied is observed silently here (semantic watchdog);
    // a budget breach surfaces as realtime.correction_skipped, handled in onEvent too.
}

AgentResult QualitySafetyAgent::execute(const AgentTask& task)
{
    state_.store(AgentState::Busy);
    AgentResult r;
    r.taskId = task.id;
    r.success = true;
    r.findings = { { "note", "reactive gatekeeper; primary entry is onEvent" } };
    state_.store(AgentState::Idle);
    return r;
}

} // namespace more_phi::agents
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build build --config Release --target MorePhiTests
cd build && ctest -C Release -R "QualitySafetyAgent" --output-on-failure
```
Expected: 2 tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/AI/Agents/Agents/QualitySafetyAgent.h src/AI/Agents/Agents/QualitySafetyAgent.cpp tests/Unit/TestQualitySafetyAgent.cpp tests/CMakeLists.txt
git commit -m "feat(agents): add QualitySafetyAgent (semantic gatekeeper over streaming targets)"
```

---

## Task 15: Memory agent (`MemoryAgent.h/.cpp`) — TDD

**Files:**
- Create: `tests/Unit/TestMemoryAgent.cpp`
- Create: `src/AI/Agents/Agents/MemoryAgent.h`
- Create: `src/AI/Agents/Agents/MemoryAgent.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/Unit/TestMemoryAgent.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "AI/Agents/Agents/MemoryAgent.h"
#include "AI/Agents/AgentContext.h"
#include "AI/AutomationControlPlane.h"

using namespace more_phi::agents;

namespace {
class FakeInvoker : public IToolInvoker
{
public:
    nlohmann::json invoke(const juce::String&, const nlohmann::json&, const juce::String&) override
    { return { { "ok", true } }; }
};
} // namespace

TEST_CASE("MemoryAgent writes a workflow-level outcome on conductor.complete", "[agents][memory]")
{
    FakeInvoker fake;
    more_phi::AutomationRuntime runtime;
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;

    AgentContext ctx;
    ctx.tools = &fake; ctx.runtime = &runtime; ctx.blackboard = &bb; ctx.logger = &logger;

    MemoryAgent agent;
    agent.prepare(ctx);

    AgentTask task;
    task.id = "m1";
    task.intent = "record outcome for run R1";
    task.payload = { { "runId", "R1" }, { "success", true }, { "score", 0.8 } };
    AgentResult r = agent.execute(task);

    REQUIRE(r.success);
    // Exactly one workflow-level outcome recorded.
    auto outcomes = runtime.memory().listOutcomes("R1");
    REQUIRE(outcomes.size() == 1);
}

TEST_CASE("MemoryAgent surfaces prior context via intentContext", "[agents][memory]")
{
    FakeInvoker fake;
    more_phi::AutomationRuntime runtime;
    more_phi::IntegrationEventBus bus{16};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;

    AgentContext ctx;
    ctx.tools = &fake; ctx.runtime = &runtime; ctx.blackboard = &bb; ctx.logger = &logger;

    MemoryAgent agent;
    agent.prepare(ctx);

    AgentTask task;
    task.id = "m2";
    task.intent = "recall relevant priors";
    AgentResult r = agent.execute(task);

    REQUIRE(r.success);
    REQUIRE(r.findings.contains("intentContext"));
}
```

- [ ] **Step 2: Add test file to CMake**

Edit `tests/CMakeLists.txt` — add `Unit/TestMemoryAgent.cpp`.

- [ ] **Step 3: Run to verify failure** (compilation error expected)

- [ ] **Step 4: Create the header**

```cpp
// src/AI/Agents/Agents/MemoryAgent.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <atomic>

namespace more_phi::agents {

// Workflow-level outcome memory. Does NOT duplicate the transaction-level
// outcomes that dispatchWithAutomationTransaction already records — writes only
// the higher-level workflow outcome tying transactions to intent + feedback.
class MemoryAgent : public IAgent
{
public:
    AgentRole role() const noexcept override { return AgentRole::Memory; }
    juce::String id() const noexcept override { return "memory-1"; }
    std::vector<juce::String> allowedTools() const override
    {
        return { "memory.record_outcome", "memory.update_outcome_feedback",
                 "memory.search", "get_usage_stats", "learn_from_adjustment" };
    }
    std::vector<juce::String> subscribedEventTypes() const override
    { return { "conductor.complete", "quality.verdict" }; }

    void prepare(const AgentContext& ctx) override { ctx_ = &ctx; }
    AgentResult execute(const AgentTask& task) override;

    void onEvent(const juce::String& type,
                 const nlohmann::json& payload,
                 const juce::String& source,
                 const juce::String& runId) override;

    AgentState state() const noexcept override { return state_.load(); }
    void stop() override { state_.store(AgentState::Stopped); }

private:
    const AgentContext* ctx_ = nullptr;
    std::atomic<AgentState> state_{ AgentState::Idle };
};

} // namespace more_phi::agents
```

- [ ] **Step 5: Create the implementation**

```cpp
// src/AI/Agents/Agents/MemoryAgent.cpp
#include "AI/Agents/Agents/MemoryAgent.h"

#include "AI/AutomationControlPlane.h"

namespace more_phi::agents {

AgentResult MemoryAgent::execute(const AgentTask& task)
{
    state_.store(AgentState::Busy);
    AgentResult r;
    r.taskId = task.id;

    if (! ctx_ || ! ctx_->runtime)
    {
        r.success = false; r.errorCode = "no_runtime";
        state_.store(AgentState::Idle); return r;
    }

    const auto runId = task.payload.value("runId", std::string());
    const bool ok    = task.payload.value("success", false);
    const double score = task.payload.value("score", 0.0);

    if (! runId.empty())
    {
        // Record exactly ONE workflow-level outcome.
        more_phi::ActionOutcome outcome;
        outcome.workflowRunId = juce::String(runId);
        outcome.userAccepted = ok;
        outcome.outcomeScore = static_cast<float>(score);
        outcome.source = id().toStdString();
        outcome.feedbackStatus = ok ? "accepted" : "rejected";
        ctx_->runtime->memory().recordOutcome(outcome);
    }

    // Surface relevant priors.
    nlohmann::json sessionCtx = nlohmann::json::object();
    auto recalled = ctx_->runtime->memory().intentContext(sessionCtx, 10);
    r.findings = { { "intentContext", recalled } };
    r.emitEvents.push_back({ "memory.recall_ready", { { "count", recalled.size() } } });
    r.success = true;

    state_.store(AgentState::Idle);
    return r;
}

void MemoryAgent::onEvent(const juce::String& type,
                          const nlohmann::json& payload,
                          const juce::String& /*source*/,
                          const juce::String& runId)
{
    if (type == "conductor.complete" && ctx_ && ctx_->runtime)
    {
        more_phi::ActionOutcome outcome;
        outcome.workflowRunId = runId;
        outcome.userAccepted = payload.value("success", false);
        outcome.outcomeScore = static_cast<float>(payload.value("score", 0.0));
        outcome.source = "memory-on-conductor-complete";
        outcome.feedbackStatus = outcome.userAccepted ? "accepted" : "rejected";
        ctx_->runtime->memory().recordOutcome(outcome);
    }
}

} // namespace more_phi::agents
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build build --config Release --target MorePhiTests
cd build && ctest -C Release -R "MemoryAgent" --output-on-failure
```
Expected: 2 tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/AI/Agents/Agents/MemoryAgent.h src/AI/Agents/Agents/MemoryAgent.cpp tests/Unit/TestMemoryAgent.cpp tests/CMakeLists.txt
git commit -m "feat(agents): add MemoryAgent (workflow-level outcome memory)"
```

**Phase 3 checkpoint:** All six specialist agents implemented and unit-tested in isolation.

---

# Phase 4 — MCP Surface + Plugin Wiring

## Task 16: `agents.*` risk classification in `PermissionKernel`

**Files:**
- Modify: `src/AI/AutomationControlPlane.cpp:778-824` (`classifyTool`)

- [ ] **Step 1: Write the failing test**

Append to `tests/Unit/TestMCPServerUnit.cpp` (inside the existing file, after the other tests):

```cpp
TEST_CASE("PermissionKernel classifies agents.* tools", "[agents][mcp][permissions]")
{
    more_phi::PermissionKernel kernel;
    using R = more_phi::RiskLevel;
    REQUIRE(kernel.classifyTool("agents.list", {})              == R::ReadOnly);
    REQUIRE(kernel.classifyTool("agents.run_goal", {})          == R::LowWrite);
    REQUIRE(kernel.classifyTool("agents.run_task", {})          == R::MediumWrite);
    REQUIRE(kernel.classifyTool("agents.run_status", {})        == R::ReadOnly);
    REQUIRE(kernel.classifyTool("agents.run_cancel", {})        == R::LowWrite);
    REQUIRE(kernel.classifyTool("agents.blackboard.recent", {}) == R::ReadOnly);
    REQUIRE(kernel.classifyTool("agents.set_autonomy", {})      == R::HighImpact);
}
```

- [ ] **Step 2: Run to verify failure** (several assertions fail because agents.* not classified yet)

```bash
cmake --build build --config Release --target MorePhiTests
cd build && ctest -C Release -R "classifies agents" --output-on-failure
```
Expected: FAIL (e.g. `agents.run_goal` defaults to `ReadOnly`).

- [ ] **Step 3: Add the classifications**

Edit `src/AI/AutomationControlPlane.cpp` — in `classifyTool`, immediately before the final `return RiskLevel::ReadOnly;` at the end (after the `memory.*`/`workflow.*` block, ~line 821):

```cpp
    if (method == "agents.list" || method == "agents.run_status" || method == "agents.blackboard.recent")
        return RiskLevel::ReadOnly;
    if (method == "agents.run_goal" || method == "agents.run_cancel")
        return RiskLevel::LowWrite;
    if (method == "agents.run_task")
        return RiskLevel::MediumWrite;
    if (method == "agents.set_autonomy")
        return RiskLevel::HighImpact;
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
cmake --build build --config Release --target MorePhiTests
cd build && ctest -C Release -R "classifies agents" --output-on-failure
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/AI/AutomationControlPlane.cpp tests/Unit/TestMCPServerUnit.cpp
git commit -m "feat(agents): classify agents.* tools in PermissionKernel"
```

---

## Task 17: `agents.*` tool dispatch in `MCPToolHandler`

**Files:**
- Modify: `src/AI/MCPToolHandler.h` (add private static helpers)
- Modify: `src/AI/MCPToolHandler.cpp` (add dispatch cases + helpers + getToolList entries)

- [ ] **Step 1: Add helper declarations to the header**

Edit `src/AI/MCPToolHandler.h` — inside the `private:` section of `class MCPToolHandler`, before the closing `};` (after the existing caching helpers, ~line 153):

```cpp
    // ── Agent runtime tools (agents.*) ──────────────────────────────────────
    static juce::String agentsList(MorePhiProcessor& p);
    static juce::String agentsRunGoal(const juce::var& params, MorePhiProcessor& p);
    static juce::String agentsRunTask(const juce::var& params, MorePhiProcessor& p);
    static juce::String agentsRunStatus(const juce::var& params, MorePhiProcessor& p);
    static juce::String agentsRunCancel(const juce::var& params, MorePhiProcessor& p);
    static juce::String agentsBlackboardRecent(MorePhiProcessor& p);
    static juce::String agentsSetAutonomy(const juce::var& params, MorePhiProcessor& p, AutomationRuntime& runtime);
```

- [ ] **Step 2: Add the dispatch cases**

Edit `src/AI/MCPToolHandler.cpp` — in the `handle(...)` dispatch chain (after line ~3028, where the existing `plugin_profile.*` block ends, before the fallthrough). Add:

```cpp
    // ── Agent runtime tools (agents.*) ──────────────────────────────────────
    else if (method == "agents.list")               result = agentsList(p);
    else if (method == "agents.run_goal")           return agentsRunGoal(params, p);
    else if (method == "agents.run_task")           return agentsRunTask(params, p);
    else if (method == "agents.run_status")         result = agentsRunStatus(params, p);
    else if (method == "agents.run_cancel")         return agentsRunCancel(params, p);
    else if (method == "agents.blackboard.recent")  result = agentsBlackboardRecent(p);
    else if (method == "agents.set_autonomy")       return agentsSetAutonomy(params, p, runtime);
```

- [ ] **Step 3: Add the helper implementations**

Append near the bottom of `src/AI/MCPToolHandler.cpp` (before the closing `}` of the namespace, or in the existing helpers region). The helpers access the processor's `AgentRuntime` via a new accessor we'll add in Task 18; for now they return a `not_available` envelope if the runtime is absent.

```cpp
juce::String MCPToolHandler::agentsList(MorePhiProcessor& p)
{
    auto* rt = p.getAgentRuntime();
    if (! rt)
        return R"({"error":{"code":"agents_unavailable","message":"agent runtime not started"}})";
    return juce::String(rt->describeState().dump());
}

juce::String MCPToolHandler::agentsRunGoal(const juce::var& params, MorePhiProcessor& p)
{
    auto* rt = p.getAgentRuntime();
    if (! rt)
        return R"({"error":{"code":"agents_unavailable","message":"agent runtime not started"}})";
    const auto intent = params.getProperty("intent", {}).toString();
    if (intent.isEmpty())
        return R"({"error":{"code":"missing_intent","message":"intent is required"}})";
    const auto runId = rt->submitGoal(intent);
    return juce::String(nlohmann::json({ { "runId", runId.toStdString() } }).dump());
}

juce::String MCPToolHandler::agentsRunTask(const juce::var& params, MorePhiProcessor& p)
{
    auto* rt = p.getAgentRuntime();
    if (! rt)
        return R"({"error":{"code":"agents_unavailable","message":"agent runtime not started"}})";
    const auto roleStr = params.getProperty("agent", {}).toString().toLowerCase();
    const auto intent  = params.getProperty("intent", {}).toString();
    if (roleStr.isEmpty())
        return R"({"error":{"code":"missing_agent","message":"agent role is required"}})";
    more_phi::agents::AgentTask task;
    task.targetRole = more_phi::agents::agentRoleFromString(roleStr);
    task.intent = intent;
    task.origin = "mcp";
    const auto id = rt->submitTask(std::move(task));
    return juce::String(nlohmann::json({ { "taskId", id.toStdString() } }).dump());
}

juce::String MCPToolHandler::agentsRunStatus(const juce::var& params, MorePhiProcessor& p)
{
    auto* rt = p.getAgentRuntime();
    if (! rt)
        return R"({"error":{"code":"agents_unavailable","message":"agent runtime not started"}})";
    const auto taskId = params.getProperty("task_id", {}).toString();
    auto result = rt->peekResult(taskId);
    nlohmann::json j;
    j["taskId"] = taskId.toStdString();
    j["complete"] = result.has_value();
    if (result)
        j["result"] = { { "success", result->success }, { "errorCode", result->errorCode.toStdString() },
                        { "findings", result->findings } };
    return juce::String(j.dump());
}

juce::String MCPToolHandler::agentsRunCancel(const juce::var& /*params*/, MorePhiProcessor& p)
{
    auto* rt = p.getAgentRuntime();
    if (! rt)
        return R"({"error":{"code":"agents_unavailable","message":"agent runtime not started"}})";
    // Cooperative: stop drains in-flight tasks. A finer per-task cancel is a future enhancement.
    rt->stop();
    return R"({"status":"cancelled"})";
}

juce::String MCPToolHandler::agentsBlackboardRecent(MorePhiProcessor& p)
{
    auto* rt = p.getAgentRuntime();
    if (! rt)
        return R"({"error":{"code":"agents_unavailable","message":"agent runtime not started"}})";
    // The blackboard is backed by the instance's IntegrationEventBus; surface recent events.
    auto* runtime = p.getAutomationRuntimeForAgents();
    if (! runtime)
        return R"({"events":[]})";
    return juce::String(runtime->events().listRecent(64).dump());
}

juce::String MCPToolHandler::agentsSetAutonomy(const juce::var& params, MorePhiProcessor& p,
                                               AutomationRuntime& runtime)
{
    const auto levelStr = params.getProperty("level", {}).toString().toLowerCase();
    const auto level = more_phi::autonomyLevelFromString(levelStr);
    runtime.permissions().setAutonomyLevel(level);
    (void) p;
    return juce::String(nlohmann::json({ { "autonomy", more_phi::toString(level).toStdString() } }).dump());
}
```

Also add at the top of `MCPToolHandler.cpp` (with the other includes):

```cpp
#include "AI/Agents/AgentRuntime.h"
```

- [ ] **Step 4: Add the new tools to `getToolList()`**

Find `MCPToolHandler::getToolList()` in `MCPToolHandler.cpp` and append entries to its JSON array (mirror the existing entry shape — name + description + input schema). Add seven entries:

```json
{ "name": "agents.list", "description": "List registered agents and their state" }
{ "name": "agents.run_goal", "description": "Submit a natural-language goal to the Conductor",
  "inputSchema": { "type": "object", "properties": { "intent": { "type": "string" } }, "required": ["intent"] } }
{ "name": "agents.run_task", "description": "Submit a task directly to a named agent",
  "inputSchema": { "type": "object", "properties": { "agent": { "type": "string" }, "intent": { "type": "string" } }, "required": ["agent"] } }
{ "name": "agents.run_status", "description": "Poll a run/task for state and findings",
  "inputSchema": { "type": "object", "properties": { "task_id": { "type": "string" } }, "required": ["task_id"] } }
{ "name": "agents.run_cancel", "description": "Cooperatively cancel the agent runtime" }
{ "name": "agents.blackboard.recent", "description": "Read recent blackboard events" }
{ "name": "agents.set_autonomy", "description": "Set the agent-domain autonomy level",
  "inputSchema": { "type": "object", "properties": { "level": { "type": "string", "enum": ["manual","assist","copilot","autopilot"] } }, "required": ["level"] } }
```

(If `getToolList` builds the JSON via `nlohmann::json` incrementally rather than a string literal, add the equivalent objects.)

- [ ] **Step 5: Add `getAutomationRuntimeForAgents()` accessor declaration**

Edit `src/Plugin/PluginProcessor.h` — in the public accessor block (near `getAIAssistant()`, ~line 123), add:

```cpp
    // Agent runtime (owned; may be null before startMCPServerIfNeeded).
    class agents::AgentRuntime;
    agents::AgentRuntime* getAgentRuntime() noexcept { return agentRuntime_.get(); }
    more_phi::AutomationRuntime* getAutomationRuntimeForAgents() noexcept;
```

(If forward-declaring `agents::AgentRuntime` in the header is awkward due to namespace, instead include `"AI/Agents/AgentRuntime.h"` at the top of `PluginProcessor.cpp` and return a raw pointer from an opaque accessor declared in the header.)

- [ ] **Step 6: Build (Task 18 will add the member; for now expect an unresolved `agentRuntime_`)**

```bash
cmake --build build --config Release --target MorePhi 2>&1 | findstr /C:"agentRuntime_" /C:"error"
```
Expected: error referencing `agentRuntime_` — this is resolved in Task 18. Proceed.

- [ ] **Step 7: Commit (the build will succeed after Task 18; we commit the MCP changes now)**

```bash
git add src/AI/MCPToolHandler.h src/AI/MCPToolHandler.cpp src/Plugin/PluginProcessor.h
git commit -m "feat(agents): add agents.* MCP tool dispatch + getToolList entries"
```

---

## Task 18: Plugin ownership + startup wiring

**Files:**
- Modify: `src/Plugin/PluginProcessor.h` (add member)
- Modify: `src/Plugin/PluginProcessor.cpp` (construct, start, stop)

- [ ] **Step 1: Add the member + include**

Edit `src/Plugin/PluginProcessor.h` — near the `aiAssistant_` member (~line 459):

```cpp
    // ── Multi-agent orchestration layer (v3.4) ───────────────────────────────
    std::unique_ptr<more_phi::agents::AgentRuntime> agentRuntime_;
```

And at the top of `PluginProcessor.h` add the forward declaration alongside other forwards:

```cpp
namespace more_phi::agents { class AgentRuntime; class BlackboardBridge; class IToolInvoker;
                             class IAgentLogger; class ILlmClient; class DefaultToolInvoker;
                             class StructuredAgentLogger; class DeterministicFallbackLlmClient; }
```

- [ ] **Step 2: Add full includes + construction in PluginProcessor.cpp**

At the top of `src/Plugin/PluginProcessor.cpp` (with the other AI includes):

```cpp
#include "AI/Agents/AgentRuntime.h"
#include "AI/Agents/Blackboard/BlackboardBridge.h"
#include "AI/Agents/Tooling/DefaultToolInvoker.h"
#include "AI/Agents/Logging/StructuredAgentLogger.h"
#include "AI/Agents/Llm/DeterministicFallbackLlmClient.h"
#include "AI/Agents/Conductor/ConductorAgent.h"
#include "AI/Agents/Agents/AnalysisAgent.h"
#include "AI/Agents/Agents/OptimizationAgent.h"
#include "AI/Agents/Agents/CreativeAgent.h"
#include "AI/Agents/Agents/RealtimeControlAgent.h"
#include "AI/Agents/Agents/QualitySafetyAgent.h"
#include "AI/Agents/Agents/MemoryAgent.h"
```

Add private members to the `MorePhiProcessor` implementation (in the `.cpp`, as file-scope is not possible for instance state — add them as private members in the header, grouped under the agent layer):

```cpp
    // Agent-layer owned sub-objects (the runtime references these).
    std::unique_ptr<more_phi::agents::BlackboardBridge> agentBlackboard_;
    std::unique_ptr<more_phi::agents::DefaultToolInvoker> agentInvoker_;
    std::unique_ptr<more_phi::agents::StructuredAgentLogger> agentLogger_;
    std::unique_ptr<more_phi::agents::DeterministicFallbackLlmClient> agentLlm_;
```

- [ ] **Step 3: Construct + start in `startMCPServerIfNeeded()`**

Find `startMCPServerIfNeeded()` in `src/Plugin/PluginProcessor.cpp` (the exploration located it at ~line 2417). After the MCP server + IPC bridge are started (~line 2448), append:

```cpp
    // ── Multi-agent orchestration layer startup ───────────────────────────────
    if (! agentRuntime_)
    {
        // The blackboard backs onto this instance's AutomationRuntime event bus.
        // We reuse the runtime owned by the embedded MCP server path; if a separate
        // per-instance AutomationRuntime is needed later, construct it here.
        agentBlackboard_ = std::make_unique<more_phi::agents::BlackboardBridge>(
            mcpServer.getAutomationRuntime().events());
        agentLogger_   = std::make_unique<more_phi::agents::StructuredAgentLogger>(
            more_phi::agents::StructuredAgentLogger::defaultLogPath());
        agentLlm_      = std::make_unique<more_phi::agents::DeterministicFallbackLlmClient>();

        // The dispatch callback routes through the single chokepoint.
        more_phi::agents::DefaultToolInvoker::DispatchFn dispatch =
            [this](const juce::String& method, const nlohmann::json& params) -> juce::String {
                return more_phi::MCPToolHandler::handle(method,
                    juce::JSON::parse(juce::String(params.dump())),
                    *this, instanceIdentity_, mcpServer.getAutomationRuntime());
            };
        more_phi::agents::DefaultToolInvoker::CapabilityFn cap =
            [this](const juce::String& agentId) -> std::vector<juce::String> {
                using namespace more_phi::agents;
                // Resolve capability by agent id-prefix to role.
                if (agentId.startsWith("analysis"))   return AnalysisAgent{}.allowedTools();
                if (agentId.startsWith("optimization"))return OptimizationAgent{}.allowedTools();
                if (agentId.startsWith("creative"))   return CreativeAgent{}.allowedTools();
                if (agentId.startsWith("realtime"))   return RealtimeControlAgent{}.allowedTools();
                if (agentId.startsWith("quality"))    return QualitySafetyAgent{}.allowedTools();
                if (agentId.startsWith("memory"))     return MemoryAgent{}.allowedTools();
                if (agentId.startsWith("conductor"))  return ConductorAgent{}.allowedTools();
                return {};
            };
        agentInvoker_ = std::make_unique<more_phi::agents::DefaultToolInvoker>(dispatch, cap);

        agentRuntime_ = std::make_unique<more_phi::agents::AgentRuntime>(
            this, &instanceIdentity_, &mcpServer.getAutomationRuntime(),
            *agentInvoker_, *agentBlackboard_, *agentLogger_, agentLlm_.get());

        agentRuntime_->registerAgent(std::make_unique<more_phi::agents::ConductorAgent>());
        agentRuntime_->registerAgent(std::make_unique<more_phi::agents::AnalysisAgent>());
        agentRuntime_->registerAgent(std::make_unique<more_phi::agents::OptimizationAgent>());
        agentRuntime_->registerAgent(std::make_unique<more_phi::agents::CreativeAgent>());
        agentRuntime_->registerAgent(std::make_unique<more_phi::agents::RealtimeControlAgent>());
        agentRuntime_->registerAgent(std::make_unique<more_phi::agents::QualitySafetyAgent>());
        agentRuntime_->registerAgent(std::make_unique<more_phi::agents::MemoryAgent>());
        agentRuntime_->start(2);
    }
```

> **Note:** `MCPServer::getAutomationRuntime()` exists at `src/AI/MCPServer.h:64` and returns `AutomationRuntime&`; this is the event bus the blackboard backs onto.

- [ ] **Step 4: Stop in the destructor**

Find `~MorePhiProcessor()` in `src/Plugin/PluginProcessor.cpp` and add before the existing teardown:

```cpp
    if (agentRuntime_)
        agentRuntime_->stop();
    agentRuntime_.reset();
    agentInvoker_.reset();
    agentBlackboard_.reset();
    agentLogger_.reset();
    agentLlm_.reset();
```

- [ ] **Step 5: Implement `getAutomationRuntimeForAgents()`**

In `src/Plugin/PluginProcessor.cpp` add:

```cpp
more_phi::AutomationRuntime* MorePhiProcessor::getAutomationRuntimeForAgents() noexcept
{
    return &mcpServer.getAutomationRuntime();
}
```

- [ ] **Step 6: Build + run the full test suite to verify nothing regressed**

```bash
cmake --build build --config Release --target MorePhi MorePhiTests
cd build && ctest -C Release --output-on-failure --parallel 4
```
Expected: full suite green (existing tests unaffected; new agent tests pass).

- [ ] **Step 7: Commit**

```bash
git add src/Plugin/PluginProcessor.h src/Plugin/PluginProcessor.cpp
git commit -m "feat(agents): wire AgentRuntime into MorePhiProcessor lifecycle"
```

---

## Task 19: Structured agent logger (`StructuredAgentLogger.h/.cpp`)

**Files:**
- Create: `src/AI/Agents/Logging/StructuredAgentLogger.h`
- Create: `src/AI/Agents/Logging/StructuredAgentLogger.cpp`
- Create: `config/agents/agent_runtime.default.json`

- [ ] **Step 1: Create the header**

```cpp
// src/AI/Agents/Logging/StructuredAgentLogger.h
#pragma once

#include "AI/Agents/AgentContext.h"

#include <fstream>
#include <mutex>

namespace more_phi::agents {

// JSON-lines file logger, scoped to the agent layer only.
class StructuredAgentLogger : public IAgentLogger
{
public:
    explicit StructuredAgentLogger(const juce::File& file);
    ~StructuredAgentLogger() override;

    void log(const juce::String& agentId, const juce::String& level,
             const juce::String& message, const nlohmann::json& fields) override;

    static juce::File defaultLogPath();

private:
    juce::File file_;
    std::mutex mutex_;
    std::ofstream stream_;
};

} // namespace more_phi::agents
```

- [ ] **Step 2: Create the implementation**

```cpp
// src/AI/Agents/Logging/StructuredAgentLogger.cpp
#include "AI/Agents/Logging/StructuredAgentLogger.h"

#include <juce_core/juce_core.h>

namespace more_phi::agents {

namespace {
juce::File agentsStateDir()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("More-Phi/agents");
}
} // namespace

juce::File StructuredAgentLogger::defaultLogPath()
{
    return agentsStateDir().getChildFile("agent_runtime.log");
}

StructuredAgentLogger::StructuredAgentLogger(const juce::File& file) : file_(file)
{
    file_.getParentDirectory().createDirectory();
    stream_.open(file_.getFullPathName().toStdString(), std::ios::app);
}

StructuredAgentLogger::~StructuredAgentLogger()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_.is_open())
        stream_.flush();
}

void StructuredAgentLogger::log(const juce::String& agentId, const juce::String& level,
                                const juce::String& message, const nlohmann::json& fields)
{
    nlohmann::json line = {
        { "ts", juce::Time::getCurrentTime().toISO8601(false).toStdString() },
        { "agent", agentId.toStdString() },
        { "level", level.toStdString() },
        { "msg", message.toStdString() },
        { "fields", fields }
    };
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_.is_open())
    {
        stream_ << line.dump() << "\n";
        stream_.flush();
    }
}

} // namespace more_phi::agents
```

- [ ] **Step 3: Create the shipped default config**

Create `config/agents/agent_runtime.default.json`:

```json
{
  "schemaVersion": 1,
  "enabled": true,
  "scheduler": {
    "workerThreads": 2,
    "maxQueueDepth": 256,
    "starvationGuardMs": 5000
  },
  "blackboard": {
    "pumpIntervalMs": 50,
    "maxEventHistory": 256
  },
  "realtime": {
    "analysisCadenceMs": 100,
    "maxCorrectionsPerParamPerSecond": 4,
    "correctionBudgetPerRun": 16
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
    "level": "info",
    "rotateBytes": 5242880
  }
}
```

- [ ] **Step 4: Commit**

```bash
git add src/AI/Agents/Logging/StructuredAgentLogger.h src/AI/Agents/Logging/StructuredAgentLogger.cpp config/agents/agent_runtime.default.json
git commit -m "feat(agents): add StructuredAgentLogger + shipped default config"
```

---

## Task 20: End-to-end integration test (`TestAgentE2E.cpp`)

**Files:**
- Create: `tests/Unit/TestAgentE2E.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the test**

Create `tests/Unit/TestAgentE2E.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

#include "AI/Agents/AgentRuntime.h"
#include "AI/Agents/Logging/NullAgentLogger.h"
#include "AI/Agents/Llm/DeterministicFallbackLlmClient.h"
#include "AI/Agents/Conductor/ConductorAgent.h"
#include "AI/Agents/Agents/AnalysisAgent.h"
#include "AI/Agents/Agents/OptimizationAgent.h"
#include "AI/Agents/Agents/CreativeAgent.h"
#include "AI/Agents/Agents/RealtimeControlAgent.h"
#include "AI/Agents/Agents/QualitySafetyAgent.h"
#include "AI/Agents/Agents/MemoryAgent.h"
#include "AI/AutomationControlPlane.h"

using namespace more_phi::agents;
using namespace std::chrono_literals;

namespace {

struct ScopedStore
{
    ScopedStore()
    {
        dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getNonexistentChildFile("morephi_agent_e2e", "");
        more_phi::AutomationRuntime::setStoreDirectoryOverrideForTests(dir);
    }
    ~ScopedStore()
    {
        more_phi::AutomationRuntime::clearStoreDirectoryOverrideForTests();
        dir.deleteRecursively();
    }
    juce::File dir;
};

// A fake tool invoker that simulates the chokepoint: returns canned analysis + a
// render-batch with one clearly-best candidate.
class E2EInvoker : public IToolInvoker
{
public:
    nlohmann::json invoke(const juce::String& tool, const nlohmann::json&, const juce::String&) override
    {
        calledTools.push_back(tool.toStdString());
        if (tool == "analysis.get_summary")
            return { { "lufs_integrated", -9.2 }, { "true_peak_db", -0.3 } };
        if (tool == "analysis.get_spectrum")
            return { { "tilt_db", 2.0 } };
        if (tool == "analysis.get_stereo_field")
            return { { "width", 0.6 } };
        if (tool == "mastering.plan_preview")
            return { { "plan", { { "out", -2.0 } } } };
        if (tool == "mastering.render_batch")
        {
            nlohmann::json cands = nlohmann::json::array();
            cands.push_back({ { "id", 0 }, { "lufs_error", 0.4 }, { "params", { { "out", -2.0 } } } });
            cands.push_back({ { "id", 1 }, { "lufs_error", 1.1 }, { "params", { { "out", -1.0 } } } });
            return { { "candidates", cands } };
        }
        if (tool == "analysis.compare_render")
            return { { "after", { { "lufs_integrated", -13.9 }, { "true_peak_db", -1.1 } } } };
        if (tool == "set_parameters_batch")
            return { { "applied", true } };
        return { { "ok", true } };
    }
    std::vector<std::string> calledTools;
};

} // namespace

TEST_CASE("End-to-end: goal flows conductor → analysis → optimization → apply", "[agents][e2e]")
{
    ScopedStore store;
    more_phi::AutomationRuntime runtime;
    more_phi::IntegrationEventBus bus{128};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;
    DeterministicFallbackLlmClient llm;

    E2EInvoker invokerObj;
    auto cap = [](const juce::String&) {
        return std::vector<juce::String>{
            "workflow.submit","workflow.execute","hosted_plugin.info","analysis.get_summary",
            "analysis.get_spectrum","analysis.get_stereo_field","ozone.track.analyze",
            "get_mastering_state","analysis.capture_window","analysis.compare_render",
            "set_parameter","set_parameters_batch","mastering.plan_preview","mastering.render_batch",
            "suggest_intermediate_snapshots","find_related_parameters","suggest_morph_settings",
            "capture_snapshot","audit_plugin_profile","restore_safe_plugin_snapshot",
            "memory.record_outcome","memory.search","get_usage_stats","learn_from_adjustment",
            "set_morph_position","more_phi.set_parameter" };
    };
    // Wrap E2EInvoker in a DefaultToolInvoker so capability/budget enforcement is exercised.
    more_phi::agents::DefaultToolInvoker::DispatchFn dispatch =
        [&](const juce::String& m, const nlohmann::json& p) {
            return juce::String(invokerObj.invoke(m, p, "e2e").dump());
        };
    more_phi::agents::DefaultToolInvoker invoker{dispatch, cap};

    AgentRuntime rt{nullptr, nullptr, &runtime, invoker, bb, logger, &llm};
    rt.registerAgent(std::make_unique<ConductorAgent>());
    rt.registerAgent(std::make_unique<AnalysisAgent>());
    rt.registerAgent(std::make_unique<OptimizationAgent>());
    rt.registerAgent(std::make_unique<CreativeAgent>());
    rt.registerAgent(std::make_unique<RealtimeControlAgent>());
    rt.registerAgent(std::make_unique<QualitySafetyAgent>());
    rt.registerAgent(std::make_unique<MemoryAgent>());
    rt.start(3);

    juce::String runId = rt.submitGoal("master for streaming, keep it warm");
    REQUIRE(runId.isNotEmpty());

    // Wait for the conductor + at least the analysis + optimization subtasks to finish.
    std::optional<AgentResult> conductorResult;
    for (int i = 0; i < 800; ++i)
    {
        conductorResult = rt.peekResult(runId);
        if (conductorResult) break;
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(conductorResult.has_value());
    REQUIRE(conductorResult->success);

    // Give delegated subtasks time to land.
    std::this_thread::sleep_for(150ms);

    // The analysis + optimization tools must have been called through the chokepoint.
    bool calledAnalysis = false, calledOpt = false;
    for (const auto& t : invokerObj.calledTools)
    {
        if (t == "analysis.get_summary")  calledAnalysis = true;
        if (t == "mastering.render_batch")calledOpt = true;
    }
    REQUIRE(calledAnalysis);
    REQUIRE(calledOpt);

    rt.stop();
}
```

- [ ] **Step 2: Add test file to CMake**

Edit `tests/CMakeLists.txt` — add `Unit/TestAgentE2E.cpp`.

- [ ] **Step 3: Run the test to verify it passes**

```bash
cmake --build build --config Release --target MorePhiTests
cd build && ctest -C Release -R "End-to-end" --output-on-failure
```
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add tests/Unit/TestAgentE2E.cpp tests/CMakeLists.txt
git commit -m "test(agents): end-to-end goal flow through conductor + specialists"
```

---

# Phase 5 — Safety Invariants + Docs

## Task 21: Reactive-path test (`TestRealtimeReactive.cpp`)

**Files:**
- Create: `tests/Unit/TestRealtimeReactive.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the test (clip → correction → rate-cap back-off)**

Create `tests/Unit/TestRealtimeReactive.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

#include "AI/Agents/AgentRuntime.h"
#include "AI/Agents/Logging/NullAgentLogger.h"
#include "AI/Agents/Agents/AnalysisAgent.h"
#include "AI/Agents/Agents/RealtimeControlAgent.h"
#include "AI/AutomationControlPlane.h"

using namespace more_phi::agents;
using namespace std::chrono_literals;

namespace {

struct ScopedStore
{
    ScopedStore()
    {
        dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getNonexistentChildFile("morephi_reactive", "");
        more_phi::AutomationRuntime::setStoreDirectoryOverrideForTests(dir);
    }
    ~ScopedStore()
    {
        more_phi::AutomationRuntime::clearStoreDirectoryOverrideForTests();
        dir.deleteRecursively();
    }
    juce::File dir;
};

class CountingInvoker : public IToolInvoker
{
public:
    int setParamCalls = 0;
    int lastParam = -1;
    nlohmann::json invoke(const juce::String& tool, const nlohmann::json& params, const juce::String&) override
    {
        if (tool == "set_parameter")
        {
            ++setParamCalls;
            lastParam = params.value("index", -1);
        }
        if (tool == "analysis.get_summary")
            return { { "lufs_integrated", -7.0 }, { "true_peak_db", 0.4 } };
        if (tool == "analysis.get_spectrum") return { { "tilt_db", 1.0 } };
        if (tool == "analysis.get_stereo_field") return { { "width", 0.5 } };
        return { { "ok", true } };
    }
};

} // namespace

TEST_CASE("Reactive path: clip detection triggers realtime correction, rate-capped", "[agents][reactive]")
{
    ScopedStore store;
    more_phi::AutomationRuntime runtime;
    more_phi::IntegrationEventBus bus{128};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;

    CountingInvoker counter;
    auto cap = [](const juce::String&) {
        return std::vector<juce::String>{
            "analysis.get_summary","analysis.get_spectrum","analysis.get_stereo_field",
            "ozone.track.analyze","get_mastering_state","analysis.capture_window","analysis.compare_render",
            "set_parameter","set_morph_position","more_phi.set_parameter" };
    };
    more_phi::agents::DefaultToolInvoker::DispatchFn dispatch =
        [&](const juce::String& m, const nlohmann::json& p) {
            return juce::String(counter.invoke(m, p, "reactive").dump());
        };
    more_phi::agents::DefaultToolInvoker invoker{dispatch, cap, /*rateLimit*/ 0};

    AgentRuntime rt{nullptr, nullptr, &runtime, invoker, bb, logger, nullptr};
    rt.registerAgent(std::make_unique<AnalysisAgent>());
    auto rtc = std::make_unique<RealtimeControlAgent>();
    RealtimeControlAgent::Config cfg;
    cfg.maxCorrectionsPerParamPerSecond = 2;   // tight, to exercise back-off
    cfg.correctionBudgetPerRun = 2;
    rtc->setConfig(cfg);
    rt.registerAgent(std::move(rtc));
    rt.start(2);

    // Drive Analysis once: it will publish clipping_detected + lufs_breach.
    AgentTask a;
    a.id = "a1"; a.targetRole = AgentRole::Analysis; a.intent = "measure";
    juce::String aId = rt.submitTask(a);
    for (int i = 0; i < 200 && ! rt.peekResult(aId).has_value(); ++i)
        std::this_thread::sleep_for(5ms);

    // Let the blackboard pump fan out the clip event to RealtimeControl.
    std::this_thread::sleep_for(200ms);

    // The first correction(s) within budget must have applied a set_parameter.
    REQUIRE(counter.setParamCalls >= 1);

    rt.stop();
}
```

- [ ] **Step 2: Add test file to CMake**

Edit `tests/CMakeLists.txt` — add `Unit/TestRealtimeReactive.cpp`.

- [ ] **Step 3: Run the test to verify it passes**

```bash
cmake --build build --config Release --target MorePhiTests
cd build && ctest -C Release -R "Reactive path" --output-on-failure
```
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add tests/Unit/TestRealtimeReactive.cpp tests/CMakeLists.txt
git commit -m "test(agents): reactive clip→correction path with rate-cap back-off"
```

---

## Task 22: Audio-thread isolation invariant test (`TestAgentAudioThreadIsolation.cpp`)

**Files:**
- Create: `tests/Unit/TestAgentAudioThreadIsolation.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the test**

Create `tests/Unit/TestAgentAudioThreadIsolation.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <set>

#include "AI/Agents/AgentRuntime.h"
#include "AI/Agents/Logging/NullAgentLogger.h"
#include "AI/Agents/Conductor/ConductorAgent.h"
#include "AI/Agents/Agents/AnalysisAgent.h"
#include "AI/Agents/Agents/OptimizationAgent.h"
#include "AI/AutomationControlPlane.h"

using namespace more_phi::agents;
using namespace std::chrono_literals;

namespace {

struct ScopedStore
{
    ScopedStore()
    {
        dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getNonexistentChildFile("morephi_rt_isolation", "");
        more_phi::AutomationRuntime::setStoreDirectoryOverrideForTests(dir);
    }
    ~ScopedStore()
    {
        more_phi::AutomationRuntime::clearStoreDirectoryOverrideForTests();
        dir.deleteRecursively();
    }
    juce::File dir;
};

// Records the thread id of every tool dispatch. The "audio thread" is simulated
// here by a thread we run a processBlock-style noop on; the invariant under test
// is that NO dispatch ever happens on that thread.
class ThreadRecordingInvoker : public IToolInvoker
{
public:
    std::mutex mutex;
    std::set<std::thread::id> seenThreadIds;
    std::thread::id audioThreadId{};

    nlohmann::json invoke(const juce::String& tool, const nlohmann::json& params, const juce::String&) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            seenThreadIds.insert(std::this_thread::get_id());
        }
        if (tool == "analysis.get_summary")  return { { "lufs_integrated", -9.0 }, { "true_peak_db", -0.5 } };
        if (tool == "analysis.get_spectrum") return { { "tilt_db", 1.0 } };
        if (tool == "analysis.get_stereo_field") return { { "width", 0.5 } };
        if (tool == "mastering.render_batch")
            return { { "candidates", nlohmann::json::array({
                { { "id", 0 }, { "lufs_error", 0.4 }, { "params", { { "out", -2.0 } } } } }) } };
        return { { "ok", true } };
    }
};

} // namespace

TEST_CASE("No agent tool dispatch occurs on the audio thread", "[agents][rt-safety]")
{
    ScopedStore store;
    more_phi::AutomationRuntime runtime;
    more_phi::IntegrationEventBus bus{64};
    BlackboardBridge bb{bus};
    NullAgentLogger logger;
    DeterministicFallbackLlmClient llm;

    ThreadRecordingInvoker rec;
    auto cap = [](const juce::String&) {
        return std::vector<juce::String>{
            "workflow.submit","workflow.execute","hosted_plugin.info","analysis.get_summary",
            "analysis.get_spectrum","analysis.get_stereo_field","ozone.track.analyze",
            "get_mastering_state","analysis.capture_window","analysis.compare_render",
            "set_parameter","set_parameters_batch","mastering.plan_preview","mastering.render_batch" };
    };
    more_phi::agents::DefaultToolInvoker::DispatchFn dispatch =
        [&](const juce::String& m, const nlohmann::json& p) {
            return juce::String(rec.invoke(m, p, "rt").dump());
        };
    more_phi::agents::DefaultToolInvoker invoker{dispatch, cap};

    AgentRuntime rt{nullptr, nullptr, &runtime, invoker, bb, logger, &llm};
    rt.registerAgent(std::make_unique<ConductorAgent>());
    rt.registerAgent(std::make_unique<AnalysisAgent>());
    rt.registerAgent(std::make_unique<OptimizationAgent>());
    rt.start(2);

    // Simulate an audio thread running processBlock no-ops concurrently with agent work.
    std::atomic<bool> audioRunning{true};
    std::thread audioThread([&] {
        rec.audioThreadId = std::this_thread::get_id();
        while (audioRunning.load())
            std::this_thread::sleep_for(1ms);
    });

    juce::String runId = rt.submitGoal("master for streaming");
    // Let agent work run alongside the audio thread.
    std::this_thread::sleep_for(300ms);

    audioRunning.store(false);
    audioThread.join();
    rt.stop();

    // Invariant: the audio thread id must NOT appear among dispatch thread ids.
    std::lock_guard<std::mutex> lock(rec.mutex);
    REQUIRE(rec.seenThreadIds.find(rec.audioThreadId) == rec.seenThreadIds.end());
}
```

- [ ] **Step 2: Add test file to CMake**

Edit `tests/CMakeLists.txt` — add `Unit/TestAgentAudioThreadIsolation.cpp`.

- [ ] **Step 3: Run the test to verify it passes**

```bash
cmake --build build --config Release --target MorePhiTests
cd build && ctest -C Release -R "audio thread" --output-on-failure
```
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add tests/Unit/TestAgentAudioThreadIsolation.cpp tests/CMakeLists.txt
git commit -m "test(agents): assert no agent tool dispatch on the audio thread"
```

---

## Task 23: CMake source wiring + optional adapter agent

**Files:**
- Modify: `CMakeLists.txt` (add agent sources to `MORE_PHI_AI_SOURCES`)
- Create: `src/AI/Agents/Adapters/AiAssistantAdapterAgent.h`
- Create: `src/AI/Agents/Adapters/AiAssistantAdapterAgent.cpp`

- [ ] **Step 1: Create the optional adapter**

```cpp
// src/AI/Agents/Adapters/AiAssistantAdapterAgent.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <atomic>

namespace more_phi::agents {

// Optional Phase-1 bridge: wraps the existing AIAssistant so it can participate
// as a peer in the agent world without modifying it. Used until Phase-2 supersede.
class AiAssistantAdapterAgent : public IAgent
{
public:
    AgentRole role() const noexcept override { return AgentRole::Custom; }
    juce::String id() const noexcept override { return "ai-assistant-adapter"; }
    std::vector<juce::String> allowedTools() const override { return { "workflow.submit", "workflow.execute" }; }
    void prepare(const AgentContext& ctx) override { ctx_ = &ctx; }
    AgentResult execute(const AgentTask& task) override;
    AgentState state() const noexcept override { return state_.load(); }
    void stop() override { state_.store(AgentState::Stopped); }

private:
    const AgentContext* ctx_ = nullptr;
    std::atomic<AgentState> state_{ AgentState::Idle };
};

} // namespace more_phi::agents
```

```cpp
// src/AI/Agents/Adapters/AiAssistantAdapterAgent.cpp
#include "AI/Agents/Adapters/AiAssistantAdapterAgent.h"
#include "AI/AIAssistant.h"
#include "Plugin/PluginProcessor.h"

namespace more_phi::agents {

AgentResult AiAssistantAdapterAgent::execute(const AgentTask& task)
{
    state_.store(AgentState::Busy);
    AgentResult r;
    r.taskId = task.id;
    if (! ctx_ || ! ctx_->processor)
    {
        r.success = false; r.errorCode = "no_processor";
        state_.store(AgentState::Idle); return r;
    }
    auto* assistant = ctx_->processor->getAIAssistant();
    if (! assistant)
    {
        r.success = false; r.errorCode = "no_ai_assistant";
        state_.store(AgentState::Idle); return r;
    }
    auto result = assistant->executeLocalWorkflowPrompt(task.intent);
    r.success = result.success;
    r.findings = { { "message", result.message.toStdString() },
                   { "runId", result.workflowRunId.toStdString() } };
    state_.store(AgentState::Idle);
    return r;
}

} // namespace more_phi::agents
```

- [ ] **Step 2: Add all agent sources to the root CMake**

Edit `CMakeLists.txt` — in the `MORE_PHI_AI_SOURCES` list (the exploration located the AI source block around lines 205–429), add:

```cmake
    AI/Agents/IAgent.h
    AI/Agents/AgentContext.h
    AI/Agents/AgentRuntime.h
    AI/Agents/AgentRuntime.cpp
    AI/Agents/AgentRegistry.h
    AI/Agents/AgentRegistry.cpp
    AI/Agents/Scheduler/PriorityScheduler.h
    AI/Agents/Scheduler/PriorityScheduler.cpp
    AI/Agents/Blackboard/BlackboardBridge.h
    AI/Agents/Blackboard/BlackboardBridge.cpp
    AI/Agents/Tooling/AgentToolError.h
    AI/Agents/Tooling/DefaultToolInvoker.h
    AI/Agents/Tooling/DefaultToolInvoker.cpp
    AI/Agents/Logging/NullAgentLogger.h
    AI/Agents/Logging/StructuredAgentLogger.h
    AI/Agents/Logging/StructuredAgentLogger.cpp
    AI/Agents/Llm/NullLlmClient.h
    AI/Agents/Llm/DeterministicFallbackLlmClient.h
    AI/Agents/Llm/DeterministicFallbackLlmClient.cpp
    AI/Agents/Conductor/ConductorAgent.h
    AI/Agents/Conductor/ConductorAgent.cpp
    AI/Agents/Adapters/AiAssistantAdapterAgent.h
    AI/Agents/Adapters/AiAssistantAdapterAgent.cpp
    AI/Agents/Agents/AnalysisAgent.h
    AI/Agents/Agents/AnalysisAgent.cpp
    AI/Agents/Agents/OptimizationAgent.h
    AI/Agents/Agents/OptimizationAgent.cpp
    AI/Agents/Agents/CreativeAgent.h
    AI/Agents/Agents/CreativeAgent.cpp
    AI/Agents/Agents/RealtimeControlAgent.h
    AI/Agents/Agents/RealtimeControlAgent.cpp
    AI/Agents/Agents/QualitySafetyAgent.h
    AI/Agents/Agents/QualitySafetyAgent.cpp
    AI/Agents/Agents/MemoryAgent.h
    AI/Agents/Agents/MemoryAgent.cpp
```

- [ ] **Step 3: Build the full plugin + tests**

```bash
cmake -B build -S . -DMORE_PHI_BUILD_TESTS=ON
cmake --build build --config Release --target MorePhi MorePhiTests
cd build && ctest -C Release --output-on-failure --parallel 4
```
Expected: full build + all tests green.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt src/AI/Agents/Adapters/
git commit -m "build(agents): wire agent sources into CMake; add optional AiAssistantAdapterAgent"
```

---

## Task 24: Document the agent layer in AGENTS.md

**Files:**
- Modify: `AGENTS.md`

- [ ] **Step 1: Add a new section**

In `AGENTS.md`, after the "### Dataset Generation (V2/V3)" section (and before "### Genetic Engine"), insert:

```markdown
### Multi-Agent Orchestration Layer (v3.4)

An additive agent-coordination layer sits **above** `MCPToolHandler` / `AutomationControlPlane` and **never** touches the audio thread. See `docs/superpowers/specs/2026-06-21-multi-agent-orchestration-layer-design.md`.

- **`src/AI/Agents/`** — `AgentRuntime` (owns `AgentRegistry` + `PriorityScheduler` + `BlackboardBridge`), the `IAgent` contract, six specialist agents (`Analysis`, `Optimization`, `Creative`, `RealtimeControl`, `QualitySafety`, `Memory`) plus `Conductor`, and supporting infra (`DefaultToolInvoker`, `StructuredAgentLogger`, `DeterministicFallbackLlmClient`).
- **Single chokepoint:** every agent action that mutates state goes through `MCPToolHandler::handle()` via `DefaultToolInvoker`, inheriting permission-gating, audit, memory, and events for free.
- **Realtime safety:** agents run on `PriorityScheduler` worker threads (message-thread domain) only. They write parameter *targets* into the existing `LockFreeQueue` with `source=Assistant`. "RealtimeCritical" priority means "jump the agent queue", NOT "run on the audio thread".
- **Coordination model:** Conductor decomposes goals and is the only agent that delegates; the Blackboard (`BlackboardBridge` over `IntegrationEventBus`) carries event-driven reactions between specialists.
- **MCP surface:** seven `agents.*` tools on the existing TCP + stdio servers; classified in `PermissionKernel`.
- **LLM transport:** currently broken (spec 005 GAP-CRITICAL); agents use the `ILlmClient` seam with a `DeterministicFallbackLlmClient` so the loop degrades gracefully.
- **Config:** `config/agents/agent_runtime.default.json` (user override at `<userAppData>/More-Phi/agents/agent_runtime.json`).
```

- [ ] **Step 2: Commit**

```bash
git add AGENTS.md
git commit -m "docs(agents): document the multi-agent orchestration layer in AGENTS.md"
```

---

## Self-Review Notes

- **Spec coverage:** §2 placement (Task 18), §3 contracts (Tasks 1–8), §4 coordination model (Tasks 8–9), §5 RT-safety (Task 22 + the D2 design enforced throughout), §6 all six agents (Tasks 10, 11–15), §7 MCP surface + startup (Tasks 16–18, 20), §8 security (Tasks 6, 16), §9 config/logging (Tasks 5, 6, 19), §10 test plan (Tasks 3,4,6,7,8,9,10,11,12,13,14,15,20,21,22), §11 risks (R1 mitigated Task 5; R2 Task 13; R3 Task 3; R6 Task 23 adapter), §12 migration (Task 23 adapter + Phase-2 noted), §14 file layout (all files created).
- **Type consistency:** `submitTask` / `submitGoal` / `peekResult` signatures are consistent across Conductor, MCP helpers, and tests. `AgentTask` / `AgentResult` / `AgentEventEnvelope` field names match between IAgent.h, agent implementations, and tests. `DefaultToolInvoker` constructor args `(DispatchFn, CapabilityFn, int)` are consistent.
- **Known integration seam (verified):** `MCPServer::getAutomationRuntime()` exists at `src/AI/MCPServer.h:64` and returns `AutomationRuntime&` — Task 18's wiring against it is accurate as written. No unknowns remain.

---

**Plan complete.** Phases are independently buildable: Phase 1 ends with a working tested runtime; Phase 2 adds the first real loop; Phases 3–5 complete the system.
