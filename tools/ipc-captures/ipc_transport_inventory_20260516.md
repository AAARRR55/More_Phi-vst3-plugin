# IPC Transport Inventory - 2026-05-16

## Scope

Safe, read-only transport inventory for the active FL Studio / More-Phi session.
No API hooks, process patching, arbitrary process-memory reads, credential
extraction, license traffic decoding, or live IPC writes were performed.

## Observed Processes

| PID | Process | Notes |
| --- | --- | --- |
| 24352 | FL64 | Active DAW process |

No separate iZotope/Ozone/Neutron/Nectar/RX service process was observed by
process-name filtering during this run.

## Loopback Sockets

`FL64` was listening on:

| Address | Port | Classification |
| --- | ---: | --- |
| 127.0.0.1 | 30001 | More-Phi JSON-RPC MCP endpoint, auth required |
| 127.0.0.1 | 30002 | More-Phi JSON-RPC MCP endpoint, auth required |
| 127.0.0.1 | 30003 | More-Phi JSON-RPC MCP endpoint, auth required |

Unauthenticated `initialize` requests returned `Unauthorized:
invalid bearer_token`, confirming these are More-Phi MCP control endpoints, not
iZotope IPC transports.

The saved local MCP bridge token in `docs/mcp_config.json` did not authenticate
to the active endpoints during this run, so live in-plugin IPC tools could not be
called through the running plugin instance.

## Named Pipes

The only pipe matching the broad local filter
`izotope|ozone|neutron|nectar|rx|visual|morephi|ipc` was:

| Pipe | Classification |
| --- | --- |
| `\\.\pipe\codex-ipc` | Codex-local IPC, unrelated to iZotope |

No iZotope/Ozone named pipe was observed.

## Services

No Windows service matching iZotope/Ozone/Neutron/Nectar/RX was observed by
service-name, display-name, or path filtering.

## Shared Memory Attempt

The repository's read-only IPC discovery path was run through
`MorePhiMcpServer.exe` against the active FL Studio PID:

```text
tool: izotope_ipc_attach
daw_process_id: 24352
mapped_size_bytes: 4194304
```

Result:

```text
error: attach_failed
message: OpenFileMappingW failed for read-only segment.
```

Because attach failed, no bounded snapshot or diff capture was taken and no raw
IPC bytes were written.

## Conclusion

This session exposes More-Phi MCP loopback endpoints but does not expose the
expected default iZotope shared-memory mapping
`Global\iZotope_IPC_Session_24352`. No iZotope named pipe or service transport
was visible through the safe metadata inventory performed here.

Next safe step, if further live inspection is required: read the active bearer
token from the More-Phi AI Status panel and run the same read-only
`izotope_ipc_status` / `izotope_ipc_attach` / `izotope_ipc_capture` sequence
through the authenticated plugin endpoint.

## Re-Run: Defensive Execute Request

The passive inventory and read-only attach path were re-run after the request to
execute the security-research workflow.

Observed state remained the same:

- `FL64` was still the only matching DAW/vendor process, PID `24352`.
- `FL64` still listened on `127.0.0.1:30001`, `30002`, and `30003`.
- Those ports remain classified as More-Phi MCP JSON-RPC endpoints.
- No iZotope/Ozone named pipe was visible.
- No iZotope/Ozone Windows service was visible.
- `izotope_ipc_attach` against `daw_process_id=24352` still failed with
  `OpenFileMappingW failed for read-only segment.`

No snapshot or capture was taken because no shared-memory segment was attached.
No hooks, injection, anti-tamper bypass, licensing interception, or message
injection were attempted.

## User-Reported Assistant/Link Observation

The operator reported one high-level live observation from an external watch:

- Ozone showed activity on an internal audio-data stream path during the
  Assistant/link investigation.
- Neutron Visual Mixer and Relay-related tables were present in the session but
  did not show activity during that capture window.
- The live side observed so far is therefore Ozone's audio-data stream path, not
  a confirmed Visual Mixer or Relay control path.

This report intentionally omits internal addresses, object pointers, hook
mechanics, raw buffers, and handler relationships. The observation should be
used only as correlation context for external, supported captures such as
hosted-plugin parameter snapshots and More-Phi MCP before/after diffs.

## User-Reported Relay/Visual Mixer Correlation

The operator later reported a useful external correlation during a longer
Assistant/link capture window:

- Relay parameters changed during the capture window.
- Relay and Neutron Visual Mixer Track Assist-related activity was observed by
  the operator during the same window.
- This makes Relay parameter changes the current best external correlation
  target for the linked Assistant/Visual Mixer workflow.

This note intentionally excludes internal hit sites, disassembly results,
addresses, object graphs, raw payloads, and hook mechanics. Any follow-up in
More-Phi should use supported external evidence: before/after hosted-parameter
snapshots, MCP parameter diffs, and timestamped operator actions.

## Static Import Evidence: Neutron Visual Mixer

The operator provided a `dumpbin /imports` style import listing for
`iZNeutron3VisualMixer.dll`. This is useful only as transport-family evidence;
it does not prove that any specific IPC path was active during the capture.

Relevant import families observed:

| Import Family | Examples | Defensive Interpretation |
| --- | --- | --- |
| Shared memory / mappings | `CreateFileMappingA/W`, `MapViewOfFile`, `UnmapViewOfFile`, `FlushViewOfFile` | The binary is capable of using file mappings or shared-memory-style buffers. This supports, but does not prove, a mapping-based transport hypothesis. |
| Synchronization | `CreateMutexA/W`, `ReleaseMutex`, `CreateEventA/W`, `SetEvent`, `ResetEvent`, `CreateSemaphoreA`, wait APIs | The binary uses cross-thread or cross-component coordination primitives that could guard shared buffers or background work. |
| File / pipe client primitives | `CreateFileA/W`, `ReadFile`, `WriteFile`, `WaitNamedPipeA`, `PeekNamedPipe` | Named-pipe or file-handle IPC is plausible. The import list does not show `CreateNamedPipe` or `ConnectNamedPipe`, so this dump is more consistent with client-side pipe/file-handle use than pipe-server creation. |
| Networking | `WinHttp*`, `WinINet*`, `WS2_32` ordinal imports | Loopback or remote HTTP/socket communication is possible, but static imports do not identify endpoints or message schemas. |
| Crypto / trust / certificates | `Crypt*`, `Cert*`, `WinVerifyTrust`, `BCrypt*`, image certificate APIs | These are treated as protection, trust, update, or security-adjacent surfaces and are out of scope for decoding or interception. |
| UI / rendering / host integration | `USER32`, `GDI32`, `gdiplus`, `OPENGL32`, `d3d9`, `OLEACC`, `ole32` | Expected for plugin UI and host integration; not sufficient evidence of IPC by itself. |

The section table included `IPPCODE` and `IPPDATA`, which is consistent with a
protected/vendor-specific code region. This report does not analyze those
sections. Anti-tamper, licensing, signature validation, certificate handling,
or entitlement behavior remains out of scope.

Static conclusion:

- Visual Mixer has imports compatible with shared memory, named-pipe/file-handle
  I/O, HTTP/socket communication, and synchronization.
- The import table does not reveal concrete object names, endpoint addresses,
  message types, payload layouts, or active runtime use.
- The best safe next step remains external correlation: timestamped operator
  actions plus hosted-parameter diffs, especially around Relay parameter
  changes observed during the linked Assistant/Visual Mixer window.
