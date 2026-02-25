---
stepsCompleted: [1, 2, 3, 4, 5, 6]
inputDocuments:
  - docs/snappy-snap-vst-plugin-research.md
  - docs/SNAPPY_SNAP_RESEARCH.md
  - docs/USER_GUIDE.md
  - docs/MASTER_ARCHITECTURAL_BLUEPRINT.md
date: 2026-02-24
author: 20190
status: complete
---

# Product Brief: morphy

## Executive Summary

**Morphy** is the world's first AI-native, community-powered sound exploration platform. Unlike static preset management or expensive closed-source alternatives, Morphy enables real-time parameter morphing between up to 12 plugin states using an intuitive XY pad, with AI-guided sound discovery that helps producers explore "the sonic territories between presets." As the first audio plugin with native MCP (Model Context Protocol) integration, Morphy bridges AI assistants with music production workflows, enabling natural language sound design: "make this darker" becomes intelligent parameter morphing.

---

## Core Vision

### Problem Statement

Music producers and live performers face a fundamental limitation: **static plugins create static sounds**. While modern synthesizers and effects offer thousands of parameters, exploring the infinite sonic possibilities between preset states requires:
- Hours of manual parameter adjustment (time-consuming and imprecise)
- Preset loading (instant but jarring, no intermediate states)
- Complex automation programming (tedious to create, impossible to perform live)

There's no elegant way to **continuously explore sonic territories between states** in real-time, especially during live performances.

### Problem Impact

- **Live performers** cannot expressively transition between sounds mid-performance
- **Sound designers** waste hours manually finding "in-between" sounds that could be discovered through AI-guided exploration
- **Ambient/drone producers** struggle to create evolving textures without complex automation
- **AI-assisted workflows** lack direct control over plugin parameters through natural language

### Why Existing Solutions Fall Short

| Solution | Limitation |
|----------|------------|
| DAW Automation | Time-consuming to program; not performable in real-time |
| Hardware Controllers | Limited to MIDI CC; cannot morph multiple parameters simultaneously |
| Blue Cat PatchWork ($199) | No 2D morphing surface; no AI integration |
| DDMF Metaplugin ($49) | Hosts plugins but only static switching |
| **SnappySnap ($55)** | Closed source; NO AI integration (key gap) |

### Proposed Solution

Morphy provides a **meta-plugin architecture** that:
1. Hosts any VST3/AU plugin within its open-source environment
2. Captures complete parameter states as snapshots (up to 12 slots in clock layout)
3. Enables real-time morphing via XY pad with distance-weighted interpolation
4. Applies physics simulations (spring/damper, Perlin noise) for organic movement
5. Integrates AI agents via MCP for conversational sound design
6. Builds community network effects through preset sharing and crowdsourced AI models
7. Offers all features free under MIT license

### Key Differentiators

1. **AI-Native Pioneer** - World's first audio plugin with MCP server integration for conversational AI control
2. **Community Moat** - Network effects via preset sharing, community-trained models, plugin compatibility database
3. **AI-Guided Discovery** - "Find me sounds between these presets" - AI explores parameter space and surfaces discoveries
4. **Open Source** - MIT licensed vs. commercial alternatives; transparent, extensible, community-driven
5. **Physics Engine** - Elastic and Drift modes for autonomous sound evolution
6. **12-Slot Architecture** - Clock-layout XY pad enables intuitive 2D interpolation vs. linear preset chains

---

## Target Users

### Primary Users

#### 1. Morgan - The AI Early Adopter
**Profile:** Tech-forward producer who already uses Claude Desktop, experiments with AI stem separation, and follows AI audio developments on Twitter/Product Hunt.

**Current Pain:** Context-switching between DAW, plugin interfaces, and AI chat. No audio plugins accept conversational control.

**What They Need:** Native MCP integration for natural language sound design. Immediate gratification - "make this darker" → parameters morph.

**Success Looks Like:** Completes full production session via voice commands without touching the plugin UI. Shares workflow screenshots on social media.

**Strategic Role:** Launch target. Builds community, generates buzz, forgives early bugs. **Adoption effort: LOW, Value: MEDIUM**

---

#### 2. Jordan - The Sound Designer
**Profile:** Creates custom sounds for games, films, sample libraries. Power user of Serum, Massive, Phase Plant. Spends hours in plugin parameters.

**Current Pain:** Finding "in-between" sounds requires manually adjusting dozens of parameters. The sonic space between two good presets is unexplored.

