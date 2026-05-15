/*
 * More-Phi — Core/IModulationEngine.h
 * Abstract interface for the V2 modulation engine subsystem.
 *
 * Thread safety contract
 * ----------------------
 *   Audio thread  : processBlock(), processMIDI()
 *   Message thread: all other methods (route management, LFO/envelope/step
 *                   control, macro setters, serialization)
 *
 * Implementations must ensure that parameter changes made on the message
 * thread are visible to processBlock() without causing data races. The
 * recommended pattern is atomic stores for scalar parameters and a
 * double-buffer / lock-free queue for structural changes (route add/remove).
 *
 * processBlock() and processMIDI() are declared noexcept; implementations
 * must uphold this guarantee — no exceptions, no heap allocation after
 * prepare() has been called.
 */
#pragma once

#include "ModulationTypes.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <memory>
#include <vector>

namespace more_phi {

/**
 * Modulation engine interface.
 *
 * The engine owns a set of modulation sources (LFOs, envelope followers,
 * step sequencers, drift generators, MIDI sources, macro knobs) and a
 * routing table that maps each source to a hosted-plugin parameter index
 * with a signed depth.
 *
 * During processBlock() the engine evaluates all enabled sources at the
 * current sample time, accumulates their contributions scaled by depth,
 * and adds the result to the corresponding entries in morphOutput. Values
 * in morphOutput are normalized [0, 1] hosted-plugin parameter values;
 * modulation is applied additively and clamped to [0, 1] by the caller.
 *
 * Route IDs returned by addRoute() are opaque non-negative integers that
 * remain valid until the route is removed. IDs are not recycled within a
 * single engine lifetime.
 */
class IModulationEngine
{
public:
    virtual ~IModulationEngine() = default;

    // -----------------------------------------------------------------------
    // Lifecycle (message thread)
    // -----------------------------------------------------------------------

    /**
     * Prepares the engine for audio processing.
     * Must be called before the first processBlock(). Pre-allocates all
     * internal buffers so that processBlock() can run without heap activity.
     *
     * @param sampleRate  Host sample rate in Hz.
     * @param blockSize   Maximum expected samples per processBlock() call.
     */
    virtual void prepare(double sampleRate, int blockSize) = 0;

    /**
     * Resets all modulation source phases and envelope states to their
     * initial conditions without clearing routes or configuration.
     *
     * noexcept: Must not allocate or throw. Safe to call from the audio thread.
     */
    virtual void reset() noexcept = 0;

    // -----------------------------------------------------------------------
    // Audio-thread processing
    // -----------------------------------------------------------------------

    /**
     * Advances all modulation sources by one block and accumulates their
     * output into morphOutput.
     *
     * morphOutput[i] holds the current normalized parameter value for
     * hosted-plugin parameter i. This method adds (not replaces) the
     * modulation contributions; callers are responsible for clamping to
     * [0, 1] after the call.
     *
     * @param morphOutput  In/out buffer of normalized parameter values.
     *                     Size must equal the hosted plugin's parameter count.
     * @param dt           Block duration in seconds (blockSize / sampleRate).
     *
     * noexcept: No heap allocation or exceptions after prepare().
     * Audio-thread only.
     */
    virtual void processBlock(std::vector<float>& morphOutput, float dt) noexcept = 0;

    /**
     * Feeds raw MIDI events to MIDI-sourced modulators (velocity, aftertouch,
     * mod wheel). Call this before processBlock() with the same MidiBuffer
     * passed to the hosted plugin.
     *
     * noexcept: No heap allocation or exceptions.
     * Audio-thread only.
     */
    virtual void processMIDI(const juce::MidiBuffer& midi) noexcept = 0;

    // -----------------------------------------------------------------------
    // Route management (message thread)
    // -----------------------------------------------------------------------

    /**
     * Adds a new modulation route and returns its opaque route ID.
     * The route is enabled by default.
     *
     * @param source         Modulation source slot.
     * @param destParamIndex Hosted-plugin parameter index (0-based).
     * @param depth          Bipolar depth [-1.0, +1.0].
     * @return               Non-negative route ID, or -1 on failure (e.g. table full).
     *
     * Message-thread only.
     */
    virtual int addRoute(ModSourceId source, int destParamIndex, float depth) = 0;

    /**
     * Removes a previously added route by ID.
     * Silently ignores unknown IDs.
     *
     * Message-thread only.
     */
    virtual void removeRoute(int routeId) = 0;

