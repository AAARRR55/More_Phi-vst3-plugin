# More-Phi API Reference

> **Audit Score: 7.9/10** — See [VST3_TECHNICAL_AUDIT_AND_MARKET_ANALYSIS.md](../VST3_TECHNICAL_AUDIT_AND_MARKET_ANALYSIS.md) for the complete 39 KB technical audit with 26 verifiable claims.

This document describes the MCP (Model Context Protocol) API for More-Phi, enabling AI assistants and external tools to control the plugin programmatically. More-Phi exposes **30+ MCP tools** across 8 categories including hosted plugin control, snapshot/morph management, analysis/metering, mastering workflow, More-Phi parameter control, AI/Learn Mode, agent orchestration, and dataset generation.

---


---

## Overview

More-Phi exposes a JSON-RPC 2.0 server on `localhost:30001` that accepts tool calls for:
- Plugin information queries
- Parameter manipulation
- Snapshot management
- Morphing control

---

## Connection

| Property | Value |
|----------|-------|
| Protocol | JSON-RPC 2.0 |
| Host | localhost (127.0.0.1) |
| Port | 30001 |
| Transport | TCP |

### Connection Example (Python)

```python
import socket
import json

def send_mcp_request(method, params=None):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(('127.0.0.1', 30001))

    request = {
        "jsonrpc": "2.0",
        "method": method,
        "params": params or {},
        "id": 1
    }

    sock.send((json.dumps(request) + '\n').encode())
    response = sock.recv(4096).decode()
    sock.close()

    return json.loads(response)
```

### Connection Example (Node.js)

```javascript
const net = require('net');

async function sendMcpRequest(method, params = {}) {
    return new Promise((resolve, reject) => {
        const client = new net.Socket();

        client.connect(30001, '127.0.0.1', () => {
            const request = JSON.stringify({
                jsonrpc: '2.0',
                method: method,
                params: params,
                id: 1
            }) + '\n';

            client.write(request);
        });

        let data = '';
        client.on('data', (chunk) => {
            data += chunk;
            client.destroy(); // Close after first response
        });

        client.on('close', () => {
            resolve(JSON.parse(data));
        });

        client.on('error', reject);
    });
}
```

---

## JSON-RPC Format

All requests follow the JSON-RPC 2.0 specification:

```json
{
    "jsonrpc": "2.0",
    "method": "tools/call",
    "params": {
        "name": "<tool_name>",
        "arguments": { ... }
    },
    "id": <request_id>
}
```

### Success Response

```json
{
    "jsonrpc": "2.0",
    "result": { ... },
    "id": <request_id>
}
```

### Error Response

```json
{
    "jsonrpc": "2.0",
    "error": {
        "code": <error_code>,
        "message": "<error_message>"
    },
    "id": <request_id>
}
```

---

## MCP Tools

### initialize

Initialize the MCP connection and get server capabilities.

**Method:** `initialize`

**Parameters:**
```json
{
    "protocolVersion": "2024-11-05",
    "capabilities": {}
}
```

**Response:**
```json
{
    "protocolVersion": "2024-11-05",
    "capabilities": {
        "tools": {}
    },
    "serverInfo": {
        "name": "More-Phi",
        "version": "1.0.0"
    }
}
```

---

### tools/list

List all available MCP tools.

**Method:** `tools/list`

**Response:**
```json
{
    "tools": [
        {
            "name": "get_plugin_info",
            "description": "Get information about the loaded plugin",
            "inputSchema": { "type": "object", "properties": {} }
        },
        // ... more tools
    ]
}
```

---

### get_plugin_info

Returns information about the currently hosted plugin.

**Method:** `tools/call`

**Parameters:**
```json
{
    "name": "get_plugin_info",
    "arguments": {}
}
```

**Response:**
```json
{
    "name": "Serum",
    "manufacturer": "Xfer Records",
    "type": "instrument",
    "parameterCount": 472,
    "isLoaded": true
}
```

---

### list_parameters

Lists all hosted plugin parameters with their current values and mapping metadata.

**Method:** `tools/call`

**Parameters:**
```json
{
    "name": "list_parameters",
    "arguments": {}
}
```

**Response:**
```json
[
    {
        "id": 0,
        "index": 0,
        "stableId": "master_volume",
        "name": "Master Volume",
        "value": 0.75,
        "displayValue": "-3.0 dB",
        "label": "dB",
        "discrete": false,
        "boolean": false,
        "numSteps": 0,
        "defaultValue": 1.0
    }
]
```

---

### get_parameter

Get a single parameter by `stableId`, numeric `index`/`id`, or exact name.

**Method:** `tools/call`

**Parameters:**
```json
{
    "name": "get_parameter",
    "arguments": {
        "stableId": "master_volume"
    }
}
```

