---
stepsCompleted:
  - step-01-init
  - step-02-discovery
  - step-02b-vision
  - step-02b-vision
  - step-02c-executive-summary
  - step-03-success
  - step-04-journeys
  - step-05-domain
  - step-06-innovation
  - step-07-project-type
  - step-08-scoping
  - step-09-functional
  - step-10-nonfunctional
  - step-11-polish
  - step-12-complete
classification:
  projectType: desktop_app
  domain: general
  complexity: medium
  projectContext: brownfield
inputDocuments:
  - _bmad-output/planning-artifacts/product-brief-morphy-2026-02-24.md
  - docs/SNAPPY_SNAP_RESEARCH.md
  - docs/snappy-snap-vst-plugin-research.md
  - docs/USER_GUIDE.md
  - docs/MASTER_ARCHITECTURAL_BLUEPRINT.md
  - docs/API_REFERENCE.md
  - docs/ARCHITECTURE.md
  - docs/CPP_IMPLEMENTATION_STRATEGY.md
  - docs/DAW_TESTING_CHECKLIST.md
  - docs/DEVELOPER_GUIDE.md
  - docs/EPIC_COMPLETION_REPORT.md
  - docs/EPIC_PLANNING_DOCUMENT.md
  - docs/FEATURE_REFERENCE.md
  - docs/IMPLEMENTATION_PLAN.md
  - docs/LEARN_MODE_GUIDE.md
  - docs/SNAPPY_SNAP_ARCHITECTURE_DESIGN.md
  - docs/TECHNICAL_AUDIT_REPORT.md
workflowType: 'prd'
---

# Product Requirements Document - morphy

**Author:** 20190
**Date:** 2026-02-24

## Executive Summary

Morphy is the world's first AI-native, community-powered sound exploration platform that transforms how music producers interact with plugin parameters. Unlike static preset management systems that force binary switching between saved states, Morphy enables real-time parameter morphing between up to 12 snapshots using an intuitive XY pad, revealing the vast sonic territories that exist between presets.

**The Problem:** Music producers and live performers are constrained by static plugin architectures. Current solutions require hours of manual parameter adjustment, produce jarring preset switches, or demand complex automation programming that cannot be performed live. The infinite possibilities between preset states remain unexplored.

**The Solution:** Morphy operates as a universal VST3/AU meta-plugin host that intercepts and interpolates parameter values from any hosted instrument or effect. Users capture complete plugin states as snapshots, then morph between them via XY pad using inverse-distance weighted interpolation. Native MCP (Model Context Protocol) integration enables conversational AI control—"make this darker" becomes intelligent parameter adjustment. Physics-based modes (Elastic spring-damper, Drift Perlin noise) provide organic, evolving sound movement unavailable in competing products.

### What Makes This Special

| Differentiator | Description | Competitive Advantage |
|----------------|-------------|----------------------|
| **Universal Plugin Host** | Works with any VST3/AU instrument or effect—Serum, Vital, Pro-Q, FabFilter, anything | Unlike closed-format solutions, Morphy is agnostic to hosted plugin |
| **First Native MCP Integration** | Direct AI control without middleware; AI-assisted exploration, not replacement | SnappySnap and competitors have zero AI integration |
| **Physics-Based Morphing** | Elastic (spring-damper) and Drift (Perlin noise) modes for organic performance | No competitor offers physics-driven interpolation |
| **Open-Source Model** | MIT licensed with community-driven preset sharing and crowdsourced AI models | Free vs. $55+ commercial alternatives; no proprietary API lock-in |
| **12-Slot Clock Architecture** | Circular XY pad layout enables intuitive 2D interpolation across all snapshots | Competitors use linear chains; Morphy enables true multi-point morphing |

**Core Insight:** The sonic space *between* presets is a vast, unexplored continent. Mathematical interpolation provides the map; AI provides the compass; the sound designer chooses the destination.

## Project Classification

| Attribute | Value | Implications |
|-----------|-------|--------------|
| **Project Type** | Desktop Application (VST3/AU Plugin) | Cross-platform native code; real-time audio constraints; plugin host architecture |
| **Domain** | General (Audio/Music Production) | No regulatory compliance; standard software quality requirements |
| **Complexity** | Medium | Real-time audio processing + AI integration; lock-free architecture requirements |
| **Project Context** | Brownfield | Extensive existing codebase; refactor for MVP scope; 30-day delivery target |

