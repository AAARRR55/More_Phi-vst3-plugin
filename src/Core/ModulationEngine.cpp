/*
 * MorphSnap — Core/ModulationEngine.cpp
 * V2 Modulation Engine implementation.
 *
 * updateSourceValues() inner loop (per block):
 *   sourceValues_[LFO_1..4]       ← LFO::process(dt)          → [-1, 1]
 *   sourceValues_[Envelope_1..2]  ← EnvelopeFollower::getCurrentValue()  → [0, 1]
 *   sourceValues_[Macro_1..16]    ← macros_[i]                → [0, 1]
 *   sourceValues_[StepSeq_1..2]   ← StepSequencer::process(dt, bpm) → [-1, 1]
 *   sourceValues_[MorphX/Y]       ← morphX_, morphY_          → [0, 1]
 *   sourceValues_[FaderPos]       ← faderPos_                 → [0, 1]
 *   sourceValues_[MIDIVelocity]   ← midiVelocity_             → [0, 1]
 *   sourceValues_[MIDIAftertouch] ← midiAftertouch_           → [0, 1]
 *   sourceValues_[MIDIModWheel]   ← midiModWheel_             → [0, 1]
 *   sourceValues_[DriftRandom_1/2]← per-block jitter          → [-1, 1]
 *
 * Then ModulationMatrix::apply() accumulates all enabled routes into morphOutput.
 */
#include "ModulationEngine.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>   // std::rand / RAND_MAX
#include <stdexcept>

namespace morphsnap {

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void ModulationEngine::prepare(double sampleRate, int blockSize)
{
    prepare(sampleRate, blockSize, 2048);
}

void ModulationEngine::prepare(double sampleRate, int blockSize, int maxParamCount)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

    for (auto& lfo : lfos_)
        lfo.prepare(sampleRate_);

    for (auto& env : envelopes_)
        env.prepare(sampleRate_);

    for (auto& seq : stepSequencers_)
        seq.prepare(sampleRate_);

    matrix_.prepare(maxParamCount > 0 ? maxParamCount : 2048);

    sourceValues_.fill(0.0f);
    macros_.fill(0.0f);

    prepared_ = true;
    (void)blockSize; // reserved for future per-block buffer pre-allocation
}

void ModulationEngine::reset() noexcept
{
    for (auto& lfo : lfos_)    lfo.reset();
    for (auto& env : envelopes_) env.reset();
    for (auto& seq : stepSequencers_) seq.reset();

    matrix_.reset();
    sourceValues_.fill(0.0f);
    midiVelocity_   = 0.0f;
    midiAftertouch_ = 0.0f;
    midiModWheel_   = 0.0f;
    morphX_         = 0.5f;
    morphY_         = 0.5f;
    faderPos_       = 0.0f;
}

// ── Audio-thread setters ──────────────────────────────────────────────────────

void ModulationEngine::setMorphPosition(float x, float y, float fader) noexcept
{
    morphX_   = std::clamp(x,     0.0f, 1.0f);
    morphY_   = std::clamp(y,     0.0f, 1.0f);
    faderPos_ = std::clamp(fader, 0.0f, 1.0f);
}

void ModulationEngine::processAudioInput(const float* audioData, int numSamples) noexcept
{
    // Feed the same block into both envelope followers
    // (In a stereo setup the caller can feed different channels; here both share input)
    for (auto& env : envelopes_)
        env.process(audioData, numSamples);
}

// ── MIDI ──────────────────────────────────────────────────────────────────────

void ModulationEngine::processMIDI(const juce::MidiBuffer& midi) noexcept
{
    for (const auto& meta : midi)
    {
        const auto msg = meta.getMessage();

        if (msg.isNoteOn())
        {
            midiVelocity_ = static_cast<float>(msg.getVelocity()) / 127.0f;
        }
        else if (msg.isAftertouch())
        {
            midiAftertouch_ = static_cast<float>(msg.getAfterTouchValue()) / 127.0f;
        }
        else if (msg.isChannelPressure())
        {
            midiAftertouch_ = static_cast<float>(msg.getChannelPressureValue()) / 127.0f;
        }
        else if (msg.isController())
        {
            if (msg.getControllerNumber() == 1)  // CC1 = Mod Wheel
                midiModWheel_ = static_cast<float>(msg.getControllerValue()) / 127.0f;
        }
    }
}

