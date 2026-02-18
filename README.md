# MorphSnap

**Advanced Parameter Morphing Engine** — A VST3/AU plugin that hosts other plugins and morphs between parameter snapshots with physics-based interpolation and AI integration.

---

## Features

- **Plugin Hosting** — Load any VST3/AU plugin inside MorphSnap
- **12-Slot Snapshot System** — Capture and recall parameter states in clock layout
- **2D Morph Pad** — XY pad with inverse-distance weighted interpolation
- **1D Snap Fader** — Linear interpolation across occupied snapshots
- **Physics Modes** — Direct, Elastic (spring-damper), Drift (Perlin noise)
- **Genetic Breeding** — Crossover + mutation to evolve new presets
- **MCP Server** — JSON-RPC 2.0 on localhost:30001 for AI integration
- **MIDI Mapping** — Note triggers for snapshots, CC routing for morphing
- **8 Macro Knobs** — Quick access to hosted plugin parameters

---

## Installation

### Windows

1. Download `MorphSnap.vst3` from the [Releases](https://github.com/your-repo/releases) page
2. Copy to your VST3 folder:
   - `C:\Program Files\Common Files\VST3\` (all users)
   - `C:\Users\<username>\Documents\VST3\` (current user)
3. Rescan plugins in your DAW

### macOS

1. Download `MorphSnap.component` (AU) or `MorphSnap.vst3` from Releases
2. Copy to:
   - `/Library/Audio/Plug-Ins/Components/` (AU, all users)
   - `/Library/Audio/Plug-Ins/VST3/` (VST3, all users)
   - Or `~/Library/Audio/Plug-Ins/...` for current user
3. Rescan plugins in your DAW

### Build from Source

**Requirements:**
- CMake 3.24+
- C++20 compiler (MSVC 2022+, Clang 14+, GCC 11+)
- Git

```bash
# Clone repository
git clone https://github.com/your-repo/morphsnap.git
cd morphsnap

# Configure and build
cmake -B build -S .
cmake --build build --config Release

# Output location
# Windows: build/MorphSnap_artefacts/Release/VST3/MorphSnap.vst3
# macOS: build/MorphSnap_artefacts/Release/VST3/MorphSnap.vst3
```

---

## Quick Start

### 1. Load a Plugin

1. Add MorphSnap to a track in your DAW
2. Click **"Load Plugin"** in the plugin browser panel
3. Select a VST3 or AU plugin to host
4. The hosted plugin's UI opens automatically

### 2. Capture Snapshots

1. Tweak the hosted plugin's parameters to a sound you like
2. **Double-click** on the MorphPad to capture a snapshot
3. The snapshot appears as a red dot on the clock layout
4. Repeat for up to 12 snapshots

### 3. Morph Between Sounds

- **XY Pad:** Drag the cursor anywhere on the circular pad
- **Snap Fader:** Drag the vertical fader to interpolate through snapshots linearly
- **Physics:** Enable Elastic or Drift mode for organic movement

### 4. Connect AI (Optional)

1. Check the AI Status panel shows "MCP Running" on port 30001
2. Connect from an MCP-compatible AI client
3. Send commands to automate morphing, capture snapshots, or modify parameters

---

## Interface Overview

```
┌────────────────────────────────────────────────────────────────┐
│  ┌──────────┐  ┌─────────────────────────┐  ┌──────────────┐  │
│  │  Mode    │  │                         │  │   Plugin     │  │
│  │  Bar     │  │       MORPH PAD         │  │   Browser    │  │
│  │          │  │    (2D XY Control)      │  │              │  │
│  │ Direct   │  │                         │  │  [Load]      │  │
│  │ Elastic  │  │    12 Snapshot Dots     │  │  [Unload]    │  │
│  │ Drift    │  │    in Clock Layout      │  │              │  │
│  └──────────┘  └─────────────────────────┘  └──────────────┘  │
│                                                                │
│  ┌──────────┐  ┌─────────────────────────┐  ┌──────────────┐  │
│  │  Snap    │  │     MACRO KNOBS         │  │  AI Status   │  │
│  │  Fader   │  │  ○ ○ ○ ○ ○ ○ ○ ○        │  │              │  │
│  │          │  │  1 2 3 4 5 6 7 8        │  │  MCP: ON     │  │
│  │  ▲       │  │                         │  │  Port: 30001 │  │
│  │  │       │  └─────────────────────────┘  └──────────────┘  │
│  │  ▼       │                                                  │
│  └──────────┘  ┌─────────────────────────────────────────────┐│
│                │  BREEDING PANEL                              ││
│                │  [Breed] [Mutate] [Randomize]                ││
│                └─────────────────────────────────────────────┘│
└────────────────────────────────────────────────────────────────┘
```

---

## Physics Modes

| Mode | Description | Use Case |
|------|-------------|----------|
| **Direct** | Instant response, cursor follows input exactly | Precise control |
| **Elastic** | Spring-damper physics, cursor bounces to target | Smooth transitions |
| **Drift** | Perlin noise-based wandering around target | Evolving textures |

### Elastic Mode
- Spring constant controls how quickly cursor reaches target
- Damping prevents oscillation
- Great for smooth morphing without manual control

### Drift Mode
- Speed: How fast the cursor moves
- Distance: How far from target it wanders
- Chaos: Randomness of movement
- Perfect for ambient textures and evolving soundscapes

---

## MCP Integration

MorphSnap includes a built-in MCP (Model Context Protocol) server for AI integration.

### Connection Details
- **Protocol:** JSON-RPC 2.0
- **Port:** 30001 (configurable)
- **Host:** localhost

### Available Tools

| Tool | Parameters | Description |
|------|------------|-------------|
| `get_plugin_info` | - | Returns loaded plugin name, type, parameter count |
| `list_parameters` | - | Lists all parameters with current values |
| `get_parameter` | `index` | Get a single parameter by index |
| `set_parameter` | `index`, `value` | Set a parameter (audio-thread safe) |
| `set_parameters_batch` | `parameters[]` | Set multiple parameters atomically |
| `capture_snapshot` | `slot` | Capture current state to a slot (0-11) |
| `recall_snapshot` | `slot` | Recall a snapshot slot |
| `set_morph_position` | `x`, `y`, `source` | Set XY pad or fader position |
| `get_morph_state` | - | Get current morph position and mode |

### Example JSON-RPC Request

```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "set_morph_position",
    "arguments": { "x": 0.5, "y": 0.5, "source": "xy" }
  },
  "id": 1
}
```

---

## MIDI Mapping

### Snapshot Triggers
- **Notes C3-B3** (MIDI 60-71) trigger snapshots 1-12
- Only triggers if snapshot slot is occupied

### Morph Control
- **Mod Wheel (CC 1)** controls fader position
- Automatically switches to fader mode when CC received

### Configuration
MIDI mapping is automatic. Just enable MIDI input on the track hosting MorphSnap.

---

## Troubleshooting

### Plugin Won't Load
- Verify plugin is in correct folder (see Installation)
- Rescan plugins in DAW
- Check DAW is 64-bit (32-bit not supported)

### No Sound
- Ensure a hosted plugin is loaded
- Check hosted plugin is receiving MIDI (if instrument)
- Verify audio routing in DAW

### Plugin Crashes on Load (FL Studio)
- FL Studio requires 4MB stack for plugin-in-plugin hosting
- MorphSnap is configured with `/STACK:4194304` in CMake
- If crashes persist, try increasing FL Studio's plugin stack size

### MCP Server Won't Start
- Check if port 30001 is already in use
- Look for firewall blocking
- Check logs in DAW console

### Hosted Plugin Parameters Not Capturing
- Some plugins use internal state that isn't exposed as parameters
- Only automatable parameters are captured
- Try using the hosted plugin's preset system as a workaround

### Poor Performance
- Reduce number of hosted plugin parameters
- Switch to Direct physics mode (least CPU)
- Increase audio buffer size in DAW

---

## Building

### Debug Build
```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
```

### Release Build (Optimized)
```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### Build with SIMD Diagnostics
```bash
cmake -B build -S . -DMORPHSNAP_TRACK_ALLOCATIONS=ON
cmake --build build --config Debug
```

---

## Architecture

```
src/
├── Plugin/     PluginProcessor, PluginEditor
├── Core/       InterpolationEngine, PhysicsEngine, GeneticEngine, MorphProcessor
├── Host/       PluginHostManager, ParameterBridge, PluginScanner
├── AI/         MCPServer (JSON-RPC 2.0), MCPToolHandler (9 tools)
├── MIDI/       MIDIRouter (note triggers, CC routing)
├── Preset/     MetaPresetManager, PresetSerializer
└── UI/         MorphPad, SnapFader, SnapshotRing, HostedPluginWindow, etc.
```

For detailed architecture, see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)

---

## Documentation

- [User Guide](docs/USER_GUIDE.md) - Complete usage tutorial
- [API Reference](docs/API_REFERENCE.md) - MCP tools and protocols
- [Developer Guide](docs/DEVELOPER_GUIDE.md) - Building and contributing
- [Architecture](docs/ARCHITECTURE.md) - Technical design details

---

## License

MIT License - See [LICENSE](LICENSE) for details.

---

## Credits

Built with [JUCE](https://juce.com/) 8.0
