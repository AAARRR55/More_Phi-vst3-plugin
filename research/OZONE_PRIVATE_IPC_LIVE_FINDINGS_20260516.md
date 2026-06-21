# Ozone Private IPC Live Findings - 2026-05-16

## Scope

This document records authorized interoperability findings from the active
FL Studio + More-Phi + iZotope session. The scope is limited to Ozone Assistant,
Relay, Neutron Visual Mixer, and plugin-control/data-link behavior.

Licensing, entitlement, PACE/iLok, anti-tamper, credential, and account-security
traffic are out of scope.

## Active Process Context

| Item | Value |
| --- | --- |
| Host process | `FL64.exe` |
| PID | `24352` |
| Process isolation | No `ilbridge.exe` observed |
| Loaded More-Phi endpoint ports | `127.0.0.1:30002`, `127.0.0.1:30003` |

The loopback ports are More-Phi MCP JSON-RPC endpoints, not iZotope IPC
listeners. They require per-instance bearer-token authentication.

## Authenticated More-Phi Endpoint Findings

The user-provided spaced 16-byte bearer tokens authenticated successfully:

| Port | More-Phi instance ID | Hosted plugin | Parameter count |
| ---: | --- | --- | ---: |
| `30002` | `a8 d2 17 13 f7 56 8d 2b 82 54 f8 38 14 08 5b 39` | `Neutron 3 Visual Mixer` | `0` |
| `30003` | `3e 12 22 a5 1b 4f 87 39 d9 78 ef f0 72 b1 14 fb` | `Relay` | `17` |

The compact no-space token form was rejected by More-Phi auth. The spaced form
is the active bearer-token representation.

Both instances report:

```json
{
  "izotope_ipc_status": {
    "attached": false,
    "segment_name": "",
    "read_only": true,
    "platform": "windows",
    "success": true
  }
}
```

This confirms the two hosted plugins are reachable through More-Phi, but the
manifest-driven iZotope shared-memory IPC layer remains unattached.

Relay's currently exposed parameters are:

| Index | Name | Value | Display |
| ---: | --- | ---: | --- |
| `0` | `Global Bypass` | `0.000000000` | `Off` |
| `1` | `Global Dirty Flag` | `0.500000000` | `0.50` |
| `2` | `Global Output Gain` | `0.648852229` | `-14.58` |
| `3` | `Pan` | `0.500000000` | `0.00` |
| `4` | `Width` | `0.500000000` | `0.00` |
| `5` | `Sum to Mono` | `0.000000000` | `Off` |
| `6` | `Swap Channels` | `0.000000000` | `Off` |
| `7` | `Invert Phase` | `0.000000000` | `Off` |
| `8` | `Delay (L)` | `0.000000000` | `0.00` |
| `9` | `Delay (R)` | `0.000000000` | `0.00` |
| `10` | `Enable HPF` | `0.000000000` | `Off` |
| `11` | `HPF Slope` | `0.000000000` | `12 dB` |
| `12` | `HPF Cutoff Frequency` | `0.600000024` | `80.00` |
| `13` | `Unmask Bypass` | `1.000000000` | `On` |
| `14` | `Unmask Amount` | `0.500000000` | `100.00` |
| `15` | `Unmask Enable Dynamics` | `0.000000000` | `Off` |
| `16` | `Unmask SC` | `0.000000000` | `Off` |

A 45-second More-Phi poll of Relay parameters observed no parameter changes.
That means the linked iZotope state was idle during the poll, or the current
link does not publish changes through ordinary Relay VST parameters until a
user-visible action occurs.

## Loaded iZotope Modules

| Module | Role |
| --- | --- |
| `Ozone Pro.vst3` | VST3 wrapper |
| `iZOzonePro.dll` | Ozone implementation |
| `Neutron 3 Visual Mixer.vst3` | VST3 wrapper |
| `iZNeutron3VisualMixer.dll` | Visual Mixer implementation |
| `Relay.vst3` | VST3 wrapper |
| `iZRelay.dll` | Relay implementation |
| `Meter Tap 3.dll` | iZotope metering support |

## Negative Transport Findings

No exposed iZotope transport was observed through the normal OS IPC inventory:

- No iZotope named `CreateFileMappingW` mapping.
- No `IZOT` shared-memory ring.
- No iZotope named pipe.
- No iZotope-owned loopback TCP listener beyond the More-Phi MCP ports.
- No `OpenFileMappingW` target that matched the previously assumed
  `Global\\iZotope_IPC_Session_{pid}` pattern.

This means the older manifest-driven shared-memory-ring assumption does not
match this Ozone/Neutron/Relay session.

