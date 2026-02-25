# SnappySnap VST Plugin - Research Documentation

> **Research Date:** February 23, 2026
> **Plugin Name:** SnappySnap
> **Developer:** Electric Smudge
> **Official Website:** https://electricsmudge.com/

---

## Overview

SnappySnap is a **creative plugin snapshot morphing host** developed by Electric Smudge. It is designed to make static plugins "come alive" by enabling smooth morphing between different parameter states. Notably, SnappySnap is the **world's first audio plugin with native Model Context Protocol (MCP) integration** for AI agent connectivity.

### Who It's For

- **Live performers** requiring instant preset switching and expressive morphing
- **Sound designers** exploring evolving textures and generative sounds
- **Ambient/drone producers** creating living, evolving soundscapes
- **Film/TV composers** needing quick exploration of plugin capabilities
- **AI-assisted music producers** using conversational control via Claude Desktop

---

## Core Functionality

### Primary Purpose

SnappySnap is a **meta-plugin** that hosts other VST3/AU plugins (instruments and effects) and adds dynamic morphing capabilities to them. Rather than being a synthesizer or effect itself, it makes other plugins dynamic by enabling:

1. **Snapshot Capture** - Full state capture of hosted plugin parameters
2. **Parameter Morphing** - Smooth interpolation between parameter states
3. **Physics-Based Movement** - Organic, natural-sounding transitions
4. **AI Integration** - Conversational control via MCP server

### How It Works

```
[Host DAW] → [SnappySnap] → [Hosted Plugin (VST3/AU)]
                    ↓
            [12 Snapshot Slots]
                    ↓
         [Morphing Engine (XY Pad / Fader)]
                    ↓
         [Physics Engine (Elastic / Drift)]
```

---

## Technical Specifications

| Attribute | Details |
|-----------|---------|
| **Plugin Formats** | VST3 (Windows/macOS), AU (macOS) |
| **Platform Support** | Windows 10+, macOS 13+ |
| **Apple Silicon** | Native support (Universal Binary) |
| **Hosted Plugin Formats** | VST3, AU only (no VST2 support) |
| **Framework** | JUCE-based |
| **MCP Server** | JSON-RPC 2.0 on localhost:30001 |
| **MIDI Support** | Note triggers (C3-B3), CC1, Program Change 0-127 |
| **License** | 2 machines per license key |
| **Activation** | Online verification every 30 days |

### DAW Compatibility

Reported to work with:
- Ableton Live
- Bitwig Studio
- FL Studio
- Logic Pro
- Reaper
- GigPerformer

### Known Limitations

- Cannot host DAW built-in instruments (proprietary formats)
- Some drum plugins may have transport/sync issues
- Crackling possible with heavy synths (mitigate with larger buffers)
- Discrete parameters (oscillator type, toggles) may glitch during morph
- Visual feedback doesn't work consistently across all plugins

---

## Features and Controls

### Snapshot System

| Feature | Details |
|---------|---------|
| **Snapshot Slots** | 12 slots arranged in circular "clock" layout |
| **Preset Storage** | 16 banks × 128 presets = 2,048 total |
| **Meta-Presets** | Store hosted plugin, internal state, all snapshots, macros, morph settings |
| **Fast Recall** | Bypasses plugin's internal state restoration for instant switching |

### Morphing Capabilities

#### 1D Morph Knob (Snap Fader)
- Linear interpolation through all snapshots
- Range: 0.0 to 1.0
- MIDI CC1 control

#### 2D XY Pad
- Distance-weighted interpolation between all 12 snapshots simultaneously
- Visual feedback showing parameter changes
- MIDI controllable

### Physics Modes

| Mode | Description |
|------|-------------|
| **Direct** | Instant response, no physics |
| **Elastic** | Spring-damper physics with mass/spring creating overshoot and bounce |
| **Drift** | Autonomous wandering using Perlin noise (3 sub-modes: Free, Anchored, Orbit) |

### Additional Features

| Feature | Description |
|---------|-------------|
| **8 Macro Knobs** | Assignable parameters for performance control |
| **Sanity Mode** | Protects critical parameters (volume, bypass, mute) from morphing |
| **Breeding Mode** | Genetic algorithm combining Parent A + Parent B with crossover and mutation |
| **Relative Drift** | Weighted drift (0-99% subtle, 100% chaos) |
| **Link Mode** | Multi-instance synchronization |
| **AI Bridge (MCP)** | Conversational control via Claude Desktop |

### MIDI Control

- **Note Triggers** - Configurable octave for snapshot recall
- **Program Change** - 0-127 preset switching
- **Bank Select** - CC#0 for bank changes
- **Mod Wheel** - CC1 for morph control
- **Remote Save** - CC#110-121

---

## AI Integration (World's First)

SnappySnap includes native **Model Context Protocol (MCP)** server integration, enabling:

- **Conversational control** via Claude Desktop
- **AI-assisted preset browsing** and parameter tweaking
- **Natural language sound design** (e.g., "make this sound darker")
- **Snapshot morphing** controlled by AI agents

This represents a significant innovation in audio plugin technology, bridging AI assistants with music production workflows.

---

## Pricing and Availability

### Pricing Structure

| Edition | Price | Features |
|---------|-------|----------|
| **Light Edition (LE)** | FREE | 2 snapshots, 1D morphing only, basic randomization |
| **Full Version** | $55 USD | 12 snapshots, 2D morphing, breeding, macros, AI Bridge, preset manager |

### Licensing

- One-time purchase (no subscription)
- 2 machines per license key
- Online activation required (checks every 30 days)
- 7-day free trial available for full version

### Where to Obtain

- **Official Website:** https://electricsmudge.com/
- **Trial Download:** Available from official website
- **LE Download:** Free from official website

---

## Community Feedback and Reviews

