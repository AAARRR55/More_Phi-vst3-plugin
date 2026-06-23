# Debugging Guide: AI Assistant Edits Not Applied to Hosted Plugin

## Context

The AI assistant (via LLMChatClient or external MCP client) sends parameter changes to the hosted plugin through a multi-stage pipeline. When edits are "accepted" by the tool call but don't visibly change the hosted plugin, the break can occur at any of 7 stages. This guide walks through each stage with concrete diagnostic steps.

## Pipeline Overview

```
AI Assistant / MCP Client
  -> Tool name resolution (sanitized API name -> MCP tool name)
  -> MCPToolHandler::setParameter / setParametersBatch
  -> ParameterBridge::resolveParameter (stableId/index/name -> validated index)
  -> MorePhiProcessor::enqueueParameterSet (push to LockFreeQueue)
  -> flushPendingParameterCommandsForAssistant (immediate drain attempt)
    -> beginExclusivePluginUse (acquire hosted plugin exclusively)
    -> drainParameterCommandQueue (pop commands, call setValue on plugin params)
  -> [OR] processBlock audio-thread drain (ScopedTryLock on commandConsumerLock_)
  -> Morph engine may OVERWRITE the value (unless liveEditHold is set)
```

## Step-by-Step Debugging Checklist

### Stage 1: Tool Name Resolution (LLMChatClient dispatch)

**Files:** `src/AI/LLMChatClient.cpp`

The LLM model calls tools using sanitized API names (underscores only). These must resolve back to the original MCP tool names (which use dots, e.g. `hosted_plugin.set_parameter`).

**Failure mode:** `resolveApiToolNameToMcpName()` returns empty string, causing `dispatchToolInProcess` to return `unknown_tool_alias` error.

**Diagnostic:**
- Check the tool response JSON for `"error":"unknown_tool_alias"`. This means the LLM used a tool name that doesn't map back to any registered MCP tool.
- Common cause: the LLM called `hosted_plugin_set_parameter` but the registered name is `set_parameter`. The system prompt tells the LLM to use underscores, and `buildChatToolNameMap()` handles the mapping, but if the LLM invents a name not in the tool list, it silently fails.
- Use the `diagnose_parameter_pipeline` MCP tool to verify tool name mappings.

### Stage 2: Parameter Resolution (ParameterBridge)

**Files:** `src/Host/ParameterBridge.cpp`, `src/AI/MCPToolHandler.cpp`

The tool handler resolves the parameter by stableId, index, or name via `ParameterBridge::resolveParameter()`.

**Failure modes:**
- `resolution.success == false` returns JSON with `"error":"invalid_param_id"`, `"invalid_stable_id"`, `"ambiguous_param_name"`, etc.
- Plugin not loaded means `getParameterDescriptors()` returns empty; all resolutions fail.

**Diagnostic:**
- Check tool response JSON for `"success":false` and the `"error"` field.
- Call `list_parameters` tool first to verify the plugin is loaded and parameters are visible.
- If using `name`, note that resolution is case-insensitive but must be unambiguous. Partial matches that hit multiple parameters return `ambiguous_param_name`.

### Stage 3: Queue Push (LockFreeQueue)

**Files:** `src/Plugin/PluginProcessor.cpp`, `src/Core/LockFreeQueue.h`

`enqueueParameterSet()` pushes a `ParamCommand` onto a ring buffer of capacity 8192.

**Failure mode:** Queue is full so `push()` returns false, then `enqueueParameterSet()` returns false, and tool response says `"error":"queue_full"`.

**Diagnostic:**
- Check tool response for `"error":"queue_full"`.
- Use `diagnose_parameter_pipeline` to check `queueUsagePercent`. If > 80%, the queue is unhealthy.
- This is rare in normal operation but can happen if audio is stopped (DAW paused, no processBlock calls to drain) and many commands are sent rapidly.

### Stage 4: Immediate Flush (flushPendingParameterCommandsForAssistant)

**Files:** `src/Plugin/PluginProcessor.cpp`

After enqueuing, `setParameter` and `setParametersBatch` call `flushPendingParameterCommandsForAssistant()` to try draining the queue immediately without waiting for the next processBlock.

**Failure modes (flush result fields in tool response JSON):**
- `pluginUnavailable: true` means no hosted plugin loaded, or `getParameters().size() == 0`.
- `exclusiveAccessTimedOut: true` means `beginExclusivePluginUse(75ms)` timed out because processBlock is actively using the plugin. This is transient; the commands remain queued and will drain on the next processBlock.
- `drained: 0` with `pendingAfter > 0` means the `commandConsumerLock_` was held by processBlock, preventing drain. Again transient.

