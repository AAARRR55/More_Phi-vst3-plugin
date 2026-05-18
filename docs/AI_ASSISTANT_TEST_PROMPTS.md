# AI Assistant Test Prompts

Manual test prompts for the More-Phi VST3 embedded AI assistant. Each prompt lists the expected tool calls and behavior. Use these in the plugin's AI chat panel with a hosted plugin loaded (unless testing the no-plugin error path).

## Prerequisites

- More-Phi loaded in a DAW with audio engine running
- A hosted plugin loaded (e.g. any EQ, compressor, or Ozone 11)
- LLM provider configured in AI settings (API key, endpoint)
- At least one snapshot slot occupied for morph-related tests

---

## Basic Discovery & Orientation

### 1. What plugin is currently loaded?
**Expected:** Calls `get_plugin_info`. Reports hosted plugin name, type, and parameter count — or says no plugin is loaded.

### 2. List all parameters
**Expected:** Calls `list_parameters`. Returns hosted plugin parameter list with indices, names, and normalized values.

### 3. Show me More-Phi's own controls
**Expected:** Calls `more_phi.parameters`. Lists APVTS runtime controls (morphX, morphY, faderPos, physics mode, etc.).

---

## Single Parameter Edits

### 4. Set parameter 0 to 0.75
**Expected:** Calls `set_parameter` with index=0, value=0.75. Reports success, appliedNow count, and the parameter name.

### 5. Set the Gain to maximum
**Expected:** Calls `set_parameter` with name="Gain", value=1.0. Resolves by name.

### 6. What's the current value of parameter 3?
**Expected:** Calls `get_parameter` with index=3. Reports name, normalized value, and display value.

---

## Batch Edits

### 7. Set Gain to 0.6, Cutoff to 0.3, and Resonance to 0.8
**Expected:** Calls `set_parameters_batch` with three entries in a single batch call, not three separate `set_parameter` calls.

### 8. Zero out all parameters
**Expected:** Calls `list_parameters` first to discover the count, then `set_parameters_batch` with value=0.0 for each parameter.

---

## Snapshot Workflow

### 9. Capture the current state into snapshot slot 0
**Expected:** Calls `capture_snapshot` with slot=0. Reports success.

### 10. Recall snapshot 2
**Expected:** Calls `recall_snapshot` with slot=2. Reports success or error if the slot is empty.

### 11. Save the current settings to slot 3, then change Gain to 0.9, then recall slot 3 to undo
**Expected:** Three-step sequence: `capture_snapshot` slot 3 → `set_parameter` Gain=0.9 → `recall_snapshot` slot 3. Verifies the undo workflow restores the original value.

---

## Morph Control

### 12. Move the morph pad to center
**Expected:** Calls `set_morph_position` with x=0.5, y=0.5.

### 13. Set the fader to 25%
**Expected:** Calls `set_morph_position` with fader=0.25.

### 14. Where's the morph pad right now?
**Expected:** Calls `get_morph_state`. Reports x, y, and fader values.

---

## Diagnostics & Pipeline

### 15. Something's wrong — my edits aren't sticking. Can you diagnose?
**Expected:** Calls `diagnose_parameter_pipeline`. Reports the status of all 7 pipeline stages. Identifies the blocker (morph overwrite, queue full, no plugin, not prepared, restoring, etc.).

### 16. Diagnose parameter 5
**Expected:** Calls `diagnose_parameter_pipeline` with index=5. Reports per-parameter state including current value, name, and live-edit hold status.

### 17. Run a self-test
**Expected:** Calls `run_self_test` with suite="quick". Reports pass/fail results.

---

## Analysis & Metering

### 18. What are the current LUFS levels?
**Expected:** Calls `analysis.get_summary` or `get_mastering_state`. Reports integrated LUFS, short-term LUFS, true peak, and the deterministic DSP methodology metadata.

### 19. Show me the spectrum
**Expected:** Calls `analysis.get_spectrum`. Returns spectral energy bins at the default or requested resolution and notes the `mono_sum` channel mode.

### 20. How's the stereo field looking?
**Expected:** Calls `analysis.get_stereo_field`. Reports mid-side balance, correlation, and per-band width.

---

## EQ Assistant

### 21. Boost the highs a little
**Expected:** Calls `eq_adjust` with a natural language description. Returns proposed EQ parameter changes.