### Industry Recognition

- Featured on **Sonicstate's SonicTALK podcast** (February 2026)
- Described as "seemingly inspired by the Neuzeit DROP" hardware controller
- Generally regarded as an **innovative concept with potential**

### User Feedback (KVR Audio Forums)

#### Positive Aspects
- Innovative concept for creative workflows
- Visual feedback showing parameter changes
- Useful for EQ morphing and sound design
- Developer responsive to community feedback

#### Reported Issues

**CPU Performance:**
- Can be CPU intensive when morphing 3+ plugins with 500+ parameters
- Performance varies significantly between different plugins

**Plugin Compatibility (Visual Feedback Issues):**

| Plugin | Visual Feedback Status |
|--------|----------------------|
| FabFilter Pro-Q 4 | ✅ Works well |
| Kirchhoff EQ | ⚠️ Slow GUI performance |
| Absynth6 | ❌ No visual feedback |
| ZL3 | ⚠️ Inconsistent |
| GRM Atelier FX | ❌ No visual feedback |

**Workflow Limitations:**
- Cannot simultaneously morph between presets AND have individual parameter control from outside
- Handling described as "cumbersome" by some users
- Workflow is "use case and specific plugin sensitive"

### Community Consensus

Users recommend:
1. **Test thoroughly** with your specific plugins before purchasing
2. **Use the free LE version** to verify compatibility first
3. Be aware it's in **early development stages** - may need refinement
4. Not yet a "buy and use" solution for everyone

---

## Use Cases and Applications

### Ideal Scenarios

| Use Case | Application |
|----------|-------------|
| **Live Performance** | Instant preset switching, expressive morphing, multi-instance sync |
| **Sound Design** | Exploring evolving textures, generative sounds, breeding variations |
| **Ambient/Drone Music** | Drift Mode creates living, evolving soundscapes |
| **Electronic Production** | Breeding new variations from existing presets |
| **Film/TV Scoring** | Quick exploration of plugin capabilities |
| **AI-Assisted Production** | Natural language sound design via MCP |

### Genre Suitability

- ✅ Ambient
- ✅ Drone
- ✅ Electronic
- ✅ Soundtrack/Score
- ✅ Experimental Music
- ⚠️ Any genre requiring expressive plugin control (compatibility dependent)

---

## Comparisons to Alternatives

### Hardware Inspiration

**Neuzeit DROP** (Hardware Controller)
- SnappySnap appears inspired by this concept
- DROP releasing Summer 2025 (hardware MIDI controller)
- SnappySnap brings similar snapshot morphing to software

### Alternative Plugin Hosts

| Plugin | Price | Key Difference |
|--------|-------|----------------|
| Blue Cat Audio PatchWork | ~$199 | Plugin chainer with modulation |
| DDMF Metaplugin | ~$49 | Host up to 8 plugins series/parallel |
| GigPerformer | ~$199 | Dedicated live performance host |
| Kushview Element | Free | Open-source modular host |
| Kilohearts Snap Heap | ~$39 | Modular effects host |

### Important Distinction

SnappySnap is NOT directly comparable to:
- **Arturia Pigments** - A synthesizer (SnappySnap is a host)
- **Zynaptiq Morph** - Audio morphing (SnappySnap morphs parameters, not audio)

SnappySnap **makes other plugins dynamic** by adding morphing capabilities to existing VST3/AU instruments and effects.

---

## Version History

| Version | Date | Notes |
|---------|------|-------|
| v1.0.0 | February 2026 | Initial release |

### Current Status

- Early/fresh development stage
- Active development ongoing
- Mac installer verification planned
- Developer responsive to feedback

---

## Limitations and Known Issues

### Technical Limitations

1. **No VST2 Support** - Only hosts VST3 and AU plugins
2. **DAW Built-ins** - Cannot host proprietary DAW instruments
3. **Transport Issues** - Some drum plugins may have sync problems
4. **Audio Artifacts** - Crackling with heavy synths (use larger buffers)
5. **Discrete Parameters** - Oscillator types, toggles may glitch during morph

### Compatibility Considerations

- Visual feedback works inconsistently across plugins
- CPU usage varies significantly by plugin complexity
- Some plugins work better than others
- **Recommendation:** Always test with the free LE version first

---

## Sources and References

### Official Sources
- [Official Website](https://electricsmudge.com/)
- [FAQ](https://electricsmudge.com/faq)

### Industry Coverage
- Sonicstate SonicTALK Podcast (February 2026)

### Community Sources
- KVR Audio Forums - User discussions and feedback

### Third-Party Listings
- audioba.com
- inpin.fun

---

## Research Notes

### Information Verification Status

| Aspect | Verification Status | Notes |
|--------|-------------------|-------|
| Core functionality | ✅ Verified | Official website and reviews |
| Technical specifications | ⚠️ Partially verified | Some details from community |
| Pricing | ✅ Verified | Official website |
| Community feedback | ✅ Verified | KVR forums |
| Compatibility issues | ⚠️ User-reported | May vary by system |

### Unverified Information

- Exact CPU impact metrics
- Comprehensive DAW compatibility matrix
- Detailed comparison to all alternatives

### Research Methodology

This documentation was compiled through:
1. Web searches for official sources
2. Industry publication coverage
3. Community forum analysis (KVR Audio)
4. Third-party plugin listing verification

---

## Note: Potential Name Confusion

During research, a similarly-named open-source project called **MorphSnap** was discovered in the local codebase (D:\morphy). This is a **different project** with similar functionality:

- **SnappySnap** - Commercial plugin by Electric Smudge ($55)
- **MorphSnap** - Open-source project (MIT license)

Both plugins offer parameter morphing capabilities but are developed independently.

---

*Documentation compiled by AI research team on February 23, 2026*
