# AI Agent + Ozone Track Assistant Integration via MCP

**Technical Implementation Guide** — More-Phi v3.3.0+
> Updated 2026-06-18.

This guide details the architectural workflow for integrating an AI agent with iZotope Ozone 11's internal Track Assistant capabilities through More-Phi's embedded Model Context Protocol (MCP) server.

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [MCP Tool Definitions for Track Assistant Functionality](#2-mcp-tool-definitions-for-track-assistant-functionality)
3. [JSON-RPC Schema Definitions](#3-json-rpc-schema-definitions)
4. [Authentication Layer](#4-authentication-layer)
5. [Example Implementation: End-to-End Tool Call Workflow](#5-example-implementation-end-to-end-tool-call-workflow)
6. [Integration Patterns and Best Practices](#6-integration-patterns-and-best-practices)

---

## 1. Architecture Overview

### 1.1 System Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  AI Agent (MCP Client) — External process (Python, Node.js, etc.)          │
│                                                                             │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────────────────────┐  │
│  │ Intent       │───▶│ Tool         │───▶│ JSON-RPC 2.0                 │  │
│  │ Analyzer     │    │ Selector     │    │ TCP Client (localhost)       │  │
│  │ (LLM)        │◀───│              │◀───│                              │  │
│  └──────────────┘    └──────────────┘    └──────────────┬───────────────┘  │
└─────────────────────────────────────────────────────────┼──────────────────┘
                                                          │ TCP localhost
┌─────────────────────────────────────────────────────────┼──────────────────┐
│  More-Phi Plugin (VST3/AU Host)                        │                  │
│                                                         ▼                  │
│  ┌──────────────────────────────────────────────────────────────────────┐ │
│  │ MCPServer (juce::Thread)                                             │ │
│  │ ┌───────────┐  ┌────────────┐  ┌─────────────┐  ┌────────────────┐ │ │
│  │ │Connection │─▶│ validate   │─▶│ dispatch    │─▶│ MCPToolHandler │ │ │
│  │ │Thread(s)  │  │ Auth       │  │ Tool        │  │ / MCPTools     │ │ │
│  │ │(max 4)    │  │ (constant  │  │             │  │ Extended       │ │ │
│  │ └───────────┘  │  time)     │  │             │  │ / MCPEQTool    │ │ │
│  └──────────────────────────────────────────────────────────────────────┘ │
│                            │                                              │
│                            ▼                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐ │
│  │ MorePhiProcessor (Audio Thread Owner)                                │ │
│  │ ┌─────────────────┐  ┌──────────────────┐  ┌─────────────────────┐ │ │
│  │ │ OzoneParameter  │  │ OzonePlan        │  │ ChainPlanExecutor   │ │ │
│  │ │ Map             │  │ Applicator       │  │ (5-step rules)      │ │ │
│  │ │                 │  │                  │  │                     │ │ │
│  │ │ - EQ[8] bands   │  │ - applyEQ()      │  │ Step 1: Dynamics    │ │ │
│  │ │ - Dynamics      │  │ - applyDynamics()│  │ Step 2: Spectral    │ │ │
│  │ │ - Imager[4]     │  │ - applyImager()  │  │ Step 3: Stereo      │ │ │
│  │ │ - Maximizer[2]  │  │ - applyMaximizer │  │ Step 4: Loudness    │ │ │
│  │ │                 │  │                  │  │ Step 5: Stage Ctrl  │ │ │
│  │ └─────────────────┘  └──────────────────┘  └─────────────────────┘ │ │
│  └──────────────────────────┬─────────────────────────────────────────┘ │
│                             │                                            │
│                             ▼                                            │
│  ┌──────────────────────────────────────────────────────────────────────┐ │
│  │ ParameterBridge → LockFreeQueue → Audio Thread → Hosted Ozone 11    │ │
│  └──────────────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────────────┘
```

### 1.2 Component Responsibilities

| Component | Role | Thread Domain |
|-----------|------|---------------|
| **AI Agent (MCP Client)** | Natural language understanding, tool selection, response formatting | External process |
| **MCPServer** | JSON-RPC 2.0 TCP listener, auth validation, rate limiting, tool dispatch, instance isolation, idle timeout, constant-time auth | MCP thread (`MorePhi-MCP`) |
| **ConnectionThread** | Per-client socket handling, request parsing, response serialization, 30-second idle timeout, socket cleanup | MCP thread (per-client) |
| **MCPToolHandler** | Core tool dispatch (12 tools + 8 hosted plugin aliases) | MCP thread |
| **MCPToolsExtended** | AI tools (Learn Mode, token optimization, morph compatibility, dataset generation) | MCP thread |
| **MCPEQTool** | EQ Assistant tools (natural language EQ control via AIAssistant) | MCP thread |
| **InstanceRegistry** | Multi-instance port management, zombie eviction after TTL expiry | Singleton |
| **OzoneParameterMap** | Static parameter index table mapping Ozone 11 parameters (EQ bands, dynamics, imager, maximizer) | Message thread |
| **OzonePlanApplicator** | Translates `MultiEffectPlan` into Ozone parameter changes via `enqueueParameterSet()` | Message thread |
| **ChainPlanExecutor** | 5-step heuristic rule planner: dynamics → spectral → stereo → loudness → stage control | Background thread (ThreadPool) |
| **ParameterBridge** | Applies normalized float vector to hosted plugin parameters | Audio thread (via LockFreeQueue) |

### 1.3 Threading Model

More-Phi enforces strict thread domain boundaries:

```
┌─────────────────────────────────────────────────────────────────────┐
│ Thread Domain          │ Responsibilities                    │ Rules│
├─────────────────────────────────────────────────────────────────────┤
│ Audio Thread           │ processBlock(), parameter apply     │      │
│                        │ LockFreeQueue drain, Ozone param    │      │
│                        │ changes via enqueueParameterSet()   │      │
│                        │                                     │ NO   │
│                        │                                     │ alloc│
│                        │                                     │ NO   │
│                        │                                     │ locks│
│                        │                                     │ NO   │
│                        │                                     │ except│
├─────────────────────────────────────────────────────────────────────┤
│ Message Thread         │ UI, Timer callbacks, deferred       │      │
│                        │ plugin loading, OzoneParameterMap   │      │
│                        │ buildFromHostedPlugin(),            │      │
│                        │ OzonePlanApplicator::apply()        │      │
│                        │                                     │ NO   │
│                        │                                     │ blocking│
├─────────────────────────────────────────────────────────────────────┤
│ MCP Thread             │ JSON-RPC server, tool dispatch,     │      │
│ (MorePhi-MCP)          │ auth validation, rate limiting      │      │
│                        │                                     │ NO   │
│                        │                                     │ audio│
│                        │                                     │ thread│
│                        │                                     │ calls│
├─────────────────────────────────────────────────────────────────────┤
│ Connection Threads     │ Per-client socket I/O (max 4)       │      │
│ (MCP-Connection)       │ Request parsing, response writing   │      │
├─────────────────────────────────────────────────────────────────────┤
│ Background ThreadPool  │ ChainPlanExecutor 5-step rules,     │      │
│                        │ dataset generation, rendering       │      │
└─────────────────────────────────────────────────────────────────────┘
```

### 1.4 Data Flow: AI Agent → Ozone Parameter Change

```
1. User Input: "Make this track louder and brighter"
   │
2. AI Agent (LLM) analyzes intent → selects MCP tool: apply_mastering_plan
   │
3. AI Agent sends JSON-RPC request via TCP to localhost:30001
   │
4. MCPServer.ConnectionThread receives request
   ├─ validateAuth(bearer_token) — constant-time comparison
   ├─ TokenOptimizer.tryConsumeRequestSlot() — rate limit check
   └─ dispatchTool("apply_mastering_plan", params)
   │
5. MCPToolHandler.applyMasteringPlan(params, processor)
   ├─ Extract: genre_index, dynamic_range, spectral_tilt, correlation_ms
   └─ Call: processor.getAutoMasteringEngine().getChainPlanner().executePlan(...)
   │
6. ChainPlanExecutor.executePlan() — applies deterministic heuristic rules
   ├─ Step 1: Dynamics Assessment → compressionNeed, useNeuralComp=false while neural backend is not loaded
   ├─ Step 2: Spectral Assessment → eqPrescriptionJSON (8-band EQ)
   ├─ Step 3: Stereo Assessment → widthCurve[4] (sub/low/mid/high)
   ├─ Step 4: Loudness Target → targetLUFS, ceilingDBTP
   └─ Step 5: Stage Control → exciterEnabled, valid
   │
7. OzonePlanApplicator.apply(plan) — called if Ozone 11 detected
   ├─ applyEQ(plan) → parses eqPrescriptionJSON → normalizes → enqueues
   ├─ applyDynamics(plan) → compressionNeed → threshold/ratio/attack/release
   ├─ applyStereoImager(plan) → widthCurve[4] → per-band width
   └─ applyMaximizer(plan) → targetLUFS → output level, ceilingDBTP → ceiling
   │
8. Each parameter enqueued via processor.enqueueParameterSet(idx, normalizedValue)
   ├─ ParamCommand pushed to SPSC LockFreeQueue (8192 capacity)
   └─ Audio thread drains queue → applies to Ozone 11 via ParameterBridge
   │
9. JSON response returned to AI Agent
   └─ { success: true, params_applied: 42, plan: {...} }
   │
10. AI Agent formats natural language response
    └─ "I've applied a mastering plan: increased loudness to -14 LUFS,
         brightened the high end with a +2dB shelf at 12kHz, and
         added gentle compression with a 2:1 ratio."
```

### 1.5 Ozone 11 Detection and Initialization

Ozone integration is activated when More-Phi detects Ozone 11 as the hosted plugin:

```cpp
// In PluginProcessor.cpp — triggered on plugin load
if (OzoneParameterMap::isOzone11(pendingPluginDesc_.name)) {
    ozoneParamMap_ = std::make_unique<OzoneParameterMap>(
        OzoneParameterMap::buildFromHostedPlugin(paramBridge));
    ozonePlanApplicator_ = std::make_unique<OzonePlanApplicator>(*this, *ozoneParamMap_);
    autoMasteringEngine_.getChainPlanner().setOzonePlanApplicator(ozonePlanApplicator_.get());
}
```

The `OzoneParameterMap` is populated via two methods:
1. **Manual audit**: `python scripts/audit_ozone_params.py` — matches 63 pattern rules against Ozone 11 parameter names
2. **Auto-discovery**: `buildFromHostedPlugin()` scans hosted plugin parameter names for patterns like "EQ Band N Frequency", "Dynamics Threshold", "Imager Sub Width", "Maximizer Ceiling"

Until the audit is complete, all indices default to `-1` and parameter changes are silently skipped (safe no-op behavior).

---

## 2. MCP Tool Definitions for Track Assistant Functionality

More-Phi exposes **50+ MCP tools** organized into functional categories. Below are the tools directly relevant to Ozone Track Assistant integration.

### 2.1 Core Ozone Mastering Tools

#### `get_mastering_state`

Returns current LUFS meters, Ozone hosting status, and key parameter values.

**Purpose**: Track Assistant needs real-time metering data to assess the current state of the track.

**Input Schema**:
```json
{
  "type": "object",
  "properties": {}
}
```

**Response**:
```json
{
  "integrated_lufs": -18.5,
  "short_term_lufs": -16.2,
  "peak_db": -0.3,
  "ozone_hosted": true,
  "ozone_applicator_active": true,
  "ozone_parameters": {
    "eq_gain_high": 0.0,
    "dynamics_threshold": -24.0,
    "maximizer_ceiling": -1.0
  }
}
```

#### `apply_mastering_plan`

Runs ChainPlanExecutor with provided analysis metrics and applies the resulting plan to Ozone 11.

**Purpose**: The primary Track Assistant tool — takes compact analysis inputs and produces a complete mastering chain.

**Input Schema**:
```json
{
  "type": "object",
  "properties": {
    "genre_index": { "type": "integer", "minimum": 0, "maximum": 11 },
    "dynamic_range": { "type": "number", "description": "Dynamic range in LU" },
    "spectral_tilt": { "type": "number", "description": "Spectral tilt in dB/octave" },
    "correlation_ms": { "type": "number", "description": "M/S correlation [-1, 1]" }
  }
}
```

**Response**:
```json
{
  "success": true,
  "params_applied": 42,
  "plan": {
    "compression_need": 0.35,
    "use_neural_comp": false,
    "eq_prescription": "{\"bands\":[...]}",
    "width_curve": [0.0, 0.6, 1.0, 1.4],
    "target_lufs": -14.0,
    "ceiling_dbtp": -1.0,
    "exciter_enabled": false,
    "valid": true
  }
}
```

### 2.2 EQ Assistant Tools (Natural Language EQ Control)

These tools enable AI-driven EQ adjustments through natural language descriptions.

#### `eq_suggest`

Get EQ suggestions from the AI assistant based on track analysis.

**Input Schema**:
```json
{
  "type": "object",
  "properties": {
    "description": { "type": "string", "description": "Natural language EQ goal, e.g. 'brighten the vocals'" },
    "context_window_seconds": { "type": "number", "default": 3.0 }
  }
}
```

#### `eq_adjust`

Apply AI-recommended EQ changes.

**Input Schema**:
```json
{
  "type": "object",
  "properties": {
    "description": { "type": "string" },
    "preview": { "type": "boolean", "default": false }
  }
}
```

#### `eq_preview`

Preview EQ changes without applying them.

**Input Schema**:
```json
{
  "type": "object",
  "properties": {
    "description": { "type": "string" }
  }
}
```

#### `eq_apply` / `eq_reject`

Apply or reject the pending EQ prescription.

#### `eq_validate`

Validate EQ changes against safety constraints (e.g., no excessive boost, phase coherence).

#### `eq_context` / `eq_reset_context`

Manage the EQ assistant's analysis context window.

### 2.3 Mastering Workflow Tools

#### `mastering.plan_preview`

Generate a mastering plan without applying it — useful for Track Assistant to show the user what changes would be made.

**Input Schema**:
```json
{
  "type": "object",
  "properties": {
    "genre_index": { "type": "integer" },
    "dynamic_range": { "type": "number" },
    "spectral_tilt": { "type": "number" },
    "correlation_ms": { "type": "number" }
  }
}
```

#### `mastering.apply_plan`

Alias for `apply_mastering_plan`.

#### `mastering.render_batch`

Create dry-run mastering candidates or start an offline file-backed hosted-plugin render job.

**Input Schema**:
```json
{
  "type": "object",
  "properties": {
    "candidate_count": { "type": "integer", "default": 3 },
    "dry_run": { "type": "boolean", "default": true },
    "input_path": { "type": "string" },
    "output_path": { "type": "string" },
    "plugin_path": { "type": "string" },
    "genre_index": { "type": "integer" },
    "dynamic_range": { "type": "number" },
    "spectral_tilt": { "type": "number" },
    "correlation_ms": { "type": "number" }
  }
}
```

#### `mastering.render_status`

Poll an offline mastering render job.

**Input Schema**:
```json
{
  "type": "object",
  "properties": {
    "job_id": { "type": "string" }
  },
  "required": ["job_id"]
}
```

#### `mastering.select_candidate`

Select a mastering candidate from a render batch.

### 2.4 Hosted Plugin Workflow Tools

#### `hosted_plugin.load`

Load Ozone 11 (or any VST3 plugin) as the hosted plugin.

**Input Schema**:
```json
{
  "type": "object",
  "properties": {
    "plugin_path": { "type": "string", "description": "Full path to .vst3 bundle" }
  }
}
```

#### `hosted_plugin.parameters`

List all Ozone 11 parameters with stable IDs, normalized values, and metadata.

**Response** (abbreviated):
```json
[
  {
    "id": 0,
    "index": 0,
    "stableId": "eq_band_1_freq",
    "name": "EQ Band 1 Frequency",
    "value": 0.5,
    "displayValue": "894 Hz",
    "label": "Hz",
    "discrete": false,
    "boolean": false,
    "numSteps": 0,
    "defaultValue": 0.5
  }
]
```

#### `hosted_plugin.set_parameters`

Batch-set Ozone 11 parameters.

**Input Schema**:
```json
{
  "type": "object",
  "properties": {
    "parameters": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "stableId": { "type": "string" },
          "index": { "type": "integer" },
          "name": { "type": "string" },
          "value": { "type": "number", "minimum": 0.0, "maximum": 1.0 }
        }
      }
    }
  }
}
```

#### `hosted_plugin.capture_state`

Capture Ozone 11's current parameter state into a More-Phi snapshot slot.

### 2.5 Plugin Profile Tools

#### `plugin_profile.audit_parameters`

Audit hosted plugin parameters into a versioned profile JSON object. Essential for Ozone 11 parameter mapping.

#### `plugin_profile.get` / `plugin_profile.save`

Load or save a plugin profile by ID.

### 2.6 Analysis Tools

#### `analysis.get_summary`

Return compact More-Phi-owned deterministic DSP metering data, including methodology metadata, model status, measurements, and warnings.

#### `analysis.capture_window`

Return rolling min/max/mean/p10/p50/p90 meter statistics for a requested window length. If no rolling samples exist yet, the response returns `success: false`, `error: "no_window_samples"`, and the current instantaneous snapshot.

#### `analysis.compare_render`

Compare two analysis summaries or compare a provided summary to current meters.

### 2.7 Extended AI Tools (MCPToolsExtended)

| Tool | Purpose |
|------|---------|
| `analyze_parameters` | AI-friendly parameter analysis with categories, importance scores, descriptions |
| `expose_parameters` | Control which Ozone parameters are visible to AI (Learn Mode) |
| `get_token_estimate` | Estimate token/cost before making changes |
| `set_parameters_optimized` | Set parameters with automatic token optimization |
| `get_morph_compatibility` | Analyze compatibility between two snapshots |
| `get_parameter_categories` | Get parameters organized by category (EQ, Dynamics, Imager, Maximizer) |
| `learn_from_adjustment` | Record parameter importance for Learn Mode |
| `get_learn_mode_status` | Get current Learn Mode configuration |
| `get_discrete_parameters` | Get non-interpolatable parameters (e.g., Ozone filter types) |
| `get_usage_stats` | AI usage statistics and cost tracking |
| `set_token_budget` | Configure token/cost budget limits |

---

## 3. JSON-RPC Schema Definitions

### 3.1 JSON-RPC 2.0 Message Format

All MCP communication uses JSON-RPC 2.0 over TCP with newline-delimited messages:

**Request**:
```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "apply_mastering_plan",
    "arguments": {
      "genre_index": 0,
      "dynamic_range": 12.5,
      "spectral_tilt": -2.3,
      "correlation_ms": 0.15
    }
  },
  "id": 1
}
```

**Success Response**:
```json
{
  "jsonrpc": "2.0",
  "result": {
    "success": true,
    "params_applied": 42,
    "plan": { ... }
  },
  "id": 1
}
```

**Error Response**:
```json
{
  "jsonrpc": "2.0",
  "error": {
    "code": -32600,
    "message": "Unauthorized: invalid bearer_token"
  },
  "id": 1
}
```

### 3.2 Tool Registration Schema (`tools/list`)

Each tool is registered with a name, description, and input schema:

```json
{
  "tools": [
    {
      "name": "apply_mastering_plan",
      "description": "Generate and apply a mastering plan from compact analysis metrics.",
      "inputSchema": {
        "type": "object",
        "properties": {
          "genre_index": { "type": "integer", "minimum": 0, "maximum": 11 },
          "dynamic_range": { "type": "number" },
          "spectral_tilt": { "type": "number" },
          "correlation_ms": { "type": "number" }
        }
      }
    }
  ]
}
```

### 3.3 MultiEffectPlan Schema (Heuristic Rule Output)

The `MultiEffectPlan` struct is the core output of the 5-step heuristic rule planner. Its JSON representation:

```json
{
  "valid": true,
  "compression_need": 0.35,
  "use_neural_comp": false,
  "eq_prescription": "{\"bands\":[{\"band\":1,\"freq\":894,\"gain_db\":0.0,\"q\":1.0,\"type\":\"peak\",\"enabled\":true},{\"band\":2,\"freq\":2500,\"gain_db\":2.0,\"q\":0.7,\"type\":\"peak\",\"enabled\":true},{\"band\":8,\"freq\":12000,\"gain_db\":1.5,\"q\":0.5,\"type\":\"highshelf\",\"enabled\":true}]}",
  "width_curve": [0.0, 0.6, 1.0, 1.4],
  "target_lufs": -14.0,
  "ceiling_dbtp": -1.0,
  "exciter_enabled": false,
  "planner_type": "heuristic_rule_engine",
  "rule_version": "mastering_rules_v1",
  "score_available": false,
  "score_basis": "not_scored_without_audio_render",
  "confidence": null
}
```

**Field Definitions**:

| Field | Type | Range | Description |
|-------|------|-------|-------------|
| `valid` | bool | — | Plan is complete and safe to apply |
| `compression_need` | float | [0, 1] | 0 = gentle, 1 = aggressive compression |
| `use_neural_comp` | bool | — | Kept for compatibility; false while the neural compressor backend is not loaded |
| `eq_prescription` | string (JSON) | — | 8-band EQ prescription (see below) |
| `width_curve` | float[4] | [0, 2] | Stereo width for sub/low/mid/high bands |
| `target_lufs` | float | [-24, -6] | Target integrated LUFS |
| `ceiling_dbtp` | float | [-3, 0] | True-peak ceiling in dBTP |
| `exciter_enabled` | bool | — | Enable harmonic exciter stage |
| `planner_type` | string | — | `heuristic_rule_engine` |
| `score_available` | bool | — | False until an audio render has been evaluated |
| `score_basis` | string | — | Reason the legacy numeric score should not be treated as authoritative |

### 3.4 EQ Prescription JSON Format

The `eq_prescription` field contains a JSON string describing an 8-band EQ:

```json
{
  "bands": [
    {
      "band": 1,
      "freq": 894,
      "gain_db": 0.0,
      "q": 1.0,
      "type": "peak",
      "enabled": true
    },
    {
      "band": 8,
      "freq": 12000,
      "gain_db": 1.5,
      "q": 0.5,
      "type": "highshelf",
      "enabled": true
    }
  ]
}
```

**Supported filter types**: `"lowshelf"`, `"peak"`, `"highshelf"`, `"highpass"`, `"lowpass"`

**Normalization for Ozone 11**:
- Frequency: `normalizeFreq(hz, 20, 20000)` — log₂ scale
- Gain: `normalizeGain(dB, -18, +18)` — linear mapping
- Q: passed directly (normalized by Ozone internally)
- Filter type: `encodeFilterType("peak")` → 0.0=lowshelf, 0.25=peak, 0.5=highshelf, 0.75=highpass, 1.0=lowpass

### 3.5 Normalization Conventions

All values delivered to Ozone 11 parameters are VST3 normalized [0..1]:

| Parameter | Normalization Function | Input Range | Output Range |
|-----------|----------------------|-------------|--------------|
| Gain (dB) | `normalizeGain(dB, -18, +18)` | [-18, +18] dB | [0, 1] |
| Frequency (Hz) | `normalizeFreq(hz, 20, 20000)` | [20, 20000] Hz (log₂) | [0, 1] |
| Threshold (dBFS) | `normalizeThreshold(dBFS, -60, 0)` | [-60, 0] dBFS | [0, 1] |
| Width | `normalizeWidth(w)` | [0, 2] | [0, 1] |
| LUFS Target | `normalizeLUFS(lufs, -24, -6)` | [-24, -6] LUFS | [0, 1] |
| Ceiling (dBTP) | `normalizeCeiling(dBTP, -3, 0)` | [-3, 0] dBTP | [0, 1] |

### 3.6 Error Codes

| Code | Meaning | Context |
|------|---------|---------|
| -32700 | Parse error | Invalid JSON in request |
| -32600 | Invalid request | Unauthorized, bad method name |
| -32601 | Method not found | Unknown tool name |
| -32602 | Invalid params | Missing required fields, type mismatch |
| -32603 | Internal error | Exception in tool handler |
| -32000 | Rate limit exceeded | TokenOptimizer slot exhausted |
| 100 | Plugin not loaded | No hosted plugin detected |
| 101 | Invalid parameter index | Index out of range |
| 102 | Invalid slot index | Snapshot slot 0-11 required |
| 103 | Snapshot slot empty | Recall attempted on empty slot |

### 3.7 `tools/call` Envelope Structure

For MCP Spec 2024-11-05 compatibility, tool calls use the `tools/call` method:

```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "eq_adjust",
    "arguments": {
      "description": "brighten the vocals",
      "preview": true
    }
  },
  "id": 2
}
```

The response wraps the tool result in a structured content envelope:

```json
{
  "jsonrpc": "2.0",
  "result": {
    "content": [
      {
        "type": "text",
        "text": "{\"success\":true,\"eq_changes\":[...]}"
      }
    ],
    "structuredContent": {
      "success": true,
      "eq_changes": [...]
    },
    "isError": false
  },
  "id": 2
}
```

---

## 4. Authentication Layer

### 4.1 InstanceIdentity Structure

Each More-Phi plugin instance generates a cryptographically secure identity on initialization:

```cpp
struct InstanceIdentity {
    juce::String instanceId;      // 32-char hex string (e.g., "a1b2c3d4...")
    juce::String morphCode;       // 8-char short code (e.g., "MRPH-42A7")
    int port;                     // Dynamic port (base 30001, up to 64 instances)
    juce::String bearerToken;     // 32-char hex secret
    juce::String createdAt;       // ISO 8601 timestamp
};
```

**Generation**: Uses platform CSPRNG:
- Windows: `BCryptGenRandom`
- macOS: `SecRandomCopyBytes`
- Linux: `getrandom()`

**Security**: Tokens are zeroized via `SecureZeroMemory` (Windows) or `explicit_bzero` (POSIX) before clearing.

### 4.2 Port Allocation

The `InstanceRegistry` singleton manages port allocation across up to 64 concurrent instances:

```cpp
// Port allocation strategy
BASE_PORT = 30001
MAX_INSTANCES = 64
// Fallback range: IANA dynamic ports 49152-65535