    /**
     * Removes all routes. Does not affect source configuration (LFO rates,
     * envelope times, step values, macro values).
     *
     * Message-thread only.
     */
    virtual void clearAllRoutes() = 0;

    /**
     * Returns the number of currently assigned (added but not removed) routes.
     *
     * Message-thread only (result may be stale by the time the caller uses it).
     */
    virtual int getAssignedRouteCount() const = 0;

    /**
     * Returns a const reference to the route with the given ID.
     * Behaviour is undefined if routeId is not a currently active route.
     *
     * Message-thread only.
     */
    virtual const ModRoute& getRoute(int routeId) const = 0;

    // -----------------------------------------------------------------------
    // Macro knobs (message thread; implementations may use atomics to allow
    // real-time updates, but callers should not rely on this)
    // -----------------------------------------------------------------------

    /**
     * Sets the value of a macro knob.
     *
     * @param macroIndex  Zero-based macro index [0, 15].
     * @param value       Normalized value [0.0, 1.0].
     */
    virtual void setMacro(int macroIndex, float value) = 0;

    /**
     * Returns the current value of a macro knob.
     *
     * @param macroIndex  Zero-based macro index [0, 15].
     * @return            Normalized value [0.0, 1.0].
     */
    virtual float getMacro(int macroIndex) const = 0;

    // -----------------------------------------------------------------------
    // LFO control (message thread)
    // -----------------------------------------------------------------------

    /**
     * Sets the waveform shape for an LFO source.
     *
     * @param lfoIndex  Zero-based LFO index [0, 3].
     * @param shape     Waveform shape.
     */
    virtual void setLFOShape(int lfoIndex, LFOShape shape) = 0;

    /**
     * Sets the free-running rate for an LFO source.
     * Ignored when the LFO is tempo-synced.
     *
     * @param lfoIndex  Zero-based LFO index [0, 3].
     * @param hz        Rate in Hz (> 0).
     */
    virtual void setLFORate(int lfoIndex, float hz) = 0;

    /**
     * Enables or disables tempo-synced rate for an LFO source.
     * When synced, the LFO rate is derived from bpm and the nearest
     * musical subdivision rather than the hz value set by setLFORate().
     *
     * @param lfoIndex  Zero-based LFO index [0, 3].
     * @param synced    true to enable tempo sync.
     * @param bpm       Host tempo in beats-per-minute (> 0 when synced).
     */
    virtual void setLFOTempoSync(int lfoIndex, bool synced, float bpm) = 0;

    // -----------------------------------------------------------------------
    // Envelope follower control (message thread)
    // -----------------------------------------------------------------------

    /**
     * Sets the attack time for an envelope follower source.
     *
     * @param envIndex  Zero-based envelope index [0, 1].
     * @param ms        Attack time in milliseconds (>= 0).
     */
    virtual void setEnvelopeAttack(int envIndex, float ms) = 0;

    /**
     * Sets the release time for an envelope follower source.
     *
     * @param envIndex  Zero-based envelope index [0, 1].
     * @param ms        Release time in milliseconds (>= 0).
     */
    virtual void setEnvelopeRelease(int envIndex, float ms) = 0;

    // -----------------------------------------------------------------------
    // Step sequencer control (message thread)
    // -----------------------------------------------------------------------

    /**
     * Sets the output value for one step of a step sequencer source.
     *
     * @param seqIndex  Zero-based sequencer index [0, 1].
     * @param step      Zero-based step index [0, stepCount - 1].
     * @param value     Normalized step value [0.0, 1.0].
     */
    virtual void setStepValue(int seqIndex, int step, float value) = 0;

    /**
     * Sets the active step count for a step sequencer source.
     *
     * @param seqIndex  Zero-based sequencer index [0, 1].
     * @param count     Number of active steps [1, 32].
     */
    virtual void setStepCount(int seqIndex, int count) = 0;

    // -----------------------------------------------------------------------
    // State serialization (message thread)
    // -----------------------------------------------------------------------

    /**
     * Serialises the complete engine state (routes, LFO settings, envelope
     * times, step values, macro values) to an XmlElement.
     * The returned element is owned by the caller.
     *
     * @return  XmlElement containing all engine state, never nullptr.
     */
    virtual std::unique_ptr<juce::XmlElement> toXml() const = 0;

    /**
     * Restores engine state from a previously serialised XmlElement.
     * Unrecognised or missing fields are silently ignored and left at
     * their current values.
     *
     * @param xml  XmlElement produced by toXml().
     */
    virtual void fromXml(const juce::XmlElement& xml) = 0;
};

} // namespace more_phi