## Success Criteria

### User Success

| Persona | Success Signal | Measurable Outcome |
|---------|---------------|-------------------|
| **Morgan (AI Early Adopter)** | "I automated my patch transition in 30 seconds" | First morph via MCP within 2 minutes of setup |
| **Jordan (Sound Designer)** | "I discovered a sound I couldn't have designed manually" | Saves AI-discovered hybrid as new preset |
| **Alex (Live Performer)** | "Zero dropouts during 2-hour sets" | 100% crash-free sessions in Performance Mode |

**The "Aha!" Moment to Optimize For:**
> First time user morphs between 3+ snapshots and hears a sound that didn't exist in any of them

**Time-to-Value Targets:**
- Install to first morph: **< 2 minutes**
- MCP command response: **< 100ms**
- Plugin load success (VST3): **> 90%**

### Business Success

**Open-Source Ecosystem Metrics:**

| Metric | 6-Month Target | 12-Month Target |
|--------|---------------|-----------------|
| GitHub Stars | 500+ | 1,000+ |
| Community Presets | 1,000+ | 10,000+ |
| Tutorial Videos | 10+ | 50+ |
| Active Discord Users | 100+ | 500+ |
| Sponsors/Funding | 3+ | 10+ |

**Commercialization (Phase 2+):**
- Pro tier launch (advanced features)
- GitHub Sponsors + Open Collective
- Preset marketplace revenue share

### Technical Success

| Requirement | Target | Measurement |
|-------------|--------|-------------|
| Audio Thread CPU | < 3% overhead | Profiling on i7/M1 |
| Parameter Latency | < 1 block (< 2.9ms @ 44.1kHz/128) | Real-time monitoring |
| MCP Response | < 100ms | Server request logs |
| Crash-Free Rate | > 95% | Error telemetry |

**DAW Validation:**
- Ableton Live 11+ ✅
- FL Studio 20+ ✅
- Logic Pro (AU deferred to v0.3) ⚠️

### Measurable Outcomes

**North Star Metric:** Sounds Discovered (unique morphed presets saved per month)
- **Target:** 10,000/month across all users by Month 12

**Leading Indicators:**
- Time to first morph: < 2 minutes
- Snapshots per session: 8+ average
- AI commands per session: 15+
- Week 2 retention: 60%+

---

## Product Scope

### MVP - Minimum Viable Product (30-Day Target)

**Core Experience:**
> User loads Serum → captures 4 snapshots → morphs between them on XY pad → hears emergent sound

**Must-Have (IN):**
- 4 snapshots (not 12) - reduces UI complexity
- XY Pad morphing with inverse-distance weighting
- Basic MCP (3 tools): set_parameter, capture_snapshot, set_morph_position
- Direct physics only (Elastic/Drift deferred)
- VST3 plugin hosting (AU deferred)
- MIDI note triggers (C3-F3)

**Explicitly Out (CUT):**
- Learn Mode, Token Optimizer, Genetic Breeding
- 12-snapshot architecture
- Preset Manager (16×128), Link Mode
- MCPToolsExtended (17 tools → 3 tools)
- Discrete Parameter Handler

### Growth Features (Post-MVP)

**v0.2 (Morgan + Jordan Focus):**
- 12 snapshots
- Genetic breeding
- Elastic physics mode
- Token optimizer
- Learn Mode v1

**v0.3 (Alex Focus - Performance Mode):**
- Bulletproof reliability
- Drift physics
- Enhanced MIDI control
- AU format support

### Vision (Future)

**v1.0:**
- Preset Manager (16 banks × 128 presets)
- Link Mode (multi-instance sync)
- Discrete Parameter Handler
- Full MCP tool suite (26 tools)
- Community preset marketplace

**v2.0+:**
- AI-guided sound discovery
- Community-trained models
- Infinite snapshot architecture
- Cloud AI services
- Native DAW integration

## Product Scope & Phased Development