## Positive Internal Findings

Static string and pointer-table analysis found repeated internal labels:

| Module | Labels |
| --- | --- |
| `iZOzonePro.dll` | `Ozone IPC 1`, `Master Assistant`, `PROCESSING_LISTENING`, `LEARNING_EQ_AND_CLASSIFYING_GENRE`, `PROCESSING_SETTING_SIGNAL_CHAIN`, `SmoothAudioDataStream` |
| `iZNeutron3VisualMixer.dll` | `Neutron IPC 2`, `SmoothAudioDataStream`, `AuxStream`, `Track Assist Processor`, `Balance Assistant Learner` |
| `iZRelay.dll` | `Neutron IPC 2`, `SmoothAudioDataStream`, `AuxStream`, `Track Assist Processor`, `Balance Assistant Learner` |

These are in-process implementation labels and data tables, not named OS IPC
objects.

## Ozone Assistant State Path

Runtime data-page monitoring identified a live Ozone Assistant state path:

| Symbolic name | Module RVA | Evidence |
| --- | ---: | --- |
| `ozone_master_assistant_state` | `iZOzonePro.dll+0xEAD3E0` | Accesses Assistant state string-table page |
| `ozone_master_assistant_caller` | `iZOzonePro.dll+0x166CA90` | Calls `+0xEAD3E0` |
| `ozone_master_assistant_secondary` | `iZOzonePro.dll+0xEAD930` | Called by the same caller after the state path |

Observed behavior:

- `+0xEAD3E0` is hot during idle polling.
- A 60-second state-diff run observed 548 calls.
- Return value was consistently `0x1`.
- Two state pointers remained stable during the capture:
  - `arg0 = 0x0e93a620`
  - `arg1 = 0x424d4bd0`
- No byte changes were observed in the first 4 KiB of those objects during the
  capture window.

Interpretation: this path is a real Assistant state/polling path, but the
captured window only saw idle/stable state. A transition capture is still needed
while Assistant moves through listening/learning/setting-chain states.

An additional 90-second trace of the caller with first-level pointer following
observed 60 calls and 41 child pointer snapshots, but no changed bytes. The
trace overhead is higher in pointer-follow mode, but it confirms that the
top-level Assistant caller object and its immediate child objects were also
stable during the observed window.

## Linked-Plugin Data Path

Runtime data-page monitoring of Ozone/Neutron/Relay link labels produced one
live data-table access:

| Symbolic name | Module RVA | Evidence |
| --- | ---: | --- |
| Ozone `SmoothAudioDataStream` constructor/access path | `iZOzonePro.dll+0xFD81EA` | Read from `SmoothAudioDataStream` table |
| Likely containing function start | `iZOzonePro.dll+0xFD7F30` | Constructor-like function building stream objects |
| Helper called nearby | `iZOzonePro.dll+0xFD7C20` | Stream/string helper |
| Downstream helpers | `iZOzonePro.dll+0xFDA470`, `iZOzonePro.dll+0xFDA670` | Called after stream label construction |

Observed behavior:

- The Ozone side touched `SmoothAudioDataStream`.
- Neutron Visual Mixer and Relay watch pages were enabled but did not fire
  during that capture window.

Interpretation: Ozone's smooth audio/data stream code path is live, but the
Visual Mixer/Relay peer-side activity was not captured in the observed interval.

## Capture Artifacts

Key artifacts generated by the live session:

| Artifact | Meaning |
| --- | --- |
| `tools/live_captures/ipc_decode/izotope_ipc_xrefs_pointer.json` | Static string/pointer-table scan |
| `tools/live_captures/ipc_decode/izotope_data_watch_20260516_031129_summary.json` | First Assistant data-page watch |
| `tools/live_captures/ipc_decode/izotope_function_trace_20260516_031425_summary.json` | Ozone Assistant state function trace |
| `tools/live_captures/ipc_decode/izotope_state_diff_trace_20260516_031910_summary.json` | 60-second Assistant state object diff |
| `tools/live_captures/ipc_decode/izotope_data_watch_20260516_032238_summary.json` | Linked-plugin data-page watch |
| `tools/live_captures/ipc_decode/izotope_state_diff_trace_20260516_032906_summary.json` | Pointer-follow Assistant caller diff |
| `tools/live_captures/linked_instances/p30002_plugin_info.json` | Authenticated Neutron Visual Mixer endpoint identity |
| `tools/live_captures/linked_instances/p30003_plugin_info.json` | Authenticated Relay endpoint identity |
| `tools/live_captures/linked_instances/p30003_relay_param_poll_20260516_033218.jsonl` | 45-second Relay parameter poll |