**Response:**
```json
{
    "id": 0,
    "index": 0,
    "stableId": "master_volume",
    "name": "Master Volume",
    "value": 0.75,
    "displayValue": "-3.0 dB",
    "label": "dB",
    "discrete": false,
    "boolean": false,
    "numSteps": 0,
    "defaultValue": 1.0
}
```

---

### set_parameter

Set a single parameter value. Identify the target by `stableId` when available, otherwise by numeric `index`/`id` or exact name.

**Method:** `tools/call`

**Parameters:**
```json
{
    "name": "set_parameter",
    "arguments": {
        "index": 0,
        "value": 0.5
    }
}
```

**Response:**
```json
{
    "success": true,
    "index": 0,
    "value": 0.5,
    "queued": 1,
    "rejected": 0,
    "appliedNow": 1,
    "pendingAfter": 0,
    "flush": {
        "pending_before": 1,
        "drained": 1,
        "pending_after": 0,
        "plugin_unavailable": false,
        "exclusive_access_timed_out": false,
        "retry_count": 0,
        "waited_ms": 5,
        "out_of_range_count": 0
    },
    "verification": {
        "status": "success",
        "requested_value": 0.5,
        "value_before": 0.25,
        "value_after": 0.5,
        "human_before": "25%",
        "human_after": "50%",
        "execution_time_ms": 8.3,
        "verified": true
    }
}
```

**Notes:**
- Value must be normalized (0.0 - 1.0)
- Changes are applied audio-thread safe via lock-free queue with immediate assistant flush
- **AUDIT-FIX 4.3:** `success` is gated on `verification.verified == true`. If the edit is queued but not yet applied, `success` is `false` and `verification.status` is `"queued"`.
- **AUDIT-FIX 4.7:** If the parameter index exceeds the hosted plugin's actual parameter count, `flush.out_of_range_count > 0` and `verification.status` is `"parameter_index_out_of_range"`.
- **AUDIT-FIX 4.1:** For discrete parameters, verification uses a step-aware tolerance. A snap to the wrong step returns `"value_drift_discrete"`.
- **AUDIT-FIX 4.5:** If the morph engine overwrites the edit, `verification.status` reports `"morph_overwrite_risk"`.
- A full queue returns `success: false`, `error: "queue_full"`, and `queued: 0`

---

### set_parameters_batch

Queue multiple parameter edits in one request. Each item accepts `stableId`, `index`/`id`, or exact `name`.

**Method:** `tools/call`

**Parameters:**
```json
{
    "name": "set_parameters_batch",
    "arguments": {
        "parameters": [
            { "stableId": "master_volume", "value": 0.5 },
            { "index": 1, "value": 0.75 },
            { "name": "Filter Resonance", "value": 0.25 }
        ]
    }
}
```

**Response:**
```json
{
    "success": true,
    "queued": 3,
    "applied": 3,
    "appliedNow": 3,
    "requested": 3,
    "rejected": 0,
    "queueFailures": 0,
    "verifiedCount": 3,
    "pendingAfter": 0,
    "flush": {
        "pending_before": 3,
        "drained": 3,
        "pending_after": 0,
        "plugin_unavailable": false,
        "exclusive_access_timed_out": false,
        "retry_count": 0,
        "waited_ms": 12,
        "out_of_range_count": 0
    },
    "verification": [
        {
            "index": 0,
            "verification": {
                "status": "success",
                "requested_value": 0.5,
                "value_before": 1.0,
                "value_after": 0.5,
                "verified": true
            }
        },
        {
            "index": 1,
            "verification": {
                "status": "success",
                "requested_value": 0.75,
                "value_before": 0.5,
                "value_after": 0.75,
                "verified": true
            }
        }
    ]
}
```

If any item cannot be resolved or queued, `success` is `false` and the response includes
`rejected`, `queueFailures`, and an `error` such as `partial_rejected` or `queue_full`.

**AUDIT-FIX 4.3:** `verifiedCount` reports how many edits passed verification. If all items
were queued but none verified as applied, `success` is `false` with a warning suggesting
the plugin may not be ready.

---

### capture_snapshot

Capture the current hosted-plugin parameter state to a snapshot slot. By default, this also captures the hosted plugin's opaque state chunk on the message thread, outside the audio callback.

**Method:** `tools/call`

**Parameters:**
```json
{
    "name": "capture_snapshot",
    "arguments": {
        "slot": 0,
        "includeState": true
    }
}
```

**Response:**
```json
{
    "success": true,
    "slot": 0,
    "includeState": true,
    "stateChunk": true
}
```

**Notes:**
- Slot must be 0-11
- Overwrites existing snapshot if slot is occupied
- `includeState` may also be sent as `include_state`; default is `true`
- Capture is performed by taking exclusive hosted-plugin state access. Audio processing skips hosted-plugin processing while this non-audio access is pending instead of blocking the audio thread.