**Diagnostic:**
- Check the `"flush"` object in the tool response: `appliedNow`, `pendingAfter`, `pluginUnavailable`, `exclusiveAccessTimedOut`.
- If `appliedNow > 0` and edits still don't take effect, the problem is downstream (Stage 6 or 7).
- If `pluginUnavailable: true`, the plugin may have been unloaded, or may be in a restoring state.

### Stage 5: Audio-Thread Drain (processBlock)

**Files:** `src/Plugin/PluginProcessor.cpp`

If the immediate flush didn't fully drain, the audio thread drains on the next processBlock call. But there are guards:

**Failure modes:**
- `prepared == false` means processBlock returns immediately. This means `prepareToPlay()` was never called or `releaseResources()` was called.
- `isRestoring_ == true` means morph processing is skipped entirely. The plugin is in state-restore mode (loading a session). Commands stay in the queue until restore completes.
- `commandConsumerLock_` try-lock fails (`canTouchHostedParameters == false`) when the flush path on another thread holds the lock. Transient, resolved next block.

**Diagnostic:**
- If DAW is **stopped/paused** and not calling processBlock, commands will sit in the queue. The immediate flush (Stage 4) is designed to handle this, but if exclusive access also fails, nothing drains.
- Use `diagnose_parameter_pipeline` to check `isRestoring` and `isPrepared` flags. If `isRestoring` is stuck true, the async timer-based plugin reload may have failed.

### Stage 6: drainParameterCommandQueue — Actual Write

**Files:** `src/Plugin/PluginProcessor.cpp`

This is where `params[index]->setValue(clamped)` is called on the hosted plugin. Two paths exist:

1. **With exclusive plugin** (flush path): writes directly to `exclusivePlugin->getParameters()[index]->setValue()`.
2. **Without exclusive plugin** (audio-thread path): writes via `paramBridge.setParameterNormalized()`.

**Failure modes:**
- Index out of range: `index >= params.size()` is silently skipped on the exclusive-flush path (path 1). **AUDIT-FIX 4.7:** `drainParameterCommandQueue` now tracks out-of-range writes via an `outOfRangeCount` counter threaded through `ParameterCommandFlushResult`. The MCP tool response includes `flush.out_of_range_count` and returns `error: "parameter_index_out_of_range"` in the verification payload instead of falling through to `value_drift`.
- ParameterBridge throttling (path 2 only): `shouldThrottle()` returns true if the same value (within 0.01) was written less than 2ms ago. Write is **silently dropped**. This is designed to prevent flooding but can suppress legitimate rapid edits.
- Hosted plugin `setValue()` throws and is caught silently.

**Diagnostic:**
- If the flush result shows `appliedNow > 0` but the plugin doesn't reflect the change, check whether the hosted plugin internally rejects `setValue()` calls (some plugins ignore values outside certain states, or only accept changes on their own thread).
- For the throttle case: verify the value being set is actually different from what the parameter already has. A delta < 0.01 within 2ms will be throttled.

### Stage 7: Morph Engine Overwrites the Value

**Files:** `src/Plugin/PluginProcessor.cpp`

**This is the most common cause of "edits don't stick."** After draining the command queue, processBlock runs the morph engine which computes `finalOutput_` values and writes them to all parameters via `paramBridge.setParameterNormalized()`. This **overwrites** the value just set by the assistant on the very same processBlock cycle.

**AUDIT-FIX 4.5:** The MCP tool response now includes `morph_overwrite_risk` detection. If `valueAfter` equals `valueBefore` (despite the write being enqueued and flushed), the verification status reports `"morph_overwrite_risk"` with a corrective action suggesting the user pause morph or increase the live-edit hold threshold. This helps AI assistants distinguish "my edit was overwritten by morph" from "the plugin rejected my value."

**Protection mechanism:** When `cmd.holdAgainstMorph == true` (which MCP and Assistant sources always set), the drain code sets `liveEditHold_[paramIndex] = 1` along with the morph position at time of edit. The morph-apply loop checks this flag and **skips** writing the morph value for that parameter until the user moves the morph pad/fader past `MORPH_POS_THRESHOLD`.

**Failure modes:**
- `holdAgainstMorph` not being set means liveEditHold is never armed and morph immediately overwrites. MCPToolHandler always passes `true` for this flag. AIAssistant also passes `true`. If a custom caller passes `false`, values will be overwritten.
- Morph position moves past threshold, `shouldReleaseLiveEditHold()` returns true, hold is released, and morph overwrites the value. This is by design.
- `snapshotBank.hasAnyOccupied() == false` means morph computation is skipped entirely, so no overwrite occurs.