// ── Source-value accumulator ──────────────────────────────────────────────────

void ModulationEngine::updateSourceValues(float dt) noexcept
{
    // ── LFOs ──────────────────────────────────────────────────────────────────
    sourceValues_[static_cast<int>(ModSourceId::LFO_1)] = lfos_[0].process(dt);
    sourceValues_[static_cast<int>(ModSourceId::LFO_2)] = lfos_[1].process(dt);
    sourceValues_[static_cast<int>(ModSourceId::LFO_3)] = lfos_[2].process(dt);
    sourceValues_[static_cast<int>(ModSourceId::LFO_4)] = lfos_[3].process(dt);

    // ── Envelope followers ────────────────────────────────────────────────────
    // process() was already called in processAudioInput(); just read current value
    sourceValues_[static_cast<int>(ModSourceId::Envelope_1)] = envelopes_[0].getCurrentValue();
    sourceValues_[static_cast<int>(ModSourceId::Envelope_2)] = envelopes_[1].getCurrentValue();

    // ── Macro knobs ───────────────────────────────────────────────────────────
    sourceValues_[static_cast<int>(ModSourceId::Macro_1)]  = macros_[0];
    sourceValues_[static_cast<int>(ModSourceId::Macro_2)]  = macros_[1];
    sourceValues_[static_cast<int>(ModSourceId::Macro_3)]  = macros_[2];
    sourceValues_[static_cast<int>(ModSourceId::Macro_4)]  = macros_[3];
    sourceValues_[static_cast<int>(ModSourceId::Macro_5)]  = macros_[4];
    sourceValues_[static_cast<int>(ModSourceId::Macro_6)]  = macros_[5];
    sourceValues_[static_cast<int>(ModSourceId::Macro_7)]  = macros_[6];
    sourceValues_[static_cast<int>(ModSourceId::Macro_8)]  = macros_[7];
    sourceValues_[static_cast<int>(ModSourceId::Macro_9)]  = macros_[8];
    sourceValues_[static_cast<int>(ModSourceId::Macro_10)] = macros_[9];
    sourceValues_[static_cast<int>(ModSourceId::Macro_11)] = macros_[10];
    sourceValues_[static_cast<int>(ModSourceId::Macro_12)] = macros_[11];
    sourceValues_[static_cast<int>(ModSourceId::Macro_13)] = macros_[12];
    sourceValues_[static_cast<int>(ModSourceId::Macro_14)] = macros_[13];
    sourceValues_[static_cast<int>(ModSourceId::Macro_15)] = macros_[14];
    sourceValues_[static_cast<int>(ModSourceId::Macro_16)] = macros_[15];

    // ── Step sequencers ───────────────────────────────────────────────────────
    sourceValues_[static_cast<int>(ModSourceId::StepSeq_1)] = stepSequencers_[0].process(dt, bpm_);
    sourceValues_[static_cast<int>(ModSourceId::StepSeq_2)] = stepSequencers_[1].process(dt, bpm_);

    // ── Drift / random sources ────────────────────────────────────────────────
    // Simple per-block jitter — different seed offsets for each source
    const float rand1 = (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX)) * 2.0f - 1.0f;
    const float rand2 = (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX)) * 2.0f - 1.0f;
    sourceValues_[static_cast<int>(ModSourceId::DriftRandom_1)] = rand1;
    sourceValues_[static_cast<int>(ModSourceId::DriftRandom_2)] = rand2;

    // ── Morph position / fader ────────────────────────────────────────────────
    sourceValues_[static_cast<int>(ModSourceId::MorphX)]   = morphX_;
    sourceValues_[static_cast<int>(ModSourceId::MorphY)]   = morphY_;
    sourceValues_[static_cast<int>(ModSourceId::FaderPos)] = faderPos_;

    // ── MIDI ──────────────────────────────────────────────────────────────────
    sourceValues_[static_cast<int>(ModSourceId::MIDIVelocity)]   = midiVelocity_;
    sourceValues_[static_cast<int>(ModSourceId::MIDIAftertouch)] = midiAftertouch_;
    sourceValues_[static_cast<int>(ModSourceId::MIDIModWheel)]   = midiModWheel_;
}