**What They Need:** AI-guided discovery of hybrid sounds. Breeding system for genetic variation. Reliable state capture and recall.

**Success Looks Like:** Discovers "impossible" hybrid sound between two favorites using AI exploration. Shares discoveries with community.

**Strategic Role:** Revenue target. Willing to pay for pro features. Needs reliability SLAs. **Adoption effort: MEDIUM, Value: HIGH**

---

#### 3. Alex - The Live Performer
**Profile:** Tours clubs/festivals with Ableton/Bitwig + MIDI controller setup. Needs bulletproof reliability.

**Current Pain:** Switching sounds kills energy. No expressive way to transition between drastically different patches live.

**What They Need:** Seamless crossfades, MIDI-controllable XY pad, **Performance Mode** (bulletproof reliability).

**Success Looks Like:** Flows from deep bass to shimmering lead via XY pad gesture - crowd feels the journey, not the jump.

**Strategic Role:** Credibility target. High stakes = high praise if delivered. **Adoption effort: HIGH, Value: VERY HIGH**

---

#### 4. Taylor - The Ambient Explorer
**Profile:** Creates evolving soundscapes and drone music. Uses complex effects chains. Wants sounds that breathe.

**Current Pain:** Creating evolving textures requires programming complex automation or manually riding faders.

**What They Need:** Drift mode (Perlin noise), autonomous sound evolution, **Discovery Mode** (experimental features).

**Success Looks Like:** Runs generative sessions overnight, wakes up to hours of unique evolving audio.

**Strategic Role:** Community target. Highly engaged, creates tutorial content. **Adoption effort: LOW, Value: MEDIUM**

---

### Secondary Users

#### Rookie Riley - The Curious Beginner
**Profile:** Bedroom producer who found morphy through YouTube. Just bought first controller. Terrified of complex plugins.

**What They Need:** Guided workflows, preset-first approach, "What does this knob do?" tooltips.

**Strategic Role:** **Invited but not primary target.** Progressive disclosure reveals depth as they grow. Today's Riley is tomorrow's Jordan.

---

### User Journey Map

#### Morgan (Launch Priority)
| Stage | Experience |
|-------|------------|
| **Discovery** | Hears about MCP integration on AI/audio Twitter |
| **Onboarding** | Configures Claude Desktop, tests first voice command in 5 minutes |
| **Core Usage** | Controls sessions via voice, barely touches plugin UI |
| **Success Moment** | First complete production session without mouse |
| **Long-term** | Builds custom MCP tools, shares workflows, community evangelist |

#### Jordan (Revenue Priority)
| Stage | Experience |
|-------|------------|
| **Discovery** | Finds on GitHub while searching "open source alternative to SnappySnap" |
| **Onboarding** | Builds from source, explores API, reads architecture docs |
| **Core Usage** | Breeds presets daily, contributes discoveries to community |
| **Success Moment** | Discovers hybrid sound AI found that they never would have manually |
| **Long-term** | Contributes to open source, builds plugin compatibility database |

#### Alex (Reliability Priority)
| Stage | Experience |
|-------|------------|
| **Discovery** | Sees demo video of XY pad morphing live on YouTube |
| **Onboarding** | Downloads LE version, creates 3 snapshots, tests in practice session |
| **Core Usage** | Maps XY pad to MIDI controller, uses live in gigs (Performance Mode ON) |
| **Success Moment** | First seamless live transition - crowd reaction confirms it works |
| **Long-term** | Evangelist in performer community, shares preset banks |

---

### User Reliability Framework

| Mode | Target User | Characteristics |
|------|-------------|-----------------|
| **Performance Mode** | Alex, Jordan (client work) | Bulletproof reliability, limited features, zero crashes |
| **Discovery Mode** | Morgan, Taylor | Experimental features, AI integration, occasional instability accepted |

### Strategic User Prioritization

```
Launch Sequence:
┌─────────────────────────────────────────────────────┐
│  Phase 1: Morgan (AI Early Adopters)               │
│  → Build community, GitHub stars, organic growth   │
├─────────────────────────────────────────────────────┤
│  Phase 2: Jordan (Sound Designers)                 │
│  → Revenue stream, pro features, reliability SLAs  │
├─────────────────────────────────────────────────────┤
│  Phase 3: Alex (Live Performers)                   │
│  → Credibility, Performance Mode hardened          │
├─────────────────────────────────────────────────────┤
│  Phase 4: Taylor (Ambient) + Riley (Beginners)     │
│  → Scale, community network effects                │
└─────────────────────────────────────────────────────┘
```