**Diagnostic:**
- If you set a parameter and it immediately reverts: check whether snapshots are occupied and the morph engine is active.
- If values persist briefly then revert: the morph pad was moved, releasing the liveEditHold.
- To make edits stick permanently: recall a snapshot with the desired values, or move the morph cursor to a position where the interpolated output matches the desired values.
- Use `diagnose_parameter_pipeline` to check `liveEditHoldsActive` and `snapshotsOccupied`.

## Quick Diagnostic Flowchart

```
1. Does the tool response show "success": true?
   NO  -> Check Stage 1 (tool name) or Stage 2 (parameter resolution)
          AUDIT-FIX 4.3: success is now gated on actual verification.
          Check "verification" → "status" field for the true outcome.
   YES |

2. Does the verification show "status": "success"?
   NO  -> "queued" = still pending in queue (Stage 4/5)
          "value_drift" = plugin snapped value differently (Stage 6)
          "value_drift_discrete" = discrete parameter snapped to wrong step
          "morph_overwrite_risk" = morph likely overwrote the edit (Stage 7)
          "parameter_index_out_of_range" = index exceeds plugin param count
          "failure" = queue full or parameter unresolved
   YES |

3. Does "appliedNow" > 0 in the flush result?
   NO  -> Check Stage 4 (pluginUnavailable? exclusiveAccessTimedOut?)
          If both false, commands are queued for processBlock -> Stage 5
   YES |

4. Does the hosted plugin's parameter value change momentarily?
   NO  -> Stage 6: throttle, index mismatch, or hosted plugin rejecting setValue
          Check flush.out_of_range_count > 0 for index-out-of-range
   YES |

5. Does the value revert after a moment?
   YES -> Stage 7: morph engine overwrite. Check verification.status for
          "morph_overwrite_risk". Use liveEditHold and morph position.
   NO  -> Edit is applied successfully. If UI doesn't reflect it, it's a UI refresh issue.
```

## Key Files

| File | Role in Pipeline |
|------|-----------------|
| `src/AI/LLMChatClient.cpp` | Tool name mapping, in-process dispatch, MCP TCP fallback |
| `src/AI/MCPToolHandler.cpp` | `setParameter` / `setParametersBatch` handlers |
| `src/Host/ParameterBridge.cpp` | Parameter resolution, throttled setValue, state capture |
| `src/Plugin/PluginProcessor.cpp` | enqueueParameterSet, drainParameterCommandQueue, flushPendingParameterCommandsForAssistant, processBlock drain, morph overwrite |
| `src/Core/LockFreeQueue.h` | Ring buffer push/pop, capacity 8191 usable slots |
| `src/Host/PluginHostManager.cpp` | Exclusive plugin access (beginExclusivePluginUse) |
| `src/AI/AIAssistant.cpp` | Preview/commit flow for staged changes |

## Verification

After identifying and fixing the issue:

1. **Build:** `cmake --preset windows-msvc-release && cmake --build build/windows-msvc-release --config Release --parallel 2`
2. **Run unit tests:** `ctest --test-dir build/windows-msvc-release --build-config Release -R "TestAIRegressions|TestMCPIntegration|TestMCPServerUnit" --output-on-failure`
3. **Manual test in DAW:**
   - Load More-Phi, then load a hosted plugin
   - Open AI chat, send "set parameter 0 to 0.75"
   - Verify the tool response JSON shows `"success":true, "verification":{"status":"success"}`
   - Verify the hosted plugin's first parameter reads 0.75
   - Move the morph pad, verify the value yields to morph interpolation (liveEditHold released)
   - Set the parameter again, verify it holds until morph pad moves
   - Check for `"morph_overwrite_risk"` in verification when morph is active and edit reverts
   - Set an out-of-range parameter index (e.g., 9999 on a small plugin) and verify `"parameter_index_out_of_range"` error
4. **Verification status reference:**

| Status | Meaning | Action |
|--------|---------|--------|
| `success` | Value applied and verified within tolerance | Edit confirmed |
| `queued` | Command in queue, not yet applied | Wait for audio callback or retry |
| `value_drift` | Applied value differs from requested (continuous param) | Re-read parameter, adjust value |
| `value_drift_discrete` | Discrete parameter snapped to different step | Choose a valid step value |
| `morph_overwrite_risk` | Morph engine likely overwrote the edit | Pause morph or increase live-edit hold |
| `parameter_index_out_of_range` | Index exceeds plugin parameter count | Use `list_parameters` to find valid indices |
| `failure` | Queue full or parameter unresolved | Check error_reason for details |