// ── Main process ──────────────────────────────────────────────────────────────

void ModulationEngine::processBlock(std::vector<float>& morphOutput, float dt) noexcept
{
    if (!prepared_ || morphOutput.empty()) return;

    // 1) Tick all sources and populate sourceValues_
    updateSourceValues(dt);

    // 2) Route and accumulate into morphOutput
    matrix_.apply(sourceValues_, morphOutput);
}

// ── Route management ─────────────────────────────────────────────────────────

int ModulationEngine::addRoute(ModSourceId source, int destParamIndex, float depth)
{
    return matrix_.addRoute(source, destParamIndex, depth);
}

void ModulationEngine::removeRoute(int routeId)
{
    matrix_.removeRoute(routeId);
}

void ModulationEngine::clearAllRoutes()
{
    matrix_.clearAll();
}

int ModulationEngine::getActiveRouteCount() const
{
    return matrix_.getActiveRouteCount();
}

const ModRoute& ModulationEngine::getRoute(int routeId) const
{
    return matrix_.getRoute(routeId);
}

// ── Macro knobs ───────────────────────────────────────────────────────────────

void ModulationEngine::setMacro(int index, float value) noexcept
{
    if (index < 0 || index >= NUM_MACROS) return;
    macros_[static_cast<size_t>(index)] = std::clamp(value, 0.0f, 1.0f);
}

float ModulationEngine::getMacro(int index) const noexcept
{
    if (index < 0 || index >= NUM_MACROS) return 0.0f;
    return macros_[static_cast<size_t>(index)];
}

// ── LFO control ───────────────────────────────────────────────────────────────

void ModulationEngine::setLFOShape(int lfoIndex, LFOShape shape)
{
    if (lfoIndex < 0 || lfoIndex >= NUM_LFOS) return;
    lfos_[static_cast<size_t>(lfoIndex)].setShape(shape);
}

void ModulationEngine::setLFORate(int lfoIndex, float hz)
{
    if (lfoIndex < 0 || lfoIndex >= NUM_LFOS) return;
    lfos_[static_cast<size_t>(lfoIndex)].setRate(hz);
}

void ModulationEngine::setLFOTempoSync(int lfoIndex, bool synced, float bpm)
{
    if (lfoIndex < 0 || lfoIndex >= NUM_LFOS) return;
    auto& lfo = lfos_[static_cast<size_t>(lfoIndex)];
    lfo.setTempoSync(synced);
    lfo.setBPM(bpm);
}

// ── Envelope follower control ─────────────────────────────────────────────────

void ModulationEngine::setEnvelopeAttack(int envIndex, float ms)
{
    if (envIndex < 0 || envIndex >= NUM_ENVELOPES) return;
    envelopes_[static_cast<size_t>(envIndex)].setAttack(ms);
}

void ModulationEngine::setEnvelopeRelease(int envIndex, float ms)
{
    if (envIndex < 0 || envIndex >= NUM_ENVELOPES) return;
    envelopes_[static_cast<size_t>(envIndex)].setRelease(ms);
}

// ── Step sequencer control ────────────────────────────────────────────────────

void ModulationEngine::setStepValue(int seqIndex, int step, float value)
{
    if (seqIndex < 0 || seqIndex >= NUM_STEP_SEQS) return;
    stepSequencers_[static_cast<size_t>(seqIndex)].setStepValue(step, value);
}

void ModulationEngine::setStepCount(int seqIndex, int count)
{
    if (seqIndex < 0 || seqIndex >= NUM_STEP_SEQS) return;
    stepSequencers_[static_cast<size_t>(seqIndex)].setStepCount(count);
}

// ── Direct accessors ──────────────────────────────────────────────────────────

LFO& ModulationEngine::getLFO(int index)
{
    if (index < 0 || index >= NUM_LFOS)
        throw std::out_of_range("ModulationEngine::getLFO — index out of range");
    return lfos_[static_cast<size_t>(index)];
}