// Algorithm:
// 1. Try BASE_PORT + instanceIndex
// 2. Verify port availability via socket probe
// 3. If occupied, scan dynamic range for free port
// 4. Return -1 if no port available (max instances reached)
```

**TTL-based zombie eviction**: `InstanceRegistry` evicts stale entries after TTL expiry to prevent port leaks from crashed or ungracefully terminated instances.

### 4.3 Bearer Token Authentication Flow

**Step 1: Client sends `initialize` request**

```json
{
  "jsonrpc": "2.0",
  "method": "initialize",
  "params": {
    "bearer_token": "a1b2c3d4e5f6..."
  },
  "id": 0
}
```

**Step 2: Server validates token**

The `validateAuth()` method performs a **constant-time comparison** to prevent timing attacks:

```cpp
bool MCPServer::validateAuth(const juce::var& params) {
    auto token = params.getProperty("bearer_token", "").toString();
    if (token.isEmpty()) return false;

    const std::string candidate = token.toStdString();
    const std::string expected  = identity_.bearerToken.toStdString();

    // Constant-time comparison:
    // Always compare to the longer of the two strings.
    // If lengths differ, XOR extra bytes with 0xFF so the loop
    // runs the same number of iterations regardless.
    const size_t compareLen = std::max(candidate.size(), expected.size());

    volatile uint8_t diff = 0;
    for (size_t i = 0; i < compareLen; ++i) {
        const uint8_t c = (i < candidate.size()) ? static_cast<uint8_t>(candidate[i]) : 0xFF;
        const uint8_t e = (i < expected.size())  ? static_cast<uint8_t>(expected[i])  : 0xFF;
        diff = static_cast<uint8_t>(diff | (c ^ e));
    }

    const volatile uint8_t result = diff;
    return result == 0;
}
```

> **Implementation note**: This loop is length-independent and uses `volatile` to prevent compiler optimization. Do not replace with `memcmp` or string equality — both leak timing information.

**Step 3: Server responds with identity**

On success:
```json
{
  "jsonrpc": "2.0",
  "result": {
    "serverInfo": { "name": "More-Phi MCP", "version": "1.0" },
    "capabilities": { "tools": { "listChanged": false } },
    "instanceId": "a1b2c3d4e5f6...",
    "morphCode": "MRPH-42A7",
    "port": 30001
  },
  "id": 0
}
```

On failure:
```json
{
  "jsonrpc": "2.0",
  "error": {
    "code": -32600,
    "message": "Unauthorized: invalid bearer_token"
  },
  "id": 0
}
```

**Step 4: Subsequent requests require prior authentication**

All non-`initialize` methods are rejected if the connection hasn't been authenticated:

```json
{
  "jsonrpc": "2.0",
  "error": {
    "code": -32600,
    "message": "Unauthorized: call initialize with bearer_token first"
  },
  "id": 1
}
```

### 4.4 Rate Limiting via TokenOptimizer

Every authenticated request consumes a rate-limit slot:

```cpp
if (!processor_.getTokenOptimizer().tryConsumeRequestSlot())
    return errResponse(-32000, "Rate limit exceeded");
