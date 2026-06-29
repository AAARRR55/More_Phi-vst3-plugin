# iZotope IPC Research Methodology

## Current More-Phi Context

More-Phi already has two relevant layers:

- `IZotopeIPCDiscovery`: read-only attachment, bounded snapshots, dumps, and diff captures for candidate shared-memory segments.
- `IZotopeIPCAssistant`: experimental, gated Assistant invocation for manifest-defined IPC rings when a verified segment exists.

Live Ozone 11 testing found named iZotope mutexes in FL Studio sessions, but no verified named shared-memory ring exposing `IZOT` frames. The production-safe path is therefore parameter capture and diffing around a normal Ozone Assistant UI run. Treat live IPC writes as lab-only until a transport is positively identified and schema-compatible.

## Research Phases

### 1. Establish Authorization and Scope

Before collecting data, record:

- Product name, version, installer channel, and plugin format.
- DAW host, DAW version, OS version, user account, and privilege level.
- Whether the session is a lab machine, CI fixture, or production user session.
- The exact feature under test: Assistant result capture, parameter sync, plugin discovery, state save/restore, or transport diagnosis.



### 2. Identify Candidate Transports

Build a transport inventory before decoding payloads. Prefer OS-level metadata and read-only observation.

Common transport candidates:

| Transport | Indicators | Safe Evidence to Record |
| --- | --- | --- |
| Local sockets | Listening loopback ports, short-lived localhost connections, binary or JSON request/response bursts | Process IDs, local endpoints, timing, byte counts |
| Named pipes | Pipe names containing product, vendor, session, or DAW IDs | Pipe name, owning process, access mode |
| Shared memory or file mappings | Named mappings, section objects, `shm_open` names, paired mutex/event objects | Segment name, mapped size, read-only snapshots |
| Mutex/event coordination | Product-prefixed mutexes/events without visible data segment | Names, owner process, creation timing |
| COM/RPC or service IPC | Background services, registered COM classes, RPC endpoints | Endpoint names, service identity, call timing |
| Plugin-host API | VST3/AU parameter changes, opaque state chunks, host callbacks | Parameter IDs, normalized values, state-diff hashes |

For iZotope/Ozone work in this repository, search first for named objects and shared-memory signatures, then fall back to hosted-plugin parameter diffs if no readable data transport is verified.

#### Windows Passive Discovery Runbook

Use these commands to build a metadata-only inventory. They do not hook APIs,
dump process memory, or decode protected traffic.

```powershell
# Candidate DAW, plugin, and vendor processes.
Get-Process |
    Where-Object {
        $_.ProcessName -match '(?i)(fl|reaper|ableton|pro tools|cubase|studio one|ozone|izotope|rx|neutron|nectar|visualmixer)'
    } |
    Select-Object Id,ProcessName,Path,StartTime

# Loopback sockets owned by a known DAW PID.
$pid = 24352
Get-NetTCPConnection -OwningProcess $pid -ErrorAction SilentlyContinue |
    Select-Object LocalAddress,LocalPort,RemoteAddress,RemotePort,State,OwningProcess

# Named pipes with product/vendor-looking names.
Get-ChildItem -Path '\\.\pipe\' -ErrorAction SilentlyContinue |
    Where-Object {
        $_.Name -match '(?i)(izotope|ozone|neutron|nectar|rx|visual|morephi|ipc)'
    } |
    Select-Object Name,FullName

# Services with product/vendor-looking names.
Get-CimInstance Win32_Service |
    Where-Object {
        ($_.Name + ' ' + $_.DisplayName + ' ' + $_.PathName) -match '(?i)(izotope|ozone|neutron|nectar|rx)'
    } |
    Select-Object Name,DisplayName,State,StartMode,PathName
```

If a candidate iZotope shared-memory segment is known, use More-Phi's read-only
discovery tool path. Do not enable IPC write gates during discovery.

```text
1. Call `izotope_ipc_attach` with `segment_name` or `daw_process_id`.
2. If attach succeeds, call `izotope_ipc_snapshot` with a small bounded range.
3. If a controlled stimulus is being tested, call `izotope_ipc_capture` with
   `include_changed_bytes=false` unless byte-level evidence is specifically
   authorized.
4. Call `izotope_ipc_detach`.
```