EnvelopeFollower& ModulationEngine::getEnvelope(int index)
{
    if (index < 0 || index >= NUM_ENVELOPES)
        throw std::out_of_range("ModulationEngine::getEnvelope — index out of range");
    return envelopes_[static_cast<size_t>(index)];
}

StepSequencer& ModulationEngine::getStepSequencer(int index)
{
    if (index < 0 || index >= NUM_STEP_SEQS)
        throw std::out_of_range("ModulationEngine::getStepSequencer — index out of range");
    return stepSequencers_[static_cast<size_t>(index)];
}

// ── Serialization ─────────────────────────────────────────────────────────────

std::unique_ptr<juce::XmlElement> ModulationEngine::toXml() const
{
    auto root = std::make_unique<juce::XmlElement>("ModulationEngine");
    root->setAttribute("version", 2);

    // ── LFOs ──────────────────────────────────────────────────────────────────
    auto* lfosEl = root->createNewChildElement("LFOs");
    for (int i = 0; i < NUM_LFOS; ++i)
    {
        const LFO& lfo = lfos_[static_cast<size_t>(i)];
        auto* lfoEl = lfosEl->createNewChildElement("LFO");
        lfoEl->setAttribute("index",        i);
        lfoEl->setAttribute("shape",        static_cast<int>(lfo.getShape()));
        lfoEl->setAttribute("rate",         static_cast<double>(lfo.getRate()));
        lfoEl->setAttribute("phaseOffset",  static_cast<double>(lfo.getPhaseOffset()));
        lfoEl->setAttribute("tempoSync",    lfo.getTempoSync() ? 1 : 0);
        lfoEl->setAttribute("bpm",          static_cast<double>(lfo.getBPM()));
        lfoEl->setAttribute("syncDivision", lfo.getSyncDivision());
    }

    // ── Envelope followers ────────────────────────────────────────────────────
    auto* envsEl = root->createNewChildElement("Envelopes");
    for (int i = 0; i < NUM_ENVELOPES; ++i)
    {
        const EnvelopeFollower& env = envelopes_[static_cast<size_t>(i)];
        auto* envEl = envsEl->createNewChildElement("Envelope");
        envEl->setAttribute("index",       i);
        envEl->setAttribute("attackMs",    static_cast<double>(env.getAttack()));
        envEl->setAttribute("releaseMs",   static_cast<double>(env.getRelease()));
        envEl->setAttribute("sensitivity", static_cast<double>(env.getSensitivity()));
    }

    // ── Macro knobs ───────────────────────────────────────────────────────────
    auto* macrosEl = root->createNewChildElement("Macros");
    for (int i = 0; i < NUM_MACROS; ++i)
    {
        auto* macroEl = macrosEl->createNewChildElement("Macro");
        macroEl->setAttribute("index", i);
        macroEl->setAttribute("value", static_cast<double>(macros_[static_cast<size_t>(i)]));
    }

    // ── Step sequencers ───────────────────────────────────────────────────────
    auto* seqsEl = root->createNewChildElement("StepSequencers");
    for (int i = 0; i < NUM_STEP_SEQS; ++i)
    {
        const StepSequencer& seq = stepSequencers_[static_cast<size_t>(i)];
        auto* seqEl = seqsEl->createNewChildElement("StepSequencer");
        seqEl->setAttribute("index",        i);
        seqEl->setAttribute("stepCount",    seq.getStepCount());
        seqEl->setAttribute("rate",         static_cast<double>(seq.getRate()));
        seqEl->setAttribute("smoothing",    static_cast<double>(seq.getSmoothing()));
        seqEl->setAttribute("direction",    static_cast<int>(seq.getDirection()));

        for (int s = 0; s < seq.getStepCount(); ++s)
        {
            auto* stepEl = seqEl->createNewChildElement("Step");
            stepEl->setAttribute("index", s);
            stepEl->setAttribute("value", static_cast<double>(seq.getStepValue(s)));
        }
    }

    // ── Modulation matrix ─────────────────────────────────────────────────────
    auto matrixXml = matrix_.toXml();
    if (matrixXml)
        root->addChildElement(matrixXml.release());

    return root;
}

