/*
 * More-Phi — Core/ModulationTypes.h
 * Shared type definitions for the V2 modulation system.
 *
 * These types are pure data — no JUCE or audio-thread allocations.
 * ModRoute and ModulationState use fixed-size storage so they can be
 * copied on the audio thread without heap interaction.
 */
#pragma once

#include <array>
#include <cstdint>

namespace more_phi {

// ---------------------------------------------------------------------------
// Modulation source identifiers
// ---------------------------------------------------------------------------

/**
 * Identifies a modulation source by logical slot.
 * Values are stable integers suitable for serialization.
 *
 * Thread safety: read-only enum, safe everywhere.
 */
enum class ModSourceId : int
{
    LFO_1 = 0,
    LFO_2,
    LFO_3,
    LFO_4,
    Envelope_1,
    Envelope_2,
    Macro_1,
    Macro_2,
    Macro_3,
    Macro_4,
    Macro_5,
    Macro_6,
    Macro_7,
    Macro_8,
    Macro_9,
    Macro_10,
    Macro_11,
    Macro_12,
    Macro_13,
    Macro_14,
    Macro_15,
    Macro_16,
    StepSeq_1,
    StepSeq_2,
    DriftRandom_1,
    DriftRandom_2,
    MorphX,
    MorphY,
    FaderPos,
    MIDIVelocity,
    MIDIAftertouch,
    MIDIModWheel,
    NUM_SOURCES
};

static constexpr int NUM_MOD_SOURCES = static_cast<int>(ModSourceId::NUM_SOURCES);

// ---------------------------------------------------------------------------
// LFO shape
// ---------------------------------------------------------------------------

/**
 * Waveform shape for LFO modulation sources.
 *
 * Thread safety: read-only enum, safe everywhere.
 */
enum class LFOShape : int
{
    Sine = 0,
    Triangle,
    Saw,
    Square,
    SampleAndHold,
    Random
};

// ---------------------------------------------------------------------------
// Modulation route
// ---------------------------------------------------------------------------

/**
 * A single modulation assignment: one source → one destination parameter.
 *
 * destParamIndex == -1 means the route slot is unassigned.
 * depth is bipolar [-1.0, +1.0]; positive depth adds to the parameter value,
 * negative depth subtracts.
 *
 * Thread safety: ModRoute is a plain-old-data struct. Writers must ensure
 * atomic visibility (e.g. store into ModulationState under a lock or via
 * a double-buffer mechanism) before the audio thread reads it.
 */
struct ModRoute
{
    ModSourceId source       = ModSourceId::LFO_1;
    int         destParamIndex = -1;   // -1 = unassigned
    float       depth          = 0.0f; // [-1.0, +1.0] bipolar
    bool        enabled        = false;
};

// ---------------------------------------------------------------------------
// Modulation state snapshot
// ---------------------------------------------------------------------------

/**
 * Fixed-size container for all active modulation routes.
 *
 * MAX_ROUTES is deliberately a compile-time constant so this struct can
 * live on the stack or inside a pre-allocated object without heap activity.
 *
 * Thread safety: The audio thread reads routes[0..activeRouteCount-1] during
 * processBlock(). The message thread must write via a thread-safe mechanism
 * (e.g. double-buffer swap) — do NOT mutate this struct on the message thread
 * while the audio thread holds a reference to it.
 */
struct ModulationState
{
    static constexpr int MAX_ROUTES = 128;

    std::array<ModRoute, MAX_ROUTES> routes{};
    int activeRouteCount = 0;
};

} // namespace more_phi