Record failures as evidence. An attach failure is a valid finding because it
means the assumed transport is absent, renamed, or inaccessible in that session.

### 3. Capture Read-Only Evidence

Use bounded captures. Avoid full-process memory dumps unless separately approved.

Recommended capture sequence:

1. Start with a baseline snapshot before triggering the feature.
2. Trigger one controlled user action, such as opening Assistant, running analysis, changing one parameter, or loading a preset.
3. Capture after the action.
4. Repeat with only one variable changed.
5. Store metadata with each capture: product version, DAW, process IDs, offsets, sizes, timestamp, and stimulus.

For More-Phi IPC tools:

- `izotope_ipc_attach` attaches read-only to a candidate segment.
- `izotope_ipc_snapshot` reads a bounded byte range and reports candidate frame positions.
- `izotope_ipc_capture` samples bounded ranges over time and can produce JSONL diff events.
- `izotope_ipc_dump` writes a bounded raw window for offline analysis.

Keep captures narrow. Start with 1-4 KiB windows around candidate headers and ring pointers, then expand only when evidence shows the relevant structure crosses a boundary.

### 4. Inspect Memory Buffers

Work from framing outward. Do not begin by assigning semantic meaning to fields.

Inspection checklist:

- Look for stable magic values, such as four-byte ASCII tags, fixed version fields, or repeating headers.
- Test endian assumptions by comparing plausible integers in little-endian and big-endian form.
- Identify length fields by checking whether candidate values bound the next record.
- Identify ring buffers by locating read/write indices, wrap-around behavior, and monotonic pointer changes.
- Search for ASCII/UTF-8 strings, UTF-16 strings, JSON delimiters, XML markers, and high-entropy encrypted/compressed blobs.
- Confirm bounds before reading any candidate payload: `offset + size` must stay inside the mapped segment.
- Reject schemas that require overlapping fields, impossible lengths, or pointer values outside the ring.

Do not mutate a live segment during initial decoding. If write behavior must be studied, reproduce the layout in a fake segment or lab fixture first.

### 5. Observe IPC API Families Safely

API observation should answer "which transport is used?" and "when does it change?", not alter target behavior.

On Windows, relevant API families to audit conceptually include:

- File mappings and sections: `CreateFileMapping`, `OpenFileMapping`, `MapViewOfFile`, `UnmapViewOfFile`.
- Synchronization: mutexes, events, semaphores, wait calls, and release calls.
- Named pipes: pipe creation, connection, read, and write calls.
- Sockets: bind, connect, accept, send, receive, and loopback endpoint metadata.
- Process/service discovery: process creation, service startup, and module load events.

On macOS/Linux, relevant families include:

- `shm_open`, `mmap`, `munmap`, `fstat`, and file-descriptor metadata.
- Unix domain sockets, loopback sockets, FIFOs, and pipes.
- `pthread`/POSIX synchronization objects when names or file descriptors are observable.

Collect metadata, call timing, object names, sizes, and return status. Avoid inline modification, bypass hooks, credential interception, or call-result spoofing.

Preferred observation order:

1. OS inventory tools and process metadata.
2. System tracing that records object names, endpoints, byte counts, and timing.
3. More-Phi read-only IPC tools for bounded snapshots and diffs.
4. More-Phi-owned fake-segment or harness instrumentation, restricted to
   metadata logging.

For Windows research, prefer non-invasive sources such as process/module lists,
handle enumeration, event tracing, loopback connection metadata, and bounded
file-mapping snapshots before considering inline interception. For macOS/Linux
research, prefer process file descriptors, `lsof`-style endpoint inventories,
`dtrace`/tracepoint-style metadata, and read-only `mmap` snapshots where
authorized.


Recommended metadata fields for each observed call:

```text
timestamp:
process_id:
thread_id:
module:
api_family:
operation:
object_name_or_endpoint:
requested_access:
buffer_size:
bytes_transferred:
return_status:
duration_us:
stimulus_id:
redaction_notes:
```

This produces enough evidence to classify the IPC transport without creating a
tool that can tamper with vendor process behavior.

### 5.1 Transport-Specific Interception Notes

Use transport-specific capture plans so that unrelated traffic is not collected.

