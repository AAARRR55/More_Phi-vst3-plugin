# UI Wireframes and Interaction Flows
## User Interface Blueprints and User Journey Maps

**Document Version:** 1.0
**Last Updated:** 2026-02-18
**Related:** ui-system-design.md, ui-visual-specifications.md

---

## Table of Contents

1. [Screen Wireframes](#1-screen-wireframes)
2. [Component Wireframes](#2-component-wireframes)
3. [User Flows](#3-user-flows)
4. [State Diagrams](#4-state-diagrams)
5. [Gesture Catalog](#5-gesture-catalog)
6. [Responsive Behaviors](#6-responsive-behaviors)

---

## 1. Screen Wireframes

### 1.1 Main Interface (Full Layout)

```
┌────────────────────────────────────────────────────────────────────────────┐
│  ┌──┐ Morphy v1.0                      [Presets ▼] [Settings] [?] [✕]     │
│  │◆│ ───────────────────────────────────────────────────────────────────  │
│  └──┘                                                                        │
├────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌─────────────────┐  ┌──────────────────────────────────┐  ┌────────────┐ │
│  │  SNAPSHOTS      │  │                                  │  │     AI     │ │
│  │                 │  │        MORPHING PAD              │  │            │ │
│  │  ┌───┐ ┌───┐   │  │                                  │  │   Active ● │ │
│  │  │ 1 │ │ 2 │   │  │                  Y Axis          │  │            │ │
│  │  ├───┤ ├───┤   │  │                  (Timbre)         │  │  [Learn]   │ │
│  │  │ ● │ │   │   │  │                      ▲           │  │            │ │
│  │  ├───┤ ├───┤   │  │                      │           │  │ Suggestions│ │
│  │  │ 3 │ │ 4 │   │  │                      │     ◆     │  │            │ │
│  │  ├───┤ ├───┤   │  │                  ◄───┼───────►    │  │ ┌────────┐ │ │
│  │  │   │ │   │   │  │                      │           │  │ │ 💡     │ │ │
│  │  ├───┤ ├───┤   │  │                  X Axis          │  │ │ Warm   │ │ │
│  │  │ 5 │ │ 6 │   │  │              (Harmonics)          │  │ │ Pad    │ │ │
│  │  ├───┤ ├───┤   │  │                                  │  │ │ 92%    │ │ │
│  │  │   │ │   │   │  │     ●      ●              ●     │  │ └────────┘ │ │
│  │  ├───┤ ├───┤   │  │   (1)    (2)            (3)       │  │            │ │
│  │  │ 7 │ │ 8 │   │  │                                  │  │ [Generate]│ │
│  │  ├───┤ ├───┤   │  └──────────────────────────────────┘  │            │ │
│  │  │   │ │   │   │                                       └────────────┘ │
│  │  └───┘ └───┘   │                                                                        │
│  │                 │                                                                        │
│  └─────────────────┘  ┌────────────────────────────────────────────────────────┐           │
│                       │  MORPH VISUALIZATION                                   │           │
│                       │  ◄───────────────────────────────────────────────────► │           │
│                       │  0:00                                    0:30          │           │
│                       │  ╱────╲        ╱───╲                                   │           │
│                       └────────────────────────────────────────────────────────┘           │
│                                                                              │
│  ┌────────────────────────────────────────────────────────────────────────┐  │
│  │  PARAMETERS                                     [Automate ▼]          │  │
│  │  ┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐          │  │
│  │  │ Cutoff          │ │ Resonance       │ │ Drive           │          │  │
│  │  │ ████████░░░░░░  │ │ ███░░░░░░░░░░░  │ │ ███████████░░░░  │          │  │
│  │  │ 2.4 kHz         │ │ 0.35 Q          │ │ 8.2 dB          │          │  │
│  │  └─────────────────┘ └─────────────────┘ └─────────────────┘          │  │
│  └────────────────────────────────────────────────────────────────────────┘  │
│                                                                              │
├────────────────────────────────────────────────────────────────────────────┤
│  [▶] [■] [◂] [▮▮] [▸] │ 120 BPM │ 1.2.3 │ [↶] [↷] │ Undo │ Redo │ CPU: 12% │
└────────────────────────────────────────────────────────────────────────────┘
```

### 1.2 Compact Layout (600-799px)

```
┌────────────────────────────────────────────┐
│  ≡ Morphy              [Presets ▼] [⚙]    │
├────────────────────────────────────────────┤
│                                            │
│         ┌─────────────────┐               │
│         │                 │               │
│         │   MORPHING PAD  │               │
│         │                 │               │
│         │       ◆         │               │
│         │                 │               │
│         │   ●     ●       │               │
│         │                 │               │
│         └─────────────────┘               │
│                                            │
│  [Snapshots ▼] [AI ▼] [Parameters ▼]      │
│                                            │
│  ┌──────────────────────────────────────┐ │
│  │ Selected: Snapshots                  │ │
│  │ ┌───┐ ┌───┐ ┌───┐ ┌───┐             │ │
│  │ │ 1 │ │ 2 │ │ 3 │ │ 4 │             │ │
│  │ └───┘ └───┘ └───┘ └───┘             │ │
│  └──────────────────────────────────────┘ │
├────────────────────────────────────────────┤
│  [▶] [■] 120 BPM  CPU: 12%                │
└────────────────────────────────────────────┘
```

### 1.3 Minimal Layout (< 600px)

```
┌────────────────────────────┐
│  ≡ Morphy                  │
├────────────────────────────┤
│                            │
│      ╭─────────╮           │
│     ╱           ╲          │
│    │      ◆      │         │
│     ╲           ╱          │
│      ╰─────────╯           │
│                            │
│  [▶] [■] [●] [⚙]           │
└────────────────────────────┘
```

---

## 2. Component Wireframes

### 2.1 Morphing Pad Component

```
┌────────────────────────────────────────────────────────────┐
│  MORPHING PAD                                              │
│  ┌──────────────────────────────────────────────────────┐ │
│  │                                                      │ │
│  │                   Y: Timbre                          │ │
│  │                        ▲                             │ │
│  │                        │                             │ │
│  │                        │                             │ │
│  │                        │         ◆                   │ │
│  │                        │      Current                │ │
│  │                        │      Position               │ │
│  │                ◀───────┼───────▶                     │ │
│  │                    X: Harmonics                      │ │
│  │                                                      │ │
│  │      1             2             3                   │ │
│  │      ●             ●             ●                   │ │
│  │   Bright         Warm          Dark                 │ │
│  │                                                      │ │
│  │   ~~~ AI Suggestion Path ~~~                        │ │
│  │   - - - - - - - - - - - - - -                      │ │
│  │                                                      │ │
│  └──────────────────────────────────────────────────────┘ │
│                                                            │
│  Position: X: 45%  Y: 72%                                 │
│  Mode: [2D ▼]  [Grid ☑]  [Path ☑]                         │
└────────────────────────────────────────────────────────────┘

States:
- Normal: As shown above
- Hover: Grid lines brighten, cursor becomes crosshair
- Dragging: Position indicator follows cursor, trail appears
- Snapshot Active: Corresponding marker glows with ring animation
- AI Suggestion: Dashed circle with pulse animation
```

### 2.2 Snapshot Panel

```
┌────────────────────────────────────────────────────────────┐
│  SNAPSHOTS                                      [+ New]    │
├────────────────────────────────────────────────────────────┤
│  ┌──────────────────────────────────────────────────────┐ │
│  │  ┌────┐  ┌────┐  ┌────┐                             │ │
│  │  │ 1  │  │ 2  │  │ 3  │                             │ │
│  │  │ ●  │  │    │  │    │                             │ │
│  │  │Warm│  │    │  │    │                             │ │
│  │  │Pad │  │Empty│  │Empty│                             │ │
│  │  └────┘  └────┘  └────┘                             │ │
│  │                                                      │ │
│  │  ┌────┐  ┌────┐  ┌────┐                             │ │
│  │  │ 4  │  │ 5  │  │ 6  │                             │ │
│  │  │    │  │    │  │    │                             │ │
│  │  │    │  │    │  │    │                             │ │
│  │  │    │  │    │  │    │                             │ │
│  │  └────┘  └────┘  └────┘                             │ │
│  │                                                      │ │
│  │  ┌────┐  ┌────┐  ┌────┐                             │ │
│  │  │ 7  │  │ 8  │  │ 9  │                             │ │
│  │  │    │  │    │  │    │                             │ │
│  │  │    │  │    │  │    │                             │ │
│  │  │    │  │    │  │    │                             │ │
│  │  └────┘  └────┘  └────┘                             │ │
│  └──────────────────────────────────────────────────────┘ │
│                                                            │
│  Selected: Snapshot 1                                      │
│  [Rename] [Delete] [Export] [Color Tag]                   │
└────────────────────────────────────────────────────────────┘

Slot States:
- Empty: Dashed border, "Empty" label, greyed out
- Occupied: Solid border, name, color tag indicator
- Active: Glowing border, pulsing ring around slot
- Recording: Red border, REC indicator, recording time
```

### 2.3 AI Control Panel

```
┌────────────────────────────────────────────────────────────┐
│  AI CONTROL                                                │
│  ┌──────────────────────────────────────────────────────┐ │
│  │  ┌────────────────────────────────────────────────┐  │ │
│  │  │  🤖 AI Learning Status                         │  │ │
│  │  │                                                 │  │ │
│  │  │  Currently analyzing your playing style...     │  │ │
│  │  │  ████████████░░░░░░░░░░  67%                   │  │ │
│  │  │                                                 │  │ │
│  │  │  [Stop Learning]  [Reset Model]                │  │ │
│  │  └────────────────────────────────────────────────┘  │ │
│  │                                                      │ │
│  │  ┌────────────────────────────────────────────────┐  │ │
│  │  │  Suggestions                                    │  │ │
│  │  ├────────────────────────────────────────────────┤  │ │
│  │  │                                                 │  │ │
│  │  │  ┌──────────────────────────────────────────┐  │  │ │
│  │  │  │ 💡 Warm Atmospheric Pad                  │  │  │ │
│  │  │  │    92% match to your current style       │  │  │ │
│  │  │  │    [Preview] [Apply] [Dismiss]           │  │  │ │
│  │  │  └──────────────────────────────────────────┘  │  │ │
│  │  │                                                 │ │ │
│  │  │  ┌──────────────────────────────────────────┐  │  │ │
│  │  │  │ ✨ Bright Harmonic Sweep                 │  │  │ │
│  │  │  │    Generated from: Snapshot 3           │  │  │ │
│  │  │  │    [Preview] [Apply] [Dismiss]           │  │  │ │
│  │  │  └──────────────────────────────────────────┘  │  │ │
│  │  │                                                 │ │ │
│  │  │  [Generate More Suggestions]                  │  │ │
│  │  └────────────────────────────────────────────────┘  │ │
│  │                                                      │ │
│  │  [Learn] [Generate] [Settings]                       │ │
│  └──────────────────────────────────────────────────────┘ │
└────────────────────────────────────────────────────────────┘

Panel States:
- Idle: No suggestions, "Start learning to get suggestions"
- Learning: Spinning indicator, progress bar
- Active: Shows list of suggestions
- Error: Error message, retry button
```

### 2.4 Parameter Display

```
┌────────────────────────────────────────────────────────────┐
│  PARAMETERS                                    [▲]         │
├────────────────────────────────────────────────────────────┤
│  ┌──────────────────────────────────────────────────────┐ │
│  │  Filter                                              │ │
│  │  ┌────────────────────────────────────────────────┐  │ │
│  │  │ Cutoff Frequency                      [A] [■]  │  │ │
│  │  │ ▓▓▓▓▓▓▓▓▓▓░░░░░░░░░░░░░░░░░░░░░░░░░░         │  │ │
│  │  │ ◆                                    2.45 kHz   │  │ │
│  │  └────────────────────────────────────────────────┘  │ │
│  │  ┌────────────────────────────────────────────────┐  │ │
│  │  │ Resonance                            [A]       │  │ │
│  │  │ ▓▓▓▓░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░         │  │ │
│  │  │ ◆                                    0.35 Q     │  │ │
│  │  └────────────────────────────────────────────────┘  │ │
│  │                                                      │ │
│  │  Drive                                               │ │
│  │  ┌────────────────────────────────────────────────┐  │ │
│  │  │ Amount                               [A] [■]  │  │ │
│  │  │ ▓▓�▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓░░░░░░░░░░░░░░░░░░░         │  │ │
│  │  │ ◆                                    +8.2 dB   │  │ │
│  │  └────────────────────────────────────────────────┘  │ │
│  │                                                      │ │
│  │  Output                                              │ │
│  │  ┌────────────────────────────────────────────────┐  │ │
│  │  │ Mix (Wet/Dry)                       [A] [■]  │  │ │
│  │  │ ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓░░░░░░░░░░░░░         │  │ │
│  │  │ ◆                                    85%       │  │ │
│  │  └────────────────────────────────────────────────┘  │ │
│  └──────────────────────────────────────────────────────┘ │
│                                                            │
│  [Show All] [Hide Automated] [Learn MIDI...]              │
└────────────────────────────────────────────────────────────┘

Legend:
- [A] = Parameter is automated
- [■] = Parameter is locked
- ◆ = Current value indicator
```

### 2.5 Modal Dialogs

```
┌────────────────────────────────────────────────────────────┐
│                      ┌──────────────────────────┐         │
│                      │  Save Preset             │         │
│                      │  ┌────────────────────┐  │         │
│                      │  │ Preset Name        │  │         │
│                      │  │ My Warm Pad        │  │         │
│                      │  └────────────────────┘  │         │
│                      │                          │         │
│                      │  ┌────────────────────┐  │         │
│                      │  │ Category           │  │         │
│                      │  │ Pads ▼             │  │         │
│                      │  └────────────────────┘  │         │
│                      │                          │         │
│                      │  ☑ Include parameters   │         │
│                      │  ☑ Include snapshots    │         │
│                      │  ☐ Include AI model     │         │
│                      │                          │         │
│                      │  [Cancel]  [Save]       │         │
│                      └──────────────────────────┘         │
│                                                            │
│  (Dimmed background with overlay)                          │
└────────────────────────────────────────────────────────────┘
```

---

## 3. User Flows

### 3.1 First Launch Flow

```
┌────────────────────────────────────────────────────────────┐
│  User Flow: First Launch                                   │
└────────────────────────────────────────────────────────────┘

1. Splash Screen
   └─> Shows logo, loading animation (1-2 seconds)
       └─> Welcome dialog appears

2. Welcome Dialog
   ┌──────────────────────────────────────┐
   │  Welcome to Morphy!                  │
   │                                      │
   │  Let's get you started:             │
   │  ☑ Show me a quick tutorial         │
   │  ☐ Load demo presets                │
   │                                      │
   │  [Get Started]  [Skip]              │
   └──────────────────────────────────────┘
   └─> If tutorial selected: Quick Tour
   └─> Otherwise: Main Interface

3. Quick Tour (if selected)
   ┌──────────────────────────────────────┐
   │  ┌────────────────────────────────┐  │
   │  │  This is your Morphing Pad     │  │
   │  │                              │  │
   │  │  Click and drag to move       │  │
   │  │  between timbres              │  │
   │  │                              │  │
   │  │  [Next] [Skip Tour]          │  │
   │  └────────────────────────────────┘  │
   └──────────────────────────────────────┘
   └─> 3-4 steps highlighting key features
       └─> Main Interface with "Ready!" toast

4. Main Interface
   └─> Plugin ready for use
```

### 3.2 Create Snapshot Flow

```
┌────────────────────────────────────────────────────────────┐
│  User Flow: Create Snapshot                                │
└────────────────────────────────────────────────────────────┘

1. User sets desired sound
   └─> Adjusts parameters manually or uses AI suggestions

2. User positions morphing pad
   └─> Clicks and drags to desired position

3. User stores snapshot
   ┌──────────────────────────────────────┐
   │  Method A: Double-click on pad        │
   │  Method B: Ctrl+1-9                  │
   │  Method C: Click empty slot + "Store"│
   └──────────────────────────────────────┘
   └─> Snapshot created dialog appears

4. Snapshot Created Dialog
   ┌──────────────────────────────────────┐
   │  Snapshot Created                    │
   │                                      │
   │  Name: [Snapshot 1          ]        │
   │  Color: [●●●●●●●●●]                 │
   │                                      │
   │  Position saved: X: 45%, Y: 72%     │
   │                                      │
   │  [Just Save]  [Edit Details]        │
   └──────────────────────────────────────┘
   └─> Snapshot appears in panel
```

### 3.3 Morph Between Snapshots Flow

```
┌────────────────────────────────────────────────────────────┐
│  User Flow: Morph Between Snapshots                        │
└────────────────────────────────────────────────────────────┘

1. User clicks snapshot
   └─> Snapshot activates, position moves instantly
       └─> Sound updates to snapshot settings

2. User wants to morph
   └─> Drags position on pad
       └─> Real-time parameter interpolation
           └─> Visual feedback: trail, parameter updates

3. User records morph
   ┌──────────────────────────────────────┐
   │  Click "Record" in footer            │
   │  └─> Transport shows "Recording"     │
   │                                      │
   │  User drags position                 │
   │  └─> Path recorded in timeline       │
   │                                      │
   │  Click "Stop"                        │
   │  └─> Automation saved                │
   └──────────────────────────────────────┘

4. Playback
   └─> Click "Play"
       └─> Morph path replays automatically
```

### 3.4 AI Suggestion Flow

```
┌────────────────────────────────────────────────────────────┐
│  User Flow: AI Suggestion                                   │
└────────────────────────────────────────────────────────────┘

1. User enables AI
   ┌──────────────────────────────────────┐
   │  Click "Learn" in AI panel           │
   │  └─> AI starts analyzing input       │
   │      and current parameters          │
   │                                      │
   │  Progress: ████████░░░░  60%         │
   └──────────────────────────────────────┘

2. AI generates suggestions
   ┌──────────────────────────────────────┐
   │  Suggestions appear in panel:        │
   │                                      │
   │  💡 Warm Atmospheric Pad (92%)       │
   │  ✨ Bright Harmonic Sweep (78%)      │
   │  🌊 Deep Filter Modulation (65%)    │
   │                                      │
   │  Each suggestion shows:              │
   │  - Match percentage                 │
   │  - Preview on hover/click            │
   │  - Apply with double-click          │
   └──────────────────────────────────────┘

3. User previews suggestion
   └─> Click suggestion once
       └─> 5-second audition at suggested position
           └─> Can drag suggestion to reposition

4. User applies suggestion
   └─> Double-click suggestion
       └─> Position moves to suggestion
           └─> Can save as snapshot
```

### 3.5 Automation Flow

```
┌────────────────────────────────────────────────────────────┐
│  User Flow: Record Automation                               │
└────────────────────────────────────────────────────────────┘

1. User prepares for recording
   └─> Sets up DAW for automation recording
       └─> Clicks "Record" in Morphy footer

2. Recording begins
   ┌──────────────────────────────────────┐
   │  ┌────────────────────────────────┐  │
   │  │  ● REC                        │  │
   │  │                               │  │
   │  │  Recording automation...      │  │
   │  │  00:00:05.23                  │  │
   │  └────────────────────────────────┘  │
   │                                      │
   │  User moves position on pad          │
   │  └─> Path drawn in real-time         │
   │  └─> Parameters update              │
   └──────────────────────────────────────┘

3. Recording stops
   └─> Click "Stop" or DAW stops playback
       └─> Automation data appears in timeline
           └─> Can edit keyframes, adjust curves

4. Edit automation
   ┌──────────────────────────────────────┐
   │  Timeline View:                      │
   │  ┌────────────────────────────────┐  │
   │  │ ●────●────────●─────●         │  │
   │  │                              │  │
   │  │ Keyframes can be:            │  │
   │  │ - Dragged to reposition      │  │
   │  │ - Double-click to delete     │  │
   │  │ - Right-click for options    │  │
   │  └────────────────────────────────┘  │
   │                                      │
   │  Curve types:                       │
   │  ○ Linear  ● Smooth  ○ Elastic     │
   └──────────────────────────────────────┘
```

---

## 4. State Diagrams

### 4.1 Morphing Pad States

```
┌────────────────────────────────────────────────────────────┐
│  State Machine: Morphing Pad                               │
└────────────────────────────────────────────────────────────┘

                    ┌──────────┐
                    │  Idle    │
                    └─────┬────┘
                          │ mouse enter / touch start
                    ┌─────▼────┐
              ┌─────│  Hover   │─────┐
              │     └─────┬────┘     │
              │           │          │
              │    mouse down        │  disabled / mouse leave
              │                     │
         ┌────▼─────┐         ┌─────▼────┐
         │ Dragging │         │  Idle    │
         └────┬─────┘         └──────────┘
              │
              │ mouse up / touch end
         ┌────▼─────┐
         │  Hover   │
         └────┬─────┘
              │
              │ mouse leave
         ┌────▼─────┐
         │  Idle    │
         └──────────┘

Recording States:
                    ┌──────────┐
                    │  Idle    │
                    └─────┬────┘
                          │ record button pressed
                    ┌─────▼─────┐
                    │ Recording │
                    └─────┬─────┘
                          │ stop button pressed
                    ┌─────▼─────┐
                    │  Idle    │
                    └──────────┘
```

### 4.2 Snapshot States

```
┌────────────────────────────────────────────────────────────┐
│  State Machine: Snapshot Slot                               │
└────────────────────────────────────────────────────────────┘

                    ┌──────────┐
                    │  Empty   │
                    └─────┬────┘
                          │ double-click / Ctrl+N
                    ┌─────▼─────┐
         ┌──────────│ Recording │──────────┐
         │          └─────┬─────┘          │
         │                │               │
    recording timeout   │             store success
         │                │               │
         │           ┌────▼─────┐         │
         │           │ Occupied │◄────────┘
         │           └────┬─────┘
         │                │
         │                │ clicked (activate)
         │           ┌────▼─────┐
         │           │  Active  │
         │           └────┬─────┘
         │                │
         │                │ another clicked / timeout
         │                │
         └───────────────┴───────┘
                    │
                    │ delete requested
                    ▼
              ┌──────────┐
              │  Empty   │
              └──────────┘
```

### 4.3 AI States

```
┌────────────────────────────────────────────────────────────┐
│  State Machine: AI Control                                  │
└────────────────────────────────────────────────────────────┘

                    ┌──────────┐
                    │   Off    │
                    └─────┬────┘
                          │ learn button pressed
                    ┌─────▼─────┐
                    │ Learning  │
                    └─────┬─────┘
                          │ learning complete / error
                    ┌─────▼─────┐
         ┌──────────│  Active   │──────────┐
         │          └─────┬─────┘          │
    generate request   │              disable / error
         │             │               │
    ┌────▼─────┐   ┌───▼────┐      ┌────▼────┐
    │Generating│   │ Active │      │   Off   │
    └────┬─────┘   └───┬────┘      └─────────┘
         │             │
         │             │ suggestion clicked
         │        ┌────▼─────┐
         │        │Previewing│
         │        └────┬─────┘
         │             │
         │             │ preview end / apply / dismiss
         └─────────────┴─────────────┘
                       │
                       │ disable / error
                       ▼
                 ┌──────────┐
                 │   Off    │
                 └──────────┘
```

---

## 5. Gesture Catalog

### 5.1 Mouse Gestures

```
┌────────────────────────────────────────────────────────────┐
│  Mouse Gesture Reference                                   │
└────────────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────┐
│  Morphing Pad Gestures                                     │
├────────────────────────────────────────────────────────────┤
│                                                             │
│  Click (Empty Area)                                        │
│  ┌────────┐                                                │
│  │   ●    │ → No action (or select nearest marker)        │
│  └────────┘                                                │
│                                                             │
│  Click (Snapshot Marker)                                   │
│  ┌────────┐                                                │
│  │   ●    │ → Activate snapshot, move position            │
│  └────────┘                                                │
│                                                             │
│  Double-Click (Empty Area)                                 │
│  ┌────────┐                                                │
│  │   ●●   │ → Create snapshot at position                 │
│  └────────┘                                                │
│                                                             │
│  Drag (Empty Area)                                         │
│  ┌────────┐                                                │
│  │  ◄────► │ → Move position, morph parameters            │
│  └────────┘                                                │
│                                                             │
│  Drag (Snapshot Marker)                                    │
│  ┌────────┐                                                │
│  │  ◄────► │ → Move snapshot location                     │
│  └────────┘                                                │
│                                                             │
│  Right-Click (Any)                                         │
│  ┌────────┐                                                │
│  │   ⚙    │ → Context menu (rename, delete, color tag)    │
│  └────────┘                                                │
│                                                             │
│  Scroll (3D Mode)                                          │
│  ┌────────┐                                                │
│  │   ↕    │ → Zoom in/out                                 │
│  └────────┘                                                │
│                                                             │
└────────────────────────────────────────────────────────────┘
```

### 5.2 Trackpad Gestures

```
┌────────────────────────────────────────────────────────────┐
│  Trackpad Gesture Reference                                │
└────────────────────────────────────────────────────────────┘

Two-Finger Scroll (Vertical)
  ┌────────┐
  │   ↕    │ → Navigate parameter lists, scroll panels
  └────────┘

Two-Finger Scroll (Horizontal)
  ┌────────┐
  │   ↔    │ → Navigate timeline, switch tabs
  └────────┘

Pinch (Zoom)
  ┌────────┐
  │  ╳     │ → Zoom morphing pad in/out (3D mode)
  └────────┘

Rotate (3D Mode)
  ┌────────┐
  │  ↻     │ → Rotate pad view
  └────────┘

Three-Finger Swipe
  ┌────────┐
  │  →     │ → Navigate history (undo/redo)
  └────────┘
```

### 5.3 Touch Gestures

```
┌────────────────────────────────────────────────────────────┐
│  Touch Gesture Reference (Tablet/Touchscreen)               │
└────────────────────────────────────────────────────────────┘

Tap
  ┌────────┐
  │   ●    │ → Select, activate
  └────────┘

Double-Tap
  ┌────────┐
  │   ●●   │ → Create snapshot, zoom to fit
  └────────┘

Long Press
  ┌────────┐
  │   ●──  │ → Context menu
  └────────┘

Drag (One Finger)
  ┌────────┐
  │  ◄────► │ → Move position, drag items
  └────────┘

Drag (Two Fingers)
  ┌────────┐
  │  ◄────► │ → Pan view (3D mode)
  └────────┘

Pinch (Two Fingers)
  ┌────────┐
  │  ╳     │ → Zoom in/out
  └────────┘

Swipe (Two Fingers)
  ┌────────┐
  │  →     │ → Navigate pages, undo
  └────────┘
```

---

## 6. Responsive Behaviors

### 6.1 Layout Transitions

```
┌────────────────────────────────────────────────────────────┐
│  Responsive Behavior Animations                            │
└────────────────────────────────────────────────────────────┘

Full → Medium (1200px → 800px)
┌────────────────────────────────────────────────────────────┐
│  BEFORE:              AFTER:                               │
│  ┌────┐ ┌────┐ ┌────┐ │  ┌────┐ ┌─────────────────┐       │
│  │Snap│ │ Pad │ │ AI │ │  │Snap│ │      Pad        │       │
│  └────┘ └────┘ └────┘ │  │[▽] │ │                 │       │
│                      │  └────┘ └─────────────────┘       │
│                      │  AI becomes collapsible top bar     │
└────────────────────────────────────────────────────────────┘

Medium → Compact (800px → 600px)
┌────────────────────────────────────────────────────────────┐
│  BEFORE:              AFTER:                               │
│  ┌────┐ ┌───────────┐│  ┌─────────────────┐               │
│  │Snap│ │    Pad    ││  │      Pad        │               │
│  │[▽] │ │           ││  │                 │               │
│  └────┘ └───────────┘│  └─────────────────┘               │
│                      │  [Snapshots ▼] [AI ▼]              │
│                      │  Panels become tabbed               │
└────────────────────────────────────────────────────────────┘

Compact → Minimal (< 600px)
┌────────────────────────────────────────────────────────────┐
│  BEFORE:              AFTER:                               │
│  ┌─────────────┐     │  ┌───────┐                          │
│  │     Pad     │     │  │  Pad  │                          │
│  │             │     │  └───────┘                          │
│  │             │     │  [≡] Menu for all panels            │
│  └─────────────┘     │                                     │
└────────────────────────────────────────────────────────────┘
```

### 6.2 Component Scaling

```
Morphing Pad Scaling:
- Base size: 500×500px (full layout)
- Medium: 400×400px
- Compact: 300×300px
- Minimal: 250×250px
- Maintains aspect ratio
- Internal elements scale proportionally

Button Scaling:
- Full: 40px height, 16px padding
- Medium: 40px height, 12px padding
- Compact: 36px height, 12px padding
- Minimal: 32px height, 8px padding
- Icon-only mode below 400px width

Text Scaling:
- Full: 16px body, 24px headings
- Medium: 16px body, 20px headings
- Compact: 14px body, 18px headings
- Minimal: 14px body, 16px headings
- Abbreviated labels in compact modes
```

---

## Appendix: Quick Reference

### Keyboard Shortcuts Summary

```
Navigation:
  Tab/Shift+Tab      Navigate focus
  Arrow Keys         Navigate within component
  Home/End           First/last item
  Page Up/Down       Page navigation

Actions:
  Enter/Space        Activate
  Escape             Close/Cancel
  Delete             Delete focused item

Morphing:
  1-9                Recall snapshot
  Ctrl+1-9           Store snapshot
  Ctrl+R             Toggle record
  A/S/V/M            Toggle panels

Edit:
  Ctrl+Z/Y           Undo/Redo
  Ctrl+S             Save preset
  Ctrl+,             Settings
```

### Color Quick Reference

```
Brand:
  Primary (Cyan)     #00d4ff
  Secondary (Orange) #ff6b35

Semantic:
  Success            #2ed573
  Warning            #ffa502
  Error              #ff4757

Backgrounds (Dark):
  Base               #1a1a2e
  Surface            #16213e
  Elevated           #232342

Text:
  Primary            #ffffff
  Secondary          #a0a0b0
  Disabled           #ffffff40
```

---

**Document End**

This document provides the visual blueprint for implementing the Morphy plugin interface. For technical implementation details, refer to `ui-system-design.md` and `ui-visual-specifications.md`.
