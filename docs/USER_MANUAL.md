# More-Phi User Manual

This manual is the definitive feature reference for More-Phi. Use it when you need to identify a control, understand a setting, troubleshoot behavior, or look up terminology.

> Screenshot placeholder: `[Screenshot: Fully loaded More-Phi UI with numbered callouts for every major region]`

## Quick Navigation

- [Product Summary](#product-summary)
- [Interface Map](#interface-map)
- [Global Controls](#global-controls)
- [Plugin Browser Row](#plugin-browser-row)
- [MorphPad](#morphpad)
- [Snapshot Ring](#snapshot-ring)
- [Snap Fader](#snap-fader)
- [Classic Tab](#classic-tab)
- [Engine Tab](#engine-tab)
- [Modulation Tab](#modulation-tab)
- [Presets Tab](#presets-tab)
- [AI Tab and LLM Settings](#ai-tab-and-llm-settings)
- [AI Status Bar](#ai-status-bar)
- [Agent Orchestration](#agent-orchestration)
- [MIDI Functions](#midi-functions)
- [MCP Feature Reference](#mcp-feature-reference)
- [Configuration and Automation Parameters](#configuration-and-automation-parameters)
- [Troubleshooting](#troubleshooting)
- [Glossary](#glossary)

## Product Summary

More-Phi is a plugin host and morphing environment. It runs inside a DAW, loads a hosted plugin, captures snapshots of the hosted plugin's parameters, and moves between those snapshots using XY, fader, physics, MIDI, AI, and automation controls.

Supported roles:

- Sound design and preset exploration.
- Live performance morphing.
- AI-assisted hosted-plugin automation.
- Snapshot-based transitions.
- Mastering and analysis experiments.
- Dataset and offline rendering workflows for developers.

## Interface Map

```text
Title Bar
  |-- More-Phi logo and version
  `-- Output meter

Plugin Browser Row
  |-- Load
  |-- Show
  |-- Capture
  |-- Hosted plugin name/status
  |-- Open Plugin
  `-- Params toggle

Main Morph Area
  |-- Snap Fader
  |-- MorphPad
  `-- Snapshot Ring

Bottom Tabs
  |-- Classic
  |-- Engine
  |-- Modulation
  |-- Presets
  |-- AI
  `-- Agent Orchestration

AI Status Bar
  |-- MCP status
  |-- Port
  |-- External clients
  |-- Agent states
  |-- Scheduler stats
  |-- Start/Stop MCP
  `-- Copy Token
```

> Diagram placeholder: `[Diagram: UI layout regions with callout numbers]`

## Global Controls

### Title Bar

| Control | Description |
|---|---|
| More-Phi logo | Identifies the active plugin window. |
| Version label | Displays the plugin version, currently `v3.3.0`. |
| OUT meter | Shows approximate output level. Green indicates safe level, coral indicates elevated level, red indicates near-clipping. |

### Resizing

The editor is resizable. The minimum size is designed to preserve access to the MorphPad, browser row, tabs, and AI status bar.

## Plugin Browser Row

The plugin browser row manages the hosted plugin.

| Control | Type | Description |
|---|---|---|
| Load | Button | Scans plugin folders on first use, then opens a popup menu of discovered plugins. |
| Show | Button | Opens the hosted plugin's own editor window. Disabled until a plugin is loaded. |
| Capture | Button | Captures the current hosted-plugin state into the next empty snapshot slot. Disabled until a plugin is loaded. |
| Plugin name/status | Label | Displays `No plugin loaded`, `Scanning plugins...`, the hosted plugin name, capture status, or load failure text. |
| Open Plugin | Button | Opens or brings forward the hosted plugin editor window. |
| Params | Toggle button | Shows or hides the parameter map side panel. |

### Plugin Loading Behavior

- Loading is performed through `PluginHostManager`.
- A successful load refreshes hosted mastering applicators and latency reporting.
- Loading a new plugin clears the previous snapshot bank because parameter layouts differ.
- If scanning finds no plugins, the status label shows **No plugins found**.

## MorphPad

The MorphPad is the central XY control surface.

| Element | Description |
|---|---|
| Gold Puck | Current active/processed XY morph position applied to hosted parameters. |
| Faint Guide Dot | Raw target position (mouse cursor or DAW automation coordinate). |
| Occupied snapshot dots | Slots containing captured parameter states (cyan glow with number). |
| Empty snapshot dots | Available slots (muted outline). |
| Movement trail | Faded trail showing recent physical/drift trajectory (up to 64 points). |
| Dashed Connectors | Pulsating links showing active blend pull to occupied snapshots. |
| Double-click capture | Captures the current hosted-plugin state near the selected slot. |
| Drag | Moves the morph cursor and updates interpolation weights. |

### Behavior

*   **Morph Interpolation:** XY morphing uses inverse-distance weighting (IDW) across occupied snapshot slots. Closer snapshots exert a stronger influence on the parameters.
*   **Dual Cursor Feedback:** In **Elastic** or **Drift** modes, the pad separates input from output. The *Faint Guide Dot* tracks your mouse/automation input immediately, while the *Gold Puck* trails and reacts organically according to the selected physics equations.
*   **Visual Movement Trail:** Trailing lines visualize spring-damper swings or Perlin drift trajectories in real time at 30 FPS.
*   **Central Blend:** Dragging to the exact center of the pad balances the parameters of all active snapshots equally.
*   **Source Binding:** Using the MorphPad automatically updates the `morphSource` parameter to `XY Pad` mode.

## Snapshot Ring

The Snapshot Ring overlays the MorphPad with a 12-slot clock layout.

| Slot count | Description |
|---:|---|
| 12 | Maximum number of snapshot positions. |
| Slots 1-12 | Mapped visually like a clock face. |
| MIDI C3-B3 | Triggers snapshot slots from MIDI notes. |

### Snapshot Slot States

| State | Meaning |
|---|---|
| Empty | No captured parameter values. |
| Occupied | Contains captured values and possibly a hosted state chunk. |
| Active/selected | Recently recalled or targeted by the morph cursor. |

## Snap Fader

The Snap Fader is the vertical 1D morph control.

| Control | Description |
|---|---|
| Fader handle | Moves through captured snapshots linearly. |
| Occupied markers | Show which snapshots are available. |
| Drag gesture | Changes fader position and switches morph source to fader mode. |
| MIDI CC 1 | Controls fader position by default. |

Best use cases:

- Linear transitions.
- Automated builds.
- Predictable progressions through a set of snapshots.

## Classic Tab

The Classic tab contains the main performance controls.

### Bottom Control Strip

The strip is divided into Safety, Recall, Output, and Sidechain sections.

#### Safety Section

| Control | Type | Description |
|---|---|---|
| Sanity | Toggle | Protects critical parameters during randomization and genetic operations. |
| Listen | Toggle | Excludes discrete toggles/dropdowns from morphing to reduce clicks and abrupt jumps. |
| Link | Toggle | Synchronizes morph position across multiple More-Phi instances in a session. |

#### Recall Section

| Control | Type | Description |
|---|---|---|
| Fast | Button | Selects parameters-only snapshot recall. Best for live use and sustained notes. |
| Full | Button | Selects recall with hosted plugin state chunks where available. Best for exact restoration. |
| Recall Toggle | Toggle | Keeps MIDI-triggered recalls in the sustain-friendly path when enabled. |

#### Output Section

| Control | Type | Description |
|---|---|---|
| Output Gain | Rotary knob | Adjusts final More-Phi output level in dB. |
| Bypass | Toggle button | Bypasses More-Phi processing. |

#### Sidechain Section

| Control | Type | Description |
|---|---|---|
| SC | Toggle | Enables sidechain-triggered snapshot cycling. |
| Threshold | Rotary knob | Sets sidechain RMS trigger threshold in dB. |

> Screenshot placeholder: `[Screenshot: Classic tab bottom control strip with Safety, Recall, Output, and SC labels]`

### Mode Bar

Controls the active morph source and physical dynamics engine.

| Button/Slider | Parameter / Mode | Description |
|---|---|---|
| **2D Pad / Fader** | `morphSource` | Selects between 2D XY morphing and 1D sequential clock-fader morphing. |
| **Direct** | `physicsMode = 0` | Cursor follows input target immediately (softened by the Smooth slider). |
| **Elastic** | `physicsMode = 1` | Mass-spring-damper physical integration on the audio thread with inertial overshoot. |
| **Drift** | `physicsMode = 2` | 2D Perlin noise walk around the target (with Free, Locked, or Orbit trajectories). |
| **Smooth** | `smoothing` | Dynamic parameter smoothing rate (from `0.0` to `0.999`) to prevent zipper noise. |

#### Physics DSP Integrator Details
*   **Symplectic Euler Integration:** Elastic mode calculates position and velocity using a phase-space energy-conserving symplectic scheme on the audio thread, preventing numerical accumulation errors.
*   **Implicit Velocity Damping:** Damping is solved implicitly ($v_{\text{new}} = \frac{v_{\text{intermediate}}}{1 + c \cdot dt}$) to guarantee the physical model is unconditionally stable and never diverges.
*   **Adaptive Sub-stepping:** Time steps are divided dynamically to maintain stiffness/damping tuning across all sample rates ($44.1\text{ kHz}$ to $96\text{ kHz}$) and buffer sizes.
*   **Anisotropic Noise Walk:** Drift mode employs 8-direction Perlin gradients with a quintic fade curve ($6t^5 - 15t^4 + 10t^3$) to provide isotropic, continuous acceleration with zero sudden direction snapping.

### Macro Knob Strip

| Control | Description |
|---|---|
| Macro knobs 1-8 | Assignable quick controls for hosted plugin parameters. |
| Assignment interaction | Right-click or parameter-map workflow depending on current UI state. |
| Drag | Adjusts the assigned normalized parameter. |

### Breeding Panel

| Control | Type | Description |
|---|---|---|
| Breed | Button | Selects two occupied snapshots, blends their parameter values, applies the blend, and captures it to the next empty slot. |
| Mutate | Button | Selects an occupied snapshot, applies small random changes, applies the result, and captures it. |
| Randomize | Button | Moves the MorphPad cursor to a random XY position. |
| Status label | Label | Reports readiness, errors, selected slots, queue state, or random coordinates. |

## Engine Tab

The Engine tab groups advanced audio-domain processors and morph-engine controls.

Functional areas may include:

| Area | Purpose |
|---|---|
| Spectral controls | Configure FFT/spectral morph behavior. |
| Granular controls | Configure grain-based morph behavior. |
| Formant controls | Manage formant-preserving behavior where available. |
| Hybrid blend | Balance multiple audio-domain engines. |
| Mastering processors | Shape loudness, dynamics, stereo, EQ, and limiting features where exposed. |

> Screenshot placeholder: `[Screenshot: Engine tab with spectral, granular, formant, and hybrid blend controls]`

## Modulation Tab

The Modulation tab contains routing and modulation tools.

Functional modules:

| Module | Description |
|---|---|
| Modulation matrix | Routes modulation sources to destinations. |
| LFO routes | Periodic parameter motion. |
| Step sequencing | Pattern-based parameter motion. |
| Route enable/disable | Activates or bypasses individual modulation routes. |

> Diagram placeholder: `[Diagram: Modulation source routed through matrix to hosted-plugin parameter destination]`

## Presets Tab

The Presets tab manages More-Phi's internal preset system.

| Feature | Description |
|---|---|
| Banks | 16 internal banks. |
| Presets | 128 presets per bank. |
| Navigation | Move between presets or banks. |
| Serialization | Saves APVTS state, snapshot bank, and metadata. |

More-Phi presets are separate from hosted-plugin presets. A More-Phi preset can include snapshot banks and More-Phi settings around a hosted plugin state.

## AI Tab and LLM Settings

The AI tab hosts chat and assistant workflows where enabled. It also displays the **Agent Status** panel when the orchestrator is active, showing the current state of all six built-in agents, the scheduler queue depth, and the Conductor's latest plan.

### Agent Status Panel

When MCP is running, the AI tab includes an **Agent Status** panel with the following readouts:

| Element | Description |
|---|---|
| Orchestrator state | `running`, `paused`, or `stopped`. |
| Agent list | One row per agent (Conductor, Analysis, Optimization, Creative, RealtimeControl, QualitySafety) showing current state. |
| Agent state badges | `idle`, `planning`, `waiting_for_approval`, `executing`, or `error`. |
| Scheduler stats | Queue depth, tasks completed, and average task latency. |
| Plan viewer | Collapsible view of the Conductor's current plan with step ownership. |
| Approval buttons | **Approve**, **Modify**, and **Reject** buttons appear when the Creative or Optimization agents propose changes. |
| Cancel button | Interrupts the currently executing plan. |

### LLM Settings Dialog

| Control | Type | Description |
|---|---|---|
| Provider | Combo box | Selects the LLM provider. |
| API Key | Password text field | Stores the provider API key. |
| Base URL | Text field | Shows or edits the provider endpoint. Some providers lock this field. |
| Fetch Models | Button | Retrieves available models for the selected provider. Requires API key and base URL. |
| Model | Combo box/text | Selects the model to use. |
| Test Connection | Button | Validates provider, key, base URL, and selected model. |
| Save | Button | Saves settings and activates the selected provider when possible. |
| Cancel | Button | Closes without saving changes. |
| Status label | Label | Shows untested, testing, success, or failure messages. |

> Screenshot placeholder: `[Screenshot: LLM Settings dialog with all fields labeled]`

### Provider Validation States

| State | Meaning |
|---|---|
| Untested | Settings have not been validated or were edited after validation. |
| Testing | A fetch or connection test is in progress. |
| Active/success | Provider is available for AI features. |
| Failed | Validation failed; check key, URL, model, or network connectivity. |

## AI Status Bar

The AI status bar is always visible at the bottom of the editor.

| Control | Type | Description |
|---|---|---|
| MCP status | Label | Shows `MCP: ON` or `MCP: OFF`. |
| Port | Label | Shows the active local port when MCP is running. |
| External clients | Label | Shows connected external client count when greater than zero. |
| Start MCP / Stop MCP | Button | Starts or stops the embedded MCP server. |
| Copy Token | Button | Copies the current MCP bearer token to the clipboard. |

## Agent Orchestration

More-Phi v3.3.0 includes a multi-agent orchestration layer that supervises six built-in agents to carry out high-level sound-design goals. The orchestrator runs on top of the MCP server and is visible through the AI tab and AI status bar.

### The Six Built-In Agents

| Agent | Role | Behavior |
|---|---|---|
| **Conductor** | Coordinates all other agents, breaks down user goals, and approves or rejects proposed plans. | Always active when orchestration is enabled. |
| **Analysis** | Reads audio measurements, spectrum, stereo field, and parameter states. | Read-only; it never changes parameters directly. |
| **Optimization** | Drafts detailed parameter plans (EQ moves, gain changes, width adjustments) to satisfy a goal. | Proposes plans only; changes are applied only through Conductor approval. |
| **Creative** | Generates novel snapshot ideas, suggests unconventional parameter combinations, and explores variations. | Always requires user explicit approval before applying changes, regardless of autonomy settings. |
| **RealtimeControl** | Manages time-critical adjustments such as gain staging or limiting during playback. | Operates automatically on **RealtimeCritical** priority; does not wait for manual approval. |
| **QualitySafety** | Monitors every proposed change against safety policies and clamps or rejects dangerous values. | Runs automatically in the background. |

### Autonomy Levels

| Level | Agents | Effect |
|---|---|---|
| **Automatic** | RealtimeControl, QualitySafety | Act immediately without user intervention. |
| **Approval-required** | Creative | Always prompts the user before applying changes. |
| **Conductor-gated** | Optimization | Drafts plans, but the Conductor queues them for review or auto-approves based on trust settings. |

### AI Status Bar Additions

When the orchestrator is active, the AI status bar also shows:

| Control | Type | Description |
|---|---|---|
| Orchestrator state | Label | `Orchestrator: running`, `paused`, or `stopped`. |
| Active agent count | Label | Number of agents currently busy. |
| Queue depth | Label | Current scheduler task queue depth. |

### Using Agent Orchestration

1. Start MCP from the AI status bar.
2. Open the AI tab and locate the **Agent Status** panel.
3. Type a goal in plain language (e.g., "make this track louder and brighter") and submit it.
4. The Conductor agent breaks the goal into sub-tasks and delegates to the appropriate agents.
5. Monitor agent states in the status panel. Click any agent row to expand its reasoning log or proposed plan.
6. If an agent is waiting for approval, a notification badge appears on the AI tab. Click the badge to review and approve, modify, or reject the proposal.
7. Click **Cancel** in the AI tab to stop an in-progress plan at any time.

## Neural Master (Preview)

Above the AI status bar, **Neural Master (Preview)** drives the built-in mastering chain from a neural decision model (`masteringbrainv2`). When enabled, the plugin continuously analyses the most recent ~6 seconds of audio on a background thread and refreshes the EQ, compressor, limiter ceiling, loudness target, and stereo width roughly every 3 seconds, crossfading parameters in over 200 ms.

| Control | Type | Description |
|---|---|---|
| Neural Master (Preview) | Toggle | Enables the background analysis loop. **Off by default.** |
| Neural Master status | Label | `off` · `collecting audio...` · `applied #N` · `held (low confidence)` · `error - see log` · `unavailable (no model)`. |

**This is a preview feature and off by default.** The model is research-grade — it failed several of its own release-quality gates (EQ MAE ≈ 2.1 dB, true-peak ≈ 0.8 dBTP) — so treat it as an assistant, not an autocrat. Every prediction is clamped by the plugin's `NeuralMasteringSafetyPolicy`, so a bad frame can never push the chain into an unsafe state; rejected frames hold the last safe setting.

**To enable it**, start the local Python inference server that hosts the model (see `tools/inference_server/README.md`), then toggle "Neural Master (Preview)" on. The toggle stays disabled ("unavailable (no model)") until the server is reachable on `127.0.0.1:8765`. The plugin drives inference via that server because the checkpoint cannot yet be exported to a faithful in-process ONNX model; the server path runs the exact inference the model was validated with.

Limitations (by design):

- It cannot react faster than ~3–6 seconds. Sudden transients or level changes are not re-mastered until the next cycle. The DSP chain's own realtime limiter and loudness normalizer remain the first line of defence during the gap.
- It is not sample-accurate: it produces static parameter settings, refreshed every cycle with a 200 ms crossfade.
- See `docs/superpowers/specs/2026-06-21-sonicmaster-vst3-realtime-integration-design.md` for the full design and failure model.

## MIDI Functions

| Input | Function |
|---|---|
| C3-B3 notes | Trigger snapshot slots 1-12. |
| CC 1 / Mod Wheel | Controls Snap Fader morph position. |
| Sidechain input | Triggers snapshot cycling when sidechain mode is enabled and threshold is crossed. |

## MCP Feature Reference

The embedded MCP server exposes local JSON-RPC tools. Use the AI status bar to get the active port and token.

### Core Tools

| Tool | Description |
|---|---|
| `get_plugin_info` | More-Phi and hosted-plugin identity. |
| `list_parameters` | Hosted parameter list. |
| `get_parameter` | Read one hosted parameter. |
| `set_parameter` | Queue one parameter write. |
| `set_parameters_batch` | Queue multiple parameter writes. |
| `capture_snapshot` | Capture a snapshot slot. |
| `recall_snapshot` | Recall a snapshot slot. |
| `set_morph_position` | Set XY or fader morph position. |
| `get_morph_state` | Read current morph state. |

### Semantic Safety Tools

| Tool | Description |
|---|---|
| `plugin_profile.describe_semantics` | Lists semantic controls and safety categories. |
| `plugin_profile.apply_safe_action` | Applies guarded semantic parameter changes. |
| `plugin_profile.restore_safe_snapshot` | Restores a rollback snapshot from a safe action. |

Safety categories:

| Category | Meaning |
|---|---|
| Safe | Intended for bounded automation. |
| Caution | Requires explicit confirmation because changes may be audible or risky. |
| Locked | Blocked from semantic automation. |

### Analysis and Mastering Tools

| Tool | Description |
|---|---|
| `analysis.get_summary` | Compact deterministic DSP meter summary with methodology and model-status metadata. |
| `analysis.get_spectrum` | Realtime mono-sum spectrum snapshot with channel-mode warning. |
| `analysis.get_stereo_field` | Stereo and mid-side snapshot. |
| `analysis.capture_window` | Rolling meter-window statistics when analysis samples are available. |
| `mastering.plan_preview` | Generate a heuristic rule-based plan without applying. |
| `mastering.apply_plan` | Apply a heuristic rule-based mastering plan. |

### Multi-Agent System

The MCP server also exposes tools for the agent orchestration layer. These tools are available when the orchestrator is active and MCP is running.

| Tool | Description |
|---|---|
| `orchestrator.describe_system_state` | Returns a JSON snapshot of the orchestrator, MCP server, agent roster, and scheduler statistics. |
| `orchestrator.submit_user_goal` | Submits a high-level goal (e.g., "make this track louder and brighter") to the Conductor agent. |
| `orchestrator.cancel_plan` | Interrupts the currently executing plan and returns all agents to idle. |
| `orchestrator.get_agent_state` | Returns the detailed state of a single agent by name. |
| `orchestrator.approve_proposal` | Approves a pending plan or action from the Creative or Optimization agent. |
| `orchestrator.reject_proposal` | Rejects a pending plan and returns the agent to idle. |

**`orchestrator.describe_system_state` response fields:**

| Field | Type | Description |
|---|---|---|
| `orchestratorRunning` | Bool | Whether the orchestrator thread is active. |
| `mcpServerRunning` | Bool | Whether the MCP server is currently accepting connections. |
| `mcpHealthy` | Bool | Whether the last MCP health check passed. |
| `mcpPort` | Int | The local port the MCP server is listening on. |
| `agentCount` | Int | Number of registered agents (always 6 for the built-in set). |
| `agentStates` | Array | One object per agent with `name`, `state`, `lastTask`, and `pendingApproval`. |
| `schedulerStats` | Object | `queueDepth`, `tasksCompleted`, and `averageLatencyMs`. |

## Configuration and Automation Parameters

| Parameter | Type | Description |
|---|---|---|
| `morphX` | Float | XY morph X position. |
| `morphY` | Float | XY morph Y position. |
| `morphFader` | Float | 1D fader morph position. |
| `morphSource` | Choice/internal | Tracks active morph source. |
| `sanityEnabled` | Bool | Enables parameter safety protection for genetic/random operations. |
| `listenMode` | Bool | Prevents discrete parameter morphing. |
| `linkMode` | Bool | Enables cross-instance morph synchronization. |
| `recallMode` | Choice | Selects Fast or Full snapshot recall. |
| `recallToggle` | Bool | Sustain-friendly recall behavior for MIDI workflows. |
| `sidechainEnabled` | Bool | Enables sidechain-triggered snapshot cycling. |
| `sidechainThreshold` | Float dB | Sets sidechain trigger threshold. |
| `outputGain` | Float dB | Controls final output gain. |
| `bypass` | Bool | Bypasses processing. |
| `smartRandomize` | Bool/trigger | DAW-automatable randomization trigger. |
| `SonicMasterAnalysisEnabled` | Bool | Enables the neural mastering preview loop (off by default). |
| `driftOutputX` | Float | Automation output for drift X position. |
| `driftOutputY` | Float | Automation output for drift Y position. |

## Troubleshooting

### More-Phi does not appear in the DAW

1. Confirm the plugin bundle is in a scanned VST3/AU folder.
2. Rescan plugins in the DAW.
3. Check whether the DAW blocks failed plugins and reset the blocklist if needed.
4. On Windows, confirm the build matches the DAW architecture.
5. On macOS, confirm any quarantine/signing requirements are satisfied.

### Hosted plugin list is empty

1. Click **Load** and wait for scanning to finish.
2. Confirm the hosted plugins are installed in standard plugin folders.
3. Confirm the plugin format is supported by the current platform.
4. Restart the DAW if a plugin was installed while the DAW was open.

### Hosted plugin loads but makes no sound

1. Confirm the DAW track has audio or MIDI input.
2. Open the hosted plugin editor and confirm it produces sound outside More-Phi settings.
3. Check More-Phi **Bypass** and **Output Gain**.
4. Confirm the hosted plugin accepts the track's input type.

### Morphing does not change the sound

1. Capture at least two snapshots.
2. Confirm the hosted plugin has continuous parameters to morph.
3. Disable Listen Mode temporarily to test whether discrete filtering is hiding changes.
4. Check that snapshot slots are occupied.
5. Move the MorphPad closer to different snapshots.

### Snapshot recall interrupts notes

1. Switch to **Fast** recall.
2. Enable **Recall Toggle**.
3. Avoid Full recall during sustained live performance when the hosted plugin does not handle state restoration smoothly.

### Genetic tools sound too extreme

1. Enable **Sanity Mode**.
2. Reduce output gain before breeding or mutating.
3. Capture more moderate parent snapshots.
4. Use Listen Mode to avoid discrete parameter jumps.

### MCP client cannot connect

1. Confirm the AI status bar shows **MCP: ON**.
2. Use the displayed port, not a hardcoded value.
3. Click **Copy Token** and update the client configuration.
4. Connect to `127.0.0.1`, not a remote interface.
5. Restart MCP from the status bar if the DAW suspended the plugin.

### LLM provider validation fails

1. Confirm the API key is current.
2. Confirm the base URL is correct for the provider.
3. Click **Fetch Models** before testing connection.
4. Select a model that the account can access.
5. Check network/firewall settings.

### Safe action restore fails

| Error | Fix |
|---|---|
| `snapshot_not_found` | Use a snapshot ID returned by the current session's safe action call. |
| `snapshot_context_mismatch` | The hosted plugin or parameter layout changed; create a new safe action snapshot. |
| `pending_parameter_edits` | Wait for the audio queue to drain before applying another safe action. |

## Glossary

| Term | Definition |
|---|---|
| APVTS | JUCE AudioProcessorValueTreeState, used for automatable plugin parameters and state. |
| AU | Audio Unit plugin format on macOS. |
| Bypass | A control that disables More-Phi processing. |
| DAW | Digital Audio Workstation. |
| Drift | Physics mode that adds organic wandering around a target point. |
| Elastic | Physics mode using spring-damper motion. |
| Fast recall | Snapshot recall using normalized parameter values only. |
| Full recall | Snapshot recall that also restores hosted plugin state chunks. |
| Hosted plugin | The third-party plugin loaded inside More-Phi. |
| Listen Mode | Safety mode that skips discrete controls during morphing. |
| MCP | Model Context Protocol, used by AI clients to call local tools. |
| MorphPad | Central XY surface for blending between snapshot states. |
| ParameterBridge | Internal layer that reads/writes hosted plugin parameters. |
| Sanity Mode | Safety mode that protects critical parameters from random/genetic edits. |
| Semantic control | A raw hosted-plugin parameter classified as a musical or technical role. |
| Snapshot | Captured hosted-plugin parameter state in one of 12 slots. |
| Snap Fader | Vertical 1D morph control. |
| VST3 | Cross-platform audio plugin format. |
| AgentOrchestrator | The multi-agent supervision layer that coordinates the six built-in agents and manages the scheduler. |
| ConductorAgent | The central agent that breaks down user goals, delegates tasks, and approves or rejects proposed plans. |
| AnalysisAgent | Read-only agent that inspects audio measurements, spectrum, stereo field, and parameter states. |
| OptimizationAgent | Agent that drafts detailed parameter plans to satisfy a goal; applies changes only through Conductor approval. |
| CreativeAgent | Agent that generates novel snapshot ideas and unconventional parameter combinations; always requires user approval. |
| RealtimeControlAgent | Agent that manages time-critical adjustments such as gain staging or limiting during playback. |
| QualitySafetyAgent | Agent that monitors proposed changes against safety policies and clamps or rejects dangerous values. |
| BlackboardBridge | Internal shared-state bridge that lets agents read and post intermediate results without direct coupling. |
| EcosystemConfig | Runtime configuration object that controls orchestrator and MCP server settings. |
| McpProtocol | JSON-RPC schema definitions used by the MCP server for message construction and tool dispatch. |