### Deliberately Ceded Segments (v1.0)
- **EDM/Drop producers** (~20% market) - Positioning too subtle for aggressive transitions
- **Film composers post-production** - No batch processing workflows yet
- **Podcast producers** - May be over-engineered for simple voice effects

---

## Success Metrics

### User Success Metrics

#### What Makes Morgan Say "The AI Integration Was Worth It"

| Success Signal | Why It Matters |
|----------------|----------------|
| "I automated my Serum patch transition in 30 seconds" | AI understands which parameters matter out of thousands, reducing cognitive load |
| "My template sessions load 3x faster" | Learn Mode exposes only relevant parameters, decluttering project files |
| "I trust the token counter—no surprise API bills" | Transparent cost estimation removes anxiety of AI-assisted workflows |
| First morph completed within 2 minutes of plugin load | Minimal friction from install to first "aha" moment |

#### What Makes Jordan Say "I'm Discovering Sounds I Never Could Before"

| Success Signal | Why It Matters |
|----------------|----------------|
| "The breeding feature gave me a sound I couldn't have designed manually" | Genetic algorithm creates emergent, unexpected sonic territories |
| "Drift mode makes my ambient tracks alive" | Perlin noise physics enables organic, evolving textures without manual automation |
| "I found the 'sweet spot' between 4 snapshots I didn't know existed" | 2D XY interpolation reveals hybrid sounds between saved states |
| "I capture ideas faster than I can think" | Double-click to snapshot removes creative friction |

#### What Makes Alex Say "I Trust This on Stage"

| Success Signal | Why It Matters |
|----------------|----------------|
| "Zero dropouts during 2-hour sets" | Lock-free architecture guarantees real-time stability |
| "MIDI snapshot triggering is sample-accurate" | Reliable performance under pressure |
| "The elastic physics feel musical, not robotic" | Spring-damper interpolation responds like an instrument |
| "I can morph without looking at the screen" | Muscle-memory friendly XY pad + MIDI CC control |

---

### Business Objectives

For an open-source audio plugin, "business success" is ecosystem gravity:

| Metric Tier | Target | Rationale |
|-------------|--------|-----------|
| 🌟 GitHub Stars | 1,000+ in Year 1 | Signals credibility to audio developers |
| 🧩 Plugin Preset Shares | 10,000+ presets on patchstorage.com | Community-created content attracts users |
| 🎥 Tutorial Videos | 50+ community tutorials | Organic education reduces support burden |
| 🐛 Issue Resolution | < 7 days average | Healthy maintainer responsiveness |
| 💬 Discord/Forum Active Users | 500+ weekly active | Community becomes self-sustaining |

#### Commercialization Paths

| Path | Model | Example |
|------|-------|---------|
| Pro Tier | One-time purchase for advanced features | More snapshots (>12), custom physics curves, VST2 support |
| Sponsorship | GitHub Sponsors + Open Collective | Companies like Ableton, iZotope, or AI providers sponsor |
| Preset Marketplace | Revenue share on curated preset packs | "Morphy Pro Sound Designer Collection" |
| Consulting | Custom MCP integrations for studios | Enterprise AI workflow consulting |

---

### Key Performance Indicators

#### Core Metrics We Can Track

| Category | Metric | Target | How Measured |
|----------|--------|--------|--------------|
| Activation | Time to first morph | < 2 minutes | In-plugin telemetry (opt-in) |
| Engagement | Snapshots per session | 8+ average | Session analytics |
| Retention | Weekly active users | 60%+ Month 2 | Unique plugin instances |
| AI Usage | MCP commands per session | 15+ | Server request logs |
| Satisfaction | NPS score | > 50 | In-plugin survey |

#### AI-Assisted Sound Design Effectiveness

| Proxy Metric | What It Tells Us |
|--------------|------------------|
| Learn Mode adoption rate | Users trust AI parameter filtering |
| Token budget compliance | Users find cost tracking valuable |
| Morph compatibility checks | Users rely on AI guidance for smooth transitions |
| Parameter adjustment via AI vs manual | Ratio of AI-assisted vs direct control |

#### The "Aha!" Moment to Optimize For

> "The first time a user morphs between 3+ snapshots and hears a sound that didn't exist in any of them"

