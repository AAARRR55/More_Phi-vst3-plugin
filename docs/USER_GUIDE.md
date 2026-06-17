# More-Phi User Guide

Welcome to More-Phi, an AI-ready parameter morphing engine for sound design, performance, and exploratory mixing. This guide teaches workflows: start with a hosted plugin, capture sounds, morph between them, and use advanced tools when you are ready.

> Screenshot placeholder: `[Screenshot: More-Phi loaded in a DAW with a hosted synth or effect]`

## Quick Navigation

- [First-Time Setup](#first-time-setup)
- [Set Up Your First Hosted Plugin](#set-up-your-first-hosted-plugin)
- [Create Your First Morphing Project](#create-your-first-morphing-project)
- [Manage Snapshots](#manage-snapshots)
- [Morph Between Sounds](#morph-between-sounds)
- [Use Physics Modes](#use-physics-modes)
- [Use Genetic Sound Design](#use-genetic-sound-design)
- [Control More-Phi with MIDI](#control-more-phi-with-midi)
- [Use AI and MCP Features](#use-ai-and-mcp-features)
- [Advanced Feature Walkthroughs](#advanced-feature-walkthroughs)
- [Best Practices](#best-practices)

## First-Time Setup

### Goal

Install More-Phi, load it in your DAW, and confirm it is ready to host another plugin.

### Before You Start

You need:

- A DAW that supports VST3 or AU plugins.
- At least one installed VST3/AU instrument or effect to host inside More-Phi.
- Audio routed through the track where More-Phi is inserted.

### Steps

1. Install or copy the More-Phi plugin bundle into your system plugin folder.
2. Open your DAW and rescan plugins.
3. Insert **More-Phi** on an audio, instrument, or bus track.
4. Confirm the More-Phi window opens and shows:
   - Title bar with output meter.
   - Plugin browser row.
   - MorphPad and Snap Fader.
   - Bottom tab section.
   - AI status bar.

> Screenshot placeholder: `[Screenshot: Empty More-Phi instance before loading a hosted plugin]`

### Success Check

You are ready when the plugin browser row says **No plugin loaded** and the interface responds to resizing and tab changes.

## Set Up Your First Hosted Plugin

### Goal

Load a synth, effect, or mastering plugin inside More-Phi.

### Steps

1. Click **Load** in the plugin browser row.
2. Wait for plugin scanning if this is your first launch.
3. Choose a plugin from the popup list.
4. Confirm the plugin name appears in the browser row.
5. Click **Show** or **Open Plugin** to bring up the hosted plugin's own editor.
6. Play audio or MIDI through the track to confirm sound passes through the hosted plugin.

> Screenshot placeholder: `[Screenshot: Plugin browser row with loaded hosted plugin name]`

### Tips

- If the hosted plugin changes latency, More-Phi reports latency back to the DAW.
- Loading a different hosted plugin clears old snapshots because the parameter layout has changed.
- Keep the hosted plugin editor open while designing your first snapshots.

## Create Your First Morphing Project

### Goal

Capture multiple sound states and morph between them from the central pad.

### Steps

1. Load a hosted plugin.
2. Open the hosted plugin editor.
3. Design a sound you like.
4. Click **Capture** in the plugin browser row, or double-click near a slot on the MorphPad.
5. Change the hosted plugin to a different sound.
6. Capture another snapshot.
7. Repeat until you have two or more occupied slots.
8. Drag the MorphPad cursor between the occupied slots.
9. Listen for smooth transitions as More-Phi interpolates parameters.

> Diagram placeholder: `[Diagram: Three occupied snapshot slots arranged around the MorphPad with interpolation weights from cursor position]`

### Success Check

You should hear the hosted plugin move between captured sounds as you drag the MorphPad.

## Manage Snapshots

Snapshots are the foundation of More-Phi. A snapshot stores normalized hosted-plugin parameters and, when enabled, the hosted plugin's state chunk.

### Capture a Snapshot

1. Set the hosted plugin to the sound you want.
2. Use one of these capture methods:
   - Click **Capture** to use the next empty slot.
   - Double-click a MorphPad slot area.
   - Use an MCP command such as `capture_snapshot`.
3. Watch the slot indicator change to show it is occupied.

### Recall a Snapshot

1. Click an occupied snapshot dot.
2. Or trigger snapshots with MIDI notes C3-B3.
3. Or call `recall_snapshot` from an MCP client.

### Overwrite a Snapshot

1. Create the new sound in the hosted plugin.
2. Capture into an occupied slot.
3. The previous stored values are replaced.

### Choose Fast or Full Recall

Use the **Fast** and **Full** buttons in the bottom control strip.

| Mode | Best for | Behavior |
|---|---|---|
| Fast | Performance, sustained notes, quick switching | Recalls normalized parameter values only. |
| Full | Exact preset/state restoration | Also restores the hosted plugin's opaque state chunk through the safe deferred path. |

### Sustain Notes During Recalls

Keep **Recall Toggle** enabled when you want MIDI-triggered recalls to avoid interrupting sustained notes.

## Morph Between Sounds

### Use the MorphPad

1. Capture at least two snapshots.
2. Drag inside the circular MorphPad.
3. Move closer to a snapshot to give it more influence.
4. Move toward the center to blend multiple occupied slots.

Best for:

- Expressive performance.
- Sound design exploration.
- Nonlinear transitions between several sounds.

### Use the Snap Fader

1. Capture multiple snapshots.
2. Drag the vertical Snap Fader.
3. Move up or down to travel through snapshots in sequence.

Best for:

- Planned progressions.
- Build-ups and breakdowns.
- DAW automation lanes.

### Use Output Gain and Bypass

The **Output Gain** knob controls More-Phi's final output level. The **Bypass** button bypasses the plugin processing path.

> Screenshot placeholder: `[Screenshot: Bottom control strip output section with gain knob and bypass button]`

## Use Physics Modes

Physics modes change how the morph cursor moves toward your input. Instead of static linear crossfades, More-Phi implements physically modeled inertial movement and fluid dynamics.

### Direct Mode

Use **Direct** when you need precise, immediate control.

1. Select **Direct** in the mode bar.
2. Drag the MorphPad or Snap Fader.
3. The cursor follows your motion immediately.

> [!NOTE]
> Direct mode includes a small amount of parameter smoothing (controlled by the **Smooth** slider) to prevent digital "zipper noise" during fast mouse drags or abrupt automation jumps.

### Elastic Mode

Use **Elastic** for smooth, organic motion with a spring-like inertia.

1. Select **Elastic** in the mode bar.
2. Drag the MorphPad cursor to a target position.
3. The cursor will glide toward your target, overshoot it slightly, and bounce to a stop.

*   **Tuning Presets:** You can select from three physical presets via DAW automation/MCP:
    *   **Slow:** Light stiffness and low damping ($\zeta = 0.35$), producing slow, highly elastic swings.
    *   **Medium:** Balanced stiffness and moderate damping ($\zeta = 0.60$), providing responsive but fluid movement.
    *   **Heavy:** High stiffness and heavy damping ($\zeta = 1.50$), making the movement overdamped (no overshoot or oscillation, settling directly into place).
*   **DSP Engine Details:** The motion is calculated on the audio thread using a **Symplectic Euler Integration** scheme that preserves phase-space energy. It employs **Fully Implicit Velocity Damping** ($\text{dampingFactor} = \frac{1}{1 + c \cdot dt}$) to guarantee numerical stability, ensuring the physics never blow up or inject artificial energy.
*   **Adaptive Sub-stepping:** To ensure sample-rate and block-size independence, the engine calculates a stability limit:
    $$\Delta t_{\text{stable}} = \frac{1}{2\sqrt{k}}$$
    If your DAW's buffer size or sample rate causes a time step $dt$ larger than this, the engine automatically sub-divides the step into $N$ stable sub-steps, ensuring the spring behaves identically at $44.1\text{ kHz}$ or $96\text{ kHz}$.

### Drift Mode

Use **Drift** for evolving, natural movement around a target point.

1. Select **Drift** in the mode bar.
2. Set a target position on the MorphPad.
3. Let More-Phi wander organically around that point.

*   **Tuning Variables:** Exposes Speed, Distance, Chaos, and Gravity parameters to the DAW/MCP.
    *   **Speed:** Controls how fast the Perlin noise cycles.
    *   **Distance:** Sets the maximum radius of the random walk.
    *   **Chaos:** Controls the number of noise octaves (up to 4) to add high-frequency detail.
    *   **Gravity:** When in *Locked* mode, pulls the drift position back toward your anchor.
*   **Drift Modes:** Exposes three modes:
    *   **Free:** Unanchored random walk.
    *   **Locked:** Anchored to your mouse/automation cursor, scaling the drift by $(1.0 - \text{gravity})$.
    *   **Orbit:** Rotates in a circular path at a defined speed, with Perlin noise acting as a secondary dynamic wobble.
*   **DSP Engine Details:** Driven by a standard 2D Perlin gradient noise generator with an 8-way hashing scheme to eliminate directional bias (anisotropy). It utilizes a quintic fade curve ($6t^5 - 15t^4 + 10t^3$) to guarantee continuous acceleration, ensuring there are no sudden "snaps" or velocity jumps in the morphing path.

---

## Physics Visualizers on the MorphPad

When using **Elastic** or **Drift** modes, the MorphPad provides active visual cues to show exactly how the physics engine behaves in relation to your input:

1.  **The Gold Puck:** Renders the *processed* physical position currently being applied to the hosted parameters.
2.  **The Faint Guide Dot:** Shows your *raw* input target (where your mouse is clicked or where DAW automation is pointing). You will see the Gold Puck stretch away from the Guide Dot and pull toward it.
3.  **Faded Line Trail:** Visualizes the recent trajectory of the Gold Puck (up to 64 points of history), showing the curvature, springiness, or noise-drift pattern of the movement.
4.  **Dashed Connectors:** Draws faint, pulsating dashed lines connecting the Gold Puck to each of the active, occupied snapshot dots on the outer ring, visually representing which snapshot profiles are exerting pull on the morph space.

## Use Genetic Sound Design

The breeding panel creates new material from existing snapshots.

### Breed Two Snapshots

1. Capture at least two snapshots.
2. Click **Breed**.
3. More-Phi chooses two occupied snapshots, blends their continuous values, applies discrete parameter snapping, and stores the result in the next empty slot.
4. Listen to the result and decide whether to keep designing from it.

### Mutate a Snapshot

1. Capture at least one snapshot.
2. Click **Mutate**.
3. More-Phi selects a snapshot, adds small randomized parameter variations, applies the result, and stores it in a slot.

### Randomize Morph Position

1. Click **Randomize**.
2. The MorphPad position jumps to a random X/Y coordinate.
3. Use this to discover unexpected blends between existing snapshots.

> Screenshot placeholder: `[Screenshot: Breeding panel with Breed, Mutate, Randomize, and status label]`

## Control More-Phi with MIDI

### Trigger Snapshots with Notes

1. Route MIDI to the track hosting More-Phi.
2. Play notes C3 through B3.
3. Each note recalls one of the 12 snapshot slots.

### Control Morphing with CC

1. Send Mod Wheel / CC 1 to the track.
2. More-Phi switches to fader-style morph control.
3. Automate CC 1 in the DAW for repeatable transitions.

### Use Sidechain Triggering

1. Route a sidechain signal to More-Phi's sidechain input bus.
2. Enable the **SC** sidechain toggle.
3. Adjust the threshold knob.
4. When the sidechain RMS crosses the threshold, More-Phi cycles snapshots.

Best for rhythmic preset cycling, drum-triggered texture changes, and sidechain-reactive sound design.

## Use AI and MCP Features

More-Phi includes a local MCP server for AI clients and external tools.

### Start or Stop MCP

1. Locate the AI status bar at the bottom.
2. Click **Start MCP** if the status says **MCP: OFF**.
3. Confirm the panel shows **MCP: ON** and a port number.
4. Click **Copy Token** to copy the bearer token for your MCP client.

### Connect an AI Client

1. Configure the client to connect to localhost at the displayed port.
2. Provide the copied bearer token during initialization.
3. Use tool calls for plugin info, snapshots, morphing, analysis, or semantic safe actions.

> MCP connections are instance-isolated and use constant-time token verification. Idle connections close automatically after 30 seconds.

### Understand Analysis and Mastering Output

More-Phi's AI/MCP layer can inspect audio measurements and propose mastering actions, but the outputs are intentionally labeled so you can tell measured data from heuristics.

- `analysis.get_summary` reports deterministic DSP meter snapshots. LUFS values are lightweight BS.1770-style rolling estimates, and true peak is a `4x_polyphase_fir_estimate`.
- `analysis.get_spectrum` uses a Hann-window FFT over a mono-sum signal. Anti-phase left/right content can cancel in this view, so use the warning fields when interpreting stereo material.
- `analysis.get_stereo_field` reports mid/side energy, banded mid-side correlation, and side-to-mid ratios. It is not a perceptual stereo-width model.
- `analysis.capture_window` reports rolling min/max/mean/p10/p50/p90 statistics only when meter history exists.
- Mastering previews are deterministic heuristic rule-engine suggestions. Review `measured_inputs`, `rules_applied`, and the `recommendation_type` fields before applying changes.
- Genre classifier and neural compressor inference are unavailable unless real model backends are loaded; fallback statuses mean default or heuristic behavior is active.
- Dataset validation's `weighted_heuristic_score` summarizes component checks for convenience and should not be treated as a scientific validity score.

### Configure LLM Settings

1. Open the AI tab or settings control that launches **LLM Settings**.
2. Choose a provider.
3. Enter an API key.
4. Confirm the base URL.
5. Click **Fetch Models**.
6. Select a model.
7. Click **Test Connection**.
8. Click **Save**.

> Screenshot placeholder: `[Screenshot: LLM Settings dialog with Provider, API Key, Base URL, Model, Fetch Models, Test Connection, Save, and Cancel]`

### Use Semantic Safe Actions

If your AI client supports More-Phi's semantic profile tools, prefer them over raw parameter writes.

Recommended workflow:

1. Call `plugin_profile.describe_semantics`.
2. Choose a semantic control such as `eq.band_1.gain`.
3. Call `plugin_profile.apply_safe_action` with `dry_run: true`.
4. Review the planned action.
5. Call it again with `dry_run: false`.
6. Save the returned `snapshot_id`.
7. If the result is not better, call `plugin_profile.restore_safe_snapshot`.

## Advanced Feature Walkthroughs

### Walkthrough: Build an Evolving Pad Texture

1. Load a pad synth inside More-Phi.
2. Capture a warm, closed-filter snapshot at slot 1.
3. Capture a brighter, wider snapshot at slot 4.
4. Capture a noisy or modulated variation at slot 8.
5. Select **Drift** mode.
6. Place the cursor between the three snapshots.
7. Enable **Listen Mode** to avoid abrupt discrete parameter jumps.
8. Record the output or DAW automation while the cursor drifts.

### Walkthrough: Create a Performance Macro Scene

1. Load an effect plugin such as a delay, reverb, or multi-effect.
2. Capture dry/subtle, medium, and extreme effect snapshots.
3. Use the Snap Fader to move through the progression.
4. Assign useful hosted parameters to the macro knobs.
5. Enable **Sanity Mode** to protect critical parameters.
6. Record fader movement as automation.

### Walkthrough: Mastering Preview with AI Assistance

1. Load a mastering-oriented hosted plugin.
2. Start MCP and connect an AI client.
3. Use analysis tools to inspect spectrum, stereo field, and levels.
4. Preview a mastering plan before applying it.
5. Apply small semantic safe actions first.
6. Use rollback snapshots if the result becomes worse.

## Best Practices

### Sound Design

- Capture snapshots that are meaningfully different.
- Avoid capturing only tiny variations unless you want subtle motion.
- Use Listen Mode when morphing plugins with many switches or dropdowns.
- Use Sanity Mode before breeding or randomizing level-sensitive effects.

### Performance

- Use Fast recall for live playing.
- Use Full recall when exact hosted-plugin preset restoration matters more than speed.
- Keep output gain conservative when breeding or mutating snapshots.
- Test MIDI note ranges before a live set.

### AI Automation

- Prefer semantic profile tools over raw `set_parameter` calls.
- Use `dry_run` for first-time AI actions.
- Keep rollback snapshot IDs until you decide the result is safe.
- Avoid automating bypass, mute, preset, license, or quality-mode controls.

### Troubleshooting While Learning

If nothing changes when you morph:

1. Confirm a hosted plugin is loaded.
2. Capture at least two snapshots.
3. Confirm the hosted plugin has continuous or discrete parameters. Discrete parameters (toggles, dropdowns) now snap correctly during morphing when Listen Mode is enabled.
4. Disable Bypass.
5. Check that audio or MIDI is reaching the track.


_Updated 2026-06-18._