### MVP Strategy & Philosophy
**Approach:** Problem-Solving & Experience MVP. We must prove that AI-guided sound design and physics-based XY morphing solve the "blank canvas" problem faster and more intuitively than traditional methods.
**Resources:** Lean team (Core C++ DSP Engineer, UI/UX Frontend Dev, AI Integration Specialist).

### Phase 1: MVP Feature Set
**Core User Journeys:** Morgan (AI Discovery), Jordan (Deep Sound Design).
**Must-Have Capabilities:**
- VST3/AU Plugin hosting
- 12-Slot XY Pad Morphing Engine
- Spring/Damper & Perlin Noise (Drift) Physics
- Native MCP Server Integration
- Local SQLite preset management for `.morphy` banks

### Phase 2: Growth Features (Post-MVP)
- Cloud-synced community preset sharing (Sam's journey)
- Advanced "Performance Mode" optimizations for live stability (Alex's journey)
- Standalone application executable
- Pro features for Sound Designers (e.g., genetic variation/breeding).

### Phase 3: Vision (Expansion)
- Custom user-defined physics scripting capabilities
- Multi-user real-time jam sessions across the network
- Crowdsourced, community-trained AI models for specialized sound design tasks.
- Fully autonomous, generative ambient sessions running overnight.

## User Journeys

### 1. Morgan - The AI Early Adopter
**"Zero-Touch Sound Design"**

**Opening Scene:** Morgan is deep in a synthwave production session. The bassline feels flat—needs character, but breaking flow to tweak 50 parameters is creativity death.

**Rising Action:** Morgan tabs to Claude Desktop and types: *"Tell Morphy to make the bass more aggressive and slightly detuned."* The MCP server intercepts, analyzes current parameter states, and shifts the morph position toward a blend of "Distorted Pulse" and "Detuned Saw" snapshots.

**Climax:** The sound transforms instantly—exactly what Morgan envisioned. The XY pad cursor visibly moves, reflecting AI's adjustment. No context switching, no UI hunting.

**Resolution:** Morgan smiles, hits record, continues producing. Later tweets the workflow screenshot. Community engagement +1.

**Requirements Revealed:** Robust MCP architecture, bidirectional UI↔AI sync, sub-100ms response time.

---

### 2. Jordan - The Sound Designer
**"The Sonic Archaeologist"**

**Opening Scene:** Jordan landed the indie sci-fi game gig. Needs alien textures that don't sound like stock Serum presets. The "between" spaces are unexplored territory.

**Rising Action:** Jordan loads a granular delay, fills all 12 snapshots with extreme settings, and activates "Discovery Mode." Prompts AI: *"Find five hybrid textures between slots 3, 7, and 11."* The AI explores 12-dimensional parameter space, surfacing new XY coordinates.

**Climax:** Third suggestion—a warbling metallic groan—is perfect. Jordan saves it as a new preset, something they never would have found manually through parameter tweaking.

**Resolution:** Jordan uploads the snapshot bank to community hub. It gains traction within hours. Reputation + contract renewal.

**Requirements Revealed:** Discovery algorithm for parameter space exploration, save/export functionality, community hub integration.

---

### 3. Alex - The Live Performer
**"The Seamless Transition"**

**Opening Scene:** Packed club. 2-hour set ahead. The transition from melodic intro to techno drop must be flawless—any glitch kills the vibe.

**Rising Action:** Morphy on master bus, complex multi-effect chain. Alex engages **Performance Mode** (disables AI polling, locks UI, prioritizes audio thread). MIDI controller mapped to XY pad.

**Climax:** Build-up peaks. Alex sweeps joystick from bottom-left (heavy reverb/delay) to top-right (dry, compressed). Spring-damper physics make it feel *musical*—not robotic interpolation. The drop hits clean. Crowd roars.

**Resolution:** Zero dropouts, zero crashes. Alex adds Morphy to the "tour-ready" equipment list. Evangelizes on performer forums.

**Requirements Revealed:** Performance Mode toggle, bulletproof real-time stability, MIDI mapping, physics-based interpolation that feels expressive.

---

### 4. Rookie Riley - The Curious Beginner
**"The Gateway Drug"**

**Opening Scene:** Riley just bought their first MIDI controller. Found Morphy through a YouTube tutorial. Terrified of the complexity but intrigued by the XY pad demo.

**Rising Action:** Guided onboarding: "Load Serum → Capture 4 snapshots → Drag the pad." Riley double-clicks, captures variations, hesitantly drags the cursor.

**Climax:** The morph happens. Riley hears a sound that doesn't exist in any of the four snapshots. The "aha!" moment—*parameters are a landscape, not knobs*.

**Resolution:** Riley saves the session, posts first track with Morphy to SoundCloud. Tags the tutorial creator. Community grows.

**Requirements Revealed:** Progressive disclosure UI, guided onboarding, preset-first approach, "What does this do?" tooltips.

---

### Journey Requirements Summary

| Journey | Core Capability | Technical Requirement |
|---------|-----------------|----------------------|
| **Morgan** | MCP AI Integration | Sub-100ms response, bidirectional sync |
| **Jordan** | Discovery Mode | Parameter space exploration algorithm |
| **Alex** | Performance Mode | Lock-free audio thread, MIDI mapping |
| **Riley** | Onboarding | Progressive disclosure, guided workflows |

**Secondary Users (Future):**
- **Community Moderator:** Preset validation, content moderation
- **Plugin Developer:** API for extending Morphy capabilities
- **Support Engineer:** Troubleshooting, diagnostics

## Innovation & Novel Patterns

### Detected Innovation Areas

1. **Native MCP Server Integration:** Bridging natural language AI clients (like Claude Desktop) directly to audio plugin parameter states inside a DAW, enabling conversational sound design ("Make this darker").
2. **AI-Guided Parametric "Breeding" and Discovery:** Using AI algorithms to interpolate, explore, and recommend coordinates in a 12-dimensional parameter space to discover new, hybrid sounds between existing snapshots.
3. **Organic Physics Engine for Parameter Morphing:** Applying physics simulations—specifically spring/damper dynamics and Perlin noise (Drift)—to an XY proxy for autonomous, non-linear, and expressive sound evolution.
4. **The Meta-Plugin Community Ecosystem:** Creating an open-source, community-driven hub for sharing snapshot banks and custom MCP prompt templates for an agnostic meta-plugin, challenging closed-source commercial alternatives.

### Market Context & Competitive Landscape

Current alternatives (like DDMF Metaplugin or Blue Cat PatchWork) focus purely on utilitarian routing and static parameter recall. Even morphing plugins (like the closed-source SnappySnap) lack any AI integration or organic physics for autonomous movement. Morphy challenges the assumption that sound designers must manually scrub through hundreds of parameters or use linear macros to find complex, transitional sounds.

### Validation Approach

- **MCP Integration Validation:** We will validate the AI workflow by measuring the time-to-result for a producer to achieve a desired sonic outcome using natural language versus manual tweaking. We will track if producers actively use the "Discovery Mode" or if they revert to manual UI manipulation.
- **Physics Engine Validation:** User testing during live performance simulated sets (with Alex persona profiles) to measure the perceived smoothness, expressiveness, and stability of the XY transitions under heavy CPU load.

### Risk Mitigation Strategy

**Technical Risks & AI Fallback:** If the AI is latent or unreliable in real-time, the core DSP functionality must remain completely isolated and unaffected. Users will gracefully fall back to manual tactile XY control. The "Performance Mode" explicitly disables AI/TCP background polling to guarantee stability during critical live sets. In-app auto-updates are explicitly deferred to prevent unexpected DAW instability.
**Market Risks:** Validation hinges on beta testing—specifically measuring the "time-to-result" workflow metrics against traditional manual synthesis tweaking.
**Resource Risks:** The team is strictly prioritizing core audio DSP and local MCP stability for Phase 1. Complex cloud community features and robust account systems are explicitly deferred until Phase 2 to ensure a focused, high-quality MVP launch.

## Desktop App Specific Requirements

### Project-Type Overview

As a desktop application in the form of a VST3/AU audio plugin, Morphy must operate within the strict real-time performance constraints of digital audio workstations (DAWs) across multiple operating systems.

### Technical Architecture Considerations

#### Platform Support
- **Operating Systems:** Windows 10+ and macOS 11+ (Native support for both Intel and Apple Silicon/M-series).
- **Plugin Formats:** VST3 and AU (Audio Unit).

#### System Integration & Networking
- **File System:** Morphy requires secure, sandbox-compliant read/write access to load third-party plugin states and save custom `.morphy` preset banks.
- **MCP Server:** Runs a local TCP/WebSocket server to communicate with AI clients. Requires robust port management and instance tracking to avoid conflicts when multiple Morphy instances are loaded in a single DAW project.

#### Update Strategy
- **Application Updates:** Handled via standard OS package installers. In-app auto-updating for the core plugin is deferred to ensure DAW stability.
- **Content Updates:** Presets are downloaded/managed via the external community hub.

#### Offline Capabilities
- **Core DSP:** The physics engine, snapshot recall, and manual XY morphing must function flawlessly without an active internet connection.
- **Graceful AI Degradation:** If the MCP server cannot reach an external LLM, the "Discovery Mode" UI must gracefully disable itself while keeping all core audio processing intact.

## Functional Requirements

### Plugin Hosting & Parameter Management

- FR1: The system can host third-party VST3 and AU plugins.
- FR2: Users can view the GUI of hosted plugins.
- FR3: Users can map parameters from hosted plugins to the internal morphing engine.
- FR4: The system can track the state of all mapped parameters.

### Snapshot & Bank Management (Local)

- FR5: Users can save the current state of all mapped parameters to one of 12 snapshot slots.
- FR6: Users can recall a snapshot by selecting its slot.
- FR7: Users can clear or overwrite existing snapshot slots.
- FR8: Users can save the entire plugin state (including hosted plugins, mappings, and snapshots) as a `.morphy` bank file to the local file system.
- FR9: Users can load `.morphy` bank files from the local file system.

### XY Morphing Engine

- FR10: Users can manually manipulate an XY proxy control to interpolate between populated snapshot slots.
- FR11: The system can calculate real-time parameter values based on the XY proxy's position relative to the slots.
- FR12: The system can apply the calculated parameter values to the hosted plugins.

### Physics Engine

- FR13: Users can enable physics-based movement on the XY proxy.
- FR14: Users can configure spring and damper dynamics for the XY proxy.
- FR15: Users can apply Perlin noise ("Drift") to the XY proxy's position.
- FR16: The system can autonomously update the XY proxy position based on active physics settings.

### AI Integration (MCP Server)

- FR17: The system runs a local MCP server.
- FR18: The system allows authorized external AI clients to connect to the MCP server.
- FR19: The system can expose current parameter states and plugin metadata via the MCP server.
- FR20: The system can receive and apply XY coordinate recommendations from the connected AI client.
- FR21: Users can gracefully disable the MCP server connectivity ("Discovery Mode" toggle).

### Performance & Workflow

- FR22: Users can map external MIDI controllers to the XY proxy.
- FR23: Users can toggle a "Performance Mode".
- FR24: The system suspends background AI/networking polling tasks when Performance Mode is active.

## Non-Functional Requirements

### Performance
- **Audio Thread CPU Usage:** The XY morphing engine and physics calculations must consume less than 3% CPU overhead on a modern Intel i7 or Apple M1 processor to ensure it doesn't bottleneck the host DAW.
- **Latency:** The internal parameter calculation and distribution from the XY proxy to the 12 snapshot slots must complete within 1 audio processing block (typically < 2.9ms at 44.1kHz / 128 samples).
- **AI Latency:** The MCP server must be capable of processing and returning an XY coordinate recommendation from a local AI client within 500ms.

### Reliability
- **Audio Thread Isolation:** TCP/WebSocket connections and local SQLite storage operations MUST run on separate background threads and must NEVER block or interrupt the primary DAW audio processing thread.
- **Graceful Degradation:** If the MCP server crashes or loses connection to the AI client, the plugin must not crash the host DAW. It must log the error and seamlessly fallback to manual XY control.

### Integration
- **DAW Compatibility:** The plugin must successfully pass validation in Apple Logic Pro (`auval`), Ableton Live, FL Studio, and Steinberg Cubase without crashing upon instantiation or when loading a `.morphy` bank.
- **Sandbox Compliance:** The plugin must strictly adhere to macOS application sandboxing and Windows protected folder rules when reading/writing `.morphy` preset files.