This is the core value proposition - emergent sound design through interpolation. Track:
- Time from install to first 3+ snapshot morph
- % of users who save a "morphed" sound as a new preset

---

### Leading Indicators

#### Early Engagement Signals (Week 1)

| Signal | Indicator |
|--------|-----------|
| User captures ≥5 snapshots | They've moved past "testing" to "using" |
| User enables MCP server | Interested in AI integration |
| User experiments with all 3 physics modes | Exploring the full feature set |
| User triggers ≥1 snapshot via MIDI | Performance-oriented use case |

#### Retention Predictors (Month 1)

| Signal | Predicts |
|--------|----------|
| Created ≥3 custom presets | Long-term user |
| Used breeding feature ≥5 times | Creative exploration mindset |
| Adjusted token budget | Cost-conscious power user |
| Loaded hosted plugin with 1000+ parameters | Professional use case |

#### Red Flags (Churn Risk)

| Signal | Intervention |
|--------|--------------|
| Zero snapshots after 3 sessions | Tutorial/onboarding failure |
| MCP enabled but zero commands | AI integration too complex |
| Only uses Direct physics | Missing value of unique features |
| Uninstalls within 48 hours | Installation or DAW compatibility issue |

---

### Success Dashboard Concept

```
┌─────────────────────────────────────────────────────────────┐
│  MORPHY SUCCESS DASHBOARD                                   │
├─────────────────────────────────────────────────────────────┤
│  NORTH STAR: Sounds Discovered (unique morphed presets)     │
│  └── Target: 10,000/month across all users                  │
├─────────────────────────────────────────────────────────────┤
│  USER SUCCESS                               │ BUSINESS       │
│  ├── Avg. Time to First Morph: 1m 42s ✅    │  ├── Stars: 847│
│  ├── Snapshots/Session: 6.3 ⚠️              │  ├── Presets: 4.2k
│  ├── AI Commands/Session: 23 ✅             │  ├── Issues: 12│
│  └── Retention (M2): 58% ⚠️                 │  └── Sponsors: 3│
├─────────────────────────────────────────────────────────────┤
│  SEGMENT HEALTH                                             │
│  ├── Morgan: 72% retention, $0.02 avg token cost ✅         │
│  ├── Jordan: 45% retention, high breeding usage ⚠️          │
│  └── Alex: 81% retention, MIDI-heavy usage ✅               │
└─────────────────────────────────────────────────────────────┘
```

---

### Ultimate Success Definition

Success for morphy isn't just adoption - it's **changing how sound designers think about parameter spaces**:

1. **Before morphy:** Parameters are static knobs to tweak
2. **After morphy:** Parameters are a continuous landscape to explore

Success = when users stop asking "what value should this knob be?" and start asking **"what's the journey between these 4 sonic states?"**

The ultimate validation? When Serum includes native morphing because the community expectation changed. We're not just building a plugin - we're proving a paradigm.

---

## MVP Scope

### Core Features (The "Aha" Moment)

The MVP delivers one core experience:
> **User loads Serum → captures 4 snapshots → morphs between them on XY pad → hears emergent sound**

| Feature | MVP Status | Rationale |
|---------|------------|-----------|
| **4 Snapshots (not 12)** | ✅ MUST | Reduces UI complexity, enough for "aha" |
| **XY Pad morphing** | ✅ MUST | Core differentiator |
| **Basic MCP (3 tools)** | ✅ MUST | set_parameter, capture_snapshot, set_morph_position |
| **Direct physics only** | ✅ MUST | Elastic/Drift deferred to v0.2 |
| **VST3 plugin hosting** | ✅ MUST | No AU for MVP |
| **MIDI note triggers (C3-F3)** | ✅ MUST | Basic performance control |

---

### Out of Scope for MVP (The Cut List)

| Feature | Status | Rationale |
|---------|--------|-----------|
| **12→4 snapshots** | 🗑️ CUT | UI simpler, still delivers "aha" |
| **Learn Mode** | 🗑️ CUT | Manual parameter selection works |
| **Token Optimizer** | 🗑️ CUT | Hard-code 50 param limit |
| **Genetic Breeding** | 🗑️ CUT | Cool but not core to morphing |
| **Elastic/Drift physics** | 🗑️ CUT | Direct mode sufficient |
| **AU format** | 🗑️ CUT | VST3 covers most users |
| **Preset Manager (16×128)** | 🗑️ CUT | DAW presets sufficient |
| **Link Mode** | 🗑️ CUT | Single instance focus |
| **Discrete Parameter Handler** | 🗑️ CUT | Document limitation |
| **MCPToolsExtended (17 tools)** | 🗑️ CUT | 3 basic tools sufficient |

