"""
More-Phi VST3 MCP server entrypoint.

Runs over stdio transport using the official Python MCP SDK. The AI host spawns
this script as a subprocess and communicates via JSON-RPC 2.0.
"""

from __future__ import annotations

import asyncio
import json
import os
import sys
import time
from datetime import datetime, timezone

import jsonschema
from dataclasses import dataclass, field
from typing import Any

from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import CallToolResult, TextContent, Tool

# Ensure the script can find the bridge/tools modules regardless of cwd.
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if _SCRIPT_DIR not in sys.path:
    sys.path.insert(0, _SCRIPT_DIR)

from audit import AuditLogger
from bridge import VST3IPCBridge, VST3IPCError, default_endpoint
from cache import ToolResultCache
from tools import HANDLERS, ParameterRegistry, ToolError, get_tool_descriptions, make_request_id


@dataclass
class ServerContext:
    bridge: VST3IPCBridge
    registry: ParameterRegistry
    cache: ToolResultCache
    audit: AuditLogger


async def create_context() -> ServerContext:
    instance_id = os.environ.get("MORE_PHI_INSTANCE_ID")
    endpoint = default_endpoint(instance_id)
    bridge = VST3IPCBridge(endpoint=endpoint)
    registry = ParameterRegistry(instance_id=instance_id)
    cache = ToolResultCache()
    audit = AuditLogger()
    return ServerContext(bridge=bridge, registry=registry, cache=cache, audit=audit)


# Output-schema index. Schemas are static (defined in tools/registry.py), so this
# is built once at import time and used to self-validate structured tool output.
_OUTPUT_SCHEMAS: dict[str, dict[str, Any]] = {
    descriptor["name"]: descriptor["outputSchema"]
    for descriptor in get_tool_descriptions()
}


def _is_failure(output: dict[str, Any]) -> bool:
    """A tool result is an error unless its status is explicitly 'success'."""
    return output.get("status") != "success"


def _iso_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _build_call_result(output: dict[str, Any], tool_name: str) -> CallToolResult:
    """Wrap a handler output dict as a CallToolResult carrying structuredContent
    and isError, self-validating against the tool's declared outputSchema.

    Returning a CallToolResult directly lets us set isError from the handler's
    status field; the MCP SDK otherwise forces isError=False for dict/tuple
    returns. Because the SDK skips its own output validation for CallToolResult
    returns, we validate here so the declarative outputSchema stays meaningful
    and contract drift is caught instead of silently shipped.
    """
    schema = _OUTPUT_SCHEMAS.get(tool_name)
    if schema is not None:
        try:
            jsonschema.validate(instance=output, schema=schema)
        except jsonschema.ValidationError as exc:
            return CallToolResult(
                content=[
                    TextContent(type="text", text=f"Output validation error: {exc.message}")
                ],
                structuredContent=None,
                isError=True,
            )
    return CallToolResult(
        content=[TextContent(type="text", text=json.dumps(output))],
        structuredContent=output,
        isError=_is_failure(output),
    )


async def main() -> None:
    app = Server("vst3-mastering-server")
    ctx = await create_context()

    @app.list_tools()
    async def list_tools() -> list[Tool]:
        # Refresh registry so newly loaded plugins are reflected.
        ctx.registry.reload()
        return [Tool(**descriptor) for descriptor in get_tool_descriptions()]

    @app.call_tool()
    async def call_tool(name: str, arguments: dict[str, Any]) -> CallToolResult:
        request_id = make_request_id()
        t0 = time.perf_counter()

        # Check cache for read-only tools.
        cached = ctx.cache.get(name, arguments)
        if cached is not None:
            return _finalize(cached, ctx, request_id, name, arguments, t0)

        handler = HANDLERS.get(name)
        if handler is None:
            output = {
                "status": "failure",
                "error_message": f"Unknown tool: {name}",
                "corrective_action": "Check tools/list for available tools.",
            }
        else:
            try:
                output = await handler(ctx.bridge, arguments, ctx.registry)
            except ToolError as e:
                output = {
                    "status": "failure",
                    "error_message": str(e),
                    "corrective_action": e.code,
                }
            except VST3IPCError as e:
                output = {
                    "status": "failure",
                    "error_message": str(e),
                    "corrective_action": "PIPE_BROKEN",
                }
            except Exception as e:
                output = {
                    "status": "failure",
                    "error_message": f"Internal error: {e}",
                    "corrective_action": "Check VST3 host logs for details.",
                }

            # Write-through cache for eligible tools. Never cache failures -- a
            # transient IPC error must not poison a TTL-cached read (e.g.
            # list_parameters) for the full TTL window.
            if ctx.cache.is_write_tool(name):
                ctx.cache.invalidate()
            elif output.get("status") == "success":
                ctx.cache.put(name, arguments, output)

        return _finalize(output, ctx, request_id, name, arguments, t0)

    async with stdio_server() as (read_stream, write_stream):
        await app.run(read_stream, write_stream, app.create_initialization_options())


def _finalize(
    output: dict[str, Any],
    ctx: ServerContext,
    request_id: str,
    tool_name: str,
    arguments: dict[str, Any],
    t0: float,
) -> CallToolResult:
    """Stamp latency, append to the audit log, and build the CallToolResult.

    Works on a copy so stamping latency_ms never mutates a cached entry in place.
    Every call is audited (spec S10.2), including cache hits.
    """
    latency_ms = (time.perf_counter() - t0) * 1000.0
    stamped = dict(output)
    stamped["latency_ms"] = latency_ms
    stamped["request_id"] = request_id
    stamped["timestamp"] = _iso_now()
    ctx.audit.log(request_id, tool_name, arguments, stamped, latency_ms)
    return _build_call_result(stamped, tool_name)


if __name__ == "__main__":
    asyncio.run(main())
