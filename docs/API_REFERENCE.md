# More-Phi API Reference

This document describes the MCP (Model Context Protocol) API for More-Phi, enabling AI assistants and external tools to control the plugin programmatically.

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
    "rejected": 0
}
```

**Notes:**
- Value must be normalized (0.0 - 1.0)
- Changes are applied audio-thread safe via lock-free queue
- Effect may be delayed by one audio buffer
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
    "requested": 3,
    "rejected": 0,
    "queueFailures": 0
}
```

If any item cannot be resolved or queued, `success` is `false` and the response includes
`rejected`, `queueFailures`, and an `error` such as `partial_rejected` or `queue_full`.

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