---

### MVP Success Criteria

**Morgan's "First 5 Minutes" Experience:**

| Minute | Experience |
|--------|------------|
| 1 | Install plugin, load in DAW |
| 2 | Click "Load Plugin", select Serum |
| 3 | Tweak Serum knobs, double-click snapshot 1 |
| 4 | Tweak more, capture snapshots 2, 3, 4 |
| 5 | Drag XY pad → **HEAR THE MORPH** → "aha!" |

**MVP Validation Metrics:**
- Time to first morph: < 2 minutes
- MCP response time: < 100ms
- Plugin load success rate: > 90% (VST3)
- Crash-free sessions: > 95%

---

### The 30-Day MVP Timeline

#### Week 1: Fix the Foundation (Days 1-7)

| Fix | Effort |
|-----|--------|
| ParameterClassifier - add missing includes, simplify to name-matching | 4 hours |
| IPluginHostManager - add missing interface methods | 2 hours |
| TokenOptimizer - fix const-correctness or DELETE for MVP | 4 hours |
| MCPToolsExtended - DELETE, use only MCPToolHandler (3 tools) | 8 hours |

#### Week 2: Core Functionality (Days 8-14)

| Task | Effort |
|------|--------|
| SnapshotBank: Reduce from 12 to 4 slots | 2 hours |
| MorphPad: Ensure XY interpolation works | 8 hours |
| ParameterBridge: Test with actual VST3 | 8 hours |
| PluginHostManager: Load/unload reliability | 8 hours |

#### Week 3: Integration & Testing (Days 15-21)

| Task | Effort |
|------|--------|
| MCP basic tool testing | 8 hours |
| MIDI snapshot triggers (notes C3-F3) | 4 hours |
| UI polish: basic but functional | 16 hours |

#### Week 4: DAW Validation (Days 22-30)

| Task | Effort |
|------|--------|
| Test in Ableton Live | 8 hours |
| Test in FL Studio | 8 hours |
| Documentation & release | 16 hours |

---

### Future Vision

#### v0.2 (Morgan + Jordan Focus)
- 12 snapshots
- Genetic breeding
- Elastic physics mode
- Token optimizer
- Learn Mode v1

#### v0.3 (Alex Focus - Performance Mode)
- Bulletproof reliability
- Drift physics
- Enhanced MIDI control
- AU format support

#### v1.0 (Full Product Vision)
- Link Mode (multi-instance sync)
- Preset Manager (16 banks × 128 presets)
- Discrete Parameter Handler
- Full MCP tool suite (26 tools)
- Community preset marketplace

#### v2.0+ (Paradigm Shift)
- AI-guided sound discovery
- Community-trained models
- Infinite snapshot architecture
- Cloud AI services
- Native integration with major DAWs

---

### The "v0.1 Morgan Would Tweet About"

**Minimum Viable Tweet:**
> "Just tried Morphy v0.1 — loaded my Serum patch, captured 4 variations, and morphed between them on an XY pad. Heard sounds I've never designed intentionally. The MCP integration means I can automate this with Python. This is going in my template."

**What's In:**
- ✅ Load VST3 plugin
- ✅ 4-snapshot capture (double-click MorphPad)
- ✅ XY morphing with inverse-distance weighting
- ✅ MCP: set_parameter, set_morph_position, capture_snapshot
- ✅ MIDI: Note triggers for snapshots 1-4

**What's Out:**
- 🗑️ Learn Mode
- 🗑️ Token tracking
- 🗑️ Genetic breeding
- 🗑️ Elastic/Drift physics
- 🗑️ Preset banks
- 🗑️ Multi-instance sync
- 🗑️ Discrete parameter handling

---

### Immediate Action Plan

**This Week:**
1. Create `mvp` branch
2. Remove broken components:
   - Delete MCPToolsExtended.cpp/h (or stub out)
   - Delete TokenOptimizer.cpp/h (or stub out)
   - Simplify ParameterClassifier to basic name-matching only
3. Fix compilation errors (~16 hours)
4. Get a working build — even if feature-light

**Then (Weeks 2-4):**
5. Test with 3-5 real VST3 plugins
6. Validate in 2-3 DAWs
7. Ship v0.1-alpha to early testers