## Post-Restart Live IPC Invoke Attempt

After restarting FL Studio with the More-Phi IPC write gate enabled, the active
host process was:

| Item | Value |
| --- | --- |
| Host process | `FL64.exe` |
| PID | `19604` |
| Process status | Responsive |
| Process isolation | No `ilbridge.exe` observed |

The live More-Phi endpoint map was:

| Port | Token | More-Phi instance ID | Hosted plugin | Parameter count |
| ---: | --- | --- | --- | ---: |
| `30001` | `71 b5 40 6c 47 9c 40 c5 4b 01 be 8f 6c a8 3e 7f` | `70 a0 db 00 68 4d e1 a1 b6 a0 8b bf b2 01 0f 0c` | `Ozone Pro` | `646` |
| `30002` | `68 9c 36 58 f6 4b 30 5a 1c 6e f5 37 54 6e b7 eb` | `23 23 e5 73 fc 64 02 cc 43 a5 7a ed 72 25 b1 a8` | `Relay` | `17` |
| `30003` | `11 c5 d8 08 a1 33 e9 9a d4 b6 e0 d8 0f 22 21 74` | `16 27 43 34 9d 25 50 a5 9d 6d 7b 67 fb 62 6d d9` | `Neutron 3 Visual Mixer` | `0` |

All three ports were listening on `127.0.0.1` and owned by `FL64.exe` PID
`19604`. The loaded iZotope modules in the same process were:

- `iZOzonePro.dll`
- `iZRelay.dll`
- `iZNeutron3VisualMixer.dll`
- `Ozone Pro.vst3`
- `Relay.vst3`
- `Neutron 3 Visual Mixer.vst3`
- `Meter Tap 3.dll`

This rules out plugin process isolation as the active cause of the IPC attach
failure in this session.

The write-gated `ozone_run_assistant` call was then attempted against the Ozone
endpoint using the manifest template
`Global\\iZotope_IPC_Session_{pid}`. The result was:

```json
{
  "code": "ipc_attach_failed",
  "error": "OpenFileMappingW failed for read/write segment.",
  "success": false
}
```

The failure artifact is:

`tools/live_captures/linked_instances/ozone_run_assistant_ipc_invoke_global_template_20260516_041640.json`

The handle scan found iZotope mutexes:

- `\Sessions\1\BaseNamedObjects\iZotopeOZONEPROMSMutex`
- `\Sessions\1\BaseNamedObjects\iZotopeRELAYMutex`
- `\Sessions\1\BaseNamedObjects\iZotopeVISUALMIXERMutex`

It did not find an iZotope/Ozone/Relay/Neutron named `Section` object. Candidate
named sections that could be opened read-only were scanned for:

- `IZOT` little-endian and big-endian byte patterns
- `Ozone`
- `iZotope`
- `Neutron`
- `Relay`
- `IPC`
- `Assistant`
- `SmoothAudioDataStream`
- `Track Assist`
- `Neutron IPC 2`
- `Ozone IPC 1`

No signature hits were found. The scan artifact is:

`tools/sysinternals/named_section_content_scan_20260516_041923.json`

Interpretation: the More-Phi write gate is open and the Ozone MCP endpoint is
healthy, but the manifest-driven shared-memory Assistant path still has no
attachable named segment in this live session. The visible iZotope IPC evidence
is limited to mutexes and in-process module state, not a public file-mapping
ring.

## Working Conclusion

The private iZotope path in this session is not an externally addressable named
mapping. The useful attack surface for interoperability research is now the
in-process state/data path:

1. Ozone Assistant state machine around `iZOzonePro.dll+0x166CA90`,
   `+0xEAD3E0`, and `+0xEAD930`.
2. Ozone data-stream construction around `iZOzonePro.dll+0xFD7F30` and
   `+0xFD81EA`.
3. Neutron/Relay tables for `Neutron IPC 2`, `AuxStream`,
   `SmoothAudioDataStream`, `Track Assist Processor`, and
   `Balance Assistant Learner`.

The next decoding step is to run the state-diff tracer while the user starts the
Assistant or manipulates the linked Relay/Visual Mixer session, then correlate
changed offsets with visible state transitions and parameter diffs.

## Assistant Transition Trace - Args 2/3

After two idle captures of the Ozone Assistant polling path, the secondary
handler at `iZOzonePro.dll+0xEAD930` was traced again with snapshots focused on
arguments `2` and `3`:

```powershell
python tools\izotope_state_diff_trace.py --pid 19604 `
  --target iZOzonePro.dll+0xEAD930:ozone_master_assistant_secondary_args23 `
  --duration 60 --snapshot-args 2,3 --snapshot-bytes 4096 --heartbeat-every 50
```