### 22. Preview those EQ changes before applying
**Expected:** Calls `eq_preview` to stage changes without committing them to the hosted plugin.

### 23. Reject the EQ preview and undo
**Expected:** Calls `eq_reject` to discard staged changes and clear the preview state.

### 24. Suggest an EQ curve for a warm vocal sound
**Expected:** Calls `eq_suggest` with the description. Returns recommended EQ band settings.

---

## Semantic Plugin Profile

### 25. What controls are safe to automate on this plugin?
**Expected:** Calls `plugin_profile.describe_semantics`. Returns controls grouped by safety category (safe, caution, locked).

### 26. Safely increase the EQ band 1 gain by 2 dB
**Expected:** Calls `plugin_profile.apply_safe_action` with the appropriate action object and `allow_caution=false`. Returns a rollback snapshot ID.

### 27. Undo that last safe action
**Expected:** Calls `plugin_profile.restore_safe_snapshot` with the snapshot ID from prompt 26.

---

## More-Phi Internal Controls

### 28. Switch the physics mode to spring
**Expected:** Calls `more_phi.set_parameter` targeting the `physicsMode` APVTS parameter with the normalized value corresponding to "spring".

### 29. Set morphX to 0.8 and morphY to 0.2 through More-Phi's own controls
**Expected:** Calls `more_phi.set_parameters` with both APVTS entries in a single batch.

---

## Edge Cases & Error Handling

### 30. Set parameter 99999 to 0.5
**Expected:** Returns a resolution error (parameter index out of range). The assistant should report the error verbatim and stop — not retry or guess.

### 31. Set parameter 0 to 5.0
**Expected:** Value is clamped to 1.0 by the queue. Should succeed but the assistant should note the clamped value.

### 32. Set parameter "Gian" to 0.5
*(Intentional typo of "Gain")*
**Expected:** Returns `ambiguous_param_name` or resolution failure. The assistant should report the error, not guess the intended parameter.

### 33. Load the plugin at C:\plugins\SomePlugin.vst3
**Expected:** The assistant asks for user confirmation before calling `hosted_plugin.load` (this tool is in the "confirm before invoking" list per the system prompt).

---

## Multi-Step Workflows

### 34. Create an A/B comparison: save current as snapshot A in slot 0, make the mix brighter, then save as snapshot B in slot 1
**Expected:** `capture_snapshot` slot 0 → `set_parameters_batch` increasing high-frequency parameters → `capture_snapshot` slot 1. Reports both states.

### 35. Morph smoothly from snapshot 0 to snapshot 1 by sweeping the fader from 0 to 1 in 5 steps
**Expected:** Calls `set_morph_position` five times with fader values 0.0, 0.25, 0.5, 0.75, 1.0 in sequence.

### 36. Analyze the current mix, then apply a mastering plan for EDM
**Expected:** Calls `analysis.get_summary` to read current meters, then `apply_mastering_plan` or `mastering.plan_preview` with genre_index for EDM and the analysis metrics. Describes the result as a heuristic rule-based plan.

---

## Coverage Matrix

| Area | Prompts | Tools Exercised |
|------|---------|-----------------|
| Discovery | 1-3 | `get_plugin_info`, `list_parameters`, `more_phi.parameters` |
| Single edits | 4-6 | `set_parameter`, `get_parameter` |
| Batch edits | 7-8 | `set_parameters_batch`, `list_parameters` |
| Snapshots | 9-11 | `capture_snapshot`, `recall_snapshot` |
| Morph | 12-14 | `set_morph_position`, `get_morph_state` |
| Diagnostics | 15-17 | `diagnose_parameter_pipeline`, `run_self_test` |
| Analysis | 18-20 | `analysis.get_summary`, `analysis.get_spectrum`, `analysis.get_stereo_field` |
| EQ | 21-24 | `eq_adjust`, `eq_preview`, `eq_reject`, `eq_suggest` |
| Semantic profile | 25-27 | `plugin_profile.describe_semantics`, `plugin_profile.apply_safe_action`, `plugin_profile.restore_safe_snapshot` |
| More-Phi controls | 28-29 | `more_phi.set_parameter`, `more_phi.set_parameters` |
| Error handling | 30-33 | `set_parameter`, `hosted_plugin.load` |
| Multi-step | 34-36 | Multiple tools chained in sequence |