| Transport | What to Log | What Not to Log |
| --- | --- | --- |
| Shared memory | Mapping name, mapped size, access mode, view base, bounded offset windows, read/write index candidates | Full process dumps, unrelated mappings, account/license blobs |
| Named pipes | Pipe name, open mode, byte counts, message boundaries if visible, caller process | Unbounded payload streams, credentials, activation traffic |
| Loopback sockets | Local endpoint, peer endpoint, byte counts, request/response timing, protocol hints | TLS secrets, login tokens, license payload contents |
| COM/RPC/services | Service name, interface or endpoint identity when visible, call timing, process relationship | Private auth material, entitlement responses |
| Plugin-host API | Parameter ID, normalized value, text label, state chunk hash, before/after diffs | Vendor proprietary opaque chunks beyond hashes unless separately approved |

For parameter synchronization and plugin management, most useful evidence is
often the correlation between a controlled UI/host stimulus and a small set of
changed bytes or parameter values. Capture the correlation first; decode payloads
only after the transport and feature boundary are clear.

### 5.2 Static Analysis Boundary

Static review can help explain an already observed transport, but it must stay
inside authorized interoperability limits.

Allowed static evidence:

- Public SDK contracts such as VST3, AU, JUCE, JSON-RPC, and documented host
  callback behavior.
- Import tables and plain string references that indicate transport families,
  such as file mappings, sockets, named pipes, mutexes, or service endpoints.
- More-Phi-owned source code and test fixtures.
- Vendor documentation or symbols that are intentionally published.


### 6. Reconstruct Message Schema

Once candidate frames are stable, define a manifest-like schema instead of hard-coding offsets in analysis notes.

A useful schema records:

- Segment size and segment-name resolution rules.
- Ring read-index offset, write-index offset, data offset, and capacity.
- Frame header size and field offsets.
- Magic value, version rules, message type field, sender ID, target ID, payload length, and timestamp.
- Registry layout if the segment advertises plugin instances.
- Result payload layout, including count field, entry size, parameter index offset, and normalized value offset.

Schema reconstruction method:

1. Propose the smallest schema that parses multiple captures.
2. Validate it against captures from at least two sessions.
3. Verify that payload lengths never cross mapped bounds.
4. Classify unknown message types without guessing semantics.
5. Keep message-type names provisional until matched to a repeated stimulus.
6. Store unsupported or unknown fields as raw values.

Prefer "unknown valid frame" over speculative labels. A frame should only receive semantic names like `AssistantRequest`, `AssistantResult`, `ParameterSync`, or `StateSync` after a controlled stimulus repeatedly changes that frame in the expected direction.

Candidate manifest shape:

```json
{
  "name": "ozone-ipc-candidate",
  "product": "Ozone",
  "productVersion": "unknown",
  "transport": {
    "kind": "shared_memory",
    "namePattern": "unknown",
    "segmentSize": 4194304
  },
  "ring": {
    "readPtrOffset": 256,
    "writePtrOffset": 260,
    "dataOffset": 512,
    "capacity": 4193792
  },
  "frameHeader": {
    "size": 28,
    "endianness": "little",
    "fields": [
      {"name": "magic", "offset": 0, "type": "u32"},
      {"name": "version", "offset": 4, "type": "u16"},
      {"name": "messageType", "offset": 6, "type": "u16"},
      {"name": "senderId", "offset": 8, "type": "u32"},
      {"name": "targetId", "offset": 12, "type": "u32"},
      {"name": "payloadSize", "offset": 16, "type": "u32"},
      {"name": "timestamp", "offset": 20, "type": "u64"}
    ]
  },
  "messageTypes": {
    "0x0021": "candidate_assistant_result"
  },
  "payloads": {
    "candidate_assistant_result": {
      "status": "provisional",
      "fields": [
        {"name": "parameterCount", "offset": 0, "type": "u16"}
      ]
    }
  }
}
```

Treat this as a living research artifact. Unknown fields should remain unknown
until repeated captures and controlled stimuli justify a name.

### 7. Distinguish Serialization Formats

Use evidence, not expectation:

- JSON: braces/brackets, quoted keys, UTF-8 text, parseable root object/array.
- XML/plist: angle-bracket tags, headers, property-list structure.
- Binary fixed records: repeated header, fixed offsets, length fields, numeric ranges.
- TLV/protobuf-like: field tags, varint patterns, repeated length-delimited fields.
- Compressed/encrypted: high entropy, unstable output for similar inputs, no obvious field boundaries.