void ModulationEngine::fromXml(const juce::XmlElement& xml)
{
    if (!xml.hasTagName("ModulationEngine")) return;

    // ── LFOs ──────────────────────────────────────────────────────────────────
    if (auto* lfosEl = xml.getChildByName("LFOs"))
    {
        forEachXmlChildElementWithTagName(*lfosEl, lfoEl, "LFO")
        {
            const int i = lfoEl->getIntAttribute("index", -1);
            if (i < 0 || i >= NUM_LFOS) continue;
            LFO& lfo = lfos_[static_cast<size_t>(i)];
            lfo.setShape(static_cast<LFOShape>(lfoEl->getIntAttribute("shape", 0)));
            lfo.setRate(static_cast<float>(lfoEl->getDoubleAttribute("rate", 1.0)));
            lfo.setPhaseOffset(static_cast<float>(lfoEl->getDoubleAttribute("phaseOffset", 0.0)));
            lfo.setTempoSync(lfoEl->getIntAttribute("tempoSync", 0) != 0);
            lfo.setBPM(static_cast<float>(lfoEl->getDoubleAttribute("bpm", 120.0)));
            lfo.setSyncDivision(lfoEl->getIntAttribute("syncDivision", 4));
        }
    }

    // ── Envelope followers ────────────────────────────────────────────────────
    if (auto* envsEl = xml.getChildByName("Envelopes"))
    {
        forEachXmlChildElementWithTagName(*envsEl, envEl, "Envelope")
        {
            const int i = envEl->getIntAttribute("index", -1);
            if (i < 0 || i >= NUM_ENVELOPES) continue;
            EnvelopeFollower& env = envelopes_[static_cast<size_t>(i)];
            env.setAttack     (static_cast<float>(envEl->getDoubleAttribute("attackMs",    10.0)));
            env.setRelease    (static_cast<float>(envEl->getDoubleAttribute("releaseMs",  100.0)));
            env.setSensitivity(static_cast<float>(envEl->getDoubleAttribute("sensitivity", 1.0)));
        }
    }

    // ── Macro knobs ───────────────────────────────────────────────────────────
    if (auto* macrosEl = xml.getChildByName("Macros"))
    {
        forEachXmlChildElementWithTagName(*macrosEl, macroEl, "Macro")
        {
            const int i = macroEl->getIntAttribute("index", -1);
            if (i < 0 || i >= NUM_MACROS) continue;
            macros_[static_cast<size_t>(i)] =
                std::clamp(static_cast<float>(macroEl->getDoubleAttribute("value", 0.0)), 0.0f, 1.0f);
        }
    }

    // ── Step sequencers ───────────────────────────────────────────────────────
    if (auto* seqsEl = xml.getChildByName("StepSequencers"))
    {
        forEachXmlChildElementWithTagName(*seqsEl, seqEl, "StepSequencer")
        {
            const int i = seqEl->getIntAttribute("index", -1);
            if (i < 0 || i >= NUM_STEP_SEQS) continue;
            StepSequencer& seq = stepSequencers_[static_cast<size_t>(i)];
            seq.setStepCount (seqEl->getIntAttribute("stepCount", 16));
            seq.setRate      (static_cast<float>(seqEl->getDoubleAttribute("rate", 0.25)));
            seq.setSmoothing (static_cast<float>(seqEl->getDoubleAttribute("smoothing", 0.0)));
            seq.setDirection (static_cast<StepSequencer::Direction>(seqEl->getIntAttribute("direction", 0)));

            forEachXmlChildElementWithTagName(*seqEl, stepEl, "Step")
            {
                const int s = stepEl->getIntAttribute("index", -1);
                if (s < 0 || s >= StepSequencer::MAX_STEPS) continue;
                seq.setStepValue(s, static_cast<float>(stepEl->getDoubleAttribute("value", 0.0)));
            }
        }
    }

    // ── Modulation matrix ─────────────────────────────────────────────────────
    if (auto* matrixEl = xml.getChildByName("ModulationMatrix"))
        matrix_.fromXml(*matrixEl);
}

} // namespace morphsnap