---

### recall_snapshot

Recall a snapshot from a slot. Fast recall queues normalized parameter values through the real-time-safe parameter command path. Full recall also schedules the hosted plugin's opaque state chunk for the next message-thread maintenance tick.

**Method:** `tools/call`

**Parameters:**
```json
{
    "name": "recall_snapshot",
    "arguments": {
        "slot": 0,
        "mode": "full"
    }
}
```

**Response:**
```json
{
    "success": true,
    "slot": 0,
    "queued": 472,
    "mode": "full"
}
```

**Notes:**
- Returns error if slot is empty
- Use `mode: "fast"` for params-only recall. Full recall can also be requested with `full: true`, `includeState: true`, or `include_state: true`.
- MIDI-triggered full recall keeps parameter recall on the audio-safe queue and defers opaque hosted-state recall to the message thread. This may complete on the next maintenance tick.

---

### set_morph_position

Set the morph cursor position.

**Method:** `tools/call`

**Parameters:**
```json
{
    "name": "set_morph_position",
    "arguments": {
        "x": 0.5,
        "y": 0.5,
        "source": "xy"
    }
}
```

| Parameter | Type | Range | Description |
|-----------|------|-------|-------------|
| x | float | 0.0 - 1.0 | Horizontal position |
| y | float | 0.0 - 1.0 | Vertical position |
| fader | float | 0.0 - 1.0 | Snap-fader position |
| source | string | "xy" \| "fader" | Which input mode to use |

**Response:**
```json
{
    "success": true
}
```

`set_morph_position` updates the processor atomics and cached APVTS raw values immediately, then posts host automation begin/set/end notifications from the message thread. Internal morph response is block-accurate.

---

### get_morph_state

Get current morph position and mode.

**Method:** `tools/call`

**Parameters:**
```json
{
    "name": "get_morph_state",
    "arguments": {}
}
```

**Response:**
```json
{
    "x": 0.5,
    "y": 0.5,
    "faderPos": 0.25,
    "source": "xy",
    "physicsMode": "elastic",
    "processedX": 0.48,
    "processedY": 0.52
}
```

---

## Orchestrator API

More-Phi v3.3.0 exposes a system-level orchestration API on top of the MCP server. These tools allow AI clients and external scripts to inspect the multi-agent system, submit goals, and manage agent lifecycle.

### describeSystemState

Returns a JSON snapshot of the orchestrator, MCP server, all registered agents, and scheduler statistics.

**Method:** `tools/call`

**Parameters:**
```json
{
    "name": "orchestrator.describe_system_state",
    "arguments": {}
}
```

**Response:**
```json
{
    "orchestratorRunning": true,
    "mcpServerRunning": true,
    "mcpHealthy": true,
    "mcpPort": 30001,
    "agentCount": 6,
    "agentStates": [
        {
            "name": "Conductor",
            "state": "idle",
            "lastTask": "decompose_goal",
            "pendingApproval": false
        },
        {
            "name": "Analysis",
            "state": "executing",
            "lastTask": "get_spectrum",
            "pendingApproval": false
        }
    ],
    "schedulerStats": {
        "queueDepth": 2,
        "tasksCompleted": 14,
        "averageLatencyMs": 12.5
    }
}
```

| Field | Type | Description |
|---|---|---|
| `orchestratorRunning` | Bool | Whether the orchestrator thread is active. |
| `mcpServerRunning` | Bool | Whether the MCP server is currently accepting connections. |
| `mcpHealthy` | Bool | Whether the last MCP health check passed. |
| `mcpPort` | Int | The local port the MCP server is listening on. |
| `agentCount` | Int | Number of registered agents (always 6 for the built-in set). |
| `agentStates` | Array | One object per agent with `name`, `state`, `lastTask`, and `pendingApproval`. |
| `schedulerStats` | Object | `queueDepth`, `tasksCompleted`, and `averageLatencyMs`. |

### submitUserGoal

Submits a high-level natural-language goal to the Conductor agent. The Conductor breaks the goal into sub-tasks, delegates to the other agents, and returns a plan ID.

**Method:** `tools/call`

**Parameters:**
```json
{
    "name": "orchestrator.submit_user_goal",
    "arguments": {
        "intent": "make this track louder and brighter"
    }
}
```

| Parameter | Type | Description |
|---|---|---|
| `intent` | String | A plain-language goal describing the desired change. |

**Response:**
```json
{
    "success": true,
    "planId": "plan_2026_abcdef",
    "conductorState": "planning"
}
```

### AgentOrchestrator Lifecycle

The `AgentOrchestrator` is the C++ class that manages the multi-agent system. Its lifecycle is:

1. **Construct** — Created by `MorePhiProcessor` during plugin initialization. Registers the six built-in agents and the scheduler.
2. **Start** — Called when the user clicks **Start MCP** or when the MCP server is started programmatically. The orchestrator spins up its background thread and begins accepting goals.
3. **Submit** — A user goal is submitted via `submitUserGoal(intent)`. The Conductor agent decomposes it and queues tasks for the other agents.
4. **Stop** — Called when the user clicks **Stop MCP** or when the plugin is destroyed. All agents are returned to idle, the scheduler is drained, and the background thread exits cleanly.

### MCP Protocol Schemas

The `McpProtocol` namespace defines the JSON-RPC schemas used for message construction and tool dispatch. When building custom MCP clients, reference these schemas to ensure compatibility:

- `McpProtocol::Request` — wraps `jsonrpc`, `method`, `params`, and `id`.
- `McpProtocol::ToolCallParams` — wraps `name` and `arguments` for `tools/call`.
- `McpProtocol::InitializeParams` — wraps `protocolVersion` and `capabilities`.
- `McpProtocol::SystemState` — wraps the full `describeSystemState` response shape.

All messages follow JSON-RPC 2.0. The `tools/call` method is the standard entry point for every tool, including orchestrator tools.

---

## Configuration API

### EcosystemConfig

`EcosystemConfig` is the runtime configuration object that controls orchestrator and MCP server settings. It is loaded during plugin initialization and can be queried or updated through the MCP layer.

**Common fields:**

| Field | Type | Description |
|---|---|---|
| `mcpEnabled` | Bool | Whether the MCP server should start automatically on plugin load. |
| `mcpPort` | Int | Preferred local port for the MCP server (default: 30001). |
| `orchestratorEnabled` | Bool | Whether the multi-agent orchestrator should be created and started. |
| `agentAutonomyLevel` | Choice | Global autonomy override: `manual`, `conductor_gated`, or `automatic`. |
| `realtimePriority` | Choice | Thread priority for the RealtimeControl agent: `normal`, `elevated`, or `realtime_critical`. |
| `blackboardHistorySize` | Int | Number of past agent results retained in the BlackboardBridge for context. |
| `safetyPolicy` | String | Name of the active safety policy loaded by the QualitySafety agent. |

**Example — querying current configuration:**

```json
{
    "jsonrpc": "2.0",
    "method": "tools/call",
    "params": {
        "name": "ecosystem.get_config",
        "arguments": {}
    },
    "id": 1
}
```

**Example — updating a field:**

```json
{
    "jsonrpc": "2.0",
    "method": "tools/call",
    "params": {
        "name": "ecosystem.set_config",
        "arguments": {
            "agentAutonomyLevel": "conductor_gated"
        }
    },
    "id": 2
}
```

Changes to `mcpPort` or `mcpEnabled` require a restart of the MCP server to take effect. Changes to `agentAutonomyLevel` apply immediately to the next submitted goal.

---

## Error Codes

| Code | Description |
|------|-------------|
| -32700 | Parse error (invalid JSON) |
| -32600 | Invalid request |
| -32601 | Method not found |
| -32602 | Invalid params |
| -32603 | Internal error |
| 100 | Plugin not loaded |
| 101 | Invalid parameter index |
| 102 | Invalid slot index |
| 103 | Snapshot slot empty |

---

## Rate Limiting

The MCP server does not implement explicit rate limiting. However:
- Parameter changes are queued and processed per audio buffer
- Rapid changes may be coalesced
- Recommended: max 100 commands per second

---

## Security Considerations

- Server only accepts localhost connections
- No authentication (designed for local use only)
- No encryption (not needed for localhost)
- Don't expose port 30001 to external networks

---

## Complete Example: AI-Driven Morphing

```python
#!/usr/bin/env python3
import socket
import json
import time

def mcp_call(method, args=None):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(('127.0.0.1', 30001))

    request = {
        "jsonrpc": "2.0",
        "method": "tools/call",
        "params": {"name": method, "arguments": args or {}},
        "id": 1
    }

    sock.send((json.dumps(request) + '\n').encode())
    response = json.loads(sock.recv(4096).decode())
    sock.close()
    return response.get('result', {})

# Initialize connection
mcp_call("initialize")

# Capture snapshot at current position
mcp_call("capture_snapshot", {"slot": 0})

# Change some parameters
mcp_call("set_parameter", {"index": 0, "value": 0.25})
mcp_call("set_parameter", {"index": 1, "value": 0.75})

# Capture another snapshot
mcp_call("capture_snapshot", {"slot": 1})

# Morph between snapshots
for i in range(10):
    t = i / 9.0
    mcp_call("set_morph_position", {"x": t, "y": 0.5, "source": "xy"})
    time.sleep(0.1)

print("Morph complete!")
```