```

**TokenOptimizer configuration**:

| Budget Type | Default | Purpose |
|-------------|---------|---------|
| Per-request tokens | Model-dependent | Claude 3.5 Sonnet: $3/M input, $15/M output |
| Per-session tokens | Configurable | Total session budget |
| Per-minute requests | Configurable | Throughput cap |

**LLM Cost Models**:

| Model | Input Cost | Output Cost |
|-------|-----------|-------------|
| Claude 3.5 Sonnet | $3/M tokens | $15/M tokens |
| GPT-4 Turbo | $10/M tokens | $30/M tokens |
| GPT-3.5 Turbo | $0.50/M tokens | $1.50/M tokens |
| Local LLM | $0 | $0 |

**Parameter compression strategies**:
- `Immediate` — send all parameters
- `Debounce100ms` — coalesce rapid changes
- `Debounce500ms` — batch slower changes
- `OnSnapshot` — send on snapshot change
- `Manual` — explicit send only

### 4.5 Connection Security

| Property | Value |
|----------|-------|
| Allowed hosts | `127.0.0.1` only (localhost) |
| Max concurrent clients | 4 per instance |
| Max request size | 256 KB |
| Non-local connections | Rejected immediately |
| Connection timeout | 500ms for socket readiness |
| Idle timeout | 30 seconds (connections closed after inactivity) |
| Error recovery | Auto-retry on bind (3 attempts), consecutive error tracking |

### 4.6 Per-Instance Auth Isolation

With up to 64 concurrent More-Phi instances, each instance has:
- Unique port
- Unique bearer token
- Unique `instanceId` and `morphCode`

This prevents cross-instance authentication attacks and ensures AI agents connect to the intended plugin instance.

---

## 5. Example Implementation: End-to-End Tool Call Workflow

### 5.1 Python MCP Client Library

```python
"""
More-Phi MCP Client — Python reference implementation
Connects to a More-Phi instance, authenticates, and calls Track Assistant tools.
"""
import socket
import json
import time
from typing import Optional

