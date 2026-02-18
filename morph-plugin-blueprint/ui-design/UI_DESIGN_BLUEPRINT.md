# UI Design Blueprint

## UX Goals

- Make morph state obvious at a glance.
- Prioritize direct manipulation over modal workflows.
- Provide clear AI connectivity and action transparency.

## Primary Regions

- Center: morph surface (XY pad and trajectory overlay).
- Ring/perimeter: up to 12 snapshot slots with status/color.
- Left controls: mode, speed, physics/drift parameters.
- Right controls: host/plugin controls, AI panel, macros.
- Bottom strip: transport-linked state, MIDI learn, preset actions.

## Component Contracts

- `MorphPad`: emits normalized position + gestures.
- `SnapshotRing`: capture/select/rename/recolor snapshot slots.
- `ModeBar`: mutually exclusive mode selection.
- `AIStatusPanel`: connection, auth, pending commands, last action.
- `AnalysisOverlay`: RMS/FFT or lightweight metering view.

## Interaction Rules

- Pointer drag updates morph in real time.
- Double-click on slot captures/overwrites based on explicit mode.
- Right-click exposes contextual actions (rename, lock, clear).
- Keyboard modifiers enable fine adjustments.

## Rendering and Performance

- Repaint only dirty regions.
- Use cached vector paths for static geometry.
- Keep animation timers bounded to target 60 FPS.
- Degrade visual detail under sustained frame pressure.

## Accessibility and Clarity

- Minimum control hit target for desktop usability.
- Color is never the only state indicator.
- Tooltips expose parameter IDs/value ranges when relevant.
- High-contrast variant should be supported by theme tokens.

## Visual State Model

- `Disconnected`: AI panel muted, explicit connect CTA.
- `Connected`: active indicator + backend name.
- `Executing`: command spinner/progress state.
- `Error`: non-blocking banner with retry action.

## Epic Ticket Mapping

- `MORPH-022` MorphPad UI component
- `MORPH-023` Snapshot ring UI
- `MORPH-024` AI status panel (partial, to finalize)
- `MORPH-025` Plugin browser panel
- `MORPH-026` Macro knob strip
- `MORPH-027` LookAndFeel theme system

## Milestone Checkpoints

- All primary interaction flows are stable in supported DAWs.
- AI panel behavior is finalized to close partial status on `MORPH-024`.
- Rendering remains at target frame rate during active morphing and metering.
