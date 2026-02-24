# MorphSnap User Guide

A complete guide to using MorphSnap, the AI-ready parameter morphing engine for audio plugins.

---

## Table of Contents

1. [Introduction](#introduction)
2. [Getting Started](#getting-started)
3. [The Interface](#the-interface)
4. [Working with Snapshots](#working-with-snapshots)
5. [Morphing](#morphing)
6. [Physics Modes](#physics-modes)
7. [Genetic Breeding](#genetic-breeding)
8. [MIDI Control](#midi-control)
9. [AI Integration](#ai-integration)
10. [Tips & Tricks](#tips--tricks)

---

## Introduction

MorphSnap is a unique audio plugin that lets you:
- **Host other plugins** inside it (synths, effects, instruments)
- **Capture snapshots** of parameter states
- **Morph between sounds** in real-time using a 2D pad or 1D fader
- **Apply physics** to create organic, evolving movements
- **Breed new sounds** using genetic algorithms
- **Connect AI assistants** via MCP for automated control

### Use Cases

- Sound design and preset exploration
- Live performance with expressive control
- Ambient/drone texture generation
- AI-assisted music production
- Creating smooth transitions between presets

---

## Getting Started

### Step 1: Add MorphSnap to Your DAW

1. Insert MorphSnap on an audio track or instrument track
2. The plugin window opens showing the main interface

### Step 2: Load a Plugin to Host

1. Find the **Plugin Browser** panel (right side)
2. Click **"Load Plugin"**
3. Select a VST3 or AU plugin from your installed plugins
4. The hosted plugin's interface opens automatically

### Step 3: Create Your First Snapshot

1. Tweak the hosted plugin's parameters to create a sound you like
2. **Double-click** anywhere on the circular **MorphPad**
3. A red dot appears indicating a captured snapshot
4. The slot number shows below the dot

### Step 4: Create More Snapshots

1. Modify the hosted plugin's parameters for a different sound
2. Double-click again to capture another snapshot
3. Repeat to fill up to 12 snapshot slots

### Step 5: Morph Between Sounds

1. Click and drag on the **MorphPad**
2. Watch parameters smoothly interpolate between snapshots
3. The cursor shows your current morph position
4. Closer snapshots have more influence

---

## The Interface

### MorphPad (Center)

The circular 2D control surface where morphing happens.

- **Clock Layout:** 12 snapshot positions arranged like a clock face
- **Red Dots:** Occupied snapshot slots
- **Purple Dots:** Empty snapshot slots
- **Cursor:** Your current morph position
- **Trail:** Shows recent cursor movement

### Mode Bar (Left)

Selects the physics behavior:

| Button | Mode | Description |
|--------|------|-------------|
| Direct | Instant | Cursor follows input exactly |
| Elastic | Spring | Cursor bounces to target with physics |
| Drift | Perlin | Cursor wanders organically |

### Snap Fader (Left, Vertical)

Alternative linear control through snapshots:

- Drag up/down to morph through snapshots in order
- Markers show occupied snapshot positions
- Automatically switches to fader mode when used

### Macro Knobs (Bottom)

8 assignable knobs for quick access to hosted plugin parameters:

- Right-click to assign a parameter
- Drag to adjust the assigned parameter
- Great for performance control

### Plugin Browser (Right)

Manage hosted plugins:

- **Load:** Open plugin selector dialog
- **Unload:** Remove current hosted plugin
- **Rescan:** Refresh plugin list

### AI Status Panel (Right, Bottom)

Monitor MCP server status:

- **MCP Status:** Running/Stopped
- **Port:** Server port (default 30001)
- **Clients:** Number of connected AI clients
- **Copy Token:** Copy authentication token

### Breeding Panel (Bottom, Right)

Genetic sound design tools:

- **Breed:** Combine two snapshots into a new one
- **Mutate:** Randomly vary a snapshot
- **Randomize:** Jump to random morph position
- **Smart Randomize:** DAW-automatable trigger for randomization

### Safety & Link Controls (Bottom)

Advanced control toggles:

- **Sanity Mode:** Protects volume/bypass parameters during morphing
- **Listen Mode:** Excludes discrete toggles/dropdowns from morphing to prevent clicks
- **Link Mode (Amber):** Synchronizes morph position across multiple plugin instances

---

## Working with Snapshots

### Capturing Snapshots

**Method 1: Double-click**
1. Position your sound in the hosted plugin
2. Double-click near an empty slot on the MorphPad
3. Snapshot captures to nearest slot

**Method 2: AI Command**
```
capture_snapshot(slot: 0)
```

### Recalling Snapshots

**Method 1: Click on Dot**
1. Click directly on a red snapshot dot
2. Parameters jump to that snapshot's values

**Method 2: MIDI Note**
- Notes C3-B3 trigger snapshots 1-12
- Works even during playback

**Method 3: AI Command**
```
recall_snapshot(slot: 0)
```

### Overwriting Snapshots

1. Create a new sound
2. Double-click on an existing snapshot dot
3. Old snapshot is replaced

### Clearing All Snapshots

Currently requires manually overwriting each slot or reloading the plugin.

### Meta Preset Manager

MorphSnap provides its own internal preset system saving snapshot banks:

- **Banks:** 16 banks available (switchable via UI or MIDI CC#0)
- **Presets:** 128 presets per bank
- Total configuration saves as JSON containing all APVTS values and 12 snapshot slots.

---

## Morphing

### XY Pad Morphing

The MorphPad uses **inverse-distance weighted interpolation**:

1. Each occupied snapshot has a position on the pad
2. Your cursor position determines influence weights
3. Closer snapshots have stronger influence
4. All snapshots contribute based on distance

**Example:** If you have snapshots at 12, 3, and 6 o'clock:
- Cursor at center = equal blend of all three
- Cursor near 12 o'clock = mostly that snapshot

### Fader Morphing

The Snap Fader provides **linear interpolation**:

1. Snapshots are ordered by capture sequence
2. Fader position determines position in sequence
3. Smooth crossfade between adjacent snapshots

**Best for:** Creating smooth progressions through sounds

### Switching Between Modes

- Using the MorphPad automatically sets **XY mode**
- Using the Snap Fader automatically sets **Fader mode**
- The active mode shows in the status display

---

## Physics Modes

### Direct Mode

**Behavior:** Cursor follows your input exactly.

**Best for:**
- Precise control
- Recording automation
- Live performance

**Tips:**
- Use with automation recording
- Good for dramatic instant changes

### Elastic Mode

**Behavior:** Cursor acts like a spring, bouncing to your target.

**Controls:**
- Spring tension (how fast it moves)
- Damping (how quickly it settles)

**Best for:**
- Smooth transitions
- Natural movement
- Reducing jittery automation

**Tips:**
- Great for ambient and pads
- Let go and watch it settle
- Creates interesting bounce effects

### Drift Mode

**Behavior:** Cursor wanders around your target position using Perlin noise.

**Controls:**
- **Speed:** How fast it moves
- **Distance:** How far from target it wanders
- **Chaos:** Randomness of movement

**Best for:**
- Evolving textures
- Ambient soundscapes
- Generative music

**Tips:**
- Set target to center and let it wander
- Low chaos = smooth, organic movement
- High chaos = unpredictable variation

---

## Genetic Breeding

### Breed (Crossover)

Combines two snapshots to create a new one:

1. Click **"Breed"**
2. Two random occupied snapshots are selected
3. Their parameters are blended with random weights
4. New snapshot is created in an empty slot

**Result:** A hybrid sound combining characteristics of both parents

### Mutate

Randomly varies a snapshot's parameters:

1. Click **"Mutate"**
2. A random snapshot is selected
3. Each parameter gets a small random change
4. New snapshot is created in an empty slot

**Result:** A variation of the original with subtle differences

### Randomize

Jumps to a random morph position:

1. Click **"Randomize"**
2. Morph cursor jumps to random X/Y position
3. Creates unexpected sound combinations

**Best for:** Happy accidents and exploration

---

## MIDI Control

### Snapshot Triggers

| MIDI Note | Snapshot |
|-----------|----------|
| C3 (60) | Slot 1 (12 o'clock) |
| C#3 (61) | Slot 2 (1 o'clock) |
| D3 (62) | Slot 3 (2 o'clock) |
| ... | ... |
| B3 (71) | Slot 12 (11 o'clock) |

**Behavior:**
- Only triggers if snapshot slot is occupied
- Instant recall (no morphing)

### Continuous Control

| MIDI CC | Function |
|---------|----------|
| CC 1 (Mod Wheel) | Snap Fader position |
| CC 0 | Bank Select (1-16) |

**Behavior:**
- Automatically switches to Fader mode
- Full range (0-127) maps to 0.0-1.0

### Recall Toggle (Sustain)

When triggering snapshots via MIDI, you can toggle **Full Recall vs Param-only Recall**:
- **On (Default):** Full VST state chunk recall.
- **Off (Sustain):** Fast param-only recall. This allows synth notes to sustain while switching snapshots.

### Setup

1. Enable MIDI input on the track hosting MorphSnap
2. Route MIDI from controller or another track
3. MIDI is processed automatically

---

## AI Integration

### What is MCP?

MCP (Model Context Protocol) is a standard for AI assistants to interact with applications. MorphSnap runs an MCP server that lets AI tools:

- Query plugin state
- Modify parameters
- Capture and recall snapshots
- Control morphing

### Connecting an AI Assistant

1. Check AI Status panel shows "MCP Running"
2. Note the port number (default 30001)
3. Configure your AI client to connect to localhost:30001
4. AI can now send commands to MorphSnap

### Example AI Workflows

**"Analyze my sound and suggest improvements"**
```
1. AI calls get_plugin_info()
2. AI calls list_parameters()
3. AI analyzes current parameter values
4. AI suggests parameter changes
5. AI can apply changes via set_parameter()
```

**"Create a smooth transition between these presets"**
```
1. AI captures current state (slot 0)
2. AI calls set_morph_position to move cursor
3. AI captures at different positions (slots 1-4)
4. AI records automation moving through positions
```

**"Evolve a new sound from my favorites"**
```
1. AI identifies best snapshots
2. AI uses breeding logic to combine them
3. AI applies mutations for variation
4. AI iterates until satisfied
```

---

## Tips & Tricks

### Sound Design

1. **Start with extremes:** Capture very different sounds for more dramatic morphing
2. **Use physics:** Drift mode creates endless variations without manual input
3. **Breed for surprise:** Genetic operations often create unexpected useful sounds

### Performance

1. **Map MIDI:** Use a controller for expressive live control
2. **Prepare snapshots:** Set up 12 sounds before the show
3. **Use Elastic mode:** Smoother transitions than direct mode

### Automation

1. **Record in Direct mode:** Clean automation curves
2. **Switch modes after:** Apply physics to recorded automation
3. **Use AI:** Let AI assistants create complex automation patterns

### Workflow

1. **Save often:** Plugin state saves with your project
2. **Name your snapshots:** Remember what each sound represents
3. **Document successful breeds:** Some combinations are worth remembering

### Troubleshooting

1. **No sound:** Check hosted plugin is loaded and receiving MIDI
2. **Parameters not changing:** Some plugins don't expose all parameters
3. **CPU spikes:** Reduce physics complexity or increase buffer size
4. **MCP won't connect:** Check firewall and port availability

---

## Keyboard Shortcuts

Currently, MorphSnap does not have keyboard shortcuts. All interaction is via mouse, MIDI, or MCP.

---

## Further Reading

- [API Reference](API_REFERENCE.md) - For developers and AI integration
- [Architecture](ARCHITECTURE.md) - Technical details
- [Developer Guide](DEVELOPER_GUIDE.md) - Building and contributing
