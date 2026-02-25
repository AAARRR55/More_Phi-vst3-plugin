# MorphSnap Plugin Disconnection Troubleshooting Guide

## Problem Description

When using MorphSnap plugin in a DAW, the connection to the hosted (morphed) plugin is lost during:
- **Track export** (offline rendering/bounce)
- **Closing and reopening** MorphSnap
- **DAW project load/save cycles**

The morphed plugin slot shows "No Loaded Plugin" and the preset/settings disappear, requiring manual reloading of the target plugin.

---

## Root Causes (Fixed in Latest Update)

The issue was caused by several factors in the plugin state restoration system:

### 1. Timer-Based Deferred Loading Failure
The original implementation used `MessageManager::callAsync()` which could silently drop callbacks when:
- The editor was closed (common in FL Studio)
- The DAW was in "Smart disable" mode
- During offline rendering (export)

### 2. Insufficient Retry Logic
The original retry mechanism only attempted 5 times (250ms), which wasn't enough for:
- Slow plugin scanners
- Complex plugin initialization
- DAWs with delayed plugin format registration

### 3. Race Condition in State Restoration
The morph engine could start processing before the hosted plugin was fully loaded, causing:
- Partial state application
- Plugin loading to be abandoned mid-process

### 4. Missing State Persistence
When the plugin was unloaded (but description remained), the pending state wasn't being saved to the DAW's project file.

---

## Solutions Implemented

### 1. Robust Timer-Based Loading
- Replaced unreliable `callAsync()` with `Timer`-based polling
- Timer fires on the message thread every 50ms
- Works even when editor is closed or during export

### 2. Extended Retry Window
- Increased retry attempts from 5 to **10 attempts** (500ms total)
- Added proper state tracking across retries
- Only unblocks the morph engine after success or final failure

### 3. Synchronous Load Path
- When on the message thread, loads synchronously for immediate results
- Falls back to timer-based loading when on other threads
- Ensures plugin is ready before morph processing begins

### 4. Enhanced State Persistence
- `getStateInformation()` now saves `pendingHostedState_` even when plugin is unloaded
- `lastDescription` is always persisted via `getLastDescription()`
- MCP identity is preserved across export cycles

### 5. Plugin Format Readiness Check
- New `ensurePluginFormatsReady()` function verifies format manager is initialized
- Prevents premature load attempts before VST3/AU formats are registered

### 6. Comprehensive Logging
- Added detailed DBG logging throughout the load/restore process
- Helps diagnose issues in debug builds
- Tracks retry attempts and failure points

---

## User Workarounds (Before Update)

If you cannot update to the fixed version immediately:

### For Track Export
1. **Bounce in real-time** instead of offline rendering:
   - In Ableton Live: Enable "Realtime" in export dialog
   - In FL Studio: Use "Render to wav file" with realtime option
   - In Reaper: Check "Render at normal speed" in render dialog

2. **Freeze/Flatten the track** before export:
   - This captures the current plugin state as audio
   - Bypasses the need for plugin restoration during export

3. **Export in smaller segments**:
   - Reduces the chance of timeout during plugin restoration

### For Project Save/Load
1. **Always save the project with the plugin editor open**:
   - The issue is more likely when the editor is closed
   - Keep MorphSnap's UI visible during save

2. **Create a preset file as backup**:
   - Use MorphSnap's internal preset system (16 banks × 128 presets)
   - Save important configurations as named presets

3. **Save hosted plugin preset separately**:
   - Save the hosted plugin's preset within its own interface
   - This provides a backup if MorphSnap loses the connection

### DAW-Specific Settings

#### FL Studio
- Disable "Smart disable" for the MorphSnap plugin:
  - Right-click plugin wrapper → "Smart disable" → Uncheck
- Increase plugin delay compensation if needed

#### Ableton Live
- Disable "Freeze Track" before saving
- Use "Collect All and Save" to ensure plugin state is preserved

#### Reaper
- Increase " anticipative FX processing" buffer
- Disable "Allow complete unload of VST plug-ins" in Preferences → Plug-ins → VST

#### Logic Pro
- Use "Bounce in Place" before final export
- Save project as "Project Alternative" before major changes

---

## Verification Steps (After Update)

To verify the fix is working:

### Test 1: Basic State Persistence
1. Load MorphSnap on a track
2. Load a hosted plugin (e.g., a synth or effect)
3. Capture a snapshot
4. Save the DAW project
5. Close and reopen the DAW
6. Load the project
7. **Expected**: Hosted plugin loads automatically with saved state

