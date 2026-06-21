# More-Phi Multi-Agent Ecosystem Enhancement Plan

## Current State Assessment

The More-Phi project (v3.3.0) already contains a production-ready, integrated ecosystem of:

1. **VST3 Plugin** (`MorePhiProcessor`): JUCE 8-based VST3/AU plugin with real-time audio processing, parameter automation, preset management, and hosted plugin support
2. **MCP Server** (`MCPServer`): JSON-RPC 2.0 server on localhost:30001 with bearer auth, multi-instance identity, connection pooling, error recovery, and health monitoring
3. **Multi-Agent Orchestration** (`AgentRuntime` + `AgentRegistry` + `PriorityScheduler` + `BlackboardBridge`):
   - **ConductorAgent**: Decomposes user goals into specialist subtasks
   - **AnalysisAgent**: Read-only audio analysis (LUFS, spectrum, stereo field)
   - **OptimizationAgent**: Drives parameters toward target metrics
   - **CreativeAgent**: Artistic suggestions with mandatory approval
   - **RealtimeControlAgent**: Reactive corrections via lock-free queue
   - **QualitySafetyAgent**: Validates proposals before application
   - **PriorityScheduler**: Worker pool with 4 priority levels (Background, Normal, High, RealtimeCritical)
   - **BlackboardBridge**: Typed pub/sub over IntegrationEventBus
4. **Thread-Safe Communication**: LockFreeQueue (SPSC ring buffer, 8192 capacity), atomics, seqlock in SnapshotBank, juce::SpinLock
5. **Build System**: CMake with JUCE 8.0.4, nlohmann/json, Catch2 v3
6. **Test Suite**: 100+ tests covering unit, integration, and performance

## Gap Analysis vs. User Requirements

| Requirement | Status | Gap |
|---|---|---|
| VST3 Plugin with communication bridge | ✅ Exists | Needs explicit facade |
| Multi-agent orchestration with specialized agents | ✅ Exists | Needs orchestrator facade |
| MCP Server with message schemas | ✅ Exists | Needs explicit protocol schemas |
| Unified configuration | ⚠️ Partial | Needs `EcosystemConfig` |
| Comprehensive logging/monitoring | ⚠️ Partial | Needs unified metrics |
| Security boundaries (message validation) | ⚠️ Partial | Needs `SecurityValidator` |
| Integration examples / startup sequence | ❌ Missing | Needs `IntegrationExamples` |
| Architectural documentation / diagrams | ❌ Missing | Needs `docs/ECOSYSTEM.md` |
| Extensibility for new agents | ✅ Exists | AgentRegistry is open |

## Enhancement Plan

### Stage 1: Core Facades and Protocols (parallel)

**Worker A — AgentOrchestrator + McpProtocol**
- Create `src/AI/Orchestrator/AgentOrchestrator.h` and `.cpp`
  - Single-initialization facade that wires PluginProcessor → AgentRuntime → MCPServer
  - Provides `start()`, `stop()`, `submitUserGoal()`, `describeSystemState()`
  - Handles graceful degradation if MCP is unavailable
- Create `src/AI/Orchestrator/McpProtocol.h`
  - Explicit JSON-RPC 2.0 message schemas as structs (`McpRequest`, `McpResponse`, `McpNotification`, `McpToolCall`, `McpError`)
  - Validation helpers (`validateRequest`, `validateAuth`)
  - Serialization/deserialization helpers

**Worker B — EcosystemConfig + SecurityValidator + IntegrationExamples**
- Create `src/AI/Orchestrator/EcosystemConfig.h` and `.cpp`
  - Unified TOML-like JSON configuration for all three components
  - Load from file, validate schema, apply defaults
  - Hot-reload support for non-audio-critical settings
- Create `src/AI/Orchestrator/SecurityValidator.h` and `.cpp`
  - MCP message sanitization (max depth, max size, allowed fields)
  - Auth token validation with constant-time comparison
  - Rate limiting per client IP
  - Input validation for all exposed parameters
- Create `examples/IntegrationExamples.cpp`
  - Example 1: Full startup sequence (plugin → agents → MCP)
  - Example 2: Submitting a user goal through the orchestrator
  - Example 3: Handling an MCP tool request from an external client
  - Example 4: Graceful shutdown and error recovery

### Stage 2: Documentation

**Worker C — Comprehensive Ecosystem Documentation**
- Create `docs/ECOSYSTEM.md` with:
  - Architecture overview with Mermaid diagrams
  - Data flow between VST3 plugin, AgentRuntime, and MCP Server
  - Threading model (audio thread, message thread, MCP thread, scheduler workers)
  - Message protocol reference
  - Agent capability matrix
  - Configuration reference
  - Security model
  - Extension guide (how to add a new agent)

### Stage 3: Integration and Verification

- Update `CMakeLists.txt` to include new source files
- Add new test files for `AgentOrchestrator`, `SecurityValidator`, `EcosystemConfig`
- Run build verification
- Run test suite

## File Inventory (new files)

```
src/AI/Orchestrator/
  AgentOrchestrator.h
  AgentOrchestrator.cpp
  McpProtocol.h
  McpProtocol.cpp
  EcosystemConfig.h
  EcosystemConfig.cpp
  SecurityValidator.h
  SecurityValidator.cpp
examples/
  IntegrationExamples.cpp
  CMakeLists.txt
docs/
  ECOSYSTEM.md
tests/Unit/
  TestAgentOrchestrator.cpp
  TestSecurityValidator.cpp
  TestEcosystemConfig.cpp
```

## Shared Contract

- All new code lives in `more_phi::orchestrator` namespace (except protocol structs which are `more_phi::mcp`)
- Must compile with C++20, MSVC 2022, Clang, GCC
- Must use JUCE types for strings where interfacing with existing JUCE code
- Must use `nlohmann::json` for JSON
- Must be thread-safe; audio-thread code must be `noexcept` and zero-alloc after prepare
- Must not break existing tests or API
- `AgentOrchestrator` must reference the existing `MorePhiProcessor`, `AgentRuntime`, `MCPServer`
- `McpProtocol` structs must be compatible with the existing `MCPServer::processRequest` JSON format
- `SecurityValidator` must be usable from `MCPServer` without changing its existing logic
- `EcosystemConfig` must use the existing `InstanceIdentity` for port/auth defaults

## Merge Order

1. Worker A (AgentOrchestrator + McpProtocol) → main
2. Worker B (EcosystemConfig + SecurityValidator + IntegrationExamples) → main
3. Worker C (Documentation) → can merge anytime (no code impact)
4. Main agent: Update CMakeLists.txt, add tests, run build verification