class MorePhiMCPClient:
    def __init__(self, host: str = "127.0.0.1", port: int = 30001, bearer_token: str = ""):
        self.host = host
        self.port = port
        self.bearer_token = bearer_token
        self.authenticated = False
        self._request_id = 0

    def _connect(self) -> socket.socket:
        """Create a new TCP connection for a single request-response cycle."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5.0)
        sock.connect((self.host, self.port))
        return sock

    def _send_request(self, sock: socket.socket, method: str, params: dict = None) -> dict:
        """Send a JSON-RPC request and receive the response."""
        self._request_id += 1

        if method == "initialize":
            request = {
                "jsonrpc": "2.0",
                "method": method,
                "params": {"bearer_token": self.bearer_token},
                "id": self._request_id
            }
        elif method == "tools/call":
            request = {
                "jsonrpc": "2.0",
                "method": method,
                "params": {
                    "name": params.get("name"),
                    "arguments": params.get("arguments", {})
                },
                "id": self._request_id
            }
        else:
            request = {
                "jsonrpc": "2.0",
                "method": method,
                "params": params or {},
                "id": self._request_id
            }

        message = json.dumps(request) + "\n"
        sock.sendall(message.encode("utf-8"))

        # Read response (handle newline-delimited streaming)
        buffer = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buffer += chunk
            if b"\n" in buffer:
                break

        response_text = buffer.decode("utf-8").strip()
        return json.loads(response_text)

    def initialize(self) -> dict:
        """Authenticate and get server identity."""
        sock = self._connect()
        try:
            response = self._send_request(sock, "initialize")
            if "result" in response:
                self.authenticated = True
                return response["result"]
            else:
                raise ConnectionError(f"Auth failed: {response.get('error', {}).get('message')}")
        finally:
            sock.close()

    def call_tool(self, tool_name: str, arguments: dict = None) -> dict:
        """Call an MCP tool and return the structured result."""
        if not self.authenticated:
            raise RuntimeError("Not authenticated. Call initialize() first.")

        sock = self._connect()
        try:
            response = self._send_request(sock, "tools/call", {
                "name": tool_name,
                "arguments": arguments or {}
            })

            if "error" in response:
                error = response["error"]
                raise RuntimeError(f"MCP Error {error['code']}: {error['message']}")

            # Unwrap tools/call envelope
            result = response.get("result", {})
            if "structuredContent" in result:
                return result["structuredContent"]
            return result
        finally:
            sock.close()

    def tools_list(self) -> list:
        """List all available tools."""
        sock = self._connect()
        try:
            response = self._send_request(sock, "tools/list")
            return response.get("result", {}).get("tools", [])
        finally:
            sock.close()
```

### 5.2 Complete Workflow: "Make This Track Louder and Brighter"

```python
"""
End-to-end example: AI agent identifies user intent, selects the appropriate
Track Assistant tool, and processes the response into natural language.
"""

def main():
    # ── Step 1: Connect and authenticate ──────────────────────────────────
    BEARER_TOKEN = "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4"  # From plugin UI status panel

    client = MorePhiMCPClient(bearer_token=BEARER_TOKEN)

    print("Connecting to More-Phi MCP server...")
    identity = client.initialize()
    print(f"Connected to instance: {identity['instanceId']}")
    print(f"Morph code: {identity['morphCode']}")
    print(f"Port: {identity['port']}")

    # ── Step 2: Verify Ozone 11 is loaded ─────────────────────────────────
    plugin_info = client.call_tool("get_plugin_info")
    hosted = plugin_info.get("hostedPlugin")

    if not hosted:
        print("ERROR: No plugin is hosted. Load Ozone 11 first.")
        return

    print(f"\nHosted plugin: {hosted['name']}")
    print(f"Parameters: {hosted['paramCount']}")

    if "ozone" not in hosted["name"].lower() or "11" not in hosted["name"]:
        print("WARNING: Ozone 11 is not loaded. Track Assistant requires Ozone 11.")
        print("Load Ozone 11 via: hosted_plugin.load(plugin_path='.../Ozone 11.vst3')")
        return

    # ── Step 3: Get current mastering state (metering data) ───────────────
    print("\nAnalyzing current track state...")
    mastering_state = client.call_tool("get_mastering_state")

    integrated_lufs = mastering_state["integrated_lufs"]
    short_term_lufs = mastering_state["short_term_lufs"]
    peak_db = mastering_state["peak_db"]
    ozone_active = mastering_state["ozone_applicator_active"]

    print(f"  Integrated LUFS:  {integrated_lufs:.1f}")
    print(f"  Short-term LUFS:  {short_term_lufs:.1f}")
    print(f"  Peak:             {peak_db:.1f} dBTP")
    print(f"  Ozone applicator: {'Active' if ozone_active else 'Inactive'}")

    if not ozone_active:
        print("\nERROR: Ozone Plan Applicator is not active.")
        print("Run: python scripts/audit_ozone_params.py to map Ozone 11 parameters.")
        return

    # ── Step 4: AI Agent analyzes user intent ─────────────────────────────
    # In a real implementation, this would be an LLM call.
    # For this example, we simulate the AI's reasoning:
    #
    # User input: "Make this track louder and brighter"
    #
    # AI Agent reasoning:
    # 1. "Louder" → increase target LUFS from current -18.5 to -14.0 (genre-agnostic pop target)
    # 2. "Brighter" → positive spectral tilt → boost high frequencies in EQ prescription
    # 3. Current dynamic range: ~12 LU (moderate) → light compression needed
    # 4. Stereo: no specific request → keep current width curve

    genre_index = 0       # Pop (from GenreClassifier or user selection)
    dynamic_range = 12.5  # LU (measured from analysis)
    spectral_tilt = -2.3  # dB/oct (negative = dark, positive = bright)
    correlation_ms = 0.15 # M/S correlation (slightly correlated)

    print(f"\nAI Analysis:")
    print(f"  Genre:          Pop (index {genre_index})")
    print(f"  Dynamic Range:  {dynamic_range} LU")
    print(f"  Spectral Tilt:  {spectral_tilt} dB/oct")
    print(f"  M/S Correlation: {correlation_ms}")

    # ── Step 5: Preview the mastering plan (optional, for user approval) ──
    print("\nPreviewing mastering plan...")
    preview = client.call_tool("mastering.plan_preview", {
        "genre_index": genre_index,
        "dynamic_range": dynamic_range,
        "spectral_tilt": spectral_tilt,
        "correlation_ms": correlation_ms
    })

    if not preview.get("valid"):
        print("ERROR: Generated mastering plan is invalid.")
        return

    plan = preview.get("plan", {})
    print(f"  Compression need:  {plan.get('compression_need', 0):.2f}")
    print(f"  Target LUFS:       {plan.get('target_lufs', -14):.1f}")
    print(f"  Ceiling:           {plan.get('ceiling_dbtp', -1):.1f} dBTP")
    print(f"  Exciter:           {'On' if plan.get('exciter_enabled') else 'Off'}")

    # Parse EQ prescription
    eq_json_str = plan.get("eq_prescription", "{}")
    eq_data = json.loads(eq_json_str)
    eq_bands = eq_data.get("bands", [])
    active_bands = [b for b in eq_bands if b.get("enabled")]
    print(f"  Active EQ bands:   {len(active_bands)}")
    for band in active_bands[:3]:  # Show first 3
        print(f"    Band {band['band']}: {band['freq']:.0f} Hz, {band['gain_db']:+.1f} dB, {band['type']}")
    if len(active_bands) > 3:
        print(f"    ... and {len(active_bands) - 3} more bands")

    # ── Step 6: Apply the mastering plan to Ozone 11 ──────────────────────
    print("\nApplying mastering plan to Ozone 11...")
    result = client.call_tool("apply_mastering_plan", {
        "genre_index": genre_index,
        "dynamic_range": dynamic_range,
        "spectral_tilt": spectral_tilt,
        "correlation_ms": correlation_ms
    })

    if not result.get("success"):
        print(f"ERROR: Failed to apply mastering plan: {result.get('error')}")
        return

    params_applied = result.get("params_applied", 0)
    print(f"SUCCESS: Applied {params_applied} parameter changes to Ozone 11.")

    # ── Step 7: AI Agent formats natural language response ────────────────
    compression = plan.get("compression_need", 0)
    target_lufs = plan.get("target_lufs", -14)
    lufs_improvement = target_lufs - integrated_lufs

    # Generate EQ summary
    high_freq_bands = [b for b in active_bands if b["freq"] > 5000]
    high_freq_boost = sum(b["gain_db"] for b in high_freq_bands)

    response = generate_ai_response(
        compression=compression,
        lufs_improvement=lufs_improvement,
        target_lufs=target_lufs,
        high_freq_boost=high_freq_boost,
        high_freq_band_count=len(high_freq_bands),
        plan=plan
    )
    print(f"\n{'='*60}")
    print(response)
    print(f"{'='*60}")


def generate_ai_response(compression, lufs_improvement, target_lufs,
                          high_freq_boost, high_freq_band_count, plan):
    """
    AI Agent formats a natural language response from the mastering plan.
    In production, this would be generated by an LLM. Here we use a template.
    """
    parts = []

    # Loudness summary
    if lufs_improvement > 0:
        parts.append(
            f"I've increased the overall loudness by {lufs_improvement:.1f} LU, "
            f"targeting {target_lufs:.1f} LUFS integrated."
        )
    else:
        parts.append(
            f"The track is already at {target_lufs:.1f} LUFS. "
            f"No loudness changes were needed."
        )

    # Brightness summary (EQ high-frequency boost)
    if high_freq_boost > 0:
        parts.append(
            f"To brighten the sound, I boosted {high_freq_band_count} high-frequency "
            f"EQ bands by a total of +{high_freq_boost:.1f} dB above 5 kHz."
        )
    elif high_freq_boost < 0:
        parts.append(
            f"I reduced high-frequency content by {-high_freq_boost:.1f} dB "
            f"to tame harshness."
        )

    # Compression summary
    if compression > 0.5:
        parts.append(
            f"Moderate-to-heavy compression was applied (need: {compression:.2f}) "
            f"to control dynamics and increase perceived loudness."
        )
    elif compression > 0.2:
        parts.append(
            f"Light compression was applied (need: {compression:.2f}) "
            f"for gentle dynamic control."
        )
    else:
        parts.append(
            f"No compression was needed (need: {compression:.2f}) — "
            f"the track's dynamics are already well-controlled."
        )

    # Stereo width
    width_curve = plan.get("width_curve", [0, 0.6, 1.0, 1.4])
    avg_width = sum(width_curve) / len(width_curve)
    if avg_width > 1.0:
        parts.append(
            f"Stereo width was increased to an average of {avg_width:.1f}x "
            f"for a wider image."
        )

    return " ".join(parts)


if __name__ == "__main__":
    main()
```

### 5.3 Error Handling and Retry Logic

```python
import socket
import json
import time
from typing import Optional

def robust_mcp_call(client: MorePhiMCPClient, tool_name: str, arguments: dict,
                    max_retries: int = 3, backoff_ms: int = 500) -> dict:
    """
    Call an MCP tool with retry logic for transient failures.
    Handles: connection errors, rate limits, queue full.
    """
    last_error = None

    for attempt in range(max_retries):
        try:
            result = client.call_tool(tool_name, arguments)

            # Check for queue_full error (audio queue saturated)
            if result.get("error") == "queue_full":
                wait_ms = backoff_ms * (2 ** attempt)  # Exponential backoff
                print(f"  Queue full, retrying in {wait_ms}ms...")
                time.sleep(wait_ms / 1000.0)
                continue

            return result

        except ConnectionError as e:
            last_error = e
            if attempt < max_retries - 1:
                wait_ms = backoff_ms * (2 ** attempt)
                print(f"  Connection error, reconnecting in {wait_ms}ms...")
                time.sleep(wait_ms / 1000.0)
                # Re-initialize connection
                client.authenticated = False
                client.initialize()
            else:
                raise RuntimeError(f"Connection failed after {max_retries} attempts: {e}")

        except RuntimeError as e:
            error_msg = str(e)
            if "Rate limit exceeded" in error_msg:
                print(f"  Rate limited, waiting 60s...")
                time.sleep(60)
                continue
            elif "MCP Error" in error_msg:
                # Non-retryable MCP error (invalid params, unknown tool, etc.)
                raise
            else:
                last_error = e
                if attempt < max_retries - 1:
                    time.sleep(backoff_ms / 1000.0)
                else:
                    raise

    raise RuntimeError(f"Tool call failed after {max_retries} retries: {last_error}")
```

### 5.4 Node.js MCP Client (Alternative)

```javascript
const net = require('net');

class MorePhiMCPClient {
    constructor(host = '127.0.0.1', port = 30001, bearerToken = '') {
        this.host = host;
        this.port = port;
        this.bearerToken = bearerToken;
        this.authenticated = false;
        this.requestId = 0;
    }

    async initialize() {
        return this._sendRequest('initialize', { bearer_token: this.bearerToken })
            .then(result => {
                this.authenticated = true;
                return result;
            });
    }

    async callTool(toolName, arguments = {}) {
        if (!this.authenticated) {
            throw new Error('Not authenticated. Call initialize() first.');
        }
        return this._sendRequest('tools/call', {
            name: toolName,
            arguments: arguments
        });
    }

    async _sendRequest(method, params) {
        return new Promise((resolve, reject) => {
            const client = new net.Socket();
            this.requestId++;

            const request = {
                jsonrpc: '2.0',
                method: method,
                params: params || {},
                id: this.requestId
            };

            client.connect(this.port, this.host, () => {
                client.write(JSON.stringify(request) + '\n');
            });

            let data = '';
            client.on('data', (chunk) => {
                data += chunk;
                if (data.includes('\n')) {
                    client.destroy();
                }
            });

            client.on('close', () => {
                try {
                    const response = JSON.parse(data.trim());
                    if (response.error) {
                        reject(new Error(`MCP Error ${response.error.code}: ${response.error.message}`));
                    } else {
                        const result = response.result || {};
                        // Unwrap tools/call envelope
                        resolve(result.structuredContent || result);
                    }
                } catch (e) {
                    reject(new Error(`Failed to parse response: ${e.message}`));
                }
            });

            client.on('error', reject);

            // Timeout after 5 seconds
            setTimeout(() => {
                client.destroy();
                reject(new Error('Request timed out'));
            }, 5000);
        });
    }
}

// Usage example
async function main() {
    const client = new MorePhiMCPClient('127.0.0.1', 30001, 'your-bearer-token-here');

    try {
        const identity = await client.initialize();
        console.log(`Connected to More-Phi: ${identity.instanceId}`);

        const state = await client.callTool('get_mastering_state');
        console.log(`Integrated LUFS: ${state.integrated_lufs}`);

        const result = await client.callTool('apply_mastering_plan', {
            genre_index: 0,
            dynamic_range: 12.5,
            spectral_tilt: -2.3,
            correlation_ms: 0.15
        });
        console.log(`Applied ${result.params_applied} parameters`);
    } catch (error) {
        console.error(`Error: ${error.message}`);
    }
}

main();
```

---

## 6. Integration Patterns and Best Practices

### 6.1 Ozone 11 Parameter Audit Workflow

Before the AI agent can control Ozone 11, the parameter indices must be mapped:

**Step 1: Load Ozone 11 in More-Phi**

Open your DAW, load More-Phi, and load Ozone 11 as the hosted plugin.

**Step 2: Run the audit script**

```bash
python scripts/audit_ozone_params.py --out-map ozone11_param_map.json
```

The script:
1. Connects to More-Phi MCP server via TCP
2. Authenticates with bearer token
3. Lists all hosted plugin parameters via `list_parameters`
4. Matches parameter names against 63 known Ozone 11 patterns:
   - `"EQ Band (\d+) Frequency"` → `eq[N-1].freqIdx`
   - `"EQ Band (\d+) Gain"` → `eq[N-1].gainIdx`
   - `"EQ Band (\d+) Q"` → `eq[N-1].qIdx`
   - `"EQ Band (\d+) Filter Type"` → `eq[N-1].typeIdx`
   - `"EQ Band (\d+) Enabled"` → `eq[N-1].enabledIdx`
   - `"Dynamics Threshold"` → `dynamics.thresholdIdx`
   - `"Dynamics Ratio"` → `dynamics.ratioIdx`
   - `"Dynamics Attack"` → `dynamics.attackIdx`
   - `"Dynamics Release"` → `dynamics.releaseIdx`
   - `"Imager Sub Width"` → `imager.widthIdx[0]`
   - `"Imager Low Width"` → `imager.widthIdx[1]`
   - `"Imager Mid Width"` → `imager.widthIdx[2]`
   - `"Imager High Width"` → `imager.widthIdx[3]`
   - `"Maximizer Output Level"` → `maximizer.outputLevelIdx`
   - `"Maximizer Ceiling"` → `maximizer.ceilingIdx`

**Step 3: Update the code**

Copy the generated `ozone11_param_map.json` values into `OzoneParameterMap::buildForOzone11()` in `src/AI/OzoneParameterMap.cpp`.

**Step 4: Verify**

Rebuild More-Phi and test with `get_mastering_state` — it should return non-default Ozone parameter values.

### 6.2 Safe Defaults When Indices Are Unmapped

All `OzoneParameterMap` indices default to `-1`. The `OzonePlanApplicator` skips unmapped parameters:

```cpp
int OzonePlanApplicator::enqueueIfMapped(int idx, float normalizedValue) {
    if (idx == -1) return 0;  // Skip unmapped parameters
    return processor_.enqueueParameterSet(idx, normalizedValue,
                                          ParameterEditSource::MCP,
                                          /*holdAgainstMorph=*/false) ? 1 : 0;
}
```

This means:
- The code compiles and runs safely before the audit is complete
- Unmapped parameters are silently skipped (no errors, no crashes)
- Partial audits work correctly — only mapped parameters are controlled
- The AI agent can query `ozone_applicator_active` to check if the full map is populated

### 6.3 Dynamics Mapping Constants

The `OzonePlanApplicator` maps `compressionNeed` [0..1] to Ozone dynamics parameters:

```cpp
// Threshold mapping: compressionNeed [0..1] → threshold dBFS
static constexpr float kThresholdAtMinNeed = -10.0f;  // gentle: -10 dBFS
static constexpr float kThresholdAtMaxNeed = -30.0f;  // aggressive: -30 dBFS

// Ratio mapping: compressionNeed [0..1] → ratio
static constexpr float kRatioAtMinNeed = 1.5f;   // light compression
static constexpr float kRatioAtMaxNeed = 6.0f;   // heavy compression

// Attack/release defaults
static constexpr float kDefaultAttackMs   = 10.0f;
static constexpr float kDefaultReleaseMs  = 100.0f;

// Ozone parameter ranges for normalization
static constexpr float kOzoneRatioMin = 1.0f;
static constexpr float kOzoneRatioMax = 20.0f;
static constexpr float kOzoneAttackMin    =   0.1f;
static constexpr float kOzoneAttackMax    = 100.0f;
static constexpr float kOzoneReleaseMin   =  10.0f;
static constexpr float kOzoneReleaseMax   = 1000.0f;
```

**Mapping formulas**:

```cpp
// Threshold: linear interpolation from -10 dBFS (gentle) to -30 dBFS (aggressive)
float thresholdDB = kThresholdAtMinNeed + compressionNeed *
    (kThresholdAtMaxNeed - kThresholdAtMinNeed);
float normalizedThreshold = normalizeThreshold(thresholdDB, -60.0f, 0.0f);

// Ratio: linear interpolation from 1.5:1 to 6:1
float ratio = kRatioAtMinNeed + compressionNeed *
    (kRatioAtMaxNeed - kRatioAtMinNeed);
float normalizedRatio = (ratio - kOzoneRatioMin) /
    (kOzoneRatioMax - kOzoneRatioMin);
```

### 6.4 Genre LUFS Targets

The `ChainPlanExecutor` uses genre-specific LUFS targets:

```cpp
static constexpr float kGenreLUFS[12] = {
    -9.f,   // 0: Pop
    -9.f,   // 1: Rock
    -11.f,  // 2: Hip-Hop
    -13.f,  // 3: Jazz
    -12.f,  // 4: Electronic
    -16.f,  // 5: Classical
    -17.f,  // 6: Folk/Acoustic
    -20.f,  // 7: Ambient
    -18.f,  // 8: R&B
    -10.f,  // 9: EDM
    -14.f,  // 10: Podcast/Spoken Word
    -23.f   // 11: Dynamic/Streaming (EBU R128)
};
```

### 6.5 Token Budget Management for Cost Control

Use `TokenOptimizer` to manage AI API costs:

```python
# Check token budget before expensive operations
budget = client.call_tool("get_usage_stats")
print(f"Budget remaining: ${budget['budget_remaining_usd']:.2f}")

# Set budget limits
client.call_tool("set_token_budget", {
    "max_cost_usd": 5.0,
    "max_tokens_per_session": 100000,
    "enable_compression": True,
    "prioritize_important_params": True
})

# Get cost estimate before operation
estimate = client.call_tool("get_token_estimate", {
    "operation": "set_parameter",
    "parameter_count": 42
})
print(f"Estimated cost: ${estimate['estimated_cost_usd']:.4f}")
print(f"Within budget: {estimate['within_budget']}")
```

### 6.6 Learn Mode for Parameter Exposure

Learn Mode automatically exposes important Ozone parameters based on user behavior:

```python
# Enable Learn Mode
client.call_tool("set_learn_mode_config", {
    "enabled": True,
    "exposure_threshold": 0.3,
    "auto_learn": True,
    "prioritize_recent": True,
    "max_exposed_parameters": 50
})

# Record that a parameter was important (user adjusted it)
client.call_tool("learn_from_adjustment", {
    "parameter_index": 42,
    "importance_boost": 0.1
})

# Check Learn Mode status
status = client.call_tool("get_learn_mode_status")
print(f"Exposed parameters: {status['exposed_count']}")
print(f"Top parameters by importance:")
for param in status['top_parameters'][:5]:
    print(f"  {param['name']}: importance {param['importance_score']:.2f}")
```

### 6.7 Discrete Parameter Handling

Ozone 11 has discrete parameters (filter types, mode selectors) that cannot be interpolated. The `ParameterClassifier` identifies these:

```python
# Get discrete parameters to avoid during morphing
discrete = client.call_tool("get_discrete_parameters", {
    "include_binary": True,
    "include_enums": True
})
print(f"Discrete parameters: {len(discrete['parameters'])}")
for param in discrete['parameters'][:5]:
    print(f"  {param['name']} (index {param['index']}): {param['discrete_type']}")
```

The `DiscreteParameterHandler` ensures these parameters snap to valid steps during morphing rather than interpolating through invalid intermediate values.

### 6.8 Batch Rendering for Mastering Candidates

Use `mastering.render_batch` to generate multiple mastering candidates:

```python
# Start a dry-run render batch (generates 3 candidate plans without processing audio)
render_result = client.call_tool("mastering.render_batch", {
    "candidate_count": 3,
    "dry_run": True,
    "genre_index": 0,
    "dynamic_range": 12.5,
    "spectral_tilt": -2.3,
    "correlation_ms": 0.15
})

print(f"Render job ID: {render_result['job_id']}")
print(f"Candidates generated: {len(render_result['candidates'])}")

for candidate in render_result['candidates']:
    print(f"  Candidate {candidate['index']}: "
          f"peak={candidate['peak_db']:.1f} dB, "
          f"rms={candidate['rms_db']:.1f} dB")

# Select the best candidate
best_id = render_result['candidates'][0]['id']
client.call_tool("mastering.select_candidate", {
    "candidate_id": best_id
})
```

### 6.9 Multi-Instance Considerations

When running multiple More-Phi instances (e.g., one per track), each instance has its own MCP server:

```python
# Discover all running instances
instances = client.call_tool("list_instances")
print(f"Active instances: {len(instances['instances'])}")

for instance in instances['instances']:
    print(f"  {instance['morphCode']} on port {instance['port']} "
          f"(plugin: {instance['hosted_plugin_name']})")

# Connect to a specific instance
ozone_client = MorePhiMCPClient(
    bearer_token="instance-specific-bearer-token",
    port=30002  # Port for this specific instance
)
ozone_client.initialize()
```

**Best practices for multi-instance**:
- Each instance has a unique bearer token — never share tokens across instances
- Use `list_instances` to discover which instance is hosting Ozone 11
- Route mastering commands to the instance with Ozone 11 loaded
- Track per-instance costs via `get_usage_stats`

### 6.10 Security Checklist

| Check | Status |
|-------|--------|
| Bearer token generated via CSPRNG | ✅ `BCryptGenRandom` / `SecRandomCopyBytes` / `getrandom()` |
| Constant-time token comparison | ✅ Volatile XOR loop, length-independent timing |
| Localhost-only connections | ✅ `127.0.0.1` enforced, non-local rejected |
| Max concurrent clients | ✅ 4 per instance |
| Token zeroization on cleanup | ✅ `SecureZeroMemory` / `explicit_bzero` |
| Rate limiting | ✅ `TokenOptimizer` per-request/per-session/per-minute |
| Max request size | ✅ 256 KB limit |
| Per-instance auth isolation | ✅ Unique tokens, ports, instance IDs |
| No external port exposure | ✅ Firewall rule recommended |

### 6.11 Security Best Practices

- **Cache key isolation**: Cache keys must include the instance ID to prevent cross-instance contamination.
- **Constant-time auth**: Token comparison must be constant-time; do not use `memcmp` or `==` on strings.

### 6.12 Testing Strategy

**Unit tests** exist at `tests/Unit/TestOzoneIntegration.cpp`:

```cpp
// Test coverage:
TEST_CASE("OzoneParameterMap::normalizeGain") {
    REQUIRE(normalizeGain(-18.0f) == Approx(0.0f));
    REQUIRE(normalizeGain(+18.0f) == Approx(1.0f));
    REQUIRE(normalizeGain(0.0f)   == Approx(0.5f));
}

TEST_CASE("OzoneParameterMap::normalizeFreq") {
    REQUIRE(normalizeFreq(20.0f)    == Approx(0.0f));
    REQUIRE(normalizeFreq(20000.0f) == Approx(1.0f));
    REQUIRE(normalizeFreq(894.0f)   == Approx(0.5f).margin(0.01));
}

TEST_CASE("OzonePlanApplicator all-minus-one map is a safe no-op") {
    OzoneParameterMap emptyMap;  // All indices = -1
    OzonePlanApplicator applicator(processor, emptyMap);
    MultiEffectPlan plan;
    REQUIRE(applicator.apply(plan) == 0);  // No params applied
}
```

**Integration testing** workflow:
1. Load Ozone 11 in More-Phi
2. Run audit script to populate parameter map
3. Call `get_mastering_state` — verify non-default values
4. Call `apply_mastering_plan` — verify parameters change
5. Listen to audio output — verify changes are audible
6. Call `get_mastering_state` again — verify metering reflects changes

---

## Appendix A: Complete Tool Reference

| Tool | Category | Parameters | Description |
|------|----------|-----------|-------------|
| `initialize` | Auth | `bearer_token` | Authenticate and get identity |
| `tools/list` | Discovery | — | List all available tools |
| `get_plugin_info` | Core | — | Get More-Phi and hosted plugin info |
| `list_parameters` | Core | — | List hosted plugin parameters |
| `get_parameter` | Core | `stableId`, `index`, `name` | Read a single parameter |
| `set_parameter` | Core | `stableId`, `index`, `name`, `value` | Queue a parameter change |
| `set_parameters_batch` | Core | `parameters[]` | Batch parameter changes |
| `capture_snapshot` | Core | `slot`, `includeState` | Capture state to snapshot slot |
| `recall_snapshot` | Core | `slot`, `mode` | Recall a snapshot |
| `set_morph_position` | Core | `x`, `y`, `fader`, `source` | Set morph cursor position |
| `get_morph_state` | Core | — | Get morph state |
| `get_mastering_state` | Ozone | — | LUFS meters, Ozone status |
| `apply_mastering_plan` | Ozone | `genre_index`, `dynamic_range`, `spectral_tilt`, `correlation_ms` | Apply 5-step heuristic rule plan |
| `mastering.plan_preview` | Ozone | same as above | Preview plan without applying |
| `mastering.render_batch` | Ozone | `candidate_count`, `dry_run`, `input_path`, `output_path`, ... | Offline render batch |
| `mastering.render_status` | Ozone | `job_id` | Poll render job |
| `mastering.select_candidate` | Ozone | `candidate_id` | Select render candidate |
| `eq_suggest` | EQ Assistant | `description`, `context_window_seconds` | Get EQ suggestions |
| `eq_adjust` | EQ Assistant | `description`, `preview` | Apply EQ changes |
| `eq_preview` | EQ Assistant | `description` | Preview EQ changes |
| `eq_apply` | EQ Assistant | — | Apply pending EQ |
| `eq_reject` | EQ Assistant | — | Reject pending EQ |
| `eq_validate` | EQ Assistant | — | Validate EQ safety |
| `eq_context` | EQ Assistant | — | Get analysis context |
| `eq_reset_context` | EQ Assistant | — | Reset analysis context |
| `hosted_plugin.load` | Hosted | `plugin_path` | Load a plugin |
| `hosted_plugin.parameters` | Hosted | — | List plugin parameters |
| `hosted_plugin.set_parameters` | Hosted | `parameters[]` | Batch set parameters |
| `hosted_plugin.capture_state` | Hosted | `slot`, `include_values`, `includeState` | Capture plugin state |
| `analysis.get_summary` | Analysis | — | Get analysis summary |
| `analysis.capture_window` | Analysis | `window_seconds` | Return rolling DSP meter-window statistics |
| `analysis.compare_render` | Analysis | `before`, `after` | Compare analysis data |
| `plugin_profile.audit_parameters` | Profile | — | Audit plugin parameters |
| `plugin_profile.get` | Profile | `profile_id` | Load saved profile |
| `plugin_profile.save` | Profile | — | Save current profile |
| `analyze_parameters` | Extended | `include_descriptions`, `include_hidden`, `max_parameters` | AI-friendly parameter analysis |
| `expose_parameters` | Extended | `action`, `parameter_indices`, `category` | Control AI visibility |
| `get_token_estimate` | Extended | `operation`, `parameter_count` | Estimate token cost |
| `set_parameters_optimized` | Extended | `parameters[]`, `max_tokens` | Set with token optimization |
| `get_morph_compatibility` | Extended | `snapshot_a`, `snapshot_b` | Analyze morph compatibility |
| `get_parameter_categories` | Extended | `include_empty` | Parameters by category |
| `learn_from_adjustment` | Extended | `parameter_index`, `importance_boost` | Record parameter importance |
| `get_learn_mode_status` | Extended | — | Get Learn Mode status |
| `set_learn_mode_config` | Extended | `enabled`, `exposure_threshold`, `auto_learn`, ... | Configure Learn Mode |
| `get_discrete_parameters` | Extended | `include_binary`, `include_enums` | Get non-interpolatable params |
| `get_usage_stats` | Extended | — | AI usage statistics |
| `set_token_budget` | Extended | `max_cost_usd`, `max_tokens_per_session`, ... | Set budget limits |

---

## Appendix B: Glossary

| Term | Definition |
|------|-----------|
| **MCP** | Model Context Protocol — JSON-RPC 2.0 based protocol for AI tool use |
| **5-step rule planner** | Deterministic heuristic sequence used by ChainPlanExecutor |
| **LUFS** | Loudness Units Full Scale — EBU R128 loudness measurement standard |
| **dBTP** | Decibels True Peak — true-peak level measurement |
| **VST3** | Steinberg plugin format (Virtual Studio Technology 3) |
| **AU** | Apple Audio Units plugin format |
| **CSPRNG** | Cryptographically Secure Pseudo-Random Number Generator |
| **Seqlock** | Sequence lock — lock-free read pattern with retry on write detection |
| **LockFreeQueue** | SPSC (Single Producer Single Consumer) ring buffer for audio-thread-safe command passing |
| **OzoneParameterMap** | Static parameter index table mapping Ozone 11 parameters to VST3 indices |
| **OzonePlanApplicator** | Component that translates MultiEffectPlan into Ozone parameter changes |
| **ChainPlanExecutor** | 5-step heuristic mastering chain planner with genre-based LUFS targets |
| **TokenOptimizer** | Rate limiter and cost estimator for AI API calls |
| **Learn Mode** | Automatic parameter exposure system based on user behavior tracking |

---

## Appendix C: File Reference

| File | Role |
|------|------|
| `src/AI/MCPServer.h` | MCP server class definition with ConnectionThread |
| `src/AI/MCPServer.cpp` | JSON-RPC server implementation, auth, dispatch |
| `src/AI/MCPToolHandler.h` | Tool dispatch header |
| `src/AI/MCPToolHandler.cpp` | Core tool implementations (12 tools + aliases) |
| `src/AI/MCPToolsExtended.h` | Extended AI tools header |
| `src/AI/MCPToolsExtended.cpp` | Extended AI tool implementations (21 tools) |
| `src/AI/MCPEQTool.h` | EQ Assistant MCP tools header |
| `src/AI/OzoneParameterMap.h` | Ozone 11 parameter index table |
| `src/AI/OzoneParameterMap.cpp` | Factory, normalization, auto-discovery |
| `src/AI/OzonePlanApplicator.h` | Plan-to-parameter translator |
| `src/AI/OzonePlanApplicator.cpp` | EQ, Dynamics, Imager, Maximizer apply methods |
| `src/AI/ChainPlanExecutor.h` | 5-step heuristic rule planner |
| `src/AI/ChainPlanExecutor.cpp` | Rule step implementations |
| `src/AI/InstanceIdentity.h` | Per-instance identity generation |
| `src/AI/InstanceRegistry.h` | Multi-instance port management |
| `src/AI/TokenOptimizer.h` | Token budget and rate limiting |
| `src/Core/ParameterClassifier.h` | Parameter classification and Learn Mode |
| `scripts/audit_ozone_params.py` | Ozone 11 parameter audit script |
| `tests/Unit/TestOzoneIntegration.cpp` | Unit tests for Ozone integration |