### Test 2: Track Export
1. Create a simple pattern with MorphSnap + hosted instrument
2. Use offline render/export (not real-time)
3. **Expected**: Exported audio contains the processed sound without "No Loaded Plugin" errors

### Test 3: Rapid State Changes
1. Load MorphSnap + hosted plugin
2. Quickly switch between snapshots while playing
3. Save and load project repeatedly
4. **Expected**: Plugin connection remains stable

### Test 4: Editor Closed
1. Load MorphSnap + hosted plugin
2. Close MorphSnap's editor window
3. Save and reopen project
4. **Expected**: Plugin is restored even with editor closed

---

## Debug Information

If issues persist after the update, enable debug logging:

### Windows
Run your DAW from command line with debug output:
```cmd
set JUCE_DEBUG=1
"C:\Program Files\YourDAW\YourDAW.exe" 2> morphsnap_debug.log
```

Look for lines containing:
- `setStateInformation` - State restoration process
- `loadHostedPluginFromState` - Plugin loading attempts
- `timerCallback` - Retry attempts

### macOS
```bash
JUCE_DEBUG=1 /Applications/YourDAW.app/Contents/MacOS/YourDAW 2> ~/morphsnap_debug.log
```

### Key Log Messages

**Success case:**
```
setStateInformation: Found hosted plugin description: PluginName (VST3)
setStateInformation: On message thread, loading synchronously
loadHostedPluginFromState: Successfully loaded plugin: PluginName
loadHostedPluginFromState: Restored plugin state, size: 1234 bytes
```

**Retry case (normal during export):**
```
setStateInformation: Not on message thread, deferring to timer
timerCallback: Attempt 1 to load plugin: PluginName
loadHostedPluginFromState: Plugin formats not ready yet, will retry
timerCallback: Attempt 2 to load plugin: PluginName
loadHostedPluginFromState: Successfully loaded plugin: PluginName
```

**Failure case:**
```
timerCallback: gave up after 10 attempts: PluginName
```
If you see this, the plugin may not be installed or the format manager isn't registering properly.

---

## Technical Details for Developers

### State Flow During Export

```
DAW calls processBlock (offline rendering)
  ↓
DAW may create new plugin instance for export
  ↓
DAW calls setStateInformation() with saved data
  ↓
If on message thread: load synchronously
If not on message thread: start Timer(50ms)
  ↓
timerCallback() attempts load (up to 10 retries)
  ↓
loadHostedPluginFromState():
  1. Check plugin formats ready
  2. Call hostManager.loadPlugin()
  3. Apply pendingHostedState_ if available
  4. Refresh discrete parameter map
  5. Unblock morph engine
```

### Critical Code Paths

**State Saving** (`getStateInformation`):
- Always saves `hostManager.getLastDescription()` (persists even after unload)
- Saves `pendingHostedState_` if plugin state not yet applied
- Preserves MCP identity for port reuse

**State Loading** (`setStateInformation`):
- Sets `isRestoring_ = true` to block morph processing
- Buffers hosted plugin state in `pendingHostedState_`
- Uses Timer-based loading when not on message thread
- Retries up to `MAX_PLUGIN_LOAD_RETRIES` (10) times

### Thread Safety

- `isRestoring_`: atomic bool, audio thread reads, message thread writes
- `pendingHostedState_`: protected by `pendingStateMutex_`
- `pendingPluginDesc_`: only accessed from message thread
- Timer callback: always runs on message thread (JUCE guarantee)

---

## Known Limitations

1. **Plugin Must Be Installed**: If the hosted plugin is uninstalled, MorphSnap cannot restore it (will retry 10 times then give up)

2. **Format Registration Delay**: Some DAWs delay VST3/AU format registration - the 500ms retry window handles most cases, but very slow systems may need more time

3. **Offline Rendering Context**: During export, some DAWs create plugin instances in a restricted environment where certain plugins may not initialize properly

4. **Copy Protection**: Some copy-protected plugins may refuse to load during offline rendering or in multiple instances

---

## Support

If you continue to experience issues after applying this update:

1. Enable debug logging (see above)
2. Reproduce the issue
3. Collect the debug log
4. File an issue with:
   - DAW name and version
   - MorphSnap version
   - Hosted plugin name and format (VST3/AU)
   - Debug log output
   - Steps to reproduce

---

## Changelog

### Version 3.3.0 (Current)
- Fixed plugin disconnection during export
- Added robust timer-based state restoration
- Extended retry window (5 → 10 attempts)
- Added comprehensive debug logging
- Fixed state persistence when editor is closed

### Previous Versions
- Plugin disconnection issues during export/track freeze
- Relied on unreliable `callAsync()` mechanism
- Limited retry logic (5 attempts)
