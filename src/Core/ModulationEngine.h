/*
 * MorphSnap — Core/ModulationEngine.h
 * V2 Modulation Engine: owns all sources (LFOs, envelope followers, macro
 * knobs, step sequencers) and routes their output through the ModulationMatrix
 * to modify the morph parameter vector in real-time.
 *
 * Implements IModulationEngine (see IModulationEngine.h).
 *
 * Sits in the processBlock pipeline AFTER MorphProcessor::process():
 *   MorphProcessor::process() → morphOutput[]
 *   ModulationEngine::processBlock(morphOutput, dt)  ← here
 *   DiscreteParameterHandler::snap()
 *   ParameterBridge::applyAll()
 *
 * noexcept guarantee: processBlock(), processMIDI(), processAudioInput(), and
 * setMorphPosition() are noexcept because:
 * - All sources pre-allocate state in prepare()
 * - updateSourceValues() calls only noexcept source process() methods
 * - ModulationMatrix::apply() is noexcept
 * - No heap allocation after prepare()
 *
 * Thread safety:
 * - processBlock / processAudioInput / processMIDI / setMorphPosition → audio thread
 * - All other public methods → message thread
 */
#pragma once

#include "IModulationEngine.h"
#include "LFO.h"
#include "EnvelopeFollower.h"
#include "StepSequencer.h"
#include "ModulationMatrix.h"
#include "ModulationTypes.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <array>
#include <vector>
#include <memory>

namespace morphsnap {

class ModulationEngine : public IModulationEngine
{
public:
    // ── Compile-time capacities ───────────────────────────────────────────────

    static constexpr int NUM_LFOS      =  4;
    static constexpr int NUM_ENVELOPES =  2;
    static constexpr int NUM_MACROS    = 16;
    static constexpr int NUM_STEP_SEQS =  2;

    ModulationEngine() = default;
    ~ModulationEngine() override = default;

    ModulationEngine(const ModulationEngine&)            = delete;
    ModulationEngine& operator=(const ModulationEngine&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /**
     * Initialise all sub-systems for a given sample rate, block size, and
     * maximum number of hosted-plugin parameters.
     * Must be called before processBlock(). Not noexcept (called from prepareToPlay).
     */
    void prepare(double sampleRate, int blockSize) override;

    /**
     * Overload that also accepts maxParamCount. The IModulationEngine interface
     * only mandates (sampleRate, blockSize); this overload is for direct use
     * from MorphSnapProcessor::prepareToPlay() where paramCount is known.
     */
    void prepare(double sampleRate, int blockSize, int maxParamCount);

    /** Reset all sources, the matrix, and all MIDI / morph-position state. */
    void reset() noexcept override;

    // ── Audio-thread API ──────────────────────────────────────────────────────

    /**
     * 1. Tick all modulation sources.
     * 2. Apply the modulation matrix to morphOutput in-place.
     * dt = blockSize / sampleRate.
     * noexcept: All sub-operations are noexcept after prepare().
     */
    void processBlock(std::vector<float>& morphOutput, float dt) noexcept override;

    /**
     * Extract MIDI velocity, aftertouch, and mod-wheel from the buffer.
     * Call before processBlock() so MIDI state is current for this block.
     * noexcept: Iterates pre-allocated MidiBuffer, no allocation.
     */
    void processMIDI(const juce::MidiBuffer& midi) noexcept override;

    /**
     * Feed audio for all envelope followers (first channel only).
     * audioData may be nullptr when numSamples == 0.
     * Call before processBlock().
     * noexcept: Delegates to EnvelopeFollower::process() (noexcept).
     */
    void processAudioInput(const float* audioData, int numSamples) noexcept;

    /**
     * Update morph-position source values.
     * x, y, fader all ∈ [0, 1].
     * noexcept: Trivial float stores.
     */
    void setMorphPosition(float x, float y, float fader) noexcept;

    // ── Route management (message thread, IModulationEngine) ─────────────────

    int  addRoute(ModSourceId source, int destParamIndex, float depth) override;
    void removeRoute(int routeId) override;
    void clearAllRoutes() override;
    int  getActiveRouteCount() const override;
    const ModRoute& getRoute(int routeId) const override;

    // ── Macro knobs (IModulationEngine) ───────────────────────────────────────

    void  setMacro(int index, float value) noexcept override;
    float getMacro(int index) const noexcept override;

    // ── LFO control (IModulationEngine) ───────────────────────────────────────

    void setLFOShape(int lfoIndex, LFOShape shape) override;
    void setLFORate(int lfoIndex, float hz) override;
    void setLFOTempoSync(int lfoIndex, bool synced, float bpm) override;

    // ── Envelope follower control (IModulationEngine) ─────────────────────────

    void setEnvelopeAttack(int envIndex, float ms) override;
    void setEnvelopeRelease(int envIndex, float ms) override;

    // ── Step sequencer control (IModulationEngine) ────────────────────────────

    void setStepValue(int seqIndex, int step, float value) override;
    void setStepCount(int seqIndex, int count) override;

    // ── Direct source accessors (message thread) ───────────────────────────────

    LFO&              getLFO(int index);
    EnvelopeFollower& getEnvelope(int index);
    StepSequencer&    getStepSequencer(int index);

    // ── BPM (from host playhead, set each block) ──────────────────────────────

    /** Update the current host BPM; used by tempo-synced LFOs and step seqs. */
    void setBPM(float bpm) noexcept { bpm_ = bpm; }

    // ── Serialization (IModulationEngine) ─────────────────────────────────────

    std::unique_ptr<juce::XmlElement> toXml() const override;
    void fromXml(const juce::XmlElement& xml) override;

private:
    // ── Sub-systems ───────────────────────────────────────────────────────────

    std::array<LFO,              NUM_LFOS>      lfos_;
    std::array<EnvelopeFollower, NUM_ENVELOPES> envelopes_;
    std::array<float,            NUM_MACROS>    macros_{};
    std::array<StepSequencer,    NUM_STEP_SEQS> stepSequencers_;

    /**
     * Per-block source value accumulator, indexed by ModSourceId.
     * Written entirely in updateSourceValues(); read by ModulationMatrix::apply().
     * Declared as a member to avoid any per-block stack/heap allocation.
     */
    std::array<float, static_cast<int>(ModSourceId::NUM_SOURCES)> sourceValues_{};

    ModulationMatrix matrix_;

    // ── MIDI state ────────────────────────────────────────────────────────────

    float midiVelocity_   = 0.0f; // [0, 1]
    float midiAftertouch_ = 0.0f; // [0, 1]
    float midiModWheel_   = 0.0f; // [0, 1]

    // ── Morph-position state ──────────────────────────────────────────────────

    float morphX_   = 0.5f;
    float morphY_   = 0.5f;
    float faderPos_ = 0.0f;

    // ── BPM ───────────────────────────────────────────────────────────────────

    float bpm_ = 120.0f;

    // ── Init guard ────────────────────────────────────────────────────────────

    double sampleRate_ = 48000.0;
    bool   prepared_   = false;

    // ── Helpers (audio thread) ────────────────────────────────────────────────

    /**
     * Tick all sources for this block and write their current values to
     * sourceValues_[]. Called from processBlock().
     * noexcept: All source process() methods are noexcept.
     */
    void updateSourceValues(float dt) noexcept;
};

} // namespace morphsnap
