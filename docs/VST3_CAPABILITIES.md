# VST3 — Capabilities & Developer Reference

> **Scope:** This document describes the **VST3 plug-in standard itself** (architecture, APIs, runtime model) as a standalone reference and integration guide. It is format-level documentation, independent of any specific host or plug-in implementation.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Core Architecture](#2-core-architecture)
3. [Audio Processing](#3-audio-processing)
4. [MIDI and Event Handling](#4-midi-and-event-handling)
5. [Parameter Management](#5-parameter-management)
6. [UI / View System](#6-ui--view-system)
7. [Plug-in Categories and Types](#7-plug-in-categories-and-types)
8. [Host Requirements](#8-host-requirements)
9. [API Reference](#9-api-reference)
10. [Backward Compatibility](#10-backward-compatibility)
11. [Performance Considerations](#11-performance-considerations)
12. [Security and Sandboxing](#12-security-and-sandboxing)
13. [Advantages Over VST2](#13-advantages-over-vst2)

---

## 1. Overview

**VST3** (Virtual Studio Technology 3) is Steinberg's cross-platform audio plug-in standard, introduced in 2008 as the successor to VST 2.x. A VST3 module is a dynamically loaded binary that a **host** application (a DAW — REAPER, Cubase, Nuendo, Ableton Live, Studio One, FL Studio, etc.) loads to add audio effects or instruments to its signal graph.

### Why VST3 exists

VST3 was a ground-up redesign that fixed structural limitations of VST2:

| Concern in VST2 | VST3 answer |
|-----------------|-------------|
| Fixed input/output pin count declared at build time | **Dynamic bus** configuration negotiated at runtime |
| Single object combining DSP + UI | **Dual-component** split (processor vs. edit controller) |
| 32-bit float only | **32- and 64-bit** sample processing |
| Block-level automation | **Sample-accurate** automation |
| Channel-wide MIDI only | **Note Expression** — per-note polyphonic control |
| Fixed bank/program files | Flexible **units + program lists** |

### Key facts

- **Specification owner:** Steinberg Media Technologies GmbH.
- **SDK license:** Dual-licensed — **GPLv3** (open source) or a proprietary commercial license. The SDK source is public.
- **Repository:** [`steinbergmedia/vst3sdk`](https://github.com/steinbergmedia/vst3sdk) on GitHub.
- **Developer portal:** <https://steinbergmedia.github.io/vst3_dev_portal/>
- **Platforms:** Windows, macOS, Linux. On macOS, VST3 sits alongside **AU** (Audio Unit); on Windows/Linux it is the dominant format.
- **Binary packaging:** a `.vst3` bundle — a directory (package) on macOS and modern Windows, containing the compiled module and metadata.

---

## 2. Core Architecture

VST3 is built on a **COM-like component model**: every object derives from `FUnknown`, is reference-counted, and supports runtime interface discovery through `queryInterface(FIDString iid)`. Two `FUnknown`-based components form the heart of a plug-in.

### 2.1 The dual-component model

This is the single most important architectural change from VST2. A VST3 effect is split into **two cooperating but independent objects**, both identified by the same GUID:

```
            ┌───────────────────────────┐         ┌───────────────────────────┐
            │   Processing Component     │         │   Edit Controller          │
            │   (IComponent +            │         │   (IEditController +       │
            │    IAudioProcessor)        │         │    IEditController2)       │
            │                             │         │                             │
            │   • owns DSP state          │         │   • owns parameter defs    │
            │   • runs on the audio       │         │   • owns the GUI view      │
            │     thread                  │         │   • runs on the UI thread  │
            │   • declares audio/event    │         │   • converts normalized ↔  │
            │     buses                   │         │     plain/display values   │
            └───────────────────────────┘         └───────────────────────────┘
                    ▲ state (IBStream)                     ▲ state (IBStream)
                    │                                      │
                 host syncs both halves of state
```

Why the split matters:

- **Sandboxing:** the processor can run in a separate process/sandbox from the UI controller.
- **Preset browsing without DSP:** a host can instantiate *only* the controller to scan presets or draw a thumbnail — no audio engine loaded.
- **Clean threading:** DSP lives on the audio thread; parameter/GUI logic lives on the message thread. No shared mutable object across both.

### 2.2 Component lifecycle

The host drives the lifecycle via the factory:

```cpp
// Entry point the module must export
IPluginFactory* PLUGIN_API GetPluginFactory();

// Host side:
factory->createInstance(classId, IComponent::iid, (void**)&component);   // processor
component->getControllerClassId(controllerUid);
factory->createInstance(controllerUid, IEditController::iid, (void**)&controller);
component->initialize(hostContext);   // pass IHostApplication
controller->initialize(hostContext);
controller->setComponentHandler(handler);  // give it an IComponentHandler
```

### 2.3 Buses — the I/O abstraction

Instead of fixed pins, VST3 declares **buses** of a given `MediaType`:

- **Audio buses** — each carries channel buffers for a `SpeakerArrangement` (Mono, Stereo, 5.1, 7.1, …). Main bus vs. **auxiliary** bus (side-chain input).
- **Event buses** — carry note/MIDI/expression events.

A plug-in advertises a *static* bus layout at registration; the host may then **activate a subset** via `activateBus(...)`, and the plug-in can react. This is how a 5.1-capable limiter runs in a stereo project, or how a synth exposes a sidechain input only when the user routes one.

```
Instrument example:
  Audio: 1 main output (Stereo)
  Event: 1 input (MIDI / note events)

FX with sidechain example:
  Audio: 1 main input (Stereo), 1 main output (Stereo), 1 aux input (Stereo, sidechain)
  Event: none
```

---

## 3. Audio Processing

### 3.1 The processing entry point

All real-time audio flows through one method:

```cpp
tresult MyAudioProcessor::process(ProcessData& data) {
    // data.inputs / data.outputs : arrays of AudioBusBuffers (one per active bus)
    // data.numSamples            : frames to process this block
    // data.symbolicSampleSize    : kSample32 or kSample64
    // data.processContext        : tempo, time-sig, transport state, sample position
    // data.inputParameterChanges : sample-accurate automation to read
    // data.inputEvents           : note/MIDI/expression events to consume

    // 1. Drain parameter changes (sample-accurate)
    // 2. Read input events
    // 3. Render into data.outputs
    return kResultOk;
}
```

The `ProcessData` structure carries everything the plug-in needs for one block:

| Field | Purpose |
|-------|---------|
| `numSamples` | Number of sample frames in this block |
| `numInputs` / `numOutputs` | Count of active input/output buses |
| `inputs` / `outputs` | `AudioBusBuffers[]` — each bus holds channel pointers |
| `symbolicSampleSize` | `kSample32` (float) or `kSample64` (double) |
| `processMode` | `kRealtime`, `kPrefetch`, or `kOffline` |
| `processContext` | Transport, tempo, time signature, bar position |
| `inputParameterChanges` | Automation to apply this block |
| `outputParameterChanges` | Changes the plug-in reports back (e.g. generated LFO) |
| `inputEvents` / `outputEvents` | Note / MIDI / expression events |

### 3.2 Setup and precision

Before `process` is called, the host configures the engine:

```cpp
tresult setupProcessing(ProcessSetup& setup) override {
    // setup.sampleRate, setup.maxSamplesPerBlock, setup.processMode, setup.symbolicSampleSize
    // Allocate / pre-size all buffers HERE — never inside process().
}

tresult setProcessing(TBool state) override {
    // state == true: DSP is about to start. Allocate per-voice tables, clear denormals, etc.
    // state == false: DSP stopped. Safe to release transient resources.
}
```

The plug-in declares whether it can handle a given precision:

```cpp
tresult canProcessSampleSize(int32 symbolicSampleSize) override {
    return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
}
```

### 3.3 Latency and tails

Two declared values let the host compensate correctly:

- **`getLatencySamples()`** — constant delay introduced by the plug-in (lookahead limiter, FFT window). The host shifts downstream tracks to realign.
- **`getTailSamples()`** — how many samples of trailing output the plug-in produces after input goes silent (reverb decay). `kInfiniteTail` for endless tails.

### 3.4 Real-time contract

`process()` runs on the host's real-time audio thread. The rules are absolute:

- ❌ No heap allocation (`new`/`malloc`/`std::vector` growth).
- ❌ No blocking locks (no `std::mutex::lock`, no I/O, no `sleep`).
- ❌ No file, network, or system calls.
- ❌ No exceptions (`process` paths must not throw).
- ✅ Pre-allocate everything in `setupProcessing` / `setProcessing`.
- ✅ Communicate with other threads only through atomics, lock-free queues, or host-provided handoff.

Violating this produces dropouts (xruns) or crashes under load — the cardinal sin of an audio plug-in.

---

## 4. MIDI and Event Handling

### 4.1 Events, not raw MIDI

VST3 retired VST2's dedicated MIDI pin. Instead, MIDI and richer musical data arrive on **event buses** as typed events in an `IEventList`. Raw MIDI is still available (via `DataEvent` with `kMidiSysexEvent` / legacy MIDI), but typed events are the primary path:

| Event | Meaning |
|-------|---------|
| `NoteOnEvent` | Polyphonic note start — pitch, velocity (0–1 float), note id, tuning |
| `NoteOffEvent` | Polyphonic note end, references the originating `noteId` |
| `PolyPressureEvent` | Per-note aftertouch |
| `NoteExpressionValueEvent` | Per-note continuous controller (see below) |
| `DataEvent` | Raw bytes — SysEx or legacy MIDI |
| `ChordEvent` / `ScaleEvent` | Harmonic context for intelligent instruments |

Because notes carry a unique **`noteId`** rather than being identified only by pitch+channel, a single voice can be addressed independently — the foundation for Note Expression.

### 4.2 Note Expression — per-note polyphonic control

The headline VST3 expressive feature. While a note is sounding, the host can stream continuous per-note changes:

| Expression | Typical range |
|------------|---------------|
| **Volume** | 0 … 1 |
| **Pan** | 0 … 1 |
| **Tuning** | microtonal, ±semitones |
| **Pitch** | large-range glide (±120 semitones) |
| **Brightness** | timbre / filter cutoff |
| **Pressure** | polyphonic aftertouch curve |

This is what lets a DAW-recorded violin phrase bend pitch, swell volume, and open timbre **independently for each overlapping note** — impossible with channel-wide MIDI.

### 4.3 Sample-accurate timing

Every event carries a **`sampleOffset`** relative to the start of the current block. The host is expected to honor these offsets so rendered audio is sample-accurate, which matters for offline bounce and tight sequencing.

---

## 5. Parameter Management

### 5.1 Parameter identity and normalization

- Parameters are addressed by a **`ParamID`** (32-bit). VST3 therefore effectively removes VST2's parameter-count ceiling.
- All values are **normalized** to `[0.0, 1.0]` on the wire. The edit controller converts between normalized, **plain** (e.g. Hz, dB), and **display** (string) forms:

```cpp
ParamValue normalized = controller->plainParamToNormalized(id, 440.0);     // Hz → 0..1
double      hz        = controller->normalizedParamToPlain(id, normalized); // 0..1 → Hz
String128   display;
controller->getParamStringByValue(id, normalized, display);                 // "440.00 Hz"
```

### 5.2 Parameter metadata

Each parameter is described by a `ParameterInfo`:

```cpp
struct ParameterInfo {
    ParamID      id;
    TChar        title[128];
    TChar        shortTitle[128];   // shown in compact UI
    TChar        units[128];        // "Hz", "dB", "%"
    int32        stepCount;         // >0 → discrete/list
    ParamValue   defaultNormalizedValue;
    int32        unitId;            // groups params into a unit
    int32        flags;             // kCanAutomate, kIsReadOnly, kIsList,
                                    // kIsHidden, kIsBypass, kIsProgramChange…
};
```

Flags drive host behavior — whether a parameter can be automated, whether it's a list (e.g., waveform selector), whether it is the bypass.

### 5.3 Sample-accurate automation

Automation is delivered per block via `IParameterChanges`, a collection of `IParamValueQueue`s. Each queue holds an ordered list of `(sampleOffset, normalizedValue)` points:

```cpp
int32 paramCount = data.inputParameterChanges->getParameterCount();
for (int32 i = 0; i < paramCount; ++i) {
    IParamValueQueue* q = data.inputParameterChanges->getParameterData(i);
    ParamID id = q->getParameterId();
    int32 pointCount = q->getPointCount();
    for (int32 p = 0; p < pointCount; ++p) {
        int32 sampleOffset; ParamValue value;
        q->getPoint(p, sampleOffset, value);
        // apply value at exactly sampleOffset within the block
    }
}
```

This is **sample-accurate**: a ramp can advance multiple times within one buffer, so automated filters sweep cleanly even at large block sizes.

### 5.4 Units, program lists, presets

- **Units** group parameters (e.g. "Oscillator 1", "Filter", "LFO 1") and form a hierarchy — essential for multi-timbral instruments. Exposed via `IUnitData`.
- **Program lists** (preset slots) replace VST2's fixed bank/program arrays and are exposed via `IProgramListData`.
- **Preset files** (`.vstpreset`) store a plug-in's state as a chunk, browsable directly by the host without instantiating the DSP (thanks to the dual-component split).

### 5.5 State save / restore

Both components serialize independently to an `IBStream`:

```cpp
// Processor side
tresult IComponent::getState(IBStream*);       // host calls on save
tresult IComponent::setState(IBStream*);        // host calls on restore

// Controller side
tresult IEditController::getState(IBStream*);
tresult IEditController::setState(IBStream*);
tresult IEditController::setComponentState(IBStream*); // controller learns the processor's state
```

`setComponentState` is the canonical sync path: the host restores the processor's state, then forwards the same stream to the controller so the GUI reflects it.

---

## 6. UI / View System

### 6.1 IPlugView

The GUI is an opaque, platform-native window the host embeds, abstracted behind `IPlugView`. The plug-in creates it on demand:

```cpp
IPlugView* MyEditController::createView(FIDString name) {
    if (strcmp(name, "editor") == 0)
        return new MyEditorView(/* platform widget */);
    return nullptr;
}
```

The host calls `attached(parent, platformType)` with the native parent:

| Platform | `platformType` string | Native parent |
|----------|-----------------------|---------------|
| Windows  | `"HWND"`              | `HWND` |
| macOS    | `"NSView"`            | `NSView*` |
| Linux    | `"X11EmbedWindowID"`  | `Window` (XID) |

The plug-in never creates a top-level window; it parents its widget into the host's provided container.

### 6.2 Host ↔ UI communication

The view talks back to the host through the `IComponentHandler` the controller received at init — the **edit gesture protocol**:

```cpp
componentHandler->beginEdit(paramId);              // user grabbed a control
componentHandler->performEdit(paramId, value);     // continuous drag (many calls)
componentHandler->endEdit(paramId);                // user released
```

`beginEdit`/`endEdit` bracketing is what lets the host record a single undoable automation gesture and draw the right automation curve. Skipping them breaks automation writing and undo.

### 6.3 Resize, scale, focus

`IPlugView` also covers `getSize`/`onSize`/`canResize`/`checkSizeConstraint` (resize negotiation), `onKeyDown`/`onKeyUp`/`onWheel`/`onFocus` (input), and content-scale support for HiDPI. **VSTGUI** is Steinberg's accompanying UI toolkit, but any native or embedded UI (including web- or GPU-rendered) can implement `IPlugView`.

---

## 7. Plug-in Categories and Types

A VST3 module registers its class info (category + subcategory strings) with the factory. Categories tell hosts where to list the plug-in and how to treat it.

### 7.1 Top-level categories

| Class category constant | Meaning |
|--------------------------|---------|
| `kVstAudioEffectClass` | Audio **effect** (processes incoming audio) |
| `kVstInstrumentClass` | **Instrument** (generates audio from events) |
| `kVstInstrumentControllerClass` / `kVstComponentControllerClass` | The edit-controller half of the above |
| `kVstAuxBusEffectClass` | Effect used on an auxiliary bus |

### 7.2 Subcategory taxonomy

Subcategories follow a `Category|Subcategory` convention and refine listing (e.g. under an "Reverb" sub-folder). Representative values:

```
Fx              | Delay, Reverb, Distortion, Dynamics, EQ, Filter, Modulation,
                | Pitch Shift, Tools, Analyzer, Surround, Restoration, Mastering
Instrument      | Synth, Sampler, Drum, External, Instrument-Effects
Only-Generator  | (for tone/noise generators)
```

### 7.3 Distributability flag

`kDistributable` marks components safe to host in a separate process (the whole point of the dual-component + sandbox design). Hosts use it to decide whether a plug-in can be sandboxed.

---

## 8. Host Requirements

To support VST3, a host must implement a substantial runtime contract:

| Responsibility | Detail |
|----------------|--------|
| **Module loading** | Discover `.vst3` bundles in standard folders; call `GetPluginFactory()`. |
| **Instantiation** | Create `IComponent`, resolve its controller UID, create `IEditController`. |
| **Bus negotiation** | Read declared buses; activate a valid subset; honor speaker arrangements. |
| **Processing setup** | Provide `ProcessSetup` (rate, max block, mode, precision); call `setupProcessing`, then `setActive(true)`, then `setProcessing(true)`. |
| **Block rendering** | Repeatedly call `process(ProcessData)` on the audio thread with valid buffers. |
| **Transport context** | Fill `ProcessContext` — tempo (BPM), time signature, sample position, playing/recording/cycle flags. |
| **Parameter automation** | Provide `IParameterChanges` for playback; record via `beginEdit/performEdit/endEdit`. |
| **State management** | Save/restore via `getState`/`setState`/`setComponentState`; sync processor↔controller. |
| **Latency/tail compensation** | Read `getLatencySamples`/`getTailSamples` and compensate the graph. |
| **UI embedding** | Provide `IPlugFrame` + a parent for `IPlugView`. |
| **Host callbacks** | Implement `IHostApplication`, `IComponentHandler` (+ `IComponentHandler2`/`3` for advanced menus), `IUnitHandler`. |

### Standard plug-in install locations

| OS | Effect / Instrument folder |
|----|----------------------------|
| Windows | `%COMMONPROGRAMFILES%\VST3\` (user: `%LOCALAPPDATA%\Programs\Common\VST3`) |
| macOS | `/Library/Audio/Plug-Ins/VST3/` and `~/Library/Audio/Plug-Ins/VST3/` |
| Linux | `/usr/lib/vst3/`, `/usr/local/lib/vst3/`, `~/.vst3/` |

---

## 9. API Reference

A high-level map of the core SDK interfaces (C++ in the `Steinberg::Vst` namespace; signatures are representative — consult the SDK headers for exact field order across versions).

### 9.1 Component & factory

```cpp
extern "C" IPluginFactory* PLUGIN_API GetPluginFactory();   // module export

class IPluginFactory3 : public IPluginFactory2 /* : public IPluginFactory */ {
    tresult getClassInfoUnicode(int32 index, PClassInfoW* info);  // rich class metadata
};
class IComponent : public FUnknown {                 // the processor base
    tresult initialize(FUnknown* context);
    tresult terminate();
    tresult getControllerClassId(TUID cid) const;
    int32   getBusCount(MediaType type, BusDirection dir);
    tresult getBusInfo(MediaType, BusDirection, int32 index, BusInfo& info);
    tresult activateBus(MediaType, BusDirection, int32 index, TBool state);
    tresult setActive(TBool state);
    tresult setState(IBStream*);  tresult getState(IBStream*);
};
```

### 9.2 Audio processing

```cpp
class IAudioProcessor : public FUnknown {
    tresult setupProcessing(ProcessSetup& setup);
    tresult setProcessing(TBool state);
    tresult process(ProcessData& data);
    tresult canProcessSampleSize(int32 symbolicSampleSize);
    uint32  getLatencySamples();
    uint32  getTailSamples();
};
```

### 9.3 Parameters & controller

```cpp
class IEditController : public FUnknown {
    tresult       setComponentState(IBStream*);
    tresult       setState(IBStream*);   tresult getState(IBStream*);
    int32         getParameterCount();
    tresult       getParameterInfo(int32 index, ParameterInfo& info);
    ParamValue    normalizedParamToPlain(ParamID, ParamValue);
    ParamValue    plainParamToNormalized(ParamID, ParamValue);
    tresult       getParamStringByValue(ParamID, ParamValue, String128);
    tresult       getParamValueByString(ParamID, const TChar*, ParamValue&);
    tresult       setParamNormalized(ParamID, ParamValue);
    tresult       setComponentHandler(IComponentHandler*);
    IPlugView*    createView(FIDString name);
};
```

### 9.4 Automation, events, host

```cpp
class IComponentHandler : public FUnknown {           // UI → host
    tresult beginEdit(ParamID);  tresult performEdit(ParamID, ParamValue);
    tresult endEdit(ParamID);    tresult restartComponent(int32 flags);
};
class IParameterChanges { getParameterCount(); getParameterData(i); addParameterData(id, i); };
class IParamValueQueue  { getParameterId(); getPointCount(); getPoint(i, off, val); addPoint(off, val, i); };
class IEventList        { getEventCount(); getEvent(i, e&); addEvent(e&); };
class IPlugView         { attached(parent, type); removed(); getSize(ViewRect*); onSize(...); setFrame(IPlugFrame*); };
```

### 9.5 Interface discovery

Everything resolves through `queryInterface` with well-known IIDs: `FUnknown::iid`, `IComponent::iid`, `IAudioProcessor::iid`, `IEditController::iid`, `IConnectionPoint::iid` (inter-component messaging), `IUnitData::iid`, `IXmlRepresentationController::iid`, etc.

---

## 10. Backward Compatibility

VST3 is **not** binary- or source-compatible with VST2 — the object models differ fundamentally. Compatibility is provided by **adapters**, not by the format itself:

| Direction | Mechanism | Status |
|-----------|-----------|--------|
| **VST3 → wrapped as VST2** | SDK `vst2wrappergen` / VST2 wrapper | Deprecated; VST2 distribution is no longer licensed by Steinberg. |
| **VST2 → hosted inside a VST3 host** | "VST 2.x Adapter" (`public.sdk/source/vst/vst2wrapper`) | Lets a host that natively speaks VST3 load legacy VST2 modules. |
| **Cross-format bridging** | JUCE's `AudioPluginInstance` and similar wrappers | Many modern hosts and frameworks abstract both behind one API. |

### Migration notes for VST2 developers

- **One object → two components.** Split your single `AudioEffectX` into an `IComponent`/`IAudioProcessor` and a separate `IEditController`.
- **Pins → buses.** Replace fixed `numInputs`/`numOutputs` with declared audio/event buses and `activateBus`.
- **MIDI pins → event buses.** Read MIDI from `IEventList`, not `processEvents`.
- **`getParameter`/`setParameter` → normalized + queues.** Automation now arrives via `IParameterChanges`; UI gestures go through `beginEdit/performEdit/endEdit`.
- **Program/bank files → units + program lists + `.vstpreset`.**
- **32-bit → 32/64-bit.** Decide your precision and report it via `canProcessSampleSize`.

---

## 11. Performance Considerations

### 11.1 Threading model

| Thread | What runs on it |
|--------|-----------------|
| **Audio / real-time** | `IAudioProcessor::process`, `setupProcessing`, bus activation callbacks |
| **UI / message** | `IEditController`, `IPlugView`, `IComponentHandler::beginEdit/performEdit/endEdit` |
| **Host-controlled workers** | Preset scanning, state load, factory instantiation |

The host (not the plug-in) owns the audio thread. Cross-thread handoff must use the real-time-safe primitives above.

### 11.2 Optimization levers

- **Active buses only:** the host only routes audio for activated buses; a synth with no sidechain wired pays nothing for its aux bus.
- **Precision selection:** process at 32-bit when 64-bit buys nothing (most EQs/delays) to halve memory bandwidth.
- **Process mode hints:** `processMode` (`kRealtime` vs `kOffline`) lets a plug-in choose cheaper code paths during offline bounce (e.g., oversample aggressively because CPU isn't time-critical).
- **Tail handling:** declare an accurate `getTailSamples` so the host stops processing the reverb tail only when truly finished — neither truncating nor burning CPU forever.

### 11.3 Latency discipline

- Declare **real** latency in `getLatencySamples`. The host compensates; under-reporting causes audible misalignment, over-reporting adds needless delay.
- Prefer zero-latency algorithms for tracking-critical effects; reserve lookahead/FFT latency for mastering contexts.

### 11.4 Allocation discipline

The single most common performance defect is allocation on the audio thread. Pre-size every buffer, voice pool, and queue in `setupProcessing`/`setProcessing`; guard with the SDK's allocation-aware patterns or a real-time checker such as `pluginval`.

---

## 12. Security and Sandboxing

VST3 was explicitly designed for a hostile third-party-code world:

- **Process isolation (distributable components).** The dual-component split plus the `kDistributable` flag let a host run untrusted effect DSP in a **separate sandboxed process**, communicating via IPC. A crashing or malicious plug-in cannot take down the host.
- **macOS App Sandbox compatibility.** The controller/UI and processor can live in different sandbox entitlement domains; the standard was shaped to satisfy the App Store's sandbox rules.
- **The VST3 package.** The `.vst3` bundle carries an `Info.plist` (macOS) / structured manifest, version metadata, and code-signing hooks — giving the OS and hosts a verifiable identity.
- **Reference-counted `FUnknown`.** Object lifetime is deterministic and queryable; no implicit global singletons assumed, which is what makes cross-process hosting tractable.
- **No implicit network/disk access.** Anything a plug-in does beyond audio is its own responsibility under the host's entitlements; the format itself never auto-grants resources.

In short: VST3 treats a loaded plug-in as **untrusted, third-party code** and gives the host the structural hooks to contain it.

---

## 13. Advantages Over VST2

| Dimension | VST 2.x | VST3 |
|-----------|---------|------|
| **DSP precision** | 32-bit float only | 32-bit **and** 64-bit (double) |
| **I/O model** | Fixed input/output pins at build time | **Dynamic buses**, runtime activation, speaker arrangements |
| **Component model** | One object (edit + processor fused) | **Dual-component** split → sandboxing, DSP-free preset browse |
| **Automation** | Effectively block-level | **Sample-accurate** via parameter queues |
| **Parameter count** | Limited / awkward | Full 32-bit `ParamID` space |
| **MIDI** | Channel-wide, dedicated MIDI pin | Typed **events** + raw fallback |
| **Expression** | None beyond CC/Pitch Bend | **Note Expression** — per-note polyphonic control |
| **Programs** | Fixed bank/program files | Flexible **units + program lists + `.vstpreset`** |
| **Transport context** | Basic | Rich `ProcessContext` (tempo, time-sig, bars, cycle, recording) |
| **Licensing** | Closed/deprecated | Open SDK (GPLv3 / proprietary), actively developed |
| **Sandboxing** | Effectively impossible | Designed in (distributable, separate-process capable) |

### Summary

VST3 trades VST2's simplicity for a model that is **real-time-correct, sample-accurate, expressively rich, and sandboxable**. The dual-component split is the keystone: it is what enables both per-note expression and the secure, crash-isolated hosting that modern DAWs require. For any new plug-in development in 2026, VST3 (or its framework-backed equivalents) is the default target, with VST2 retained only as a legacy-compatibility shim.

---

## References

- **VST3 Developer Portal** — <https://steinbergmedia.github.io/vst3_dev_portal/>
- **VST3 SDK (source)** — <https://github.com/steinbergmedia/vst3sdk>
- **VST3 API documentation** — <https://steinbergmedia.github.io/vst3_doc/>
- **VST SDK forum** — <https://forums.steinberg.net/c/sdk/vst>
- **VSTGUI (UI toolkit)** — <https://github.com/steinbergmedia/vstgui>

> *Format-level reference, current as of VST3 SDK 3.7.x. Always cross-check exact signatures against the SDK headers in your pinned version — field order and helper interfaces evolve between minor releases.*
