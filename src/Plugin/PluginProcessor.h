/*
 * MorphSnap — Advanced Parameter Morphing Engine
 * PluginProcessor.h — Main VST3 Audio Processor
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "Core/ParameterState.h"
#include "Core/SnapshotBank.h"
#include "Core/InterpolationEngine.h"
#include "Core/MorphProcessor.h"
#include "Core/LockFreeQueue.h"
#include "Host/PluginHostManager.h"
#include "Host/ParameterBridge.h"
#include "MIDI/MIDIRouter.h"
#include "AI/MCPServer.h"

namespace morphsnap {

class MorphSnapProcessor : public juce::AudioProcessor
{
public:
    MorphSnapProcessor();
    ~MorphSnapProcessor() override;

    // ── AudioProcessor interface ─────────────────────────────────────────────
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "MorphSnap"; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override                                     { return 1; }
    int getCurrentProgram() override                                  { return 0; }
    void setCurrentProgram(int) override                              {}
    const juce::String getProgramName(int) override                   { return {}; }
    void changeProgramName(int, const juce::String&) override         {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // ── Public accessors for Editor ──────────────────────────────────────────
    juce::AudioProcessorValueTreeState& getAPVTS()       { return apvts; }
    PluginHostManager& getHostManager()                  { return hostManager; }
    ParameterBridge&   getParameterBridge()               { return paramBridge; }
    SnapshotBank&      getSnapshotBank()                  { return snapshotBank; }
    MorphProcessor&    getMorphProcessor()                 { return morphProcessor; }
    MCPServer&         getMCPServer()                      { return mcpServer; }
    const MCPServer&   getMCPServer() const                { return mcpServer; }

    // ── Morph position atomics (UI/MCP → audio thread) ───────────────────────
    std::atomic<float> morphX{0.5f};
    std::atomic<float> morphY{0.5f};
    std::atomic<float> faderPos{0.0f};
    std::atomic<int>   morphSource{0};   // 0=XYPad, 1=Fader

    // ── Physics mode atomics ─────────────────────────────────────────────────
    std::atomic<int>   physicsMode{0};   // 0=Direct, 1=Elastic, 2=Drift
    std::atomic<int>   elasticPreset{1}; // 0=Slow, 1=Medium, 2=Heavy
    std::atomic<float> driftSpeed{0.3f};
    std::atomic<float> driftDistance{0.4f};
    std::atomic<float> driftChaos{0.5f};
    std::atomic<float> smoothingRate{0.95f};

    // ── Audio analysis (audio → UI) ──────────────────────────────────────────
    std::atomic<float> rmsLevel{0.0f};

    // ── Command queue: MCP/LLM → audio thread ───────────────────────────────
    struct ParamCommand {
        int paramIndex;
        float value;
    };
    LockFreeQueue<ParamCommand, 512> commandQueue;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;
    PluginHostManager  hostManager;
    ParameterBridge    paramBridge;
    SnapshotBank       snapshotBank;
    MorphProcessor     morphProcessor;
    MIDIRouter         midiRouter;
    MCPServer          mcpServer;

    std::vector<float> morphOutput;  // Scratch buffer for interpolated values
    double currentSampleRate = 44100.0;
    int    currentBlockSize  = 512;
    bool   prepared = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MorphSnapProcessor)
};

} // namespace morphsnap
