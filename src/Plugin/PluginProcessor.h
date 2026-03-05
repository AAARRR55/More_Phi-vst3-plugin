/*
 * MorphSnap — Advanced Parameter Morphing Engine
 * PluginProcessor.h — Main VST3 Audio Processor
 * Version 3.3.0 - Synthesizer Edition
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "Core/ParameterState.h"
#include "Core/SnapshotBank.h"
#include "Core/InterpolationEngine.h"
#include "Core/MorphProcessor.h"
#include "Core/GeneticEngine.h"
#include "Core/LockFreeQueue.h"
#include "Core/ParameterClassifier.h"
#include "Core/DiscreteParameterHandler.h"
#include "Host/PluginHostManager.h"
#include "Host/ParameterBridge.h"
#include "MIDI/MIDIRouter.h"
#include "AI/MCPServer.h"
#include "AI/InstanceIdentity.h"
#include "AI/LinkBroadcaster.h"
#include "AI/TokenOptimizer.h"
#include "Core/ModulationEngine.h"
#include "Core/SpectralMorphEngine.h"
#include "Core/GranularMorphEngine.h"
#include "Core/FormantMorphEngine.h"
#include "Core/HybridBlend.h"
#include "Core/VAEMorphEngine.h"
#include "Core/OversamplingWrapper.h"
#include "Core/LatencyManager.h"
#include <vector>
#include <mutex>

namespace morphsnap {

class MorphSnapProcessor : public juce::AudioProcessor,
                          private juce::Timer
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
    const InstanceIdentity& getInstanceIdentity() const   { return instanceIdentity_; }
    
    // ── New v3.3.0 accessors ─────────────────────────────────────────────────
    ParameterClassifier&      getParameterClassifier()      { return parameterClassifier_; }
    DiscreteParameterHandler& getDiscreteHandler()          { return discreteHandler_; }
    TokenOptimizer&           getTokenOptimizer()           { return tokenOptimizer_; }
    const ParameterClassifier& getParameterClassifier() const { return parameterClassifier_; }
    
    // Learn Mode integration
    void refreshParameterClassification();
    void recordParameterModification(int paramIndex);
    
    // Morph compatibility checking
    bool areSnapshotsCompatible(int slotA, int slotB) const;
    juce::String getMorphCompatibilityReport(int slotA, int slotB) const;

    // ── V2 accessors ───────────────────────────────────────────────────────
    ModulationEngine&      getModulationEngine()     { return modulationEngine_; }
    SpectralMorphEngine&   getSpectralEngine()       { return spectralEngine_; }
    GranularMorphEngine&   getGranularEngine()       { return granularEngine_; }
    FormantMorphEngine&    getFormantEngine()         { return formantEngine_; }
    VAEMorphEngine&        getVAEEngine()             { return vaeEngine_; }
    OversamplingWrapper&   getOversampling()          { return oversampling_; }
    LatencyManager&        getLatencyManager()        { return latencyManager_; }
    PluginHostManager&     getHostManagerB()          { return hostManagerB_; }
    ParameterBridge&       getParameterBridgeB()      { return paramBridgeB_; }

    // Audio-domain morph controls
    void setAudioDomainEnabled(bool v) { audioDomainEnabled_.store(v, std::memory_order_relaxed); }
    bool getAudioDomainEnabled() const { return audioDomainEnabled_.load(std::memory_order_relaxed); }
    void setMorphAlpha(float v) { morphAlpha_.store(v, std::memory_order_relaxed); }
    float getMorphAlpha() const { return morphAlpha_.load(std::memory_order_relaxed); }
    void setHybridParamWeight(float v) { hybridParamWeight_.store(v, std::memory_order_relaxed); }
    float getHybridParamWeight() const { return hybridParamWeight_.load(std::memory_order_relaxed); }
    void setHybridSpectralWeight(float v) { hybridSpectralWeight_.store(v, std::memory_order_relaxed); }
    float getHybridSpectralWeight() const { return hybridSpectralWeight_.load(std::memory_order_relaxed); }
    void setHybridGranularWeight(float v) { hybridGranularWeight_.store(v, std::memory_order_relaxed); }
    float getHybridGranularWeight() const { return hybridGranularWeight_.load(std::memory_order_relaxed); }

    // Thread-safe requests (non-audio threads -> audio thread queue)
    struct ParamCommand {
        int paramIndex;
        float value;
    };
    bool enqueueParameterSet(int paramIndex, float normalizedValue);
    int enqueueParameterState(const std::vector<float>& normalizedValues);
    bool recallSnapshotQueued(int slot);

    // ── Morph state: UI/MCP writes, audio thread reads ───────────────────────
    void  setMorphX(float v)         { morphX_.store(v,        std::memory_order_relaxed); }
    float getMorphX()  const         { return morphX_.load(    std::memory_order_relaxed); }
    void  setMorphY(float v)         { morphY_.store(v,        std::memory_order_relaxed); }
    float getMorphY()  const         { return morphY_.load(    std::memory_order_relaxed); }
    void  setFaderPos(float v)       { faderPos_.store(v,      std::memory_order_relaxed); }
    float getFaderPos() const        { return faderPos_.load(  std::memory_order_relaxed); }
    void  setMorphSource(int v)      { morphSource_.store(v,   std::memory_order_relaxed); }
    int   getMorphSource() const     { return morphSource_.load(std::memory_order_relaxed); }

    // ── Physics: UI writes, audio thread reads ────────────────────────────────
    void  setPhysicsMode(int v)      { physicsMode_.store(v,    std::memory_order_relaxed); }
    int   getPhysicsMode() const     { return physicsMode_.load(std::memory_order_relaxed); }
    void  setElasticPreset(int v)    { elasticPreset_.store(v,  std::memory_order_relaxed); }
    int   getElasticPreset() const   { return elasticPreset_.load(std::memory_order_relaxed); }
    void  setDriftSpeed(float v)     { driftSpeed_.store(v,     std::memory_order_relaxed); }
    float getDriftSpeed() const      { return driftSpeed_.load( std::memory_order_relaxed); }
    void  setDriftDistance(float v)  { driftDistance_.store(v,  std::memory_order_relaxed); }
    float getDriftDistance() const   { return driftDistance_.load(std::memory_order_relaxed); }
    void  setDriftChaos(float v)     { driftChaos_.store(v,     std::memory_order_relaxed); }
    float getDriftChaos() const      { return driftChaos_.load( std::memory_order_relaxed); }
    void  setSmoothingRate(float v)  { smoothingRate_.store(v,  std::memory_order_relaxed); }
    float getSmoothingRate() const   { return smoothingRate_.load(std::memory_order_relaxed); }

    // ── Sanity Mode: protects critical params from breed/randomize ────────────
    // CRITICAL (Finding 3): SanityConfig contains std::set which is not thread-safe.
    // All access must go through these synchronized methods.
    void setSanityConfig(const SanityConfig& cfg)
    {
        const juce::SpinLock::ScopedLockType lock(sanityConfigLock_);
        sanityConfig_ = cfg;
    }
    SanityConfig getSanityConfigCopy() const
    {
        const juce::SpinLock::ScopedLockType lock(sanityConfigLock_);
        return sanityConfig_;
    }
    // Legacy API for message-thread-only access (UI components)
    const SanityConfig& getSanityConfig() const   { return sanityConfig_; }
    SanityConfig& getSanityConfig()               { return sanityConfig_; }

    // ── Recall Mode: Fast (params only) vs Full (params + state chunks) ──────
    void setRecallMode(int v)        { recallMode_.store(v,     std::memory_order_relaxed); }
    int  getRecallMode() const       { return recallMode_.load( std::memory_order_relaxed); }

    // ── Sidechain trigger: audio-driven snapshot recall ──────────────────────
    void setSidechainEnabled(bool v) { sidechainEnabled_.store(v, std::memory_order_relaxed); }
    bool getSidechainEnabled() const { return sidechainEnabled_.load(std::memory_order_relaxed); }
    void setSidechainThreshold(float v) { sidechainThreshold_.store(v, std::memory_order_relaxed); }
    float getSidechainThreshold() const { return sidechainThreshold_.load(std::memory_order_relaxed); }

    // ── Listen Mode: exclude discrete params from morphing ────────────────────
    void setListenMode(bool v)       { listenMode_.store(v, std::memory_order_relaxed); }
    bool getListenMode() const       { return listenMode_.load(std::memory_order_relaxed); }

    // ── Recall Toggle: full state vs params-only during MIDI triggers ─────────
    void setRecallToggle(bool full)  { recallToggle_.store(full ? 1 : 0, std::memory_order_relaxed); }
    bool getRecallToggle() const     { return recallToggle_.load(std::memory_order_relaxed) != 0; }

    // Refresh discrete parameter map (call after plugin load/change)
    void refreshDiscreteMap();

    // ── Link Mode: cross-instance morph synchronization ─────────────────────
    void setLinkEnabled(bool v)  { linkEnabled_.store(v, std::memory_order_relaxed); }
    bool getLinkEnabled() const  { return linkEnabled_.load(std::memory_order_relaxed); }
    void setLinkLeader(bool v)   { linkBroadcaster_.setLeader(v, juce::String(instanceIdentity_.instanceId).hashCode()); }
    bool isLinkLeader() const    { return linkBroadcaster_.isLeader(); }
    LinkBroadcaster& getLinkBroadcaster() { return linkBroadcaster_; }

    // ── Audio analysis: audio thread writes, UI reads ─────────────────────────
    void  setRmsLevel(float v)       { rmsLevel_.store(v,       std::memory_order_relaxed); }
    float getRmsLevel() const        { return rmsLevel_.load(   std::memory_order_relaxed); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;
    PluginHostManager  hostManager;
    ParameterBridge    paramBridge;
    SnapshotBank       snapshotBank;
    MorphProcessor     morphProcessor;
    MIDIRouter         midiRouter;
    MCPServer          mcpServer;
    LinkBroadcaster    linkBroadcaster_;
    InstanceIdentity   instanceIdentity_;
    
    // ── New v3.3.0 components ────────────────────────────────────────────────
    ParameterClassifier      parameterClassifier_;
    DiscreteParameterHandler discreteHandler_;
    TokenOptimizer           tokenOptimizer_;

    // ── V2 components ──────────────────────────────────────────────────────
    PluginHostManager  hostManagerB_;
    ParameterBridge    paramBridgeB_{hostManagerB_};
    ModulationEngine   modulationEngine_;
    SpectralMorphEngine spectralEngine_;
    GranularMorphEngine granularEngine_;
    FormantMorphEngine  formantEngine_;
    VAEMorphEngine      vaeEngine_;
    OversamplingWrapper oversampling_;
    LatencyManager      latencyManager_;

    // Audio-domain morph state
    std::atomic<bool>  audioDomainEnabled_{false};
    std::atomic<float> morphAlpha_{0.0f};
    std::atomic<float> hybridParamWeight_{1.0f};
    std::atomic<float> hybridSpectralWeight_{0.0f};
    std::atomic<float> hybridGranularWeight_{0.0f};

    // Scratch buffers for audio-domain processing (pre-allocated in prepareToPlay)
    juce::AudioBuffer<float> bufferB_;
    juce::AudioBuffer<float> spectralOut_;
    juce::AudioBuffer<float> granularOut_;

    std::vector<float> morphOutput;
    std::vector<float> finalOutput_;  // After discrete processing

    // Touch detection: prevents morph from overwriting manual knob changes
    // CRITICAL: These vectors are accessed from both audio thread (read/write in processBlock)
    // and message thread (write in recallSnapshotQueued). Use spinlock for synchronization.
    std::vector<float> lastApplied_;        // Last morph values we applied
    std::vector<int>   touchCooldown_;      // Per-param cooldown counter (blocks)
    mutable juce::SpinLock touchStateLock_; // Protects lastApplied_ and touchCooldown_
    static constexpr float TOUCH_THRESHOLD = 0.005f;   // Min delta to detect manual touch
    static constexpr int   TOUCH_COOLDOWN_BLOCKS = 10; // ~200ms at 48kHz/1024
    // Named capacity constant (avoids magic number in queue declaration).
    static constexpr size_t COMMAND_QUEUE_CAPACITY = 8192;
    LockFreeQueue<ParamCommand, COMMAND_QUEUE_CAPACITY> commandQueue;
    std::mutex commandQueueProducerMutex_;
    double currentSampleRate = 44100.0;
    int    currentBlockSize  = 512;
    std::atomic<bool> prepared{false};

    // State restoration guard: blocks morph processing until hosted plugin is fully restored
    std::atomic<bool> isRestoring_{false};
    // Buffered hosted plugin state to apply after async reload completes
    juce::MemoryBlock pendingHostedState_;
    std::mutex pendingStateMutex_;
    // Preserved MCP identity for port reuse across export cycles
    InstanceIdentity pendingIdentity_;
    // Pending plugin description for Timer-based deferred loading
    // (replaces unreliable callAsync — timers work even with editor closed)
    juce::PluginDescription pendingPluginDesc_;
    bool hasPendingPluginLoad_{false};
    int  pendingLoadAttempts_{0};  // retry counter (max 5 attempts, 250ms total)
    int pluginLoadRetryCount_{0};
    static constexpr int MAX_PLUGIN_LOAD_RETRIES = 10;  // 500ms total retry window
    
    // Force synchronous load flag for offline rendering contexts
    std::atomic<bool> forceSynchronousLoad_{false};

    /** Synchronous helper: loads hosted plugin from state and restores opaque data.
     *  Returns true if plugin was successfully loaded and state applied.
     */
    bool loadHostedPluginFromState(const juce::PluginDescription& desc);
    
    /** Attempts to ensure plugin format manager is ready for loading.
     *  Returns true if we can attempt plugin loading.
     */
    bool ensurePluginFormatsReady();

    /** Timer fallback for deferred plugin loading (fires on message thread). */
    void timerCallback() override;

    // Morph position (UI/MCP → audio thread)
    std::atomic<float> morphX_{0.5f};
    std::atomic<float> morphY_{0.5f};
    std::atomic<float> faderPos_{0.0f};
    std::atomic<int>   morphSource_{0};

    // Physics modes (UI → audio thread)
    std::atomic<int>   physicsMode_{0};
    std::atomic<int>   elasticPreset_{1};
    std::atomic<float> driftSpeed_{0.3f};
    std::atomic<float> driftDistance_{0.4f};
    std::atomic<float> driftChaos_{0.5f};
    std::atomic<float> smoothingRate_{0.95f};

    // Audio analysis (audio thread → UI)
    std::atomic<float> rmsLevel_{0.0f};

    // SanityMode config (UI writes, breed/randomize reads)
    // CRITICAL (Finding 3): std::set<int> is not thread-safe, protect with spinlock
    SanityConfig sanityConfig_;
    mutable juce::SpinLock sanityConfigLock_;

    // RecallMode (UI → audio thread)
    std::atomic<int> recallMode_{0};   // 0=Fast, 1=Full

    // Sidechain trigger (UI → audio thread)
    std::atomic<bool> sidechainEnabled_{false};
    std::atomic<float> sidechainThreshold_{-20.0f};

    // Listen Mode (UI → audio thread + MorphProcessor)
    std::atomic<bool> listenMode_{false};

    // Recall Toggle: 1=full recall, 0=params-only (for sustained notes)
    std::atomic<int> recallToggle_{1};

    // Link Mode (UI → audio thread)
    std::atomic<bool> linkEnabled_{false};

    JUCE_DECLARE_WEAK_REFERENCEABLE(MorphSnapProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MorphSnapProcessor)
};

} // namespace morphsnap
