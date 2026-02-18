# UI Visual Specifications
## Design Assets, Icons, and Visual Elements

**Document Version:** 1.0
**Last Updated:** 2026-02-18
**Related:** ui-system-design.md

---

## Table of Contents

1. [Color Palette](#1-color-palette)
2. [Typography System](#2-typography-system)
3. [Icon Library](#3-icon-library)
4. [Component Styling](#4-component-styling)
5. [Animation Specifications](#5-animation-specifications)
6. [Layout Grid System](#6-layout-grid-system)
7. [Visual Assets](#7-visual-assets)

---

## 1. Color Palette

### 1.1 Primary Color System

```
┌─────────────────────────────────────────────────────────────┐
│  Semantic Color Mapping                                     │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Brand/Accent:     #00d4ff  (Cyan)    RGB(0, 212, 255)     │
│  Primary Action:   #00d4ff  (Cyan)    HSL(192, 100%, 50%)  │
│  Secondary:        #ff6b35  (Orange)  RGB(255, 107, 53)    │
│  Success:          #2ed573  (Green)   RGB(46, 213, 115)    │
│  Warning:          #ffa502  (Yellow)  RGB(255, 165, 2)     │
│  Error:            #ff4757  (Red)     RGB(255, 71, 87)     │
│  Info:             #70a1ff  (Blue)    RGB(112, 161, 255)   │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 1.2 Neutral Colors (Dark Theme)

```
┌─────────────────────────────────────────────────────────────┐
│  Background/Surface System                                 │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Background Base:     #1a1a2e  (Very Dark Navy)            │
│  Background Elevated: #232342  (Dark Navy)                 │
│  Surface:            #16213e  (Dark Blue-Grey)             │
│  Surface Variant:    #1f294a  (Lighter Blue-Grey)          │
│  Surface Highlight:  #2a3558  (Border/Divider)             │
│                                                             │
│  Text Primary:       #ffffff  (White)                      │
│  Text Secondary:     #a0a0b0  (Light Grey)                 │
│  Text Tertiary:      #606070  (Medium Grey)                │
│  Text Disabled:      #ffffff40 (White @ 25% opacity)       │
│                                                             │
│  Border Subtle:      #ffffff08 (White @ 3% opacity)        │
│  Border Normal:      #ffffff15 (White @ 8% opacity)        │
│  Border Strong:      #ffffff30 (White @ 19% opacity)       │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 1.3 Neutral Colors (Light Theme)

```
Background Base:     #f5f5f7  (Very Light Grey)
Background Elevated: #ffffff  (White)
Surface:            #ffffff  (White)
Surface Variant:     #ebebed  (Light Grey)
Surface Highlight:  #d8d8dc  (Border/Divider)

Text Primary:       #1d1d1f  (Near Black)
Text Secondary:     #86868b  (Medium Grey)
Text Tertiary:      #aeaeb2  (Light Grey)
Text Disabled:      #1d1d1f40 (Black @ 25% opacity)

Border Subtle:      #00000008 (Black @ 3% opacity)
Border Normal:      #00000015 (Black @ 8% opacity)
Border Strong:      #00000030 (Black @ 19% opacity)
```

### 1.4 Semantic Color Applications

| Usage | Color | Notes |
|-------|-------|-------|
| **Current Position** | #00d4ff | Cyan, glow effect |
| **Snapshot Active** | #ff6b35 | Orange, pulsing ring |
| **Snapshot Normal** | User-defined | 8 preset options |
| **AI Suggestion** | #00ff88 | Green, dashed, pulsing |
| **Recording** | #ff4757 | Red, flashing indicator |
| **Automation Active** | #70a1ff | Blue, path overlay |
| **Focus Ring** | #00d4ff | Cyan, 3px outline |
| **Hover State** | +10% lightness | All interactive elements |
| **Disabled** | 40% opacity | All states |

### 1.5 Snapshot Color Tags

```
Preset Options:
1.  #ff6b6b  (Coral Red)
2.  #feca57  (Warm Yellow)
3.  #48dbfb  (Sky Blue)
4.  #1dd1a1  (Mint Green)
5.  #5f27cd  (Deep Purple)
6.  #ff9ff3  (Pink)
7.  #54a0ff  (Royal Blue)
8.  #00d2d3  (Teal)
9.  #ff9f43  (Orange)

Custom: HSL color picker, auto-contrast text
```

### 1.6 Gradient Definitions

```
Background Gradient (Pad):
  Radial from center
  Inner: #232342
  Outer: #1a1a2e
  Stop at 70%

Position Glow:
  Radial from point
  Center: #00d4ff @ 60%
  Edge: #00d4ff @ 0%
  Radius: 40px

Morph Path:
  Linear along path
  Start: #ff6b35 (source snapshot color)
  End: #00d4ff (current position color)
  Midpoint: Weighted blend

Button Press:
  Radial from click point
  Duration: 300ms
  Color: White @ 20% → Transparent
```

---

## 2. Typography System

### 2.1 Font Families

```
Primary Font:    Inter
Fallbacks:       -apple-system, BlinkMacSystemFont, "Segoe UI",
                 Roboto, Helvetica, Arial, sans-serif
Monospace Font:  "SF Mono", "Cascadia Code", "Fira Code",
                 "Roboto Mono", monospace

Font Loading:    System fonts (no web loading required)
Variants:        Regular (400), Medium (500), Semibold (600),
                 Bold (700)
```

### 2.2 Type Scale

```
┌─────────────────────────────────────────────────────────────┐
│  Typography Scale (1.5 ratio, Major Third)                 │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Display Large:    32px  (2.0rem)   Plugin title, hero     │
│  Display:          24px  (1.5rem)   Section headers        │
│  Heading:          20px  (1.25rem)  Subsection headers     │
│  Subheading:       18px  (1.125rem) Component titles       │
│  Body:             16px  (1.0rem)   Main content           │
│  Body Small:       14px  (0.875rem) Secondary content      │
│  Caption:          12px  (0.75rem)  Labels, metadata       │
│  Micro:            10px  (0.625rem) Fine print             │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 2.3 Font Weights

```
Regular (400):     Body text, descriptions
Medium (500):      Emphasized content, button text
Semibold (600):    Headers, labels, important values
Bold (700):        Display text, numbers, critical info

Never use:         Light (300), ExtraLight (200), Thin (100)
                   (accessibility concerns)
```

### 2.4 Line Heights

```
Display:  1.1  (tight for large text)
Heading:  1.2  (slightly loose)
Body:     1.5  (readable paragraphs)
Caption:  1.4  (balanced for small text)
```

### 2.5 Letter Spacing

```
Display/Large:  -0.02em  (optically compensated)
Heading:        0em      (normal)
Body:           0em      (normal)
Small/Caption: 0.01em   (slightly expanded for readability)
All Caps:       0.05em   (expanded for button text, etc.)
```

### 2.6 Text Style Examples

```
┌─────────────────────────────────────────────────────────────┐
│  Plugin Title (Display Large)                              │
│  "Morphy" - 32px, Bold, #ffffff                           │
├─────────────────────────────────────────────────────────────┤
│  Section Header (Display)                                  │
│  "Morphing Pad" - 24px, Semibold, #ffffff                 │
├─────────────────────────────────────────────────────────────┤
│  Component Title (Subheading)                              │
│  "Snapshot 1: Warm Pad" - 18px, Medium, #a0a0b0           │
├─────────────────────────────────────────────────────────────┤
│  Body Text                                                  │
│  "Click and drag to morph between snapshots."              │
│  14px, Regular, #a0a0b0, 1.5 line height                  │
├─────────────────────────────────────────────────────────────┤
│  Caption/Label                                             │
│  "CUTOFF FREQUENCY" - 12px, Semibold, #606070, all caps   │
├─────────────────────────────────────────────────────────────┤
│  Value Display                                              │
│  "2.4 kHz" - 16px, Medium, #ffffff, monospace for numbers │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. Icon Library

### 3.1 Icon Design Principles

```
Style:           Outline (stroke-based), 2px stroke weight
Grid:            24×24px base grid
Corner Radius:   2px (for rounded joins)
Stroke Caps:     Round (for friendly, modern look)
Stroke Joins:    Round
Fill Style:      None (outline only), or solid for small icons
```

### 3.2 Icon Set - Transport

```
┌─────────────────────────────────────────────────────────────┐
│  Play                                                       │
│  ┌────────────────┐                                        │
│  │     ●───────►  │  Triangle pointing right              │
│  └────────────────┘                                        │
│                                                             │
│  Pause                                                      │
│  ┌────────────────┐                                        │
│  │    ||    ||    │  Two vertical bars                    │
│  └────────────────┘                                        │
│                                                             │
│  Stop                                                       │
│  ┌────────────────┐                                        │
│  │   ┌──────┐     │  Square                               │
│  │   │      │     │                                       │
│  │   └──────┘     │                                       │
│  └────────────────┘                                        │
│                                                             │
│  Record                                                     │
│  ┌────────────────┐                                        │
│  │    ●  ●  ●     │  Circle                               │
│  │   ●      ●     │  (inner filled circle for recording)  │
│  │    ●  ●  ●     │                                       │
│  └────────────────┘                                        │
│                                                             │
│  Loop                                                       │
│  ┌────────────────┐                                        │
│  │   ┌────────┐   │  Rounded rectangle with arrows        │
│  │   └────►───┘   │                                       │
│  └────────────────┘                                        │
└─────────────────────────────────────────────────────────────┘
```

### 3.3 Icon Set - Edit

```
┌─────────────────────────────────────────────────────────────┐
│  Undo          Redo                                        │
│  ┌────────┐   ┌────────┐                                   │
│  │    ◄───│   │───►    │  Curved arrows                   │
│  └────────┘   └────────┘                                   │
│                                                             │
│  Copy          Paste          Delete                       │
│  ┌────────┐   ┌────────┐   ┌────────┐                     │
│  │ ┌────┐ │   │┌──────┐│   │ ┌────┐ │  Two overlapping   │
│  │ └────┘ │   ││      ││   │ │ ███ │ │  pages, clipboard,│
│  │ ┌────┐ │   │└──────┘│   │ └────┘ │  trash can         │
│  │ └────┘ │   └────────┘   └────────┘                     │
│  └────────┘                                                   │
└─────────────────────────────────────────────────────────────┘
```

### 3.4 Icon Set - View

```
┌─────────────────────────────────────────────────────────────┐
│  2D View      3D View      Spectral                        │
│  ┌────────┐   ┌────────┐   ┌────────┐                     │
│  │ ┌────┐ │   │◄─────► │   │  ╱╲    │  Grid, cube,       │
│  │ │    │ │   ││     │ │   │ ╱  ╲   │  waveform          │
│  │ └────┘ │   │└─────┘ │   │╱────╲  │                    │
│  └────────┘   └────────┘   └────────┘                     │
│                                                             │
│  Zoom In      Zoom Out      Fit to View                    │
│  ┌────────┐   ┌────────┐   ┌────────┐                     │
│  │    ╱╲  │   │  ╲╱    │   │ ┌────┐ │  Magnifying glasses │
│  │   ╱──╲ │   │ ╲──╱   │   │ │⟷⟷ │ │  and arrows         │
│  └────────┘   └────────┘   └────────┘                     │
└─────────────────────────────────────────────────────────────┘
```

### 3.5 Icon Set - AI Features

```
┌─────────────────────────────────────────────────────────────┐
│  AI Active       AI Learning    Generate                    │
│  ┌────────┐    ┌──────────┐   ┌────────┐                   │
│  │   ◉◉   │    │ ◉───► ◉  │   │  ★★★   │  Brain/Neural,   │
│  │  ◉◉◉◉  │    │         │   │ ─────── │  Sparkles, wand  │
│  │   ◉◉   │    │         │   │        │                   │
│  └────────┘    └──────────┘   └────────┘                   │
│                                                             │
│  Suggestion     History         Settings                     │
│  ┌────────┐    ┌──────────┐   ┌────────┐                   │
│  │   💡   │    │◄────────│   │  ⚙     │  Lightbulb,      │
│  │        │    │        ► │   │  ░▒▓   │  Clock, gear     │
│  └────────┘    └──────────┘   └────────┘                   │
└─────────────────────────────────────────────────────────────┘
```

### 3.6 Icon Sizing

```
Size Usage:
16px:   Inline icons, button icons (small)
20px:   Standard button icons
24px:   Default icon size, menu items
32px:   Large buttons, feature highlights
48px:   Hero icons, empty states

Scaling: Vector-based, scale without quality loss
```

---

## 4. Component Styling

### 4.1 Button Styling

```
┌─────────────────────────────────────────────────────────────┐
│  Button States                                             │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Primary Button                                            │
│  ┌─────────────────────────────────────────────────────┐   │
│  │  [ Load Preset ]                                   │   │
│  │                                                     │   │
│  │  Normal:    Fill: #00d4ff, Text: #1a1a2e           │   │
│  │  Hover:     Fill: #00e0ff, Scale: 1.05             │   │
│  │  Active:    Fill: #00c4ef, Scale: 0.98             │   │
│  │  Focus:     Outline: 3px #00d4ff, Offset: 2px      │   │
│  │  Disabled:  Fill: #ffffff20, Text: #ffffff40       │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                             │
│  Secondary Button                                          │
│  ┌─────────────────────────────────────────────────────┐   │
│  │  [ Cancel ]                                         │   │
│  │                                                     │   │
│  │  Normal:    Border: 1px #ffffff30, Text: #ffffff   │   │
│  │  Hover:     Border: 1px #ffffff50, Fill: #ffffff10 │   │
│  │  Active:    Fill: #ffffff15                        │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                             │
│  Icon Button                                               │
│  ┌─────────────────────────────────────────────────────┐   │
│  │  [⚡]  [■]  [●]                                    │   │
│  │                                                     │   │
│  │  Size:      40×40px                                 │   │
│  │  Icon:      20px                                    │   │
│  │  Hover:     Circle background #ffffff15            │   │
│  │  Active:    Circle background #00d4ff              │   │
│  │             Icon color inverts to #1a1a2e          │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 4.2 Slider Styling

```
┌─────────────────────────────────────────────────────────────┐
│  Horizontal Slider                                         │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Cutoff Frequency                        2.4 kHz           │
│  ├─────────────────────────────────────────────────┤       │
│  ●                                                 │       │
│                                                             │
│  Dimensions:                                               │
│    Height:          4px (track), 16px (handle)           │
│    Handle:          16×16px circle, shadow                │
│    Min Width:       120px                                 │
│    Recommended:     200-400px                             │
│                                                             │
│  Colors:                                                    │
│    Track Empty:     #ffffff15                              │
│    Track Filled:    #00d4ff                                │
│    Handle Normal:   #ffffff with shadow                    │
│    Handle Hover:    #00d4ff, scale 1.2                     │
│    Handle Active:   #00d4ff, scale 0.9                     │
│                                                             │
│  Tooltip:                                                  │
│    Shows:           Exact value with units                 │
│    Position:        Above handle, 8px offset               │
│    Delay:           150ms                                  │
│    Style:           Dark background, white text            │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 4.3 Input Field Styling

```
┌─────────────────────────────────────────────────────────────┐
│  Text Input                                                │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌─────────────────────────────────────────────────────┐   │
│  │ Snapshot Name                        │               │   │
│  │ Warm Pad                                    [X]     │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                             │
│  Normal:    Border: 1px #ffffff30                           │
│  Focus:     Border: 2px #00d4ff, Shadow: 0 0 0 4px #00d4ff40│
│  Error:     Border: 2px #ff4757                             │
│  Disabled:  Border: 1px #ffffff15, Text: #ffffff40         │
│  Padding:   12px horizontally, 8px vertically              │
│  Height:    40px                                           │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 4.4 Dropdown Menu Styling

```
┌─────────────────────────────────────────────────────────────┐
│  Dropdown                                                  │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌─────────────────────────────────────────────────────┐   │
│  │ Preset                                      ▼       │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                             │
│  Menu (when open):                                         │
│  ┌─────────────────────────────────────────────────────┐   │
│  │ Warm Pad                                 ✓          │   │
│  │ Bright Pad                                          │   │
│  │ Dark Ambient                                        │   │
│  │ ─────────────────────────────────────────           │   │
│  │ Save As...                                          │   │
│  │ Load From File...                                   │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                             │
│  Item Height:       36px                                    │
│  Hover:            Background: #ffffff15                    │
│  Selected:         Checkmark + bold text                   │
│  Divider:          1px line, #ffffff15                     │
│  Max Height:       300px (scrollable)                      │
│  Shadow:           Drop shadow for depth                   │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 4.5 Toggle Switch Styling

```
┌─────────────────────────────────────────────────────────────┐
│  Toggle Switch                                             │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Off State              On State                            │
│  ┌────────────────┐    ┌────────────────┐                  │
│  │  ●────────────│    │  ────────●     │                  │
│  │  Off           │    │  On            │                  │
│  └────────────────┘    └────────────────┘                  │
│                                                             │
│  Dimensions:                                               │
│    Width:           44px                                   │
│    Height:          24px                                   │
│    Track Height:    12px (centered)                        │
│    Handle Size:     20px circle                            │
│                                                             │
│  Colors:                                                    │
│    Track Off:       #ffffff30                              │
│    Track On:        #00d4ff                                │
│    Handle:          #ffffff                                │
│    Handle Glow:     #00d4ff40 (on state)                   │
│                                                             │
│  Animation:                                                │
│    Duration:        200ms                                  │
│    Easing:          ease-in-out-cubic                      │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 4.6 Tooltip Styling

```
┌─────────────────────────────────────────────────────────────┐
│  Tooltip                                                   │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌─────────────────────────────────────────────────────┐   │
│  │ Cutoff Frequency: 2,450 Hz                         │   │
│  └────▲───────────────────────────────────────────────┘   │
│       │
│     Anchor
│                                                             │
│  Background:      #1a1a2e with #ffffff40 border             │
│  Text:            #ffffff, 12px, Regular                    │
│  Padding:         8px                                      │
│  Border Radius:   4px                                      │
│  Shadow:          Drop shadow                               │
│  Arrow:          Small triangle pointing to anchor          │
│  Max Width:       200px (wrap text)                         │
│  Delay:           500ms (hover), 0ms (focus)                │
│  Duration:        Show immediately, hide after 100ms        │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 5. Animation Specifications

### 5.1 Animation Timing

```
┌─────────────────────────────────────────────────────────────┐
│  Duration Categories                                       │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Instant:        0-50ms     (State changes, no perceived)   │
│  Fast:           100-150ms  (Button press, hover)           │
│  Medium:         200-300ms  (Panel slide, toggle)           │
│  Slow:           400-500ms  (Modal open, page transition)   │
│  Very Slow:      600-1000ms (Hero animations)               │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 5.2 Easing Functions

```
┌─────────────────────────────────────────────────────────────┐
│  Easing Curves                                             │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Linear:         (No easing, constant speed)                │
│  Ease In:        (Starts slow, accelerates)                 │
│  Ease Out:       (Starts fast, decelerates)                 │
│  Ease In-Out:    (Slow start and end)                       │
│  Ease Out Back:  (Overshoots slightly, returns)             │
│  Ease Out Elastic: (Bouncy return)                          │
│  Ease Out Bounce: (Bounces at end)                          │
│                                                             │
│  Default Usage:                                             │
│    Hover:          Ease Out (100ms)                         │
│    Button Press:  Ease Out Back (150ms)                     │
│    Panel Slide:   Ease In-Out (300ms)                       │
│    Modal Open:    Ease Out Back (400ms)                     │
│    Pulse:          Sase In-Out Sine (infinite)              │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 5.3 Animation Examples

#### Button Press Animation
```
1. Mouse Down (0ms)
   - Scale: 1.0 → 0.98 (100ms, easeOutBack)
   - Shadow: Reduces slightly

2. Mouse Up (when held < 200ms)
   - Scale: 0.98 → 1.02 (150ms, easeOutBack)
   - Then: 1.02 → 1.0 (150ms, easeOut)

3. Mouse Up (when held > 200ms)
   - Scale: 0.98 → 1.0 (100ms, easeOut)
```

#### Snapshot Activation Animation
```
1. Click (0ms)
   - Scale Down: 1.0 → 0.9 (150ms, easeInBack)

2. Color Flash (150ms)
   - Background: White flash (50ms)
   - Color: Default → Accent

3. Scale Up (200ms)
   - Scale: 0.9 → 1.05 (200ms, easeOutElastic)

4. Settle (400ms)
   - Scale: 1.05 → 1.0 (100ms, easeOut)
   - Ring Pulse: Begins, repeats every 2s
```

#### Panel Collapse/Expand
```
Collapse:
- Width: Current → 44px (300ms, easeInOutCubic)
- Opacity: 1 → 0.5 (300ms, easeIn)
- Transform: Translate X (if side panel)

Expand:
- Width: 44px → Target (300ms, easeInOutCubic)
- Opacity: 0.5 → 1 (300ms, easeOut, delayed 100ms)
- Transform: None
```

#### AI Suggestion Pulse
```
Continuous Loop (2s duration, sine wave):
- Opacity: 0.5 → 1.0 → 0.5
- Scale: 1.0 → 1.05 → 1.0
- Border: Dashed line animates (stroke-dashoffset)

On Hover:
- Pause pulse
- Scale: 1.05 → 1.1 (150ms, easeOut)
- Shadow: Appears

On Click (Preview):
- Scale: 1.0 → 0.95 (100ms, easeIn)
- Color: Accent
- Hold for preview duration
```

### 5.4 Reduced Motion

```
When `prefers-reduced-motion` is enabled:

- Replace all animations with instant state changes
- Keep essential feedback (brief flashes, focus indicators)
- Remove all decorative animations
- Pulse effects become static
- Slide transitions become fades
- Duration: All set to 0ms (instant)

Exceptions (keep with minimum animation):
- Focus indicators (still essential for navigation)
- Recording indicators (still need to show active state)
- Error feedback (brief flash, <100ms)
```

---

## 6. Layout Grid System

### 6.1 Grid Definition

```
┌─────────────────────────────────────────────────────────────┐
│  8-Point Grid System                                       │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Base Unit: 8px                                            │
│  All measurements align to 8px multiples                    │
│                                                             │
│  Spacing Scale:                                             │
│    4px:   0.5× (compact, internal spacing)                 │
│    8px:   1×   (base unit, tight spacing)                  │
│    12px:  1.5× (comfortable spacing)                       │
│    16px:  2×   (section spacing)                           │
│    24px:  3×   (component spacing)                         │
│    32px:  4×   (large sections)                            │
│    48px:  6×   (page sections)                             │
│    64px:  8×   (major divisions)                           │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 6.2 Component Spacing

```
┌─────────────────────────────────────────────────────────────┐
│  Standard Layout                                           │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌─────────────────────────────────────────────────────┐   │
│  │  Header                                             │   │
│  │  Height: 40px, Padding: 0 16px                      │   │
│  ├─────────────────────────────────────────────────────┤   │
│  │                                                     │   │
│  │  Main Content           Gap: 16px between panels   │   │
│  │  Padding: 16px                                      │   │
│  │                                                     │   │
│  ├─────────────────────────────────────────────────────┤   │
│  │  Footer                                            │   │
│  │  Height: 32px, Padding: 0 16px                      │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                             │
│  Panel Internal Spacing:                                   │
│    Between sections: 16px                                  │
│    Between items: 8px                                      │
│    Section padding: 12px                                   │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 6.3 Responsive Breakpoints

```
┌─────────────────────────────────────────────────────────────┐
│  Breakpoint System                                        │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Minimal:    < 600px     (Essential controls only)         │
│  Compact:    600-799px   (Stacked layout, tabs)            │
│  Medium:     800-1199px  (Collapsible panels)              │
│  Large:      1200-1599px (Full layout)                     │
│  Extra Large: ≥ 1600px   (Expanded layout, more spacing)   │
│                                                             │
│  Spacing Adjustments:                                      │
│    Minimal:    Base unit 4px                               │
│    Compact:    Base unit 6px                               │
│    Medium:     Base unit 8px (default)                    │
│    Large:      Base unit 8px                               │
│    Extra Large: Base unit 8px, increased padding (24px)   │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 6.4 Z-Index Layering

```
┌─────────────────────────────────────────────────────────────┐
│  Z-Index Hierarchy                                         │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  0:      Background, grid lines                            │
│  10:     Morphing pad base, snapshot markers                │
│  20:     Morph path, position indicator                    │
│  30:     AI suggestion overlays                            │
│  40:     Component borders, outlines                       │
│  50:     Active elements, focused items                    │
│  100:    Dropdowns, popovers, tooltips                      │
│  200:    Modals, dialogs                                   │
│  1000:   Toast notifications                               │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 7. Visual Assets

### 7.1 Logo Specifications

```
┌─────────────────────────────────────────────────────────────┐
│  Morphy Logo                                               │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Icon Only:               Icon + Text:                     │
│  ┌────────┐              ┌────────────────────┐            │
│  │        │              │                    │            │
│  │   ◆    │              │   ◆   Morphy       │            │
│  │  ╱ ╲   │              │                    │            │
│  │ ◆───◆  │              └────────────────────┘            │
│  │  ╲ ╱   │                                                 │
│  │   ◆    │  Concept: Four points forming a morph shape   │
│  └────────┘  (diamond configuration)                        │
│                                                             │
│  Icon Size:        32×32px (header), 64×64px (splash)      │
│  Text:             "Morphy", Inter Semibold                │
│  Colors:           Icon uses gradient (cyan → purple)      │
│  Minimum Size:     16×16px ( favicon)                      │
│  Format:           SVG (vector), PNG @2x (raster)          │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 7.2 Splash Screen

```
┌─────────────────────────────────────────────────────────────┐
│  Plugin Loading Screen                                     │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌─────────────────────────────────────────────────────┐   │
│  │                                                     │   │
│  │                  ◆                                  │   │
│  │                 ╱ ╲                                 │   │
│  │                ◆   ◆                                │   │
│  │                 ╲ ╱                                 │   │
│  │                  ◆                                  │   │
│  │                                                     │   │
│  │                    Morphy                           │   │
│  │                   Loading...                        │   │
│  │                                                     │   │
│  │                  ▂▃▅▇▃▂                             │   │
│  │                                                     │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                             │
│  Background:       Dark gradient with subtle animation     │
│  Logo:            Centered, fades in (500ms)               │
│  Loading Text:    Appears after logo (200ms delay)        │
│  Progress Bar:    Simple animated line at bottom          │
│  Duration:        Show minimum 1s, fade out when ready     │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 7.3 Empty State Illustrations

```
┌─────────────────────────────────────────────────────────────┐
│  No Snapshots State                                        │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌─────────────────────────────────────────────────────┐   │
│  │                                                     │   │
│  │              ┌──────┐                               │   │
│  │              │      │   No snapshots yet           │   │
│  │              │  ◆  │                               │   │
│  │              │      │   Create your first snapshot │   │
│  │              └──────┘   by double-clicking on      │   │
│  │                         the morphing pad           │   │
│  │                                                     │   │
│  │              [ Create Snapshot ]                   │   │
│  │                                                     │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 7.4 Cursor Styles

```
┌─────────────────────────────────────────────────────────────┐
│  Cursor States                                             │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Default:           →    (Standard arrow)                  │
│  Clickable:         ↖    (Pointing hand)                   │
│  Drag:              ↔    (Resize arrows)                   │
│  Pad Navigate:      ✥    (Crosshair with circle)           │
│  Pad Dragging:      ✥    (Closed hand)                     │
│  Text:              |    (I-beam)                          │
│  Busy:              ⏳   (Spinner overlay)                 │
│  Not Allowed:       🚫   (Circle with slash)               │
│  Grab:              ✋   (Open hand)                        │
│  Grabbing:          ✊   (Closed hand)                      │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 7.5 Sound/Audio Feedback

```
┌─────────────────────────────────────────────────────────────┐
│  Optional Audio Feedback                                   │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Note: Can be disabled in settings                         │
│                                                             │
│  Snapshot Recall:    Short "click" (100ms, 800Hz)          │
│  Snapshot Store:     "Two-tone" confirm (50ms each)        │
│  Recording Start:    "Beep" (200ms, rising tone)           │
│  Recording Stop:     "Beep-beep" (200ms, falling)          │
│  Error:             "Buzz" (300ms, low freq)               │
│  Success:           "Chime" (150ms, pleasant chord)        │
│                                                             │
│  Volume:            Adjustable 0-100% (default 30%)        │
│  Trigger:          User action only, not automatic         │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## Appendix: Color Export for Implementation

### CSS Variables Format (for reference)

```css
:root {
  /* Brand Colors */
  --color-primary: #00d4ff;
  --color-primary-variant: #00b8e6;
  --color-secondary: #ff6b35;
  --color-secondary-variant: #e65a28;

  /* Semantic Colors */
  --color-success: #2ed573;
  --color-warning: #ffa502;
  --color-error: #ff4757;
  --color-info: #70a1ff;

  /* Neutral Colors (Dark Theme) */
  --color-bg-base: #1a1a2e;
  --color-bg-elevated: #232342;
  --color-surface: #16213e;
  --color-surface-variant: #1f294a;
  --color-surface-highlight: #2a3558;

  /* Text Colors */
  --color-text-primary: #ffffff;
  --color-text-secondary: #a0a0b0;
  --color-text-tertiary: #606070;
  --color-text-disabled: rgba(255, 255, 255, 0.25);

  /* Border Colors */
  --color-border-subtle: rgba(255, 255, 255, 0.03);
  --color-border-normal: rgba(255, 255, 255, 0.08);
  --color-border-strong: rgba(255, 255, 255, 0.19);

  /* Spacing */
  --space-unit: 8px;
  --space-xs: calc(var(--space-unit) * 0.5);
  --space-sm: var(--space-unit);
  --space-md: calc(var(--space-unit) * 1.5);
  --space-lg: calc(var(--space-unit) * 2);
  --space-xl: calc(var(--space-unit) * 3);
  --space-2xl: calc(var(--space-unit) * 4);

  /* Typography */
  --font-family: 'Inter', -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
  --font-mono: 'SF Mono', 'Cascadia Code', 'Fira Code', monospace;

  /* Animation */
  --duration-fast: 100ms;
  --duration-medium: 200ms;
  --duration-slow: 300ms;
  --easing-default: cubic-bezier(0.4, 0.0, 0.2, 1);
  --easing-in: cubic-bezier(0.4, 0.0, 1, 1);
  --easing-out: cubic-bezier(0.0, 0.0, 0.2, 1);
  --easing-in-out: cubic-bezier(0.4, 0.0, 0.2, 1);
}
```

---

**Document End**

For implementation guidance, refer to:
- `ui-system-design.md` - System architecture and components
- Project implementation team for specific framework requirements
