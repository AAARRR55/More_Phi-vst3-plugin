/*
 * More-Phi — Advanced Parameter Morphing Engine
 * PluginProcessor.h — Main VST3 Audio Processor
 * Version 3.3.0 - Synthesizer Edition
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
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
#include "AI/AIAssistant.h"
#include "Core/ModulationEngine.h"
#include "Core/SpectralMorphEngine.h"
#include "Core/GranularMorphEngine.h"
#include "Core/FormantMorphEngine.h"
#include "Core/HybridBlend.h"
#include "Core/OversamplingWrapper.h"
#include "Core/LatencyManager.h"
#include "Core/PerformanceProfiler.h"
#include "Core/AutoMasteringEngine.h"
#include "AI/NeuralMasteringController.h"
#include "AI/SonicMasterAnalysisEngine.h"
#include "AI/SonicMasterDecisionRunner.h"
#include "AI/SonicMasterHttpInferenceSource.h"
#include "AI/OzoneParameterMap.h"
#include "AI/OzonePlanApplicator.h"
#include "Licensing/LicenseManager.h"
#include <array>
#include <memory>
#include <vector>
// <mutex> removed — all synchronization uses juce::SpinLock for real-time safety

namespace more_phi {

class VST3IPCBridge;

namespace standalone_mcp {
class IZotopeIPCAssistant;
class IZotopeIPCDiscovery;
}

class MorePhiProcessor : public juce::AudioProcessor,
                          private juce::Timer
{
    // Allow MCP tool handlers to reach private helpers that predate the public API.
    friend class MCPToolHandler;
    friend class MCPToolsExtended;

public:
    MorePhiProcessor();
    ~MorePhiProcessor() override;

    // ── AudioProcessor interface ─────────────────────────────────────────────
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) noexcept override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "More-Phi"; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    void reportLatencyToHost();

    // C-5 FIX: VST3 program/preset interface for DAW browser integration
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
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
    VST3IPCBridge*     getVST3IPCBridge() noexcept         { return vst3IpcBridge_.get(); }
    const VST3IPCBridge* getVST3IPCBridge() const noexcept { return vst3IpcBridge_.get(); }
    standalone_mcp::IZotopeIPCDiscovery& getIZotopeIPCDiscovery();
    standalone_mcp::IZotopeIPCAssistant& getIZotopeIPCAssistant();
    const InstanceIdentity& getInstanceIdentity() const   { return instanceIdentity_; }
    licensing::LicenseRuntimeState& getLicenseRuntimeState() noexcept { return licenseRuntimeState_; }
    const licensing::LicenseRuntimeState& getLicenseRuntimeState() const noexcept { return licenseRuntimeState_; }
    licensing::LicenseManager& getLicenseManager() noexcept { return *licenseManager_; }
    const licensing::LicenseManager& getLicenseManager() const noexcept { return *licenseManager_; }

#if defined(MORE_PHI_TEST_MODE) && MORE_PHI_TEST_MODE
    void startPendingMCPServerForTesting() { startMCPServerIfNeeded(); }
#endif
    
    // ── New v3.3.0 accessors ─────────────────────────────────────────────────
    ParameterClassifier&      getParameterClassifier()      { return parameterClassifier_; }
    DiscreteParameterHandler& getDiscreteHandler()          { return discreteHandler_; }
    TokenOptimizer&           getTokenOptimizer()           { return tokenOptimizer_; }
    AIAssistant*              getAIAssistant() noexcept     { return aiAssistant_.get(); }
    const ParameterClassifier& getParameterClassifier() const { return parameterClassifier_; }
    
    // ── V2 accessors ───────────────────────────────────────────────────────
    ModulationEngine&      getModulationEngine()     { return modulationEngine_; }
    SpectralMorphEngine&   getSpectralEngine()       { return spectralEngine_; }
    GranularMorphEngine&   getGranularEngine()       { return granularEngine_; }
    FormantMorphEngine&    getFormantEngine()         { return formantEngine_; }
    OversamplingWrapper&   getOversampling()          { return oversampling_; }
    LatencyManager&        getLatencyManager()        { return latencyManager_; }

    // Audio-domain morph controls
    void setAudioDomainEnabled(bool v)
    {
        audioDomainEnabled_.store(v, std::memory_order_relaxed);
        latencyConfigDirty_.store(true, std::memory_order_release);
        requestMessageThreadMaintenance();
    }
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
    enum class ParameterEditSource : uint8_t
    {
        Unknown = 0,
        UI,
        Assistant,
        MCP,
        Snapshot
    };

    struct ParamCommand {
        int   paramIndex = -1;
        float value = 0.0f;
        bool  isSnapshotMarker = false; // If true, indicates the end of a snapshot recall batch
        int   snapshotSlot = -1;        // Applicable if isSnapshotMarker is true
        ParameterEditSource source = ParameterEditSource::Unknown;
        bool  holdAgainstMorph = false;
    };
    bool enqueueParameterSet(int paramIndex,
                             float normalizedValue,
                             ParameterEditSource source = ParameterEditSource::UI,
                             bool holdAgainstMorph = true);
    bool enqueueParameterBatch(const std::vector<ParamCommand>& commands);
    int enqueueParameterState(const std::vector<float>& normalizedValues,
                              ParameterEditSource source = ParameterEditSource::UI,
                              bool holdAgainstMorph = true);
    bool recallSnapshotQueued(int slot);

    enum class SnapshotRecallMode : uint8_t
    {
        FastParamsOnly = 0,
        FullStateAndParams
    };

    bool captureSnapshotToSlot(int slot, bool includeStateChunk = true);
    bool recallSnapshot(int slot, SnapshotRecallMode mode);

    void setMorphPositionExternal(float x, bool hasX,
                                  float y, bool hasY,
                                  float fader, bool hasFader,
                                  int source);


    // Queue health monitoring (for UI display and debugging)
    float getCommandQueueUsage() const;  // Returns usage as ratio [0,1]
    bool isCommandQueueHealthy() const;   // Returns false if >80% full
    int  getCommandQueueCapacity() const { return static_cast<int>(COMMAND_QUEUE_CAPACITY); }
    size_t getCommandQueueFreeSpaceApprox() const { return commandQueue.freeSpaceApprox(); }
    size_t getPendingParameterCommandCountApprox() const
    {
        const auto free = commandQueue.freeSpaceApprox();
        return free >= LockFreeQueue<ParamCommand, COMMAND_QUEUE_CAPACITY>::usableCapacity()
            ? 0
            : LockFreeQueue<ParamCommand, COMMAND_QUEUE_CAPACITY>::usableCapacity() - free;
    }

    struct ParameterCommandFlushResult
    {
        int pendingBefore = 0;
        int drained = 0;
        int pendingAfter = 0;
        bool pluginUnavailable = false;
        bool exclusiveAccessTimedOut = false;
        int retryCount = 0;
        int waitedMs = 0;
    };

    ParameterCommandFlushResult flushPendingParameterCommandsForAssistant(int maxCommands = 2048,
                                                                          int timeoutMs = 250);

    juce::uint64 getProcessorGenerationToken() const noexcept { return processorGenerationToken_; }
    PerformanceProfiler& getProfiler() { return profiler_; }
    const PerformanceProfiler& getProfiler() const { return profiler_; }
    void resetProfiler() { profiler_.reset(); }
    juce::String getProfilingReport() const;  // Get formatted profiling report
    void dumpProfilingReportToConsole() const; // Log profiling report to DBG output

    // ── Morph state: UI/MCP writes, audio thread reads ───────────────────────
    void  setMorphX(float v)
    {
        const float clamped = juce::jlimit(0.0f, 1.0f, v);
        morphX_.store(clamped, std::memory_order_relaxed);
        if (rawParams_.morphX != nullptr)
            rawParams_.morphX->store(clamped, std::memory_order_relaxed);
    }
    float getMorphX()  const         { return morphX_.load(    std::memory_order_relaxed); }
    void  setMorphY(float v)
    {
        const float clamped = juce::jlimit(0.0f, 1.0f, v);
        morphY_.store(clamped, std::memory_order_relaxed);
        if (rawParams_.morphY != nullptr)
            rawParams_.morphY->store(clamped, std::memory_order_relaxed);
    }
    float getMorphY()  const         { return morphY_.load(    std::memory_order_relaxed); }
    void  setFaderPos(float v)
    {
        const float clamped = juce::jlimit(0.0f, 1.0f, v);
        faderPos_.store(clamped, std::memory_order_relaxed);
        if (rawParams_.faderPos != nullptr)
            rawParams_.faderPos->store(clamped, std::memory_order_relaxed);
    }
    float getFaderPos() const        { return faderPos_.load(  std::memory_order_relaxed); }
    void  setMorphSource(int v)
    {
        const int clamped = juce::jlimit(0, 1, v);
        morphSource_.store(clamped, std::memory_order_relaxed);
        if (rawParams_.morphSource != nullptr)
            rawParams_.morphSource->store(static_cast<float>(clamped), std::memory_order_relaxed);
    }
    int   getMorphSource() const     { return morphSource_.load(std::memory_order_relaxed); }

    // ── Physics: UI writes, audio thread reads ────────────────────────────────
    void  setPhysicsMode(int v)
    {
        const int clamped = juce::jlimit(0, 2, v);
        physicsMode_.store(clamped, std::memory_order_relaxed);
        if (rawParams_.physicsMode != nullptr)
            rawParams_.physicsMode->store(static_cast<float>(clamped), std::memory_order_relaxed);
    }
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
    // m-3 FIX: Copy the config outside the lock to avoid heap allocation
    // (std::set node allocation) while holding the spinlock.
    void setSanityConfig(const SanityConfig& cfg)
    {
        SanityConfig copy = cfg;  // Copy first — may allocate std::set nodes
        const juce::SpinLock::ScopedLockType lock(sanityConfigLock_);
        sanityConfig_ = std::move(copy);  // Move is fast (pointer swap for set)
    }
    SanityConfig getSanityConfigCopy() const
    {
        const juce::SpinLock::ScopedLockType lock(sanityConfigLock_);
        return sanityConfig_;
    }
    SanityConfig getSanityConfig() const { return getSanityConfigCopy(); }

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
    void refreshHostedMasteringApplicators(const juce::PluginDescription& desc);

    // ── Link Mode: cross-instance morph synchronization ─────────────────────
    void setLinkEnabled(bool v)  { linkEnabled_.store(v, std::memory_order_relaxed); }
    bool getLinkEnabled() const  { return linkEnabled_.load(std::memory_order_relaxed); }
    void setLinkLeader(bool v)   { linkBroadcaster_.setLeader(v, juce::String(instanceIdentity_.instanceId).hashCode()); }
    bool isLinkLeader() const    { return linkBroadcaster_.isLeader(); }
    LinkBroadcaster& getLinkBroadcaster() { return linkBroadcaster_; }

    // ── Audio analysis: audio thread writes, UI reads ─────────────────────────
    void  setRmsLevel(float v)       { rmsLevel_.store(v,       std::memory_order_relaxed); }
    float getRmsLevel() const        { return rmsLevel_.load(   std::memory_order_relaxed); }

    // ── Automated mastering engine ────────────────────────────────────────────
    AutoMasteringEngine& getAutoMasteringEngine() noexcept { return autoMasteringEngine_; }
    const AutoMasteringEngine& getAutoMasteringEngine() const noexcept { return autoMasteringEngine_; }
    SonicMasterAnalysisEngine& getSonicMasterEngine() noexcept { return sonicMasterEngine_; }
    const SonicMasterAnalysisEngine& getSonicMasterEngine() const noexcept { return sonicMasterEngine_; }
    NeuralMasteringController& getNeuralMasteringController() noexcept { return neuralMasteringController_; }
    const NeuralMasteringController& getNeuralMasteringController() const noexcept { return neuralMasteringController_; }
    bool hasLastSafeNeuralMasteringPlan() const noexcept { return autoMasteringEngine_.hasLastSafeNeuralMasteringPlan(); }
    const ValidatedNeuralMasteringPlan& getLastSafeNeuralMasteringPlan() const noexcept
    {
        return autoMasteringEngine_.getLastSafeNeuralMasteringPlan();
    }
    NeuralMasteringFallbackMode getNeuralMasteringFallbackMode() const noexcept
    {
        return neuralMasteringController_.getLastStatus().fallbackMode;
    }
    NeuralMasteringEvidenceLevel getNeuralMasteringEvidenceLevel() const noexcept
    {
        return neuralMasteringController_.getLastStatus().evidenceLevel;
    }

    struct TransportContextSnapshot
    {
        bool available = false;
        bool playing = false;
        bool looping = false;
        double bpm = 0.0;
        // M-10 FIX: Time signature is currently read but not consumed by any engine.
        // TODO: Remove these fields if no tempo-synced feature needs them, or wire
        // them up to the modulation/step-sequencer engines.
        int timeSigNumerator = 4;
        int timeSigDenominator = 4;
        double ppqPosition = 0.0;
        double secondsPosition = 0.0;
    };

    TransportContextSnapshot getTransportContextSnapshot() const noexcept
    {
        return {
            transportAvailable_.load(std::memory_order_relaxed),
            transportPlaying_.load(std::memory_order_relaxed),
            transportLooping_.load(std::memory_order_relaxed),
            transportBpm_.load(std::memory_order_relaxed),
            4,  // timeSigNumerator — currently unused (M-10)
            4,  // timeSigDenominator — currently unused (M-10)
            transportPpqPosition_.load(std::memory_order_relaxed),
            transportSecondsPosition_.load(std::memory_order_relaxed)
        };
    }

#if defined(MORE_PHI_TEST_MODE) && MORE_PHI_TEST_MODE
    void testResizeExternalEditHolds(int count)
    {
        const int safeCount = juce::jlimit(0, MAX_PARAMETERS, count);
        liveEditHold_.assign(static_cast<size_t>(safeCount), uint8_t{0});
        liveEditX_.assign(static_cast<size_t>(safeCount), 0.5f);
        liveEditY_.assign(static_cast<size_t>(safeCount), 0.5f);
        liveEditFader_.assign(static_cast<size_t>(safeCount), 0.0f);
    }

    void testMarkExternalEditHold(int paramIndex, float x, float y, float fader)
    {
        if (paramIndex < 0 || static_cast<size_t>(paramIndex) >= liveEditHold_.size())
            return;

        const auto idx = static_cast<size_t>(paramIndex);
        liveEditHold_[idx] = 1;
        liveEditX_[idx] = x;
        liveEditY_[idx] = y;
        liveEditFader_[idx] = fader;
    }

    bool testShouldKeepExternalEditHold(int paramIndex, float x, float y, float fader)
    {
        if (paramIndex < 0 || static_cast<size_t>(paramIndex) >= liveEditHold_.size())
            return false;

        const auto idx = static_cast<size_t>(paramIndex);
        if (liveEditHold_[idx] == 0)
            return false;

        if (!shouldReleaseLiveEditHold(paramIndex, x, y, fader))
            return true;

        liveEditHold_[idx] = 0;
        return false;
    }

    bool testIsExternalEditHeld(int paramIndex) const
    {
        return paramIndex >= 0
            && static_cast<size_t>(paramIndex) < liveEditHold_.size()
            && liveEditHold_[static_cast<size_t>(paramIndex)] != 0;
    }

    void testClearExternalEditHolds(int count)
    {
        if (count <= 0)
            return;

        const int safeCount = juce::jmin(count, static_cast<int>(liveEditHold_.size()));
        std::fill(liveEditHold_.begin(), liveEditHold_.begin() + safeCount, uint8_t{0});
    }
#endif

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;
    PluginHostManager  hostManager;
    ParameterBridge    paramBridge;
    SnapshotBank       snapshotBank;
    MorphProcessor     morphProcessor;
    MIDIRouter         midiRouter;
    MCPServer          mcpServer;
    std::unique_ptr<VST3IPCBridge> vst3IpcBridge_;
    std::unique_ptr<standalone_mcp::IZotopeIPCDiscovery> ipcDiscovery_;
    std::unique_ptr<standalone_mcp::IZotopeIPCAssistant> ipcAssistant_;
    LinkBroadcaster    linkBroadcaster_;
    InstanceIdentity   instanceIdentity_;
    juce::uint64       processorGenerationToken_ = 0;
    licensing::LicenseRuntimeState licenseRuntimeState_;
    std::unique_ptr<licensing::LicenseManager> licenseManager_;

    // ── New v3.3.0 components ────────────────────────────────────────────────
    ParameterClassifier      parameterClassifier_;
    DiscreteParameterHandler discreteHandler_;
    TokenOptimizer           tokenOptimizer_;
    std::unique_ptr<AIAssistant> aiAssistant_;

    // ── Ozone 11 mastering integration ───────────────────────────────────────
    AutoMasteringEngine                  autoMasteringEngine_;
    NeuralMasteringController            neuralMasteringController_;
    std::unique_ptr<OzoneParameterMap>   ozoneParamMap_;
    std::unique_ptr<OzonePlanApplicator> ozonePlanApplicator_;

    // ── SonicMaster realtime neural mastering (preview, default OFF) ──────────
    SonicMasterDecisionRunner            sonicMasterRunner_;
    SonicMasterAnalysisEngine            sonicMasterEngine_;
    SonicMasterRunnerInferenceSource     sonicMasterSource_ { sonicMasterRunner_ };
    SonicMasterHttpInferenceSource       sonicMasterHttpSource_;

    // ── V2 components ──────────────────────────────────────────────────────
    PluginHostManager  hostManagerB_;
    ParameterBridge    paramBridgeB_{hostManagerB_};
    ModulationEngine   modulationEngine_;
    SpectralMorphEngine spectralEngine_;
    GranularMorphEngine granularEngine_;
    FormantMorphEngine  formantEngine_;
    OversamplingWrapper oversampling_;
    OversamplingWrapper oversamplingB_;
    LatencyManager      latencyManager_;

    // Performance profiler for CPU spike diagnosis
    PerformanceProfiler profiler_;

    // Audio-domain morph state
    std::atomic<bool>  audioDomainEnabled_{false};
    std::atomic<float> morphAlpha_{0.0f};
    std::atomic<float> hybridParamWeight_{1.0f};
    std::atomic<float> hybridSpectralWeight_{0.0f};
    std::atomic<float> hybridGranularWeight_{0.0f};
    // Fix 5: tracks whether the formant engine has captured plugin A's
    // pre-morph spectral envelope as its source this engagement. Cleared on
    // formant deactivation so a re-engagement re-captures from current audio.
    bool formantSourceCaptured_ = false;

    // Scratch buffers for audio-domain processing (pre-allocated in prepareToPlay)
    juce::AudioBuffer<float> bufferB_;
    juce::AudioBuffer<float> paramOut_;
    juce::AudioBuffer<float> spectralOut_;
    juce::AudioBuffer<float> granularOut_;
    std::array<float*, OversamplingWrapper::kMaxChannels> osParamPtrs_{};
    std::array<float*, OversamplingWrapper::kMaxChannels> osBPtrs_{};
    // C-P3 FIX: Re-used MIDI buffer — retains capacity across clear() calls
    juce::MidiBuffer midiCopyB_;
    juce::MidiBuffer filteredMidiBuffer_;  // Re-used MIDI buffer — retains capacity across clear() calls
    std::vector<float> finalOutput_;  // After discrete processing

    // PERF-C2: Cached snapshot of current hosted plugin parameter values.
    // Filled once per block via batch getValue() calls, then used for touch
    // detection instead of per-parameter getParameterNormalized() virtual
    // dispatch. Pre-allocated in prepareToPlay. Audio thread only.
    std::vector<float> currentParamSnapshot_;

    // PERF-C3: Previous morph position for early-exit when static.
    // In Direct mode, if position is unchanged for STABLE_THRESHOLD blocks,
    // the interpolation output is identical and we skip recomputation + parameter
    // writes entirely. Reset on any position change or mode switch.
    float prevMorphX_ = -1.0f;
    float prevMorphY_ = -1.0f;
    float prevFaderPos_ = -1.0f;
    int prevPhysicsMode_ = -1;
    int prevMorphSource_ = -1;
    int morphStableBlocks_ = 0;
    static constexpr int MORPH_STABLE_THRESHOLD = 2; // Skip after N identical blocks

    // Touch detection: prevents morph from overwriting manual knob changes
    // CRITICAL: These vectors are accessed from both audio thread (read/write in processBlock)
    // and message thread (write in recallSnapshotQueued). Use spinlock for synchronization.
    std::vector<float> lastApplied_;        // Last morph values we applied
    std::vector<int>   touchCooldown_;      // Per-param cooldown counter (blocks)
    std::vector<float> touchMorphX_;        // Morph X position when touch was detected
    std::vector<float> touchMorphY_;        // Morph Y position when touch was detected
    mutable juce::SpinLock touchStateLock_; // Protects lastApplied_ and touchCooldown_
    static constexpr float TOUCH_THRESHOLD = 0.005f;   // Min delta to detect manual touch
    static constexpr float MORPH_POS_THRESHOLD = 0.01f; // Min morph position change to resume
    // M-6 FIX: Dynamic cooldown — computed in prepareToPlay from sample rate & block size.
    // At 48kHz/512 ≈ 19 blocks, at 96kHz/1024 ≈ 19 blocks, at 44.1k/128 ≈ 69 blocks.
    // Always ~200ms regardless of host configuration.
    int touchCooldownBlocks_ = 10;

    // Live external edits are held against morph output until the user moves
    // the morph cursor or explicitly recalls a snapshot. Audio thread only.
    std::vector<uint8_t> liveEditHold_;
    std::vector<float> liveEditX_;
    std::vector<float> liveEditY_;
    std::vector<float> liveEditFader_;

    void clearLiveEditHoldsAudioThread() noexcept;
    bool shouldReleaseLiveEditHold(int index, float x, float y, float fader) const noexcept;
    int drainParameterCommandQueue(int cachedParamCount,
                                   int maxCommands,
                                   juce::AudioPluginInstance* exclusivePlugin = nullptr) noexcept;
    void drainParameterCommandQueue(int cachedParamCount, bool canTouchHostedParameters) noexcept;
    void processMIDIAndSidechain(juce::MidiBuffer& midi, juce::AudioBuffer<float>& buffer) noexcept;
    void applyMorphAndParameters(juce::AudioBuffer<float>& buffer, int cachedParamCount, bool canTouchHostedParameters) noexcept;
    void applyOutputGainAndMetering(juce::AudioBuffer<float>& buffer, bool isBypassed) noexcept;
    void requestFullStateRecallFromAudioThread(int slot) noexcept;
    // Named capacity constant (avoids magic number in queue declaration).
    static constexpr size_t COMMAND_QUEUE_CAPACITY = 8192;
    LockFreeQueue<ParamCommand, COMMAND_QUEUE_CAPACITY> commandQueue;
    juce::SpinLock commandConsumerLock_;
    double currentSampleRate = 44100.0;
    int    currentBlockSize  = 512;
    std::atomic<bool> prepared{false};
    std::atomic<bool> shuttingDown_{false};
    std::atomic<int> audioThreadActive_{0};

    // Audio thread writes host playhead data; MCP/UI read snapshots only.
    std::atomic<bool> transportAvailable_{false};
    std::atomic<bool> transportPlaying_{false};
    std::atomic<bool> transportLooping_{false};
    std::atomic<double> transportBpm_{0.0};
    std::atomic<double> transportPpqPosition_{0.0};
    std::atomic<double> transportSecondsPosition_{0.0};

    // State restoration guard: blocks morph processing until hosted plugin is fully restored
    std::atomic<bool> isRestoring_{false};
    // Buffered hosted plugin state to apply after async reload completes
    juce::MemoryBlock pendingHostedState_;
    juce::SpinLock pendingStateMutex_;
    // Preserved MCP identity for port reuse across export cycles
    InstanceIdentity pendingIdentity_;
    // Pending plugin description for Timer-based deferred loading
    // (replaces unreliable callAsync — timers work even with editor closed)
    juce::PluginDescription pendingPluginDesc_;
    mutable juce::SpinLock pendingPluginDescLock_;  // H8 FIX: guards pendingPluginDesc_
    std::atomic<bool> hasPendingPluginLoad_{false};
    std::atomic<int>  pendingLoadAttempts_{0};
    std::atomic<int>  pluginLoadRetryCount_{0};
    static constexpr int MAX_PLUGIN_LOAD_RETRIES = 10;  // 500ms total retry window
    
    // Force synchronous load flag for offline rendering contexts
    std::atomic<bool> forceSynchronousLoad_{false};

    /** Synchronous helper: loads hosted plugin from state and restores opaque data.
     *  Returns true if plugin was successfully loaded and state applied.
     */
    bool loadHostedPluginFromState(const juce::PluginDescription& desc);
    void clearHostedMasteringApplicators() noexcept;

    /** Attempts to ensure plugin format manager is ready for loading.
     *  Returns true if we can attempt plugin loading.
     */
    bool ensurePluginFormatsReady();

    /** Timer fallback for deferred plugin loading (fires on message thread). */
    void timerCallback() override;

    /** Sync cached atomics from APVTS values (automation/preset source of truth). */
    void syncStateFromAPVTS();
    void updateTransportContextSnapshot(juce::AudioPlayHead* playHead) noexcept;
    void cacheRawParameterPointers();
    void requestMessageThreadMaintenance() noexcept;
    bool hasPendingMessageThreadWork() const noexcept;
    void loadCachedLicenseIfNeeded();
    void refreshLicenseIfNeeded(); // auto-renew once nextOnlineCheckAtUnix passes
    void startMCPServerIfNeeded();
    void reconfigureAudioDomainProcessing();
    void updateReportedLatency();
    void applyPendingFullStateRecall();

    struct RawParameters
    {
        std::atomic<float>* morphX = nullptr;
        std::atomic<float>* morphY = nullptr;
        std::atomic<float>* faderPos = nullptr;
        std::atomic<float>* morphSource = nullptr;
        std::atomic<float>* physicsMode = nullptr;
        std::atomic<float>* smoothing = nullptr;
        std::atomic<float>* driftSpeed = nullptr;
        std::atomic<float>* driftDistance = nullptr;
        std::atomic<float>* driftChaos = nullptr;
        std::atomic<float>* recallMode = nullptr;
        std::atomic<float>* sidechainEnabled = nullptr;
        std::atomic<float>* sidechainThreshold = nullptr;
        std::atomic<float>* listenMode = nullptr;
        std::atomic<float>* recallToggle = nullptr;
        std::atomic<float>* linkMode = nullptr;
        std::atomic<float>* spectralActive = nullptr;
        std::atomic<float>* spectralFFTSize = nullptr;
        std::atomic<float>* spectralTransient = nullptr;
        std::atomic<float>* spectralFormant = nullptr;
        std::atomic<float>* granularActive = nullptr;
        std::atomic<float>* grainSize = nullptr;
        std::atomic<float>* grainDensity = nullptr;
        std::atomic<float>* grainPitch = nullptr;
        std::atomic<float>* grainScatter = nullptr;
        std::atomic<float>* audioDomainEnabled = nullptr;
        std::atomic<float>* oversampling = nullptr;
        std::atomic<float>* blendParamWeight = nullptr;
        std::atomic<float>* blendSpectralWeight = nullptr;
        std::atomic<float>* blendGranularWeight = nullptr;
        std::atomic<float>* morphAlpha = nullptr;
        std::atomic<float>* bypass = nullptr;
        std::atomic<float>* outputGain = nullptr;
        std::atomic<float>* driftOutputX = nullptr;
        std::atomic<float>* driftOutputY = nullptr;
        std::atomic<float>* coarseParameterWrites = nullptr;
        std::atomic<float>* disableTouchDetection = nullptr;
        std::atomic<float>* throttleParamCommits = nullptr;
    };
    RawParameters rawParams_{};

    // Morph position (UI/MCP → audio thread)
    std::atomic<float> morphX_{0.5f};
    std::atomic<float> morphY_{0.5f};
    std::atomic<float> faderPos_{0.0f};
    std::atomic<int>   morphSource_{0};

    std::atomic<bool> mcpStartPending_{false};
    std::atomic<bool> maintenanceTimerRequested_{false};
    std::atomic<bool> licenseLoadPending_{true};
    std::atomic<bool> licenseRefreshInFlight_{false}; // guards background refresh

    std::atomic<bool> audioDomainConfigDirty_{false};
    std::atomic<bool> latencyConfigDirty_{false};
    std::atomic<bool> audioDomainReconfiguring_{false};
    std::atomic<int>  audioDomainUsers_{0};
    std::atomic<int>  desiredOversamplingFactor_{1};
    std::atomic<int>  activeOversamplingFactor_{1};
    std::atomic<int>  desiredSpectralFFTSize_{2048};
    std::atomic<int>  activeSpectralFFTSize_{2048};
    bool lastLatencyAudioDomainEnabled_ = false;
    bool lastLatencySpectralActive_ = false;
    bool lastLatencyGranularActive_ = false;

    std::atomic<int> pendingFullStateRecallSlot_{-1};
    std::atomic<uint32_t> pendingFullStateRecallGeneration_{0};
    uint32_t appliedFullStateRecallGeneration_ = 0;
    std::atomic<int> fullStateRecallRetryCount_{0};
    static constexpr int MAX_FULL_STATE_RECALL_RETRIES = 10;  // H8 FIX: prevent infinite loops on failure

    // Physics modes (UI → audio thread)
    std::atomic<int>   physicsMode_{0};
    std::atomic<int>   elasticPreset_{1};
    std::atomic<float> driftSpeed_{0.3f};
    std::atomic<float> driftDistance_{0.4f};
    std::atomic<float> driftChaos_{0.5f};
    std::atomic<float> smoothingRate_{0.95f};

    // PERF-OPT opt-in flags (default OFF — preserve current validated behavior).
    // Coarse Parameter Writes: raise the apply-loop write deadband (1e-5 -> 5e-4)
    // so fewer setValue() calls fire during slow morphs.
    // Disable Touch Detect: skip the per-block batch getValue() read and the
    // touch/hold/cooldown bookkeeping (pure morph output application).
    std::atomic<bool> coarseParameterWrites_{false};
    std::atomic<bool> disableTouchDetection_{false};

    // Throttle Param Commits: compute morph every block but push setValue to the
    // hosted plugin only every Nth block (Drift/continuous-morph CPU relief).
    std::atomic<bool> throttleParamCommits_{false};
    int paramCommitCounter_{0}; // audio-thread only

    // Audio analysis (audio thread → UI)
    std::atomic<float> rmsLevel_{0.0f};
    int rmsSkipCounter_ = 0;
    static constexpr int RMS_THROTTLE_BLOCKS = 8;

    // M9 FIX: Output gain smoothing state (audio thread only)
    std::atomic<float> smoothedGain_{1.0f};
    bool gainSmoothingInitialized_ = false;

    // M11 FIX: Atomic flag for deferred state restore from audio thread
    std::atomic<bool> pendingStateRestore_{false};

    // SanityMode config (UI writes, breed/randomize reads)
    // CRITICAL (Finding 3): std::set<int> is not thread-safe, protect with spinlock
    SanityConfig sanityConfig_;
    mutable juce::SpinLock sanityConfigLock_;

    // RecallMode (UI → audio thread)
    std::atomic<int> recallMode_{0};   // 0=Fast, 1=Full

    // Sidechain trigger (UI → audio thread)
    std::atomic<bool> sidechainEnabled_{false};
    std::atomic<float> sidechainThreshold_{-20.0f};
    std::atomic<float> sidechainThresholdLinear_{0.1f};  // ATS-5: pre-computed from dB in syncStateFromAPVTS

    // Listen Mode (UI → audio thread + MorphProcessor)
    std::atomic<bool> listenMode_{false};

    // Recall Toggle: 1=full recall, 0=params-only (for sustained notes)
    std::atomic<int> recallToggle_{1};

    // Link Mode (UI → audio thread)
    std::atomic<bool> linkEnabled_{false};

    // C-5 FIX: Track currently selected program for DAW preset browser
    std::atomic<int> currentProgram_{0};

    JUCE_DECLARE_WEAK_REFERENCEABLE(MorePhiProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MorePhiProcessor)
};

} // namespace more_phi
