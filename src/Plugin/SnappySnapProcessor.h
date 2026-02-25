/*
 * SnappySnap - Plugin/SnappySnapProcessor.h
 * Main VST3 audio processor
 */

#pragma once
#include <JuceHeader.h>
#include "../Core/LockFreeQueue.h"
#include "../Core/MorphProcessor.h"
#include "../Core/SnapshotBank.h"
#include "../Host/PluginHostManager.h"
#include "../Host/ParameterBridge.h"
#include "../MIDI/MIDIRouter.h"
#include "../AI/MCPServer.h"
#include "../AI/InstanceIdentity.h"

namespace snap {

/**
 * Main audio processor for SnappySnap VST3 plugin.
 * 
 * Architecture:
 * - Hosts other VST3/AU plugins
 * - Captures/recalls parameter snapshots
 * - Real-time morphing between snapshots
 * - MCP AI integration
 */
class SnappySnapProcessor : public juce::AudioProcessor {
public:
    //==========================================================================
    // Construction
    //==========================================================================
    SnappySnapProcessor();
    ~SnappySnapProcessor() override;
    
    //==========================================================================
    // AudioProcessor Interface
    //==========================================================================
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    
    using AudioProcessor::processBlock;
    
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    
    const juce::String getName() const override { return "SnappySnap"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;
    
    //==========================================================================
    // Component Access
    //==========================================================================
    PluginHostManager& getHostManager() { return hostManager; }
    const PluginHostManager& getHostManager() const { return hostManager; }
    
    ParameterBridge& getParameterBridge() { return paramBridge; }
    const ParameterBridge& getParameterBridge() const { return paramBridge; }
    
    SnapshotBank& getSnapshotBank() { return snapshotBank; }
    const SnapshotBank& getSnapshotBank() const { return snapshotBank; }
    
    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts; }
    const juce::AudioProcessorValueTreeState& getAPVTS() const { return apvts; }
    
    //==========================================================================
    // Morph Control (Thread-Safe)
    //==========================================================================
    void setMorphX(float x) { morphX.store(juce::jlimit(0.0f, 1.0f, x)); }
    void setMorphY(float y) { morphY.store(juce::jlimit(0.0f, 1.0f, y)); }
    void setFaderPos(float pos) { faderPos.store(juce::jlimit(0.0f, 1.0f, pos)); }
    void setMorphSource(MorphSource source) { morphSource.store(static_cast<int>(source)); }
    void setPhysicsMode(MorphMode mode) { physicsMode.store(static_cast<int>(mode)); }
    void setElasticPreset(ElasticPreset preset) { elasticPreset.store(static_cast<int>(preset)); }
    
    float getMorphX() const { return morphX.load(); }
    float getMorphY() const { return morphY.load(); }
    float getFaderPos() const { return faderPos.load(); }
    int getMorphSource() const { return morphSource.load(); }
    int getPhysicsMode() const { return physicsMode.load(); }
    
    //==========================================================================
    // Command Queue (for MCP -> Audio Thread)
    //==========================================================================
    
    /** Enqueues a single parameter change */
    bool enqueueParameterSet(int paramIndex, float normalizedValue);
    
    /** Enqueues a full snapshot recall */
    bool recallSnapshotQueued(int slot);
    
    /** Enqueues multiple parameter changes */
    int enqueueParameterState(const std::vector<float>& normalizedValues);

private:
    //==========================================================================
    // Subsystems
    //==========================================================================
    juce::AudioProcessorValueTreeState apvts;
    PluginHostManager hostManager;
    ParameterBridge paramBridge;
    SnapshotBank snapshotBank;
    MorphProcessor morphProcessor;
    MIDIRouter midiRouter;
    MCPServer mcpServer;
    
    //==========================================================================
    // Atomic Parameters (Thread-Safe for Audio Thread)
    //==========================================================================
    std::atomic<float> morphX{0.5f};
    std::atomic<float> morphY{0.5f};
    std::atomic<float> faderPos{0.0f};
    std::atomic<int> morphSource{0};     // 0=XY, 1=Fader
    std::atomic<int> physicsMode{0};     // 0=Direct, 1=Elastic, 2=Drift
    std::atomic<int> elasticPreset{1};   // 0=Slow, 1=Medium, 2=Heavy
    std::atomic<float> driftSpeed{0.3f};
    std::atomic<float> driftDistance{0.4f};
    std::atomic<float> driftChaos{0.5f};
    std::atomic<float> smoothingRate{0.95f};
    
    //==========================================================================
    // Command Queue (MCP/UI -> Audio Thread)
    //==========================================================================
    struct ParamCommand {
        int paramIndex;
        float value;
    };
    static constexpr int QUEUE_SIZE = 1024;
    LockFreeQueue<ParamCommand, QUEUE_SIZE> commandQueue;
    std::mutex commandQueueProducerMutex;
    
    //==========================================================================
    // Processing State
    //==========================================================================
    std::vector<float> morphOutput;
    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;
    std::atomic<bool> prepared{false};
    
    //==========================================================================
    // Instance Identity
    //==========================================================================
    InstanceIdentity instanceIdentity;
    
    //==========================================================================
    // Private Methods
    //==========================================================================
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SnappySnapProcessor)
};

} // namespace snap