Artifact:

`tools/live_captures/ipc_decode/izotope_state_diff_trace_20260516_043050.jsonl`

Summary:

| Metric | Value |
| --- | ---: |
| Calls observed | `543` |
| `state_changed` events | `15` |
| Changed argument | `arg2` only |
| Return values | `0x1` for `542` calls, one non-standard return |
| Snapshot bytes | `4096` |

The moving `arg2` object is not a single stable pointer. It rotates through a
small pool:

- `0xda2f40`
- `0xda3000`
- `0xda3180`
- `0xda3240`

Observed change classes:

| Calls | Changed bytes | Interpretation |
| --- | ---: | --- |
| `49`, `58`, `66`, `177`, `178`, `200`, `233` | `8` | Pointer-slot or queue-entry movement. Four-byte values move from one slot to another, then the old slot is cleared. |
| `251`, `264`, `312`, `412` | `87`-`99` | Larger payload/state mutation. These are the strongest candidates for Assistant transition/result-state updates. |
| `366`, `367`, `374` | `9` | Compact state update. Each includes a one-byte field changing `0x04 -> 0x05`, likely a phase/status enum or small counter. |

Representative changed ranges:

| Call | Pointer | Offset(s) | Change |
| ---: | --- | --- | --- |
| `49` | `0xda3240` | `3464`, `3480` | `a0 f5 48 09 -> 90 6c 72 47`; old slot cleared |
| `177` | `0xda3180` | `3080`, `3176` | `00 00 00 00 -> 70 e5 b7 0c`; old slot cleared |
| `251` | `0xda3240` | `160`, `164`, `392+` | Larger 87-byte payload mutation |
| `264` | `0xda3180` | `160`, `164`, `352+` | Larger 90-byte payload mutation |
| `312` | `0xda3000` | `544`, `548`, `736+` | Larger 90-byte payload mutation |
| `366` | `0xda3000` | `3272`, `3320`, `3438` | Includes `0x04 -> 0x05` at offset `3438` |
| `367` | `0xda3240` | `2696`, `2744`, `2862` | Includes `0x04 -> 0x05` at offset `2862` |
| `374` | `0xda3180` | `2888`, `2936`, `3054` | Includes `0x04 -> 0x05` at offset `3054` |
| `412` | `0xda2f40` | `736`, `928`, `1160+` | Larger 99-byte payload mutation |

Interpretation: `iZOzonePro.dll+0xEAD930` receives a mutable Assistant work item
or message object in `arg2`. The pointer rotates across a small object pool,
which is consistent with a queue, ring, or allocator-backed message batch rather
than one persistent state-manager object. `arg3` remained effectively stable
during this trace, except for a new initial pointer observed later in the run.

Next decoding target: repeat the same trace while also collecting an Ozone
parameter diff through More-Phi. The goal is to correlate the larger `arg2`
payload mutations with the exact hosted-plugin parameter changes produced by
Assistant.

## Assistant Transition + Parameter Correlation

A longer synchronized run was then executed with both:

- `iZOzonePro.dll+0xEAD930` tracing `arg2,arg3`
- More-Phi MCP parameter polling of Ozone Pro on port `30001`

Artifacts:

- `tools/live_captures/ipc_decode/izotope_state_diff_trace_20260516_043615.jsonl`
- `tools/live_captures/ipc_decode/izotope_state_diff_trace_20260516_043615_summary.json`
- `tools/live_captures/linked_instances/linked_instance_monitor_20260516_043614.jsonl`
- `tools/live_captures/linked_instances/linked_instance_monitor_20260516_043614_summary.json`

Trace summary:

| Metric | Value |
| --- | ---: |
| Calls observed | `1329` |
| `state_changed` events | `126` |
| `arg2` changes | `119` |
| `arg3` changes | `7` |
| Primary `arg2` object pool | `0xda2f40`, `0xda3000`, `0xda3180`, `0xda3240` |

Parameter summary:

| Metric | Value |
| --- | ---: |
| Ozone parameter change bursts | `7` |
| Total parameter changes | `32` |
| First visible parameter burst | `60.53s` after monitor start |
| Last visible parameter burst | `74.29s` after monitor start |

The visible Ozone parameter writes occurred while the trace was still active and
were bracketed by nearby `arg2` mutations:

