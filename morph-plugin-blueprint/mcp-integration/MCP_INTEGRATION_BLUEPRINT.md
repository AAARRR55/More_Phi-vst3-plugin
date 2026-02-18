# MCP Integration Blueprint

## Objectives

- Expose Morphy state and actions to MCP-compatible AI clients.
- Keep AI operations optional and non-blocking for audio processing.
- Enforce explicit user control over AI-applied changes.

## Transport and Protocol

- Protocol: JSON-RPC 2.0 aligned with MCP conventions.
- Transport: local TCP/WebSocket (project-dependent).
- Authentication: token-based handshake required before tool calls.

## MCP Surface

### Resources

- `morphsnap://plugin/state`: current mode, snapshots, mapped parameters.
- `morphsnap://audio/context`: metering + analysis summaries.
- `morphsnap://session/health`: connection state, queue depth, version.

### Tools

- `set_morph_position(x, y)`
- `capture_snapshot(slot, label)`
- `generate_morph_trajectory(goal, durationMs)`
- `suggest_parameters(context)`
- `apply_parameter_delta(changes[])`

## Command Lifecycle

1. Receive MCP request.
2. Validate schema + auth + rate limit.
3. Convert to internal command DTO.
4. Enqueue for safe execution on owning thread.
5. Return accepted/result/error response.

## Safety Constraints

- Never execute MCP handler logic on the audio callback.
- Require explicit user consent for destructive/broad changes.
- Rate-limit repeated heavy tools.
- Sanitize all JSON and reject unknown tool payloads.

## Observability

- Counters: request count, error count, auth failures.
- Histograms: request latency, queue delay.
- Health endpoint/resource includes protocol + plugin version.

## Compatibility Strategy

- Advertise protocol version in initialize response.
- Maintain backward-compatible tool schema where possible.
- Use feature flags for experimental tools.

## Epic Ticket Mapping

- `MORPH-018` MCP server implementation
- `MORPH-019` MCP tool handler
- `MORPH-024` AI status panel integration (partial UI coupling)

## Milestone Checkpoints

- MCP auth + schema validation is mandatory before production enablement.
- Round-trip latency stays under 100 ms for standard tool calls.
- Error reporting feeds UI state model for connected/executing/error transitions.