If data appears encrypted or tied to licensing/security, stop decoding that branch and document it as excluded.

Practical classification workflow:

1. Run string extraction on the bounded payload window only.
2. Check whether the entire payload parses as UTF-8, UTF-16, JSON, XML, plist,
   or another structured text format.
3. If text parsing fails, test fixed-record hypotheses using known field sizes,
   length prefixes, and alignment.
4. If records vary in size, test TLV-like patterns by looking for repeated tag,
   length, value groups.
5. If entropy is high and repeated stimuli do not produce stable field
   locations, classify the payload as compressed, encrypted, or opaque.
6. Store parser failures as evidence; do not force a schema.

Document each candidate parser with:

- Input capture IDs used for validation.
- Byte range and bounds assumptions.
- Successful parse count and reject count.
- Unknown fields and unsupported message types.
- Redaction notes for any excluded security-sensitive regions.

### 8. Map Command and Control Semantics

Build a state machine from observed, authorized feature flows:

- Session start: process launches, plugin loads, registry entries appear.
- Instance discovery: Ozone instance IDs or plugin names become visible.
- Parameter sync: one controlled parameter change maps to a frame or hosted-plugin parameter delta.
- Assistant run: a user-initiated analysis produces result-like parameter recommendations.
- State save/restore: opaque plugin chunks change while normalized parameters may remain stable.

For each message type, record:

- Triggering stimulus.
- Required preconditions.
- Source and target identity fields.
- Payload shape.
- Whether the message is read-only observation, advisory recommendation, or state mutation.
- Failure behavior: timeout, oversized payload, corrupt frame, stale result, or missing instance.


For command-and-control mapping, use a stimulus matrix:

| Stimulus | Expected Safe Observation | Semantic Confidence |
| --- | --- | --- |
| Load plugin instance | Registry-like entry appears or hosted parameter set becomes available | Medium after repeated sessions |
| Rename or select instance | Instance label or active-target field changes | Medium if isolated |
| Change one parameter | One parameter ID/value pair changes or one small payload changes | High after repeated values |
| Run Assistant from UI | Result-like parameter recommendations appear after analysis | Medium until freshness is proven |
| Save preset/state | Opaque state chunk hash changes with stable parameter snapshot | Low unless vendor format is documented |
| License/login action | Excluded from decoding | Not applicable |

Semantic confidence should be recorded separately from parser confidence. A
binary frame can be parsed correctly while its purpose remains unknown.

### 9. Safe Write-Gate Policy

Live mutation must be the last step, not part of discovery. More-Phi uses a two-part write gate for IPC Assistant work:

- Environment gate: `MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE=1`.
- Per-call gate: `allow_unsafe_write=true`.

Before enabling either gate:

- The segment must be positively identified.
- The schema must parse read-only captures.
- Ring pointers must be in range.
- The request frame must fit without overwriting unread data.
- A fake-segment test must cover the same write path.
- The operation must be capture-only by default unless parameter application is explicitly requested.

If any condition fails, use the hosted-parameter diff workflow instead.

### 10. Hosted-Parameter Diff Fallback

When IPC transport is absent, opaque, or unsafe, capture the feature through standard plugin-host APIs:

1. Capture hosted Ozone parameters before the user action.
2. Run Ozone Assistant normally in the Ozone UI.
3. Capture parameters after the user action.
4. Diff normalized parameter values and stable IDs.
5. Review the diff before applying or replaying it.

This is the production-safe path for current Ozone 11 live sessions where no verified `IZOT` shared-memory ring is exposed.

## Evidence and Documentation Template

Use this template for each research run:

```text
Research ID:
Date:
Operator:
Authorization:
Product/version:
Host/version:
OS/build:
Process IDs:
Transport candidates:
Selected transport:
Capture bounds:
Stimulus:
Observed frame candidates:
Schema version:
Unsupported fields:
Excluded licensing/security observations:
Result:
Next action:
```




## Related More-Phi Artifacts

- `docs/OZONE_IPC_ASSISTANT_CAPABILITIES.md`
- `docs/ozone_ipc_default_flat_manifest.json`
- `src/AI/StandaloneMcp/IZotopeIPCDiscovery.*`
- `src/AI/StandaloneMcp/IZotopeIPCAssistant.*`
- `tests/Unit/TestStandaloneMcpServer.cpp`