| Monitor elapsed | Parameter changes | First parameter | Nearby trace events within +/-3s |
| ---: | ---: | --- | ---: |
| `60.53s` | `15` | `EQ: St/M/L Frequency 1` | `11` |
| `62.18s` | `12` | `EQ2: St/M/L Frequency 1` | `9` |
| `69.88s` | `1` | `MAX: Character` | `5` |
| `70.45s` | `1` | `MAX: Character` | `7` |
| `71.01s` | `1` | `MAX: Character` | `7` |
| `72.65s` | `1` | `MAX: Stereo Ind. Transient Amt` | `10` |
| `74.29s` | `1` | `MAX: Transient Shaping Amt` | `9` |

The 32 captured Ozone parameter changes grouped by module:

| Module/Area | Representative changes |
| --- | --- |
| EQ | Frequency 1, Frequency 8, Gain 1, Gain 8 |
| Maximizer | Threshold, Ceiling, Character, Stereo Independent Transient Amount, Transient Shaping Amount |
| Dynamics | Band 1 Compressor Threshold |
| Dynamic EQ | Thresholds 1-6, Frequencies 3/4 |
| EQ2 | Frequencies 1-3, Gains 1-3, Q 1-3, Enable 3, Shape 2/3 |

Interpretation: `arg2` at `iZOzonePro.dll+0xEAD930` is now strongly correlated
with Assistant work/result application. It is not yet proven to be the serialized
parameter recommendation payload, but it mutates in the same time window as
visible Assistant-applied parameter writes. `arg3` appears to carry smaller
one-byte flags or gate bits around the same operation.

Practical consequence: direct named shared-memory invocation remains blocked,
but the internal Assistant application path has a concrete decode target:
`iZOzonePro.dll+0xEAD930` argument `2`, especially larger `state_changed` events
near visible parameter bursts.

## Offline Arg2 Decoder

An offline decoder was added:

`tools/decode_ozone_assistant_arg2.py`

The decoder reads only saved JSONL artifacts. It does not attach to FL Studio,
read process memory, write process memory, patch code, or invoke plugins.

Inputs used for the first decode pass:

- `tools/live_captures/ipc_decode/izotope_state_diff_trace_20260516_043615.jsonl`
- `tools/live_captures/linked_instances/linked_instance_monitor_20260516_043614.jsonl`

Generated reports:

- `tools/live_captures/ipc_decode/ozone_arg2_decode_20260516_044510.json`
- `tools/live_captures/ipc_decode/ozone_arg2_decode_20260516_044510.md`

Decoder result:

| Metric | Value |
| --- | ---: |
| State-change events analyzed | `126` |
| Ozone parameter changes analyzed | `32` |
| Parameter bursts | `7` |
| Near-range parameter candidates | `31` |
| Strong non-common aligned `f32` matches | `0` |

The stricter decode pass intentionally excludes common float values such as
`0.0`, `0.5`, and `1.0`, because they create too many false positives in
pointer-heavy C++ objects. It also checks only aligned 32-bit float slots.

Current interpretation:

- `arg2` is strongly correlated with the Assistant apply path.
- The visible normalized parameter values are not trivially present as aligned
  `float32` values near the changed ranges in this capture.
- The remaining candidate matches are mostly weak index-like hits, useful for
  narrowing offsets but not sufficient to claim a decoded parameter record.
- The most stable structural fields remain offsets `160`/`164`, `352`/`356`,
  `544`/`548`, and `736`/`740`, which repeat as `u16 + u8` style updates across
  the rotating object pool.

Next decode step: run a controlled single-parameter apply/capture if possible,
or capture a smaller Assistant operation that changes only one module. This is
needed to distinguish true parameter IDs from incidental integer matches.

## Safe Next Commands

Assistant transition capture:

```powershell
python tools\izotope_state_diff_trace.py --pid 24352 `
  --target iZOzonePro.dll+0x166CA90:ozone_master_assistant_caller `
  --duration 90 --snapshot-args 0 --snapshot-bytes 8192 --heartbeat-every 100
```

Secondary Assistant handler capture:

```powershell
python tools\izotope_state_diff_trace.py --pid 24352 `
  --target iZOzonePro.dll+0xEAD930:ozone_master_assistant_secondary `
  --duration 90 --snapshot-args 0,1 --snapshot-bytes 8192 --heartbeat-every 100
```

Linked-plugin data-path watch:

```powershell
python tools\izotope_data_watch.py --pid 24352 --duration 90 --max-pages 40 `
  --pattern "Neutron IPC 2" `
  --pattern "SmoothAudioDataStream" `
  --pattern "AuxStream" `
  --pattern "Track Assist Processor" `
  --pattern "Balance Assistant Learner"
```

These commands are read-only. They do not patch code, write target memory, or
send messages into iZotope components.
