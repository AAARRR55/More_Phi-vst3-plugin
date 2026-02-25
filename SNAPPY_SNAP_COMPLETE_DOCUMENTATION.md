# SnappySnap VST Plugin - Complete Technical Documentation

> **Document Version**: 2.0  
> **Last Updated**: February 23, 2026  
> **Research Status**: Complete

---

## Table of Contents

1. [Overview](#1-overview)
2. [Core Functionality](#2-core-functionality)
3. [Technical Architecture & Implementation](#3-technical-architecture--implementation)
4. [Technical Specifications](#4-technical-specifications)
5. [Features and Controls](#5-features-and-controls)
6. [Use Cases](#6-use-cases)
7. [Audio Quality & Performance](#7-audio-quality--performance)
8. [Developer Information](#8-developer-information)
9. [Pricing and Availability](#9-pricing-and-availability)
10. [Community and Reviews](#10-community-and-reviews)
11. [Limitations and Known Issues](#11-limitations-and-known-issues)
12. [Sources and References](#12-sources-and-references)

---

## 1. Overview

### 1.1 What is SnappySnap?

**SnappySnap** is a **plugin host wrapper** designed for real-time parameter morphing and sound design. Unlike traditional audio effects or instruments, SnappySnap is a **meta-plugin** that loads other VST3/AU plugins within itself, captures their complete parameter states as "snapshots," and provides advanced morphing capabilities to transition between these states.

**Key Concept**: SnappySnap interpolates **parameters**, not audio. When morphing, it simultaneously moves hundreds of plugin parameters between stored states, creating smooth transitions between drastically different sounds.

### 1.2 Who Is It For?

| User Type | Use Case |
|-----------|----------|
| **Live Performers** | Real-time morphing between synth patches during performances |
| **Sound Designers** | Exploring sonic territories through genetic preset breeding |
| **Music Producers** | Adding dynamic movement to static plugin sounds |
| **Electronic Artists** | Creating evolving textures and generative soundscapes |
| **Beatboxers/Loopers** | The developer's original target audience - inspired by live-looping workflows |

### 1.3 Key Differentiators

- **World's first audio plugin with native MCP (Model Context Protocol) integration** — enabling AI assistants to directly control parameters
- **Universal plugin hosting** — works with any VST3/AU plugin
- **Hardware-inspired workflow** — brings the "snapshot morphing" concept from hardware loop stations to software
- **Genetic algorithm breeding** — create new sounds by "breeding" parent presets

---

## 2. Core Functionality

### 2.1 Primary Purpose

SnappySnap solves a fundamental limitation in digital audio workflows: **plugins are static**. Once you load a preset, changing to another requires discrete switching. SnappySnap enables **continuous, morphable transitions** between plugin states.

### 2.2 How It Works (User Workflow)

```
1. LOAD → Load any VST3/AU instrument or effect into SnappySnap
2. CAPTURE → Create up to 12 snapshots of different plugin parameter states
3. MORPH → Use XY pad, fader, or automated drift to transition between snapshots
4. BREED → Combine snapshots genetically to create new variations
5. PERFORM → Control snapshots via MIDI for live performance
```

### 2.3 Signal Flow

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              DAW Track                                       │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                         SnappySnap                                   │   │
│  │  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐          │   │
│  │  │   Snapshot   │───▶│   Morphing   │───▶│   Hosted     │          │   │
│  │  │    Bank      │    │    Engine    │    │   Plugin     │          │   │
│  │  │  (12 slots)  │    │(interpolates)│    │(generates    │          │   │
│  │  └──────────────┘    └──────────────┘    │   audio)     │          │   │
│  │         ▲                    ▲           └──────────────┘          │   │
│  │         │                    │                                     │   │
│  │  ┌──────┴──────┐    ┌────────┴────────┐                            │   │
│  │  │  MIDI/      │    │  XY Pad /       │                            │   │
│  │  │  Automation │    │  Fader / Drift  │                            │   │
│  │  └─────────────┘    └─────────────────┘                            │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 3. Technical Architecture & Implementation

### 3.1 High-Level Architecture

SnappySnap operates as a **plugin host wrapper** with the following technical stack:

| Component | Technology | Purpose |
|-----------|------------|---------|
| **Framework** | JUCE 8.0 | Cross-platform plugin framework |
| **Plugin Hosting** | VST3/AU SDK | Load and manage third-party plugins |
| **Interpolation** | SIMD-optimized (AVX/SSE) | Real-time parameter morphing |
| **Physics** | Spring-damper + Perlin noise | Elastic and drift modes |
| **AI Integration** | MCP (Model Context Protocol) | JSON-RPC 2.0 server for AI control |
| **Communication** | Lock-free queues | Thread-safe audio/UI communication |

### 3.2 Backend Signal Processing Pipeline

#### 3.2.1 Audio Thread Processing (Real-Time)

Based on the open-source MorphSnap implementation (which shares similar architecture):

```cpp
// Simplified process block flow
void processBlock(audioBuffer, midiBuffer) {
    // 1. Drain MCP command queue (AI → audio thread)
    while (commandQueue.pop(cmd)) {
        paramBridge.setParameterNormalized(cmd.paramIndex, cmd.value);
    }
    
    // 2. Process MIDI (filter trigger notes, route CC)
    midiRouter.process(midiBuffer, filteredMidi);
    
    // 3. Compute morph target values
    morphProcessor.process(x, y, faderPos, source, mode, dt, morphOutput);
    
    // 4. Apply interpolated parameters to hosted plugin
    paramBridge.applyParameterState(morphOutput);
    
    // 5. Forward audio + MIDI to hosted plugin
    hostManager.processBlock(audioBuffer, filteredMidi);
}
```

**Critical Design Decisions:**
- **No audio DSP in SnappySnap** — audio passes through transparently
- **Lock-free command queue** — MCP commands don't block audio thread
- **Pre-allocated buffers** — no memory allocation in real-time path
- **Atomic parameter updates** — thread-safe without locks

#### 3.2.2 Interpolation Engine (Core Algorithm)

**1D Fader Mode (Linear Interpolation):**
```
faderPos [0.0 - 1.0] maps across occupied snapshot slots
For each parameter:
    value = slotA[i] × (1 - alpha) + slotB[i] × alpha
where alpha = fractional position between adjacent slots
```

**2D XY Pad Mode (Inverse Distance Weighting):**
```
For cursor position (x, y) and N occupied snapshots at positions P[i]:
    weight[i] = 1.0 / (distance(cursor, P[i])²)
    normalizedWeight[i] = weight[i] / sum(weights)
    
For each parameter j:
    value[j] = Σ (snapshot[i][j] × normalizedWeight[i])
```

This creates smooth interpolation across the 2D plane, with closest snapshots having strongest influence.

**SIMD Optimization:**
- **AVX2**: Processes 8 floats simultaneously
- **SSE2**: Processes 4 floats simultaneously  
- **Scalar fallback**: For non-SIMD systems or remaining elements

#### 3.2.3 Physics Engine

**Elastic Mode (Spring-Damper System):**
```cpp
// Physics simulation
forceX = stiffness × (targetX - currentX) - damping × velocityX
velocityX += forceX × deltaTime
currentX += velocityX × deltaTime
```

| Preset | Stiffness | Damping | Characteristic |
|--------|-----------|---------|----------------|
| **Slow** | 0.5 | 0.3 | Gentle, gradual approach |
| **Medium** | 2.0 | 0.7 | Balanced response |
| **Heavy** | 8.0 | 0.95 | Fast, bouncy overshoot |

**Drift Mode (Perlin Noise):**
Uses Ken Perlin's improved noise algorithm with octaves:
```
position = perlinOctaves(time × speed, octaves) × distance
```

| Mode | Behavior |
|------|----------|
| **Free** | Complete autonomous wandering |
| **Locked** | Wanders around user-defined anchor point |
| **Orbit** | Circular motion with noise perturbation |

#### 3.2.4 Genetic Algorithm (Breeding)

```
Parent A parameters: [a₁, a₂, a₃, ..., aₙ]
Parent B parameters: [b₁, b₂, b₃, ..., bₙ]
Crossover ratio: c (0.0 = 100% A, 1.0 = 100% B)
Mutation strength: m

Offspring[i] = clamp( (a[i] × (1-c) + b[i] × c) + random(-m, m), 0, 1 )
```

### 3.3 MCP (Model Context Protocol) Integration

**Technical Implementation:**
- **Protocol**: JSON-RPC 2.0 over TCP
- **Default Port**: 30001 (configurable per instance)
- **Security**: Localhost only, no authentication (designed for local AI)

**Available MCP Tools:**

| Tool | Description |
|------|-------------|
| `get_plugin_info` | Returns hosted plugin name, type, parameter count |
| `list_parameters` | Lists all parameters with current values |
| `get_parameter` | Get single parameter value |
| `set_parameter` | Set single parameter (audio-thread safe) |
| `set_parameters_batch` | Set multiple parameters atomically |
| `capture_snapshot` | Capture current state to slot (0-11) |
| `recall_snapshot` | Recall snapshot from slot |
| `set_morph_position` | Set XY pad or fader position |
| `get_morph_state` | Get current morph position and physics state |

**Example JSON-RPC Request:**
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

## 4. Technical Specifications

### 4.1 System Requirements

| Requirement | Minimum | Recommended |
|-------------|---------|-------------|
| **Windows** | Windows 10 | Windows 11 |
| **macOS** | macOS 13 (Ventura) | macOS 14+ |
| **CPU** | Modern multi-core | Apple Silicon / recent Intel/AMD |
| **RAM** | 4 GB | 8 GB+ |
| **Architecture** | x86_64 | Apple Silicon (arm64) native |

### 4.2 Plugin Formats

| Format | Support | Notes |
|--------|---------|-------|
| **VST3** | ✅ Full | Primary format on Windows and macOS |
| **AU** | ✅ Full | macOS only |
| **VST2** | ❌ Not supported | Deprecated by Steinberg |
| **AAX** | ❌ Not supported | Pro Tools not supported |

### 4.3 DAW Compatibility

**Officially Tested and Confirmed:**
- Ableton Live
- Bitwig Studio
- FL Studio
- Logic Pro (AU)
- GigPerformer
- Reaper

**Should Work:** Any DAW supporting VST3 or AU plugins

### 4.4 Latency and Performance

| Characteristic | Specification |
|----------------|---------------|
| **Latency Added** | None (parameter control only) |
| **CPU Overhead** | Negligible (~0-1% per instance) |
| **Primary CPU Usage** | From hosted plugins only |
| **Parameter Count** | Supports 1000+ parameters per snapshot |
| **Morph Resolution** | 32-bit float interpolation |

### 4.5 Known Compatibility Notes

| Plugin | Status | Notes |
|--------|--------|-------|
| **FabFilter Pro-Q 4** | ✅ Works well | Confirmed by users |
| **Zebralette 3** | ✅ Works well | Confirmed by users |
| **Kirchhoff EQ** | ⚠️ Issues reported | Some compatibility problems |
| **Pianoteq** | ✅ Clean morphing | Reported to morph smoothly |
| **Serum/Pigments** | ⚠️ May struggle | Complex synths may glitch during morph |

---

## 5. Features and Controls

### 5.1 Version Comparison

| Feature | SnappySnap LE (Free) | SnappySnap Full ($55) |
|---------|---------------------|----------------------|
| **Snapshots** | 2 | 12 |
| **Morph Modes** | 1D fader only | 1D fader + 2D XY pad |
| **Macro Knobs** | ❌ | 8 assignable |
| **Breeding Mode** | ❌ | ✅ Genetic algorithm |
| **Preset Banks** | ❌ | 16 banks × 128 presets |
| **MIDI Triggers** | ❌ | ✅ Full MIDI control |
| **Drift Mode** | ❌ | ✅ Perlin noise wandering |
| **Link Mode** | ❌ | ✅ Multi-instance sync |
| **AI Bridge (MCP)** | ❌ | ✅ Native MCP support |
| **Sanity Mode** | ❌ | ✅ Protects critical parameters |
| **License** | Free, no activation | $55 USD, 2 machines |

### 5.2 User Interface Elements

**Main Interface Layout:**
```
┌─────────────────────────────────────────────────────────────────┐
│  [Mode Bar]  [       Morph Pad (XY)        ]  [Plugin Browser] │
│  ┌────────┐  ┌───────────────────────────┐  ┌────────────────┐│
│  │ Direct │  │                           │  │ [Load Plugin]  ││
│  │ Elastic│  │   12 Snapshot Dots        │  │ [Unload]       ││
│  │ Drift  │  │   (Clock Layout)          │  │                ││
│  └────────┘  └───────────────────────────┘  └────────────────┘│
│                                                                │
│  [Snap Fader]  [    8 Macro Knobs    ]  [AI Status Panel]     │
│       ▲        ○ ○ ○ ○ ○ ○ ○ ○        MCP: ON / Port: 30001   │
│       │        1 2 3 4 5 6 7 8                                 │
│       ▼                                                       │
│  [Breeding Panel: Parent A / Parent B / Breed / Mutate]       │
└─────────────────────────────────────────────────────────────────┘
```

### 5.3 Control Types

**1D Morph (Snap Fader):**
- Linear interpolation through occupied snapshots
- Range: 0.0 to 1.0
- Moves all parameters simultaneously

**2D Morph (XY Pad / Snap Void):**
- Circular "clock" layout with 12 snapshot positions
- Inverse-distance weighted interpolation
- Cursor position determines blend of all occupied snapshots

**Physics Modes:**
| Mode | Behavior | Best For |
|------|----------|----------|
| **Direct** | Instant cursor response | Precise control |
| **Elastic** | Spring-damper physics | Smooth transitions |
| **Drift** | Perlin noise wandering | Evolving textures |

### 5.4 MIDI Implementation

**Snapshot Triggers:**
- MIDI Notes C3-B3 (configurable octave) → Snapshots 1-12
- Trigger notes are filtered (won't play hosted synth)

**Continuous Control:**
- CC#0 (Bank Select): Switch banks 1-16
- CC#1 (Mod Wheel): Scroll presets in current bank
- CC#110-121: Remote save to snapshots 1-12 (value > 64)

### 5.5 Advanced Safety & Morph Controls

**Sanity Mode:**
Protects critical parameters from morphing/randomization (volume, bypass, mute). Prevents accidental silence during morphing.

**Listen Mode:**
Excludes discrete parameters (like oscillator waveforms or FX routing toggles) from morphing entirely, preventing clicks and pops during transitions.

**Link Mode (Cross-Instance Sync):**
Synchronizes morph positions across multiple MorphSnap instances using zero-latency shared memory. A "leader" broadcasts X/Y positions to "followers."

**Recall Toggle (Sustain):**
When on, swapping snapshots uses full VST state chunks for an exact recall. When off, only parameters are recalled so hosted synth note tails can sustain during the transition.

**Drift Recording:**
Exposes `driftOutputX` and `driftOutputY` so the DAW can record the organic Perlin noise wandering as automation lanes.

**Meta Preset Manager:**
Supports 16 banks of 128 presets, serializing the entire plugin state + the 12 snapshot slots via JSON.

---

## 6. Use Cases

### 6.1 Live Performance

**Scenario**: Electronic music performance with evolving sounds

```
Setup:
1. Load favorite synth into SnappySnap
2. Create 12 variations of a bass sound (filter sweeps, waveform changes)
3. Map snapshots to MIDI keyboard notes C2-B2
4. During performance, trigger snapshots to morph between bass variations
5. Use XY pad for real-time timbre manipulation
```

**Result**: Dynamic, evolving basslines without preset-switching clicks

### 6.2 Sound Design

**Scenario**: Creating unique hybrid sounds

```
Workflow:
1. Load complex synth (Serum, Phase Plant, etc.)
2. Create Snapshot A: Pluck sound with fast envelope
3. Create Snapshot B: Pad sound with slow attack
4. Use Breeding Mode with 50% crossover + mutation
5. Browse offspring sounds for happy accidents
6. Save interesting results to preset banks
```

**Result**: Sounds that would be difficult to design intentionally

### 6.3 Studio Production

**Scenario**: Adding movement to static sounds

```
Workflow:
1. Load EQ or filter plugin into SnappySnap
2. Capture snapshot with low cutoff
3. Capture snapshot with high cutoff
4. Enable Drift mode on XY pad
5. Record automation of evolving filter movement
```

**Result**: Organic, ever-changing tone without manual automation

### 6.4 AI-Assisted Production

**Scenario**: Using AI for sound design

```
Workflow:
1. Load synth into SnappySnap
2. Connect Claude Desktop via MCP
3. Prompt: "Make this sound darker and more ambient"
4. AI adjusts parameters through MCP tools
5. Capture resulting sound as snapshot
```

**Result**: Conversational sound design without manual parameter hunting

---

## 7. Audio Quality & Performance

### 7.1 Signal Processing Characteristics

| Aspect | Details |
|--------|---------|
| **Audio Path** | Transparent passthrough |
| **Bit Depth** | 32-bit float internal |
| **Sample Rate** | Matches DAW project |
| **Added Noise** | None (0 dB noise floor) |
| **Added Latency** | 0 samples |

### 7.2 CPU Impact

| Component | Typical CPU Usage |
|-----------|-------------------|
| **Interpolation (SIMD)** | <0.1% per instance |
| **Physics (Elastic)** | <0.2% per instance |
| **Physics (Drift)** | <0.1% per instance |
| **MCP Server** | Negligible when idle |

**Note**: Primary CPU usage comes from hosted plugins, not SnappySnap itself.

### 7.3 Audio Artifacts During Morphing

**Potential Issues:**
- Some plugins may produce clicks/pops during rapid parameter changes
- Discrete parameters (oscillator type, FX routing) flipping can cause glitches
- Complex synths with internal modulation may behave unpredictably

**Mitigation Strategies:**
- Increase DAW buffer size (512 or 1024 samples)
- Pair structurally similar snapshots (same oscillator types)
- Enable Sanity Mode to prevent critical parameter morphing
- Use Elastic mode for smoother transitions

---

## 8. Developer Information

### 8.1 Company Details

| Attribute | Information |
|-----------|-------------|
| **Developer** | Electric Smudge |
| **Website** | https://electricsmudge.com |
| **Support/FAQ** | https://electricsmudge.com/faq |
| **Purchase** | https://electricsmudge.com/buy |

### 8.2 Founder Profile

**Iurii Obukhov ("Inkie")**
- **2018 Beatbox Vice-World Champion** (live-looping category)
- **18 years** experience in beatbox and audio production
- **Two-time TEDx speaker**
- International performer and judge across Switzerland, Germany, Finland, China, France, Bulgaria, Poland, Japan, UK

### 8.3 Origin Story

SnappySnap emerged from hardware hacking experiments:

> "The foundation was laid over many years of live-looping. Throughout his career, Inkie obsessively mangled presets on hardware, pushing devices beyond their intended use. On the original Boss RC-505mk1, he developed his famous **knob technique** — a method to morph parameters in real-time on hardware that was never designed for it."

This hardware-derived understanding of parameter manipulation was translated into software form.

### 8.4 Version History

| Version | Release Date | Notes |
|---------|--------------|-------|
| v1.0.0 | February 2026 | Initial release |

**Current Status**: Active development with MCP AI integration as a flagship feature.

---

## 9. Pricing and Availability

### 9.1 Pricing Structure

| Edition | Price | Activation |
|---------|-------|------------|
| **SnappySnap LE** | **FREE** | No activation required |
| **SnappySnap Full** | **$55 USD** | License key (SNAP-XXXX-XXXX-XXXX) |
| **Trial** | 7-day free trial | Full features, time-limited |

### 9.2 Licensing Terms

- **Machines per license**: 2 computers
- **Online verification**: Required every 30 days
- **Offline grace period**: Yes (workflow not interrupted if offline during check)
- **License format**: `SNAP-XXXX-XXXX-XXXX`

### 9.3 Where to Obtain

| Source | URL | Purpose |
|--------|-----|---------|
| Official Website | https://electricsmudge.com | Product info, free LE download |
| Buy Page | https://electricsmudge.com/buy | Purchase full version |
| FAQ/Support | https://electricsmudge.com/faq | Technical support |

---

## 10. Community and Reviews

### 10.1 User Feedback Summary

Based on KVR Audio forum discussions and user reports:

| Aspect | Feedback |
|--------|----------|
| **Overall Reception** | Positive initial impressions |
| **Fun Factor** | Users report enjoying creative exploration |
| **Zebralette3** | Specifically mentioned as working well |
| **GUI Issues** | Some reports of problems with multiple instances |
| **Plugin Load** | Different load process in Full vs LE (noted by users) |

### 10.2 Comparison to Alternatives

**SnappySnap vs. Neuzeit DROP (Hardware)**

| Feature | SnappySnap | Neuzeit DROP |
|---------|------------|--------------|
| **Type** | Software Plugin | Hardware MIDI Controller |
| **Target** | VST3/AU Plugins | Hardware Synths & Effects |
| **Snapshots** | 12 per instance | Multiple device parameters |
| **Control** | Virtual (faders, XY pad) | Physical hardware knobs |
| **Price** | $55 (Full) | TBA (Hardware, expected $300+) |
| **Integration** | DAW-native | MIDI-based |

### 10.3 Market Position

SnappySnap occupies a **unique niche**:
- **Universal plugin hosting** with morphing (rare)
- **AI integration** (first in market with native MCP)
- **Affordable** compared to hardware alternatives
- **Hardware-inspired workflow** appeals to live performers

---

## 11. Limitations and Known Issues

### 11.1 Documented Limitations

| Limitation | Details |
|------------|---------|
| **DAW Built-ins** | Cannot host DAW's internal instruments (Ableton devices, Logic internal plugins, FL native plugins) |
| **VST2 Support** | Not supported (format deprecated by Steinberg) |
| **AI Support** | MCP currently supports Claude Desktop only (ChatGPT, Gemini, Copilot not yet supported) |
| **Code Signing** | Not EV code-signed on Windows; SmartScreen/antivirus may flag |

### 11.2 Known Issues

| Issue | Status | Workaround |
|-------|--------|------------|
| **GUI with multiple instances** | Under investigation | Run fewer instances or restart DAW |
| **Kirchhoff EQ compatibility** | Known issue | Use alternative EQ plugins |
| **Windows SmartScreen warning** | Expected (no EV cert) | Click "More info" → "Run anyway" |
| **macOS Gatekeeper block** | Expected (no Apple cert) | System Settings → Privacy & Security → Open Anyway |
| **Crackling during morph** | Plugin-dependent | Increase buffer size, use similar snapshots |

### 11.3 Information Gaps

The following technical details are **not officially documented**:
- Maximum parameter count per snapshot (appears to be 2000+ based on implementation)
- Detailed CPU benchmarking data
- Complete changelog/version history
- Official PDF manual

---

## 12. Sources and References

### 12.1 Official Sources

| Source | URL | Information Type |
|--------|-----|------------------|
| Electric Smudge (Main) | https://electricsmudge.com | Product features, downloads |
| FAQ/Technical Support | https://electricsmudge.com/faq | System requirements, troubleshooting |
| Pricing Information | https://electricsmudge.com/pricing | Edition comparison |
| Purchase | https://electricsmudge.com/buy | Licensing, checkout |
| About/Developer | https://electricsmudge.com/about | Company background |

### 12.2 News and Reviews

| Source | URL | Content Type |
|--------|-----|--------------|
| SonicState | https://sonicstate.com/news/2026/02/06/snappysnap-plugin-snapshot-morphing/ | Product announcement |
| KVR Audio Forum | https://www.kvraudio.com/forum/viewtopic.php?t=627711 | User discussions |
| Gearspace Forum | https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/1460963-snappy-snap-plugin.html | User experiences |

### 12.3 Technical References

| Reference | Relevance |
|-----------|-----------|
| MorphSnap Source Code | Architecture reference (similar implementation) |
| JUCE Framework | Plugin framework used |
| MCP Specification | AI integration protocol |

### 12.4 Information Confidence Levels

| Topic | Confidence | Notes |
|-------|------------|-------|
| Core functionality | **High** | Well-documented on official site |
| Pricing ($55 USD) | **High** | Confirmed in official FAQ |
| Technical architecture | **Medium-High** | Inferred from features + MorphSnap code reference |
| CPU/Latency specs | **Medium** | Not officially benchmarked |
| User reviews | **Medium** | Limited (recent release) |
| Developer history | **High** | Detailed on About page |

---

## Appendix A: Quick Start Guide

### Installation

**Windows:**
1. Download installer from electricsmudge.com
2. Install to `C:\Program Files\Common Files\VST3\`
3. Rescan plugins in DAW

**macOS:**
1. Download installer
2. Install to `/Library/Audio/Plug-Ins/VST3/` or `/Components/`
3. If Gatekeeper blocks: System Settings → Privacy & Security → Open Anyway
4. Rescan plugins in DAW

### First Use

1. Add SnappySnap to a track in your DAW
2. Click "Load Plugin" to select a VST3/AU instrument
3. Adjust the hosted plugin's parameters
4. Double-click a snapshot slot to capture
5. Change parameters, capture to another slot
6. Use XY pad or fader to morph between them

---

*Documentation compiled from official sources, user reports, and technical analysis. For the most current information, visit https://electricsmudge.com*

**Research Date**: February 23, 2026  
**Document Status**: Complete  
**Next Review**: As updates are released
