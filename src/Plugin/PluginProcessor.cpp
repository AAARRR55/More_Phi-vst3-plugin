/*
 * More-Phi — Advanced Parameter Morphing Engine
 * PluginProcessor.cpp — Main VST3 Audio Processor Implementation
 */
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Version.h"   // VERSION_STRING — single source of truth for the state-version literal
#if MORE_PHI_HAS_ONNX
#  include "SonicMasterModelHash.h"   // generated: more_phi::sonicmaster::kExpectedModelHash (W8 integrity check)
#  include <juce_cryptography/juce_cryptography.h>  // juce::SHA256 for the model integrity check
#endif
#include "Core/InterpolationEngine.h"
#include "Core/OversamplingWrapper.h"
#include "Core/ABCompareEngine.h"
#include "AI/InstanceRegistry.h"
#include "AI/OzoneParameterMap.h"
#include "AI/OzonePlanApplicator.h"
#include "AI/StandaloneMcp/MorePhiIPCAssistant.h"
#include "AI/StandaloneMcp/MorePhiIPCDiscovery.h"
#include "AI/VST3IPCBridge.h"
#include "AI/Agents/AgentRuntime.h"
#include "AI/Agents/Blackboard/BlackboardBridge.h"
#include "AI/Agents/Tooling/DefaultToolInvoker.h"
#include "AI/Agents/Logging/StructuredAgentLogger.h"
#include "AI/Agents/Llm/DeterministicFallbackLlmClient.h"
#include "AI/Agents/Llm/RestLlmClient.h"          // AUDIT-FIX: real-LLM seam for the agent layer
#include "AI/LLMSettingsStore.h"                   // AUDIT-FIX: load provider config for RestLlmClient
#include "AI/LLMConnectionValidator.h"             // AUDIT-FIX: JuceLLMHttpClient
#include "AI/AutomationControlPlane.h"   // H6: AutonomyLevel / toString / autonomyLevelFromString
#include "AI/Agents/Conductor/ConductorAgent.h"
#include "AI/Agents/Agents/AnalysisAgent.h"
#include "AI/Agents/Agents/OptimizationAgent.h"
#include "AI/Agents/Agents/CreativeAgent.h"
#include "AI/Agents/Agents/RealtimeControlAgent.h"
#include "AI/Agents/Agents/QualitySafetyAgent.h"
#include "AI/Agents/Agents/MemoryAgent.h"
#include "AI/MCPToolHandler.h"
#include "AI/Dataset/NeuralMasteringFeatureExtractor.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>

namespace more_phi {

namespace {

std::atomic<juce::uint64> gNextProcessorGenerationToken{1};

// AUDIT (E2, 2026-06-25): map the drain-side ParameterEditSource (defined here in
// PluginProcessor.h) to the HostedWriteSource enum ParameterBridge stamps. The two
// enums are intentionally separate so ParameterBridge.h need not depend on
// PluginProcessor.h (which owns a ParameterBridge — a circular include otherwise).
// The values line up 1:1 by intent.
HostedWriteSource toHostedWriteSource(MorePhiProcessor::ParameterEditSource s) noexcept
{
    switch (s)
    {
        case MorePhiProcessor::ParameterEditSource::UI:        return HostedWriteSource::UI;
        case MorePhiProcessor::ParameterEditSource::Assistant: return HostedWriteSource::Assistant;
        case MorePhiProcessor::ParameterEditSource::MCP:       return HostedWriteSource::MCP;
        case MorePhiProcessor::ParameterEditSource::Snapshot:  return HostedWriteSource::Snapshot;
        case MorePhiProcessor::ParameterEditSource::Neural:    return HostedWriteSource::Neural;
        case MorePhiProcessor::ParameterEditSource::Unknown:   break;
    }
    return HostedWriteSource::Unknown;
}

float readRawFloat(const std::atomic<float>* p, float fallback) noexcept
{
    return p != nullptr ? p->load(std::memory_order_relaxed) : fallback;
}

// L-1 DOC: Returns true when the raw parameter value is >= 0.5f.
// JUCE AudioParameterBool stores 0.0f or 1.0f, but automation curves can
// produce intermediate values during ramps. The 0.5 threshold provides
// hysteresis: a parameter ramp from 0→1 will be treated as "on" only for
// the second half of the ramp duration, preventing rapid toggling.
bool readRawBool(const std::atomic<float>* p, bool fallback) noexcept
{
    return p != nullptr ? p->load(std::memory_order_relaxed) >= 0.5f : fallback;
}

int readRawChoice(const std::atomic<float>* p, int fallback) noexcept
{
    return p != nullptr ? juce::roundToInt(p->load(std::memory_order_relaxed)) : fallback;
}

OversamplingFactor factorFromInt(int factor) noexcept
{
    switch (factor)
    {
        case 2: return OversamplingFactor::x2;
        case 4: return OversamplingFactor::x4;
        case 8: return OversamplingFactor::x8;
        default: return OversamplingFactor::x1;
    }
}

int factorFromChoiceIndex(int index) noexcept
{
    constexpr int factors[] = {1, 2, 4, 8};
    return factors[juce::jlimit(0, 3, index)];
}

int fftSizeFromChoiceIndex(int index) noexcept
{
    constexpr int fftSizes[] = {512, 1024, 2048, 4096};
    return fftSizes[juce::jlimit(0, 3, index)];
}

void copyBufferRegion(juce::AudioBuffer<float>& dst,
                      const juce::AudioBuffer<float>& src,
                      int numSamples) noexcept
{
    const int channels = std::min(dst.getNumChannels(), src.getNumChannels());
    const int samples = std::min({dst.getNumSamples(), src.getNumSamples(), numSamples});
    for (int ch = 0; ch < channels; ++ch)
        juce::FloatVectorOperations::copy(dst.getWritePointer(ch), src.getReadPointer(ch), samples);
}

juce::AudioBuffer<float> makeBlockBuffer(juce::dsp::AudioBlock<float>& block,
                                         std::array<float*, OversamplingWrapper::kMaxChannels>& ptrs) noexcept
{
    const int channels = juce::jmin(static_cast<int>(block.getNumChannels()),
                                    OversamplingWrapper::kMaxChannels);
    for (int ch = 0; ch < channels; ++ch)
        ptrs[static_cast<size_t>(ch)] = block.getChannelPointer(static_cast<size_t>(ch));

    return { ptrs.data(), channels, static_cast<int>(block.getNumSamples()) };
}

} // namespace

MorePhiProcessor::MorePhiProcessor()
    : AudioProcessor(BusesProperties()
          .withInput ("Input",    juce::AudioChannelSet::stereo(), true)
          .withOutput("Output",   juce::AudioChannelSet::stereo(), true)
          .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts(*this, nullptr, "MORE_PHI_STATE", createParameterLayout()),
      paramBridge(hostManager),
      morphProcessor(snapshotBank),
      mcpServer(*this),
      ipcDiscovery_(standalone_mcp::createMorePhiIPCDiscovery()),
      ipcAssistant_(standalone_mcp::createMorePhiIPCAssistant()),
      diagnostics_(*this, profiler_)
{
    processorGenerationToken_ = gNextProcessorGenerationToken.fetch_add(1, std::memory_order_relaxed);
    // Constructor is kept minimal for FL Studio validation.
    pendingAgentAutonomy_ = more_phi::AutonomyLevel::Assist;   // H6: default until restored from state
    cacheRawParameterPointers();
    // BP-3 FIX (audit): cache the bypass parameter for getBypassParameter().
    bypassParameter_ = apvts.getParameter("bypass");
    licenseManager_ = std::make_shared<licensing::LicenseManager>(licenseRuntimeState_);
    requestMessageThreadMaintenance();

    // SonicMaster realtime neural mastering (preview): drive the analysis loop.
    // Tries ONNX in-process inference first; falls back to the local Python
    // inference server (HTTP) when the ONNX model is not staged alongside the
    // plugin binary. See tools/inference_server/README.md and
    // tools/export_onnx/export_patched.py.
    initializeSonicMaster();

    // V2: wire the feature→delta ONNX path (63→72 model). Searches alongside
    // the plugin binary for a matching ONNX model; gracefully falls through to
    // DeterministicBaseline if none is found (current behaviour unchanged).
    initializeNeuralMasteringV2();

    // Wire MIDI router callbacks (plain C function pointers + void* context)
    // H-5: Store a WeakReference on the heap so the callback can detect if the
    // processor is destroyed before the callback fires. The weak ref member is
    // cleaned up in the destructor.
    midiCallbackWeakRef_ = new juce::WeakReference<MorePhiProcessor>(this);
    midiRouter.setSnapshotCallback([](int slot, void* ctx)
    {
        auto* weakRef = static_cast<juce::WeakReference<MorePhiProcessor>*>(ctx);
        auto* self = weakRef->get();
        if (!self) return;
        if (self->snapshotBank.isOccupied(slot))
        {
            self->clearLiveEditHoldsAudioThread();

            if (self->getRecallToggle())
                self->requestFullStateRecallFromAudioThread(slot);

            // MIDI callbacks already run on the audio thread, so parameter
            // recall is applied here. Opaque state recall is deferred above.
            // C-5 FIX (audit): recallFast snapped every hosted parameter in one
            // block → audible click on continuous params (gain/cutoff). Route
            // through paramBridge.startRecallRamp() instead, which captures the
            // current values as a ramp start and linearly ramps to the snapshot
            // target over kRecallRampBlocks blocks (advanced each block by
            // processRecallRamp in processBlock). Falls back to the instant
            // recallFast only if the ramp couldn't start (no hosted plugin).
            if (!self->snapshotBank.getSlotValuesCopy(slot, self->recallScratch_)
                || !self->paramBridge.startRecallRamp(self->recallScratch_.data(),
                                                      static_cast<int>(self->recallScratch_.size())))
            {
                self->snapshotBank.recallFast(slot, self->paramBridge);
            }
            // C4 FIX: drop the apply cache so this block's morph pass treats the
            // recalled values as the new baseline instead of overwriting them.
            self->invalidateAppliedCacheAudioThread();
        }
    }, midiCallbackWeakRef_);

    midiRouter.setMorphCallback([](float value, void* ctx)
    {
        auto* weakRef = static_cast<juce::WeakReference<MorePhiProcessor>*>(ctx);
        auto* self = weakRef->get();
        if (!self) return;
        self->setFaderPos(value);
        self->setMorphSource(1);  // Switch to fader mode
    }, midiCallbackWeakRef_);

    // Attach to shared memory for Link Mode
    linkBroadcaster_.attach(0);  // Default link group

    neuralMasteringController_.setApplicationEngine(&autoMasteringEngine_);
    neuralMasteringController_.setModelRunner(&onnxNeuralRunner_);
}

// ── SonicMaster initialization: ONNX-first with HTTP fallback ─────────────────

void MorePhiProcessor::initializeSonicMaster()
{
    sonicMasterEngine_.setApplicationEngine(&autoMasteringEngine_);

    // AUDIT-FIX-R5: wire the engine to the processor's message-thread maintenance
    // timer. When the analysis thread has a plan ready, the engine stores it,
    // sets the pending flag, and invokes this callback which requests a timer tick.
    // On the next tick, the processor calls sonicMasterEngine_.processPendingApplication()
    // on the message thread. This replaces the old MessageManager::callAsync hop
    // which can silently drop in headless hosts.
    sonicMasterEngine_.setMaintenanceRequestCallback([this]()
    {
        requestMessageThreadMaintenance();
    });

    const char* modelName = "masteringbrain_v2_decision.onnx";
    const char* contractName = "masteringbrain_v2_decision.contract.json";

#if MORE_PHI_HAS_ONNX
    // W8: try the bundled binary-data resource first (embedded at compile time
    // via juce_add_binary_data). Extract to a temp file so ONNX Runtime can
    // mmap it, then SHA-256 verify before loading.
    {
        juce::File tempDir = juce::File::getSpecialLocation(
            juce::File::SpecialLocationType::tempDirectory);
        juce::File extractedModel = tempDir.getChildFile("morephi_sonicmaster_model.onnx");
        juce::File extractedContract = tempDir.getChildFile("morephi_sonicmaster_contract.json");

        bool extracted = false;
        int modelBinSize = 0;
        const char* modelBinData = BinaryData::getNamedResource("masteringbrain_v2_decision_onnx", modelBinSize);
        int contractBinSize = 0;
        const char* contractBinData = BinaryData::getNamedResource("masteringbrain_v2_decision_contract_json", contractBinSize);

        if (modelBinData != nullptr && modelBinSize > 0)
        {
            extractedModel.replaceWithData(modelBinData, modelBinSize);
            extracted = true;
        }
        if (contractBinData != nullptr && contractBinSize > 0)
            extractedContract.replaceWithData(contractBinData, contractBinSize);

        if (extracted && extractedModel.existsAsFile())
        {
            // SHA-256 integrity check (W8)
            bool hashOk = true;
#if MORE_PHI_HAS_ONNX
            {   // Compute SHA-256 of the extracted file and compare with the
                // expected hash baked at CMake configure time.
                juce::MemoryBlock fileData;
                if (extractedModel.loadFileAsData(fileData) && fileData.getSize() > 0)
                {
                    juce::SHA256 actualHash(static_cast<const uint8_t*>(fileData.getData()),
                                            fileData.getSize());
                    const juce::String actualHex = actualHash.toHexString().toLowerCase();
                    const juce::String expectedHex = juce::String(sonicmaster::kExpectedModelHash).toLowerCase();
                    if (actualHex != expectedHex)
                    {
                        DBG("[MorePhiProcessor] W8: model SHA-256 mismatch (got " + actualHex
                            + ", expected " + expectedHex + "). Refusing bundled model.");
                        hashOk = false;
                    }
                }
                else
                {
                    hashOk = false;
                }
            }
#endif
            if (hashOk && sonicMasterRunner_.loadModel(
                    extractedModel.getFullPathName().toStdString(),
                    extractedContract.existsAsFile()
                        ? extractedContract.getFullPathName().toStdString()
                        : ""))
            {
                sonicMasterEngine_.setInferenceSource(&sonicMasterSource_);
                sonicMasterEngine_.setFallbackInferenceSource(&sonicMasterHttpSource_);
                DBG("[MorePhiProcessor] SonicMaster ONNX model loaded from bundled resource");
                return;
            }
        }
    }
#endif

    // W8 fallback: search for the model alongside the plugin binary and dev paths.
    const auto pluginDir = juce::File::getSpecialLocation(
        juce::File::SpecialLocationType::currentExecutableFile).getParentDirectory();

    const juce::File searchDirs[] = {
        pluginDir,
        pluginDir.getChildFile("sonicmaster"),
        juce::File::getCurrentWorkingDirectory().getChildFile("build/sonicmaster"),
    };

    std::string modelPath;
    std::string contractPath;
    for (auto& dir : searchDirs)
    {
        auto f = dir.getChildFile(modelName);
        if (f.existsAsFile()) { modelPath = f.getFullPathName().toStdString(); break; }
    }
    for (auto& dir : searchDirs)
    {
        auto c = dir.getChildFile(contractName);
        if (c.existsAsFile()) { contractPath = c.getFullPathName().toStdString(); break; }
    }

    if (!modelPath.empty() && sonicMasterRunner_.loadModel(modelPath, contractPath))
    {
        sonicMasterEngine_.setInferenceSource(&sonicMasterSource_);
        sonicMasterEngine_.setFallbackInferenceSource(&sonicMasterHttpSource_);
        DBG("[MorePhiProcessor] SonicMaster ONNX model loaded: " + juce::String(modelPath));
    }
#if defined(MORE_PHI_ENABLE_SONICMASTER_HTTP_FALLBACK) && MORE_PHI_ENABLE_SONICMASTER_HTTP_FALLBACK
    else
    {
        sonicMasterEngine_.setInferenceSource(&sonicMasterHttpSource_);
        DBG("[MorePhiProcessor] SonicMaster ONNX model not found, "
            "using HTTP inference server (127.0.0.1:8765)");
    }
#else
    else
    {
        // AUDIT-2026-06-25: HTTP fallback is opt-in. Without it, leave the
        // inference source unset so isAvailable() returns false and the UI
        // reports the feature as unavailable rather than silently depending on
        // an external server process.
        sonicMasterEngine_.setInferenceSource(nullptr);
        DBG("[MorePhiProcessor] SonicMaster ONNX model not found; "
            "HTTP fallback disabled. Neural Master unavailable.");
    }
#endif

    // PLUGGABLE GENRE MODEL (Phase C, 2026-06-27): load a user-supplied
    // genre-classification ONNX model from the search path. NOT embedded — we
    // don't control the bytes, so no SHA pinning (unlike the SonicMaster model).
    // Search order:
    //   1. %APPDATA%/MorePhi/models/genre_classifier.onnx   (user-writable)
    //   2. alongside the plugin binary                        (dev/drop-in)
    // If loadModel returns false (no file / ORT off / validation failed / threw)
    // the genre classifier keeps running its time-domain heuristic — nothing
    // breaks. Expected model shape: input [N, 128, T] log-mel, output [N, C]
    // softmax. See AGENTS.md "Genre-Conditioned Priors" for the full contract.
    initializeGenreClassifier();
}

void MorePhiProcessor::initializeGenreClassifier()
{
    const juce::String modelName = "genre_classifier.onnx";

    juce::File candidate;
    // 1. User data dir (survives plugin reinstalls; the canonical drop-in spot).
    {
        const auto appData = juce::File::getSpecialLocation(
            juce::File::userApplicationDataDirectory);   // %APPDATA% on Windows
        const auto userModelDir = appData.getChildFile("MorePhi").getChildFile("models");
        const auto f = userModelDir.getChildFile(modelName);
        if (f.existsAsFile()) candidate = f;
    }
    // 2. Alongside the plugin binary (dev / portable-install drop-in).
    if (candidate == juce::File())
    {
        const auto pluginDir = juce::File::getSpecialLocation(
            juce::File::SpecialLocationType::currentExecutableFile).getParentDirectory();
        const auto f = pluginDir.getChildFile(modelName);
        if (f.existsAsFile()) candidate = f;
    }

    if (candidate == juce::File())
    {
        DBG("[MorePhiProcessor] Genre classifier model not found — using heuristic fallback. "
            "Drop genre_classifier.onnx into %APPDATA%/MorePhi/models/ to enable neural genre.");
        return;
    }

    if (autoMasteringEngine_.getGenreClassifier().loadModel(candidate))
        DBG("[MorePhiProcessor] Genre classifier ONNX model loaded: " + candidate.getFullPathName());
    else
        DBG("[MorePhiProcessor] Genre classifier model load failed (validation/shape mismatch); "
            "heuristic fallback remains active. File: " + candidate.getFullPathName());
}

void MorePhiProcessor::initializeNeuralMasteringV2()
{
    // Model name: the 63→72 feature-delta ONNX model.
    // Searched alongside the plugin binary or in known dev paths.
    constexpr const char* v2ModelName = "neural_mastering_v2.onnx";

    const auto pluginDir = juce::File::getSpecialLocation(
        juce::File::SpecialLocationType::currentExecutableFile).getParentDirectory();

    const juce::File searchDirs[] = {
        pluginDir,
        pluginDir.getChildFile("models"),
        juce::File::getCurrentWorkingDirectory().getChildFile("models/sonicmaster"),
        juce::File::getCurrentWorkingDirectory().getChildFile("scripts/neural-mastering/control"),
    };

    std::string modelPath;
    for (auto& dir : searchDirs)
    {
        auto f = dir.getChildFile(v2ModelName);
        if (f.existsAsFile()) { modelPath = f.getFullPathName().toStdString(); break; }
    }

    if (!modelPath.empty() && onnxNeuralRunner_.loadModel(modelPath, "neural_mastering_v2", ""))
    {
        neuralMasteringController_.setModelRunner(&onnxNeuralRunner_);
        DBG("[MorePhiProcessor] V2 feature→delta ONNX model loaded: " + juce::String(modelPath));
    }
    else
    {
        // No model found — runner stays !available, controller falls through to
        // DeterministicBaseline transparently. This is the current behaviour.
        DBG("[MorePhiProcessor] V2 feature→delta ONNX model not found at '" + juce::String(v2ModelName)
            + "'; using DeterministicBaseline fallback.");
    }
}

// ── V2 derived-feature helpers ───────────────────────────────────────────

float MorePhiProcessor::computeMonoFoldDownDeltaDb(
    const StereoFieldAnalyzer::StereoFieldSnapshot* stereo) noexcept
{
    if (stereo == nullptr || stereo->frameIndex == 0) return 0.0f;
    float avgCorr = 0.0f;
    for (int i = 0; i < StereoFieldAnalyzer::kNumBands; ++i)
        avgCorr += stereo->correlation[i];
    avgCorr /= static_cast<float>(StereoFieldAnalyzer::kNumBands);
    avgCorr = std::clamp(avgCorr, -1.0f, 1.0f);
    return 10.0f * std::log10((1.0f + avgCorr) / 2.0f + 1e-12f);
}

float MorePhiProcessor::computeHarmonicRisk(
    const RealtimeSpectrumAnalyzer::SpectrumSnapshot* spectrum) noexcept
{
    if (spectrum == nullptr || spectrum->frameIndex == 0) return 0.0f;
    const float thd = std::max(0.0f, spectrum->thdPercent);
    return std::min(thd / 10.0f, 1.0f);
}

float MorePhiProcessor::computeTransientDensity(
    const RealtimeSpectrumAnalyzer::SpectrumSnapshot* spectrum) noexcept
{
    if (spectrum == nullptr || spectrum->frameIndex == 0) return 0.0f;
    if (v2FluxMean_ < 1e-6f) return 0.0f;
    const float ratio = spectrum->spectralFlux / v2FluxMean_;
    return std::min(ratio / 5.0f, 1.0f);
}

MorePhiProcessor::~MorePhiProcessor()
{
    DBG("[~MorePhiProcessor] entering destructor");
    stopTimer();
    DBG("[~MorePhiProcessor] stopped timer");
    // H-5 FIX: Clear any pending plugin load retry state so the destructor
    // doesn't leave stale references (pendingPluginDesc_ has its own cleanup
    // via ~PluginDescription, but the atomic flags are reset defensively for
    // clarity — they prevent a hypothetical timer-edge from observing pending
    // state after stopTimer() returns).
    hasPendingPluginLoad_.store(false, std::memory_order_release);
    pendingLoadAttempts_.store(0, std::memory_order_relaxed);
    // Stop the agent runtime FIRST — its workers borrow references to the MCP
    // server's AutomationRuntime (events/workflows/etc.) and to the four holders
    // below, so they must be joined before any of those are torn down.
    agentRuntime_.reset();
    DBG("[~MorePhiProcessor] reset agentRuntime_");
    aiAssistant_.reset();
    DBG("[~MorePhiProcessor] reset aiAssistant_");
    linkBroadcaster_.detach();
    DBG("[~MorePhiProcessor] detached linkBroadcaster_");
    if (vst3IpcBridge_ != nullptr)
    {
        DBG("[~MorePhiProcessor] stopping vst3IpcBridge_...");
        vst3IpcBridge_->stop();
        DBG("[~MorePhiProcessor] stopped vst3IpcBridge_");
    }
    DBG("[~MorePhiProcessor] stopping mcpServer...");
    mcpServer.stopServer();
    DBG("[~MorePhiProcessor] stopped mcpServer");
    InstanceRegistry::getInstance().deregisterInstance(instanceIdentity_.instanceId);
    DBG("[~MorePhiProcessor] deregistered instance");
    clearHostedMasteringApplicators();
    DBG("[~MorePhiProcessor] cleared hosted mastering applicators");
    DBG("[~MorePhiProcessor] unloading hosted plugin...");
    hostManager.unloadPlugin();
    DBG("[~MorePhiProcessor] unloaded hosted plugin");

    // H-5: Clean up the heap-allocated weak reference used by MIDI callbacks.
    delete midiCallbackWeakRef_;
    midiCallbackWeakRef_ = nullptr;
    DBG("[~MorePhiProcessor] cleaned up midiCallbackWeakRef_");

    // Clean up extracted ONNX temp files (Finding #12).
    // These files are embedded in the binary (not secret), but leaving them in
    // the temp directory is untidy. Safe to delete here because the ONNX runner
    // has already been destroyed (it's a member that precedes this destructor
    // in declaration order — see PluginProcessor.h).
    {
        const juce::File tempDir = juce::File::getSpecialLocation(
            juce::File::SpecialLocationType::tempDirectory);
        const juce::File tempModel = tempDir.getChildFile("morephi_sonicmaster_model.onnx");
        const juce::File tempContract = tempDir.getChildFile("morephi_sonicmaster_contract.json");
        if (tempModel.existsAsFile()) { tempModel.deleteFile(); DBG("[~MorePhiProcessor] deleted temp ONNX model"); }
        if (tempContract.existsAsFile()) { tempContract.deleteFile(); DBG("[~MorePhiProcessor] deleted temp ONNX contract"); }
    }
}

standalone_mcp::MorePhiIPCDiscovery& MorePhiProcessor::getMorePhiIPCDiscovery()
{
    return *ipcDiscovery_;
}

standalone_mcp::MorePhiIPCAssistant& MorePhiProcessor::getMorePhiIPCAssistant()
{
    return *ipcAssistant_;
}

void MorePhiProcessor::cacheRawParameterPointers()
{
    rawParams_.morphX = apvts.getRawParameterValue("morphX");
    rawParams_.morphY = apvts.getRawParameterValue("morphY");
    rawParams_.faderPos = apvts.getRawParameterValue("faderPos");
    rawParams_.morphSource = apvts.getRawParameterValue("morphSource");
    rawParams_.physicsMode = apvts.getRawParameterValue("physicsMode");
    rawParams_.smoothing = apvts.getRawParameterValue("smoothing");
    rawParams_.driftSpeed = apvts.getRawParameterValue("driftSpeed");
    rawParams_.driftDistance = apvts.getRawParameterValue("driftDistance");
    rawParams_.driftChaos = apvts.getRawParameterValue("driftChaos");
    rawParams_.recallMode = apvts.getRawParameterValue("recallMode");
    rawParams_.sidechainEnabled = apvts.getRawParameterValue("sidechainEnabled");
    rawParams_.sidechainThreshold = apvts.getRawParameterValue("sidechainThreshold");
    rawParams_.listenMode = apvts.getRawParameterValue("listenMode");
    rawParams_.recallToggle = apvts.getRawParameterValue("recallToggle");
    rawParams_.linkMode = apvts.getRawParameterValue("linkMode");
    rawParams_.spectralActive = apvts.getRawParameterValue("spectralActive");
    rawParams_.spectralFFTSize = apvts.getRawParameterValue("spectralFFTSize");
    rawParams_.spectralTransient = apvts.getRawParameterValue("spectralTransient");
    rawParams_.spectralFormant = apvts.getRawParameterValue("spectralFormant");
    rawParams_.granularActive = apvts.getRawParameterValue("granularActive");
    rawParams_.grainSize = apvts.getRawParameterValue("grainSize");
    rawParams_.grainDensity = apvts.getRawParameterValue("grainDensity");
    rawParams_.grainPitch = apvts.getRawParameterValue("grainPitch");
    rawParams_.grainScatter = apvts.getRawParameterValue("grainScatter");
    rawParams_.audioDomainEnabled = apvts.getRawParameterValue("audioDomainEnabled");
    rawParams_.oversampling = apvts.getRawParameterValue("oversampling");
    rawParams_.blendParamWeight = apvts.getRawParameterValue("blendParamWeight");
    rawParams_.blendSpectralWeight = apvts.getRawParameterValue("blendSpectralWeight");
    rawParams_.blendGranularWeight = apvts.getRawParameterValue("blendGranularWeight");
    rawParams_.morphAlpha = apvts.getRawParameterValue("morphAlpha");
    rawParams_.bypass = apvts.getRawParameterValue("bypass");
    rawParams_.outputGain = apvts.getRawParameterValue("outputGain");
    rawParams_.outputProtect = apvts.getRawParameterValue("outputProtect");
    rawParams_.outputCeiling = apvts.getRawParameterValue("outputCeiling");
    rawParams_.driftOutputX = apvts.getRawParameterValue("driftOutputX");
    rawParams_.driftOutputY = apvts.getRawParameterValue("driftOutputY");
    rawParams_.coarseParameterWrites = apvts.getRawParameterValue("coarseParameterWrites");
    rawParams_.disableTouchDetection = apvts.getRawParameterValue("disableTouchDetection");
    rawParams_.throttleParamCommits = apvts.getRawParameterValue("throttleParamCommits");
    rawParams_.cpuSaver = apvts.getRawParameterValue("cpuSaver");
    rawParams_.dawWrite = apvts.getRawParameterValue("dawWrite");
    rawParams_.sonicMasterEnabled = apvts.getRawParameterValue("SonicMasterAnalysisEnabled");
    rawParams_.expertMode = apvts.getRawParameterValue("expertMode");
    rawParams_.waypointEnable = apvts.getRawParameterValue("waypointEnable");
    rawParams_.waypointPlay = apvts.getRawParameterValue("waypointPlay");
    rawParams_.waypointBPM = apvts.getRawParameterValue("waypointBPM");
}

bool MorePhiProcessor::enqueueParameterSet(int paramIndex,
                                           float normalizedValue,
                                           ParameterEditSource source,
                                           bool holdAgainstMorph)
{
    if (paramIndex < 0 || paramIndex >= MAX_PARAMETERS)
        return false;

    const bool pushed = commandQueue.push(ParamCommand{
        paramIndex,
        juce::jlimit(0.0f, 1.0f, normalizedValue),
        false, -1,
        source,
        holdAgainstMorph
    });

    // P2.8 (AUDIT): arm the SonicMaster transition guard so the analysis cycle
    // discards any capture window that straddles this hosted-parameter change
    // (a hybrid window would be analyzed as a state that never existed). Called
    // for AI/MCP edits that route through this enqueue (not the audio-thread
    // morph drain, which is continuous interpolation, not a discrete change).
    // Cheap: one relaxed store + one relaxed timestamp. Coalesced by design —
    // a 40-param batch re-stamps the same flag harmlessly.
    if (pushed)
        sonicMasterEngine_.notifyHostedParameterChanged();

    return pushed;
}

bool MorePhiProcessor::enqueuePlanBoundary(std::uint64_t planId, ParameterEditSource source)
{
    // P3.10 (AUDIT): enqueue a no-op boundary command that closes a plan. The
    // audio-thread drain pops it and stamps lastDrainedPlanId_ so a caller can
    // confirm the full plan committed. paramIndex=-1 ensures writeParameter is
    // never invoked (it early-returns on out-of-range indices).
    return commandQueue.push(ParamCommand{
        -1, 0.0f,
        false, -1,
        source,
        /*holdAgainstMorph=*/false,
        /*isPlanBoundary=*/true,
        planId
    });
}

bool MorePhiProcessor::enqueueParameterBatch(const std::vector<ParamCommand>& commands)
{
    if (commands.empty())
        return true;

    std::vector<ParamCommand> sanitized;
    sanitized.reserve(commands.size());
    for (auto command : commands)
    {
        // C-3 FIX: Skip invalid indices instead of rejecting the entire batch.
        // An MCP tool sending a batch with one out-of-range parameter after a
        // hosted plugin change no longer silently drops ALL edits.
        if (command.paramIndex < 0 || command.paramIndex >= MAX_PARAMETERS)
            continue;

        command.value = juce::jlimit(0.0f, 1.0f, command.value);
        sanitized.push_back(command);
    }

    if (sanitized.empty())
        return true;

    return commandQueue.pushRange(sanitized);
}

int MorePhiProcessor::enqueueParameterState(const std::vector<float>& normalizedValues,
                                            ParameterEditSource source,
                                            bool holdAgainstMorph)
{
    const int count = juce::jmin(static_cast<int>(normalizedValues.size()), MAX_PARAMETERS);
    if (count <= 0) return 0;

    // We need space for all params in this live state update.
    if (static_cast<size_t>(count) > commandQueue.freeSpaceApprox())
        return 0;

    int queued = 0;
    for (int i = 0; i < count; ++i)
    {
        if (!commandQueue.push(ParamCommand{
            i, juce::jlimit(0.0f, 1.0f, normalizedValues[static_cast<size_t>(i)]),
            false, -1,
            source,
            holdAgainstMorph
        }))
            return queued;
        ++queued;
    }
    return queued;
}

void MorePhiProcessor::captureABCompareRef()
{
    abCompareState_ = paramBridge.captureParameterState();
    abCompareHasRef_.store(true, std::memory_order_release);
}

bool MorePhiProcessor::toggleABCompare()
{
    const bool hasRef = abCompareHasRef_.load(std::memory_order_acquire);

    if (!hasRef)
    {
        captureABCompareRef();
        abCompareActive_.store(true, std::memory_order_release);
        return true;  // captured → now in B mode
    }

    const bool wasActive = abCompareActive_.load(std::memory_order_acquire);
    if (!wasActive)
    {
        // Switching from A (live) to B (captured reference)
        captureABCompareRef();  // re-capture so B is current
        abCompareActive_.store(true, std::memory_order_release);
        if (!abCompareState_.empty())
            enqueueParameterState(abCompareState_, ParameterEditSource::UI, true);
        return true;
    }
    else
    {
        // Switching from B (captured) back to A (live morph)
        abCompareActive_.store(false, std::memory_order_release);
        return false;
    }
}

bool MorePhiProcessor::recallSnapshotQueued(int slot)
{
    if (!snapshotBank.getSlotValuesCopy(slot, recallScratch_))
        return false;

    const int count = static_cast<int>(recallScratch_.size());

    // Space for values + marker
    if (static_cast<size_t>(count + 1) > commandQueue.freeSpaceApprox())
        return false;

    // Push values
    for (int i = 0; i < count; ++i)
    {
        if (!commandQueue.push(ParamCommand{
            i, juce::jlimit(0.0f, 1.0f, recallScratch_[static_cast<size_t>(i)]),
            false, -1,
            ParameterEditSource::Snapshot,
            false
        }))
            return false;  // Queue filled up mid-push
    }

    // Push marker to signal end of snapshot recall
    if (!commandQueue.push(ParamCommand{ -1, 0.0f, true, slot, ParameterEditSource::Snapshot, false }))
        return false;

    return true;
}

bool MorePhiProcessor::captureSnapshotToSlot(int slot, bool includeStateChunk)
{
    if (slot < 0 || slot >= SnapshotBank::NUM_SLOTS)
        return false;

    // M-11 FIX: Runtime enforcement of slot 11 reservation for A/B compare rollback
    if (slot == 11)
    {
        jassertfalse;
        return false;
    }

    // Push undo record before overwriting the slot
    {
        std::vector<float> beforeVals;
        if (snapshotBank.getSlotValuesCopy(slot, beforeVals))
        {
            ParameterState beforeState;
            beforeState.capture(beforeVals.data(), static_cast<int>(beforeVals.size()));
            beforeState.occupied = snapshotBank.isOccupied(slot);
            undoRedoManager_.pushSnapshotState(slot, beforeState,
                                               "Capture slot " + juce::String(slot + 1));
        }
    }

    // Retry exclusive lock up to 3 times to handle transient contention from
    // the audio thread's processBlock or the DAW's getStateInformation calls.
    // Without retries, the CAS in beginExclusivePluginUse fails instantly if
    // another exclusive user (e.g. DAW auto-save) is in progress, causing
    // subsequent snapshot captures to silently fail.
    constexpr int kMaxExclusiveRetries = 3;
    for (int attempt = 0; attempt < kMaxExclusiveRetries; ++attempt)
    {
        if (auto* plugin = hostManager.beginExclusivePluginUse(200))
        {
            std::vector<float> values;
            juce::StringArray names;

            try
            {
                auto& params = plugin->getParameters();
                const int count = juce::jmin(static_cast<int>(params.size()), MAX_PARAMETERS);
                values.resize(static_cast<size_t>(count));
                for (int i = 0; i < count; ++i)
                {
                    values[static_cast<size_t>(i)] = params[i]->getValue();
                    names.add(params[i]->getName(128));
                }

                if (count > 0)
                    snapshotBank.captureValuesWithNames(slot, values.data(), count, names);

                if (includeStateChunk)
                    snapshotBank.captureStateChunk(slot, plugin);
            }
            catch (...)
            {
                hostManager.endExclusivePluginUse();
                return false;
            }

            hostManager.endExclusivePluginUse();
            return snapshotBank.isOccupied(slot);
        }

        // Brief sleep before retry — gives the other exclusive user time to finish.
        // Only sleep if not the last attempt.
        if (attempt < kMaxExclusiveRetries - 1)
            juce::Thread::sleep(10);
    }

    // Exclusive lock failed after retries. Fall back to non-exclusive capture
    // via ParameterBridge, which uses the shared acquirePluginForUse() path.
    // This still captures parameter values correctly; only the opaque state
    // chunk is skipped (it requires exclusive access).
    if (hostManager.hasPlugin())
    {
        snapshotBank.capture(slot, paramBridge);
        return snapshotBank.isOccupied(slot);
    }

    // No plugin loaded at all — capture whatever paramBridge has (e.g. test mode)
    snapshotBank.capture(slot, paramBridge);
    return snapshotBank.isOccupied(slot);
}

bool MorePhiProcessor::recallSnapshot(int slot, SnapshotRecallMode mode)
{
    if (slot < 0 || slot >= SnapshotBank::NUM_SLOTS || !snapshotBank.isOccupied(slot))
        return false;

    if (mode == SnapshotRecallMode::FullStateAndParams)
    {
        pendingFullStateRecallSlot_.store(slot, std::memory_order_relaxed);
        pendingFullStateRecallGeneration_.fetch_add(1, std::memory_order_release);
        fullStateRecallRetryCount_.store(0, std::memory_order_relaxed);
        requestMessageThreadMaintenance();
    }

    return recallSnapshotQueued(slot);
}

void MorePhiProcessor::requestFullStateRecallFromAudioThread(int slot) noexcept
{
    if (slot < 0 || slot >= SnapshotBank::NUM_SLOTS)
        return;

    pendingFullStateRecallSlot_.store(slot, std::memory_order_relaxed);
    pendingFullStateRecallGeneration_.fetch_add(1, std::memory_order_release);
    fullStateRecallRetryCount_.store(0, std::memory_order_relaxed);
}

void MorePhiProcessor::setMorphPositionExternal(float x, bool hasX,
                                                float y, bool hasY,
                                                float fader, bool hasFader,
                                                int source)
{
    if (hasX)
    {
        setMorphX(x);
    }

    if (hasY)
    {
        setMorphY(y);
    }

    if (hasFader)
    {
        setFaderPos(fader);
    }

    setMorphSource(source);

    // P2 FIX: Replace callAsync (which may silently drop on headless hosts /
    // editor-closed) with Timer-deferred notification. The morph position is
    // already stored in atomics; this flag tells the maintenance timer to
    // push them into the APVTS on the next tick (message thread, reliable).
    morphPositionNotifyPending_.store(true, std::memory_order_release);
    requestMessageThreadMaintenance();
}

void MorePhiProcessor::clearLiveEditHoldsAudioThread() noexcept
{
    if (!touchStateLock_.tryEnter())
        return;
    std::fill(liveEditHold_.begin(), liveEditHold_.end(), uint8_t{0});
    touchStateLock_.exit();
}

void MorePhiProcessor::invalidateAppliedCacheAudioThread() noexcept
{
    // C4 FIX: reset the apply-side cache so a recall that wrote hosted params
    // out-of-band (ParameterBridge::applyParameterState from a MIDI or MCP
    // trigger) is not immediately clobbered by this block's morph pass. The
    // touchStateLock_ try-lock matches the audio-thread guard used in
    // applyMorphAndParameters(); if it can't be acquired we skip — the morph
    // path's own hasTouchLock check will then also skip writes this block.
    if (touchStateLock_.tryEnter())
    {
        std::fill(lastApplied_.begin(), lastApplied_.end(), -1.0f);
        std::fill(touchCooldown_.begin(), touchCooldown_.end(), 0);
        touchStateLock_.exit();
    }
}

bool MorePhiProcessor::shouldReleaseLiveEditHold(int index, float x, float y, float fader) const noexcept
{
    if (index < 0 || static_cast<size_t>(index) >= liveEditHold_.size())
        return false;

    const size_t i = static_cast<size_t>(index);
    if (liveEditHold_[i] == 0)
        return false;

    const float xyDelta = std::abs(x - liveEditX_[i]) + std::abs(y - liveEditY_[i]);
    const float faderDelta = std::abs(fader - liveEditFader_[i]);
    return xyDelta > MORPH_POS_THRESHOLD || faderDelta > MORPH_POS_THRESHOLD;
}

int MorePhiProcessor::drainParameterCommandQueue(int cachedParamCount,
                                                 int maxCommands,
                                                 juce::AudioPluginInstance* exclusivePlugin,
                                                 int* outOfRangeCount,
                                                 juce::uint32 now) noexcept
{
    if (maxCommands <= 0)
        return 0;

    const int commandLimit = juce::jmin(maxCommands, static_cast<int>(COMMAND_QUEUE_CAPACITY));
    ParamCommand cmd;
    int drainedCommands = 0;

    auto readParameter = [this, exclusivePlugin](int index) noexcept -> float
    {
        if (exclusivePlugin != nullptr)
        {
            auto& params = exclusivePlugin->getParameters();
            if (index >= 0 && index < params.size() && params[index] != nullptr)
                return ParameterBridge::getValueNoexcept(params[index]);
            return 0.0f;
        }

        return paramBridge.getParameterNormalized(index);
    };

    auto writeParameter = [this, exclusivePlugin, outOfRangeCount, now](int index, float value) noexcept -> bool
    {
        const float clamped = juce::jlimit(0.0f, 1.0f, value);

        if (exclusivePlugin != nullptr)
        {
            auto& params = exclusivePlugin->getParameters();
            if (index >= 0 && index < params.size() && params[index] != nullptr)
            {
                // C-1 FIX: use SEH wrapper
                return ParameterBridge::setValueNoexcept(params[index], clamped, &paramBridge);
            }

            // AUDIT-FIX 4.7: index was out of the plugin's actual parameter range,
            // even though it passed the MAX_PARAMETERS gate in enqueueParameterSet.
            if (outOfRangeCount != nullptr)
                ++(*outOfRangeCount);
            return false;
        }

        paramBridge.setParameterNormalized(index, clamped, now);
        return true;
    };

    // Serialized by commandConsumerLock_. The audio path uses a try-lock; the
    // assistant flush path uses a blocking lock off the audio thread, so queued
    // MCP edits can be applied while FL Studio is idle without racing
    // processBlock().
    bool hasTouchLock = false;
    if (exclusivePlugin != nullptr)
    {
        touchStateLock_.enter();
        hasTouchLock = true;
    }
    else
    {
        hasTouchLock = touchStateLock_.tryEnter();
    }

    while (drainedCommands < commandLimit && commandQueue.pop(cmd))
    {
        // P3.10 (AUDIT): plan transaction boundary. A no-op command (paramIndex=-1)
        // that closes an AI/neural plan. Stamp lastDrainedPlanId_ so a caller can
        // confirm the full plan drained (vs. still partial in the queue), then
        // continue — never reach the parameter-write path.
        if (cmd.isPlanBoundary)
        {
            lastDrainedPlanId_.store(cmd.planId, std::memory_order_release);
            ++drainedCommands;
            continue;
        }
        if (cmd.isSnapshotMarker)
        {
            MORE_PHI_PROFILE(profiler_, "command_drain_snapshot");
            const int slot = cmd.snapshotSlot;
            auto positions = InterpolationEngine::getClockPositions();
            if (slot >= 0 && slot < static_cast<int>(positions.size()))
            {
                const float slotX = (positions[slot].x + 1.0f) * 0.5f;
                const float slotY = (positions[slot].y + 1.0f) * 0.5f;
                morphX_.store(slotX, std::memory_order_relaxed);
                morphY_.store(slotY, std::memory_order_relaxed);
                if (rawParams_.morphX != nullptr)
                    rawParams_.morphX->store(slotX, std::memory_order_relaxed);
                if (rawParams_.morphY != nullptr)
                    rawParams_.morphY->store(slotY, std::memory_order_relaxed);
            }

            // PERF-C3: Invalidate early-exit state — snapshot recall moved position
            morphStableBlocks_ = 0;
            prevMorphX_ = -1.0f;

            clearLiveEditHoldsAudioThread();

            const int paramCount = juce::jmin(cachedParamCount,
                                              static_cast<int>(touchCooldown_.size()));
            // PERF-C4: Batch-read current values instead of per-param
            // getParameterNormalized (which goes through withPlugin per call).
            // Reuse currentParamSnapshot_ buffer (already pre-allocated).
            if (exclusivePlugin != nullptr)
            {
                auto& params = exclusivePlugin->getParameters();
                const int readCount = juce::jmin(paramCount, static_cast<int>(params.size()));
                for (int i = 0; i < readCount; ++i)
                {
                    // C-1 FIX: use SEH wrappers for getValue
                    lastApplied_[static_cast<size_t>(i)] = (params[i] != nullptr) ? ParameterBridge::getValueNoexcept(params[i]) : 0.0f;
                    touchCooldown_[static_cast<size_t>(i)] = 0;
                }
                for (int i = readCount; i < paramCount; ++i)
                    touchCooldown_[static_cast<size_t>(i)] = 0;
            }
            else
            {
                if (auto* plugin = hostManager.acquirePluginForUse())
                {
                    struct ScopedRelease {
                        PluginHostManager& hm;
                        ~ScopedRelease() { hm.releasePluginFromUse(); }
                    } release{ hostManager };
                    auto& params = plugin->getParameters();
                    const int readCount = juce::jmin(paramCount, static_cast<int>(params.size()));
                    for (int i = 0; i < readCount; ++i)
                    {
                        // C-1 FIX: use SEH wrapper for getValue
                        lastApplied_[static_cast<size_t>(i)] = (params[i] != nullptr)
                            ? ParameterBridge::getValueNoexcept(params[i]) : 0.0f;
                        touchCooldown_[static_cast<size_t>(i)] = 0;
                    }
                    for (int i = readCount; i < paramCount; ++i)
                        touchCooldown_[static_cast<size_t>(i)] = 0;
                }
                else
                {
                    for (int i = 0; i < paramCount; ++i)
                    {
                        lastApplied_[static_cast<size_t>(i)] = readParameter(i);
                        touchCooldown_[static_cast<size_t>(i)] = 0;
                    }
                }
            }
        }
        else
        {
            // AUDIT (E2, 2026-06-25): stamp the source of this hosted write so a
            // same-parameter, different-source edit burst (e.g. a manual MCP tweak
            // of a control the neural engine is also driving) is observable. Audio-
            // safe: fixed array + relaxed atomics inside noteWriteSource. This does
            // NOT arbitrate — both hosted writers already share this FIFO queue.
            paramBridge.noteWriteSource(cmd.paramIndex, toHostedWriteSource(cmd.source), now);

            // PERF-BATCH: the MCP-flush path (exclusivePlugin != nullptr) writes
            // through immediately because it holds the exclusive lease and is
            // already O(1) in acquire cost. The audio-thread path (nullptr)
            // stashes into drainScratch_ and defers the plugin setValue to a
            // single batched ParameterBridge::applyParameterState call below —
            // collapsing N×(acquire+throttle+syscall) to one lock per block.
            MORE_PHI_PROFILE(profiler_, "command_drain_param");
            if (exclusivePlugin == nullptr)
            {
                if (cmd.paramIndex >= 0 &&
                    static_cast<size_t>(cmd.paramIndex) < drainScratch_.size())
                {
                    const auto paramIndex = static_cast<size_t>(cmd.paramIndex);
                    const float clamped = juce::jlimit(0.0f, 1.0f, cmd.value);
                    drainScratch_[paramIndex] = clamped;
                    drainTouched_[paramIndex] = 1;

                    if (paramIndex < lastApplied_.size())
                    {
                        lastApplied_[paramIndex] = clamped;
                        touchCooldown_[paramIndex] = 0;

                        if (cmd.holdAgainstMorph && paramIndex < liveEditHold_.size())
                        {
                            liveEditHold_[paramIndex] = 1;
                            liveEditX_[paramIndex] = morphX_.load(std::memory_order_relaxed);
                            liveEditY_[paramIndex] = morphY_.load(std::memory_order_relaxed);
                            liveEditFader_[paramIndex] = faderPos_.load(std::memory_order_relaxed);
                        }
                        else if (paramIndex < liveEditHold_.size())
                        {
                            liveEditHold_[paramIndex] = 0;
                        }
                    }
                }
            }
            else
            {
                writeParameter(cmd.paramIndex, cmd.value);

                if (cmd.paramIndex >= 0 &&
                    static_cast<size_t>(cmd.paramIndex) < lastApplied_.size())
                {
                    const auto paramIndex = static_cast<size_t>(cmd.paramIndex);
                    lastApplied_[paramIndex] = juce::jlimit(0.0f, 1.0f, cmd.value);
                    touchCooldown_[paramIndex] = 0;

                    if (cmd.holdAgainstMorph && paramIndex < liveEditHold_.size())
                    {
                        liveEditHold_[paramIndex] = 1;
                        liveEditX_[paramIndex] = morphX_.load(std::memory_order_relaxed);
                        liveEditY_[paramIndex] = morphY_.load(std::memory_order_relaxed);
                        liveEditFader_[paramIndex] = faderPos_.load(std::memory_order_relaxed);
                    }
                    else if (paramIndex < liveEditHold_.size())
                    {
                        liveEditHold_[paramIndex] = 0;
                    }
                }
            }
        }

        ++drainedCommands;
    }

    // PERF-BATCH: one batched apply for everything the audio-thread path stashed.
    // applyParameterState takes a single plugin lease + one throttle lock for
    // the whole drain, regardless of how many commands were queued. MCP-flush
    // path skipped (it wrote through directly above).
    if (exclusivePlugin == nullptr)
    {
        // Coalesce dirty entries to the front of a contiguous span so the
        // batched apply only touches live parameters, not the full MAX_PARAMETERS.
        const int scratchSize = static_cast<int>(drainScratch_.size());
        int firstTouched = -1;
        int lastTouched = -1;
        for (int i = 0; i < scratchSize; ++i)
        {
            if (drainTouched_[static_cast<size_t>(i)] != 0)
            {
                if (firstTouched < 0) firstTouched = i;
                lastTouched = i;
            }
        }

        if (firstTouched >= 0)
        {
            // Mark untouched indices inside the span by rewriting the span from
            // lastApplied_ so applyParameterState doesn't clobber them with 0.
            // Only indices between firstTouched..lastTouched that were NOT
            // written this drain need this — cheap, bounded by the dirty span.
            for (int i = firstTouched; i <= lastTouched; ++i)
            {
                const auto idx = static_cast<size_t>(i);
                if (drainTouched_[idx] == 0 && idx < lastApplied_.size()
                    && lastApplied_[idx] >= 0.0f)
                    drainScratch_[idx] = lastApplied_[idx];
            }

            paramBridge.applyParameterState(drainScratch_.data() + firstTouched,
                                            lastTouched - firstTouched + 1, now);

            // Clear the dirty bitmap for the span we just flushed.
            std::fill(drainTouched_.begin() + firstTouched,
                      drainTouched_.begin() + lastTouched + 1,
                      uint8_t{0});
        }
    }

    if (hasTouchLock)
        touchStateLock_.exit();

    return drainedCommands;
}

MorePhiProcessor::ParameterCommandFlushResult
MorePhiProcessor::flushPendingParameterCommandsForAssistant(int maxCommands, int timeoutMs)
{
    ParameterCommandFlushResult result;
    result.pendingBefore = static_cast<int>(getPendingParameterCommandCountApprox());

    if (result.pendingBefore <= 0 || maxCommands <= 0)
    {
        result.pendingAfter = result.pendingBefore;
        return result;
    }

    // Retry a few times if exclusive access is temporarily unavailable.  The
    // audio thread may be in the middle of a callback, or another message-thread
    // operation (state capture/restore) may hold the exclusive lease.  A single
    // short timeout is often too aggressive in a loaded DAW.
    // H-5 FIX: Reduced sleep from 5-25ms to tight spin (0ms) to keep the MCP
    // thread responsive. The exclusive lease is held for <1ms in most cases,
    // so a short busy-wait is faster than a context switch.
    constexpr int kMaxRetries = 4;
    juce::AudioPluginInstance* plugin = nullptr;
    for (int attempt = 0; attempt < kMaxRetries; ++attempt)
    {
        const auto t0 = juce::Time::getMillisecondCounter();
        plugin = hostManager.beginExclusivePluginUse(timeoutMs);
        const auto t1 = juce::Time::getMillisecondCounter();
        result.waitedMs += static_cast<int>(t1 - t0);

        if (plugin != nullptr)
        {
            result.retryCount = attempt;
            break;
        }

        // No plugin is loaded: retries cannot help.
        if (!hostManager.hasPlugin())
        {
            result.pluginUnavailable = true;
            result.pendingAfter = static_cast<int>(getPendingParameterCommandCountApprox());
            return result;
        }

        result.exclusiveAccessTimedOut = true;

        // H-5 FIX: Use tight yield instead of sleep to keep MCP thread responsive.
        // The exclusive lease is <1ms in most cases, so a short busy-wait is
        // faster than a >5ms context switch which blocked TCP handler threads.
        if (attempt + 1 < kMaxRetries)
            juce::Thread::yield();
    }

    if (plugin == nullptr)
    {
        result.pendingAfter = static_cast<int>(getPendingParameterCommandCountApprox());
        return result;
    }

    // We got the plugin lease; clear the timeout flag.
    result.exclusiveAccessTimedOut = false;

    juce::SpinLock::ScopedLockType commandGuard(commandConsumerLock_);

    struct ScopedExclusivePluginUse
    {
        explicit ScopedExclusivePluginUse(PluginHostManager& h) : host(h) {}
        ~ScopedExclusivePluginUse() { host.endExclusivePluginUse(); }
        PluginHostManager& host;
    } exclusiveUse(hostManager);

    int hostedParamCount = 0;
    try
    {
        hostedParamCount = static_cast<int>(plugin->getParameters().size());
    }
    catch (...) {}

    const int cachedParamCount = juce::jmin(hostedParamCount,
                                            static_cast<int>(finalOutput_.size()),
                                            static_cast<int>(lastApplied_.size()));
    if (cachedParamCount <= 0)
    {
        result.pluginUnavailable = true;
        result.pendingAfter = static_cast<int>(getPendingParameterCommandCountApprox());
        return result;
    }

    result.drained = drainParameterCommandQueue(cachedParamCount, maxCommands, plugin, &result.outOfRangeCount);
    result.pendingAfter = static_cast<int>(getPendingParameterCommandCountApprox());
    return result;
}

//==============================================================================

// ── Queue Health Monitoring ───────────────────────────────────────────────────

float MorePhiProcessor::getCommandQueueUsage() const
{
    // Thread-safe approximation of queue usage
    const size_t capacity = COMMAND_QUEUE_CAPACITY;
    const size_t freeSpace = commandQueue.freeSpaceApprox();
    const size_t used = (capacity > freeSpace) ? (capacity - freeSpace) : 0;
    return static_cast<float>(used) / static_cast<float>(capacity);
}

bool MorePhiProcessor::isCommandQueueHealthy() const
{
    // Queue is unhealthy if more than 80% full
    // This provides early warning before overflow occurs
    return getCommandQueueUsage() < 0.8f;
}

// ── Performance Profiling Diagnostics ────────────────────────────────────────

juce::String MorePhiProcessor::getProfilingReport() const
{
    auto stats = profiler_.getAllStats();

    if (stats.empty())
        return "No profiling data available. Audio processing may not have started yet.";

    // Calculate total time and percentages
    double totalTime = 0.0;
    for (const auto& [name, stat] : stats)
    {
        if (name != "processBlock_total")  // Don't count total in subtotal
            totalTime += stat.totalTimeMs;
    }

    juce::String report;
    report << "=== CPU Performance Profile ===\n\n";
    report << "Total profiled operations: " << stats.size() << "\n";
    report << "Total time (all operations): " << (totalTime * 1000.0) << " µs\n\n";
    // AUDIT-FIX (M4): the percentages below can sum to >100% because sections
    // nest — spectral_engine/granular_engine/formant_engine/hybrid_blend are
    // timed INSIDE audio_domain_total, which is inside processBlock_total. Only
    // processBlock_total is excluded from the subtotal; container sections still
    // double-count their children. Leaf sections (the *_engine markers) are the
    // reliable attribution; treat container rows as upper bounds. Stats are
    // running means since session start — reset() between windows for a "now" view.
    report << "Note: nested sections double-count; leaf sections are the attribution.\n\n";

    // Sort by total time (descending)
    std::vector<std::pair<std::string, ProfileStats>> sortedStats(
        stats.begin(), stats.end());
    std::sort(sortedStats.begin(), sortedStats.end(),
              [](const auto& a, const auto& b) {
                  return a.second.totalTimeMs > b.second.totalTimeMs;
              });

    report << "--- Operation Breakdown ---\n";
    for (const auto& [name, stat] : sortedStats)
    {
        double avgUs = stat.averageTimeMs * 1000.0;
        double maxUs = stat.maxTimeMs * 1000.0;
        double pct = (totalTime > 0) ? (stat.totalTimeMs / totalTime * 100.0) : 0.0;

        report << "\n" << name << ":\n";
        report << "  Calls:      " << stat.callCount << "\n";
        report << "  Avg:        " << juce::String(avgUs, 2) << " µs\n";
        report << "  Max:        " << juce::String(maxUs, 2) << " µs\n";
        // AUDIT-2026-06-25 (M4): trailing-window percentiles over the most
        // recent kRingSamples (2048) samples. p50 is the median; p99 surfaces
        // tail spikes that the running average hides. 0.0 until the first
        // sample lands. Process-block-level percentiles come from the audit
        // harness's own HighResTimer (true population); these are per-section.
        report << "  p50:        " << juce::String(stat.p50Ms * 1000.0, 2) << " µs\n";
        report << "  p95:        " << juce::String(stat.p95Ms * 1000.0, 2) << " µs\n";
        report << "  p99:        " << juce::String(stat.p99Ms * 1000.0, 2) << " µs\n";
        report << "  Total:      " << juce::String(stat.totalTimeMs * 1000.0, 2) << " µs\n";
        report << "  Percentage: " << juce::String(pct, 1) << "%\n";
    }

    report << "\n=== CPU Spike Diagnosis ===\n";

    // Identify potential issues
    bool hasIssues = false;

    // Check audio domain processing
    auto audioDomainIt = stats.find("audio_domain_total");
    if (audioDomainIt != stats.end())
    {
        double audioPct = (audioDomainIt->second.totalTimeMs / totalTime * 100.0);
        if (audioPct > 50.0)
        {
            report << "\n⚠️  HIGH: Audio-domain processing uses " << juce::String(audioPct, 1)
                   << "% of CPU time\n";
            report << "    → Consider disabling spectral/granular engines\n";
            hasIssues = true;
        }
    }

    // Check command queue drain
    auto queueDrainIt = stats.find("command_queue_drain");
    if (queueDrainIt != stats.end())
    {
        double queuePct = (queueDrainIt->second.totalTimeMs / totalTime * 100.0);
        if (queuePct > 20.0)
        {
            report << "\n⚠️  HIGH: Command queue drain uses " << juce::String(queuePct, 1)
                   << "% of CPU time\n";
            report << "    → Parameter updates too frequent (check AI/MCP rate)\n";
            report << "    → Queue usage: " << juce::String(getCommandQueueUsage() * 100.0, 1)
                   << "% (healthy: <80%)\n";
            hasIssues = true;
        }
    }

    // Check parameter application
    auto paramApplyIt = stats.find("parameter_application");
    if (paramApplyIt != stats.end())
    {
        double paramPct = (paramApplyIt->second.totalTimeMs / totalTime * 100.0);
        if (paramPct > 30.0)
        {
            report << "\n⚠️  HIGH: Parameter application uses " << juce::String(paramPct, 1)
                   << "% of CPU time\n";
            report << "    → High parameter count (consider parameter grouping)\n";
            hasIssues = true;
        }
    }

    // Check individual engines
    auto spectralIt = stats.find("spectral_engine");
    if (spectralIt != stats.end())
    {
        double spectralPct = (spectralIt->second.totalTimeMs / totalTime * 100.0);
        if (spectralPct > 25.0)
        {
            report << "\n⚠️  HIGH: Spectral engine uses " << juce::String(spectralPct, 1)
                   << "% of CPU time\n";
            report << "    → FFT size too large or too many frequency bins\n";
            hasIssues = true;
        }
    }

    auto granularIt = stats.find("granular_engine");
    if (granularIt != stats.end())
    {
        double granularPct = (granularIt->second.totalTimeMs / totalTime * 100.0);
        if (granularPct > 25.0)
        {
            report << "\n⚠️  HIGH: Granular engine uses " << juce::String(granularPct, 1)
                   << "% of CPU time\n";
            report << "    → Too many active grains or grain size too large\n";
            hasIssues = true;
        }
    }

    if (!hasIssues)
    {
        report << "\n✅ No obvious CPU bottlenecks detected.\n";
        report << "    All operations within acceptable CPU limits.\n";
    }

    return report;
}

void MorePhiProcessor::dumpProfilingReportToConsole() const
{
    auto report = getProfilingReport();
    DBG(report);
}

// ── Parameter Layout ─────────────────────────────────────────────────────────

juce::AudioProcessorValueTreeState::ParameterLayout
MorePhiProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Morph controls (automatable)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"morphX", 1}, "Morph X", 0.0f, 1.0f, 0.5f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"morphY", 1}, "Morph Y", 0.0f, 1.0f, 0.5f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"faderPos", 1}, "Snap Fader", 0.0f, 1.0f, 0.0f));

    // Morph source: 0 = 2D Pad (XY), 1 = Fader
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"morphSource", 1}, "Morph Source",
        juce::StringArray{"2D Pad", "Fader"}, 0));

    // Physics controls
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"physicsMode", 1}, "Physics Mode",
        juce::StringArray{"Direct", "Elastic", "Drift"}, 0));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"smoothing", 1}, "Smoothing",
        juce::NormalisableRange<float>(0.0f, 0.999f, 0.001f), 0.95f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"driftSpeed", 1}, "Drift Speed",
        juce::NormalisableRange<float>(0.01f, 2.0f, 0.01f), 0.3f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"driftDistance", 1}, "Drift Distance",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.4f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"driftChaos", 1}, "Drift Chaos",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f));

    // Output
    // H-1 FIX: Skew the gain range so 0dB sits at the center of the knob
    // JUCE 8: use skew factor in constructor (skew = 1.0 = linear)
    // For centre at 0.0 in range [-24, 24], skew ≈ 1.0 (already linear/symmetric)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"outputGain", 1}, "Output Gain",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f), 0.0f));

    // Output protection: lookahead brickwall limiter on the main wet path.
    // Default ON so morph-driven overshoots (high-gain snapshots, correlated
    // HybridBlend sums) cannot clip past the ceiling. Lookahead is reported to
    // the DAW via LatencyManager so PDC compensates. -1.0 dBTP is streaming-safe.
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"outputProtect", 1}, "Output Protect", true));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"outputCeiling", 1}, "Output Ceiling",
        juce::NormalisableRange<float>(-3.0f, -0.1f, 0.1f), -1.0f));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"bypass", 1}, "Bypass", false));

    // SanityMode: protects critical params during breed/randomize
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"sanityEnabled", 1}, "Sanity Mode", false));

    // RecallMode: Fast (normalized params only) vs Full (params + state chunks)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"recallMode", 1}, "Recall Mode",
        juce::StringArray{"Fast", "Full"}, 0));

    // Sidechain trigger: audio-driven snapshot recall
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"sidechainEnabled", 1}, "Sidechain Trigger", false));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sidechainThreshold", 1}, "SC Threshold",
        juce::NormalisableRange<float>(-60.0f, 0.0f, 0.5f), -20.0f));

    // Listen Mode: exclude discrete params (toggles, dropdowns) from morphing
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"listenMode", 1}, "Listen Mode", false));

    // Recall Toggle: disable full state recall during MIDI triggers (sustain notes)
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"recallToggle", 1}, "Recall Toggle", true));

    // Drift recording: output params written by processBlock for DAW automation capture
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"driftOutputX", 1}, "Drift Output X", 0.0f, 1.0f, 0.5f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"driftOutputY", 1}, "Drift Output Y", 0.0f, 1.0f, 0.5f));

    // Smart Randomize trigger: automatable from DAW
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"smartRandomize", 1}, "Smart Randomize", false));

    // Link Mode: cross-instance morph synchronization
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"linkMode", 1}, "Link Mode", false));

    // ── Engine: Spectral ────────────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"spectralActive", 1}, "Spectral Active", false));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"spectralFFTSize", 1}, "FFT Size",
        juce::StringArray{"512", "1024", "2048", "4096"}, 2));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"spectralTransient", 1}, "Transient Preserve", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"spectralFormant", 1}, "Formant Preserve", false));

    // ── Engine: Granular ────────────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"granularActive", 1}, "Granular Active", false));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"grainSize", 1}, "Grain Size",
        juce::NormalisableRange<float>(20.0f, 200.0f, 0.1f), 50.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"grainDensity", 1}, "Grain Density",
        juce::NormalisableRange<float>(5.0f, 100.0f, 0.1f), 20.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"grainPitch", 1}, "Grain Pitch Rand",
        juce::NormalisableRange<float>(0.0f, 2.0f, 0.001f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"grainScatter", 1}, "Grain Scatter",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.0f));

    // ── Engine: Hybrid Blend ────────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"audioDomainEnabled", 1}, "Audio Domain", false));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"oversampling", 1}, "Oversampling",
        juce::StringArray{"x1", "x2", "x4", "x8"}, 0));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"blendParamWeight", 1}, "Direct Weight", 0.0f, 1.0f, 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"blendSpectralWeight", 1}, "Spectral Weight", 0.0f, 1.0f, 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"blendGranularWeight", 1}, "Granular Weight", 0.0f, 1.0f, 0.0f));

    // ── Morph Alpha (shared crossfade) ──────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"morphAlpha", 1}, "Morph Alpha", 0.0f, 1.0f, 0.0f));

    // ── Performance (opt-in, default OFF — preserve current behavior) ───────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"coarseParameterWrites", 1}, "Coarse Param Writes", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"disableTouchDetection", 1}, "Disable Touch Detect", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"throttleParamCommits", 1}, "Throttle Param Commits", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"cpuSaver", 1}, "CPU Saver", false));

    // DAW Write Toggle: gates whether drift output is written to
    // driftOutputX/Y for DAW automation recording. Default ON for
    // backward compatibility with existing behaviour.
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"dawWrite", 1}, "DAW Write", true));

    // ── Waypoints: automated morph path (Phase 6) ────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"waypointEnable", 1}, "Waypoints", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"waypointPlay", 1}, "Waypoint Play", false));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"waypointBPM", 1}, "Waypoint BPM",
        juce::NormalisableRange<float>(20.0f, 300.0f, 1.0f), 120.0f));

    // SonicMaster realtime neural mastering (preview). Default OFF — the
    // checkpoint is research-grade; see
    // docs/superpowers/specs/2026-06-21-sonicmaster-vst3-realtime-integration-design.md.
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"SonicMasterAnalysisEnabled", 1},
        "Neural Master (Preview)",
        false));

    // AUDIT-2026-06-25: Expert mode toggles visibility of advanced tabs
    // (Engine, Modulation, AI) and the SonicMaster row. Default OFF so first-run
    // users see only Classic + Presets. Not exposed to host automation.
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"expertMode", 1},
        "Expert Mode",
        false,
        juce::AudioParameterBoolAttributes().withAutomatable(false)));

    return { params.begin(), params.end() };
}

// ── Audio Lifecycle ──────────────────────────────────────────────────────────

void MorePhiProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // P2 FIX: Use release semantics — pairs with the acquire load in processBlock.
    // seq_cst was unnecessarily expensive for a single-producer flag handoff.
    prepared.store(false, std::memory_order_release);

    currentSampleRate = sampleRate;
    currentBlockSize  = samplesPerBlock;

    // M-6 FIX: Dynamic touch cooldown — ~200ms regardless of host config.
    touchCooldownBlocks_ = std::max(1, static_cast<int>(0.2 * sampleRate / samplesPerBlock));

    auto* const currentPlayHead = getPlayHead();
    hostManager.setPlayHead(currentPlayHead);
    hostManagerB_.setPlayHead(currentPlayHead);
    updateTransportContextSnapshot(currentPlayHead);

    hostManager.prepare(sampleRate, samplesPerBlock, getTotalNumOutputChannels());
    autoMasteringEngine_.prepare(sampleRate, samplesPerBlock, false);
    neuralMasteringController_.setApplicationEngine(&autoMasteringEngine_);
    neuralMasteringController_.resetStatus();
    sonicMasterEngine_.prepare(sampleRate, samplesPerBlock);

    // Output-protection limiter on the main wet path. Prepared + reset every
    // prepareToPlay so a sample-rate / block-size change rebuilds the lookahead
    // window. Seed the ceiling from the APVTS default (the limiter ctor already
    // defaults to -1.0 dBTP, so this only matters when a persisted value loads).
    morphOutputLimiter_.prepare(sampleRate, samplesPerBlock);
    morphOutputLimiter_.reset();
    if (rawParams_.outputCeiling != nullptr)
        morphOutputLimiter_.setCeiling(rawParams_.outputCeiling->load(std::memory_order_relaxed));
    // Apply the user toggle so setEnabled reflects outputProtect immediately.
    if (rawParams_.outputProtect != nullptr)
        morphOutputLimiter_.setEnabled(rawParams_.outputProtect->load(std::memory_order_relaxed) > 0.5f);

    // Pre-allocate morph processor buffers
    morphProcessor.prepare(MAX_PARAMETERS);  // Max expected param count
    finalOutput_.resize(MAX_PARAMETERS, 0.0f);
    currentParamSnapshot_.resize(MAX_PARAMETERS, 0.0f); // PERF-C2: batch snapshot buffer
    lastApplied_.resize(MAX_PARAMETERS, -1.0f);     // -1 = never applied
    touchCooldown_.resize(MAX_PARAMETERS, 0);
    // PERF-BATCH: command-drain scratch (sized once; never resized on audio thread).
    drainScratch_.assign(MAX_PARAMETERS, 0.0f);
    drainTouched_.assign(MAX_PARAMETERS, uint8_t{0});
    recallScratch_.resize(MAX_PARAMETERS, 0.0f);
    abCompareState_.resize(MAX_PARAMETERS, 0.0f);
    abCompareActive_.store(false, std::memory_order_relaxed);
    abCompareHasRef_.store(false, std::memory_order_relaxed);
    touchMorphX_.resize(MAX_PARAMETERS, -1.0f);     // Morph X when touch detected
    touchMorphY_.resize(MAX_PARAMETERS, -1.0f);     // Morph Y when touch detected
    liveEditHold_.assign(MAX_PARAMETERS, uint8_t{0});
    liveEditX_.assign(MAX_PARAMETERS, 0.5f);
    liveEditY_.assign(MAX_PARAMETERS, 0.5f);
    liveEditFader_.assign(MAX_PARAMETERS, 0.0f);

    // PERF-PROFILE: Register all profiling sections before audio starts.
    // Without this, MORE_PHI_PROFILE timers are silently dropped on the audio
    // thread because updateStats() skips unregistered sections (real-time safe).
    profiler_.prepare();
    profiler_.registerSection("processBlock_total");
    profiler_.registerSection("command_queue_drain");
    profiler_.registerSection("morph_computation");
    profiler_.registerSection("parameter_application");
    // AUDIT-2026-06-25: sub-section timers that split the dominant
    // parameter_application cost (the 2048-param loop) so the audit can
    // attribute cycles to read vs touch vs write. These are LEAF sections
    // (never nested in parameter_application) to avoid the double-count caveat.
    profiler_.registerSection("param_getvalue_read");
    profiler_.registerSection("param_touch_detect");
    profiler_.registerSection("param_setvalue_write");
    // AUDIT-2026-06-25: split the command drain into its two structurally
    // distinct branches (snapshot-restore marker vs normal param commands).
    profiler_.registerSection("command_drain_snapshot");
    profiler_.registerSection("command_drain_param");
    profiler_.registerSection("audio_domain_total");
    profiler_.registerSection("spectral_engine");
    profiler_.registerSection("granular_engine");
    profiler_.registerSection("formant_engine");
    profiler_.registerSection("hybrid_blend");
    profiler_.registerSection("midi_processing");
    profiler_.registerSection("hosted_plugin_process");
    profiler_.registerSection("sonicmaster_capture");
    profiler_.registerSection("modulation_engine");
    // Output-protection brickwall limiter on the main wet path (after hosted
    // plugin + HybridBlend + output gain). Registered so its audio-thread cost
    // is captured, per the AGENTS.md rule that sections MUST be registered here
    // before audio starts (else updateStats() silently drops them).
    profiler_.registerSection("output_protect");

    // Pre-allocate snapshot bank scratch buffers (real-time safety)
    snapshotBank.prepare(MAX_PARAMETERS);

    // Pre-allocate MIDI router buffers (real-time safety)
    midiRouter.prepare(128);  // Expected max MIDI events per block
    midiRouter.prepare(sampleRate, samplesPerBlock);  // H4 FIX: sidechain coefficients

    mcpStartPending_.store(true, std::memory_order_release);

    // Initialize AI assistant
    if (!aiAssistant_)
        aiAssistant_ = std::make_unique<AIAssistant>(*this);

    // H1 FIX: Initialize discrete parameter handler for real-time discrete param snapping
    discreteHandler_.initialize(parameterClassifier_);

    // V2: Prepare second host manager
    hostManagerB_.prepare(sampleRate, samplesPerBlock, getTotalNumOutputChannels());

    // V2: Prepare modulation engine
    modulationEngine_.prepare(sampleRate, samplesPerBlock, 2048);

    desiredOversamplingFactor_.store(
        factorFromChoiceIndex(readRawChoice(rawParams_.oversampling, 0)),
        std::memory_order_relaxed);
    {
        int fftSize = fftSizeFromChoiceIndex(readRawChoice(rawParams_.spectralFFTSize, 2));
        // PERF-CPU: Apply cpuSaver on initial prepare
        if (readRawBool(rawParams_.cpuSaver, false) && fftSize > 512)
            fftSize /= 2;
        desiredSpectralFFTSize_.store(fftSize, std::memory_order_relaxed);
    }

    reconfigureAudioDomainProcessing();
    updateReportedLatency();

    // V2: Pre-allocate scratch buffers
    bufferB_.setSize(getTotalNumOutputChannels(), samplesPerBlock);
    const int scratchSamples = samplesPerBlock * activeOversamplingFactor_.load(std::memory_order_relaxed);
    paramOut_.setSize(getTotalNumOutputChannels(), scratchSamples);
    spectralOut_.setSize(getTotalNumOutputChannels(), scratchSamples);
    granularOut_.setSize(getTotalNumOutputChannels(), scratchSamples);
    // C-6 FIX (audit): dry buffer for the bypass wet/dry crossfade (host rate,
    // same size as the incoming buffer — never oversampled).
    dryBuffer_.setSize(getTotalNumOutputChannels(), samplesPerBlock);

    // H-7 FIX: Seed the getStateInformation audio-thread cache now (on the
    // message thread) so that any audio-thread call to getStateInformation
    // (e.g. DAW offline export right after load) finds a valid cached state
    // instead of falling back to an empty MemoryBlock. The MemoryBlock is
    // immediately discarded — only the side-effect of populating
    // cachedSavedState_ inside getStateInformation matters.
    {
        juce::MemoryBlock seedBlock;
        getStateInformation(seedBlock);
    }

    prepared.store(true, std::memory_order_release);
    startTimer(50);  // persistent timer — always-on after prepare so audio thread never allocates to kick it
    diagnostics_.start();
}

void MorePhiProcessor::releaseResources()
{
    // C-1 FIX: Emit a seq_cst fence AFTER the prepared store so every core
    // observes prepared==false before the spin-wait begins. Without this, a
    // processBlock that loaded prepared==true just before the store might still
    // be mid-flight, inflating audioThreadActive_ and causing a spurious timeout.
    prepared.store(false, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // W4 FIX: Bounded drain of in-flight audio-thread leases. JUCE guarantees
    // the host won't call processBlock after releaseResources, but FL Studio
    // offline-render and a few export paths have violated that contract and
    // torn down subsystems while a processBlock was still mid-flight. We bound
    // the wait (100 ms) — on expiry we proceed, matching the unloadPlugin
    // policy of preferring a (rare) tear over freezing the host.
    const auto drainStart = juce::Time::getMillisecondCounter();
    while (audioThreadActive_.load(std::memory_order_acquire) > 0)
    {
        if (static_cast<int>(juce::Time::getMillisecondCounter() - drainStart) > 100)
        {
            DBG("MorePhiProcessor::releaseResources — timeout waiting for audio thread drain");
            break;
        }
        juce::Thread::sleep(1);
    }

    diagnostics_.stop();
    hostManager.setPlayHead(nullptr);
    hostManagerB_.setPlayHead(nullptr);
    hostManager.releaseResources();
    hostManagerB_.releaseResources();
    // Join the SonicMaster analysis thread BEFORE the mastering chain resets,
    // so the analysis thread never touches a torn-down AutoMasteringEngine.
    sonicMasterEngine_.release();
    autoMasteringEngine_.reset();
    neuralMasteringController_.resetStatus();
    spectralEngine_.reset();
    granularEngine_.reset();
    formantEngine_.reset();
    oversampling_.reset();
    oversamplingB_.reset();
    stopTimer();
    maintenanceTimerRequested_.store(false, std::memory_order_release);
}

void MorePhiProcessor::reset()
{
	    // Flush internal state for clean re-initialization
	    // (e.g., when host calls setActive(false) then setActive(true))
	    morphProcessor.prepare(MAX_PARAMETERS);  // Re-init morph processor
	    autoMasteringEngine_.reset();
	    spectralEngine_.reset();
	    granularEngine_.reset();
	    formantEngine_.reset();
	    oversampling_.reset();
	    oversamplingB_.reset();
	    modulationEngine_.reset();

    // AUDIT-FIX (P4, 2026-06-27): notify the SonicMaster engine that the audio
    // source may have changed (transport restart / reconfiguration). This arms
    // the transition guard so the next analysis cycle discards any capture window
    // that blends pre-reset and post-reset audio.
    sonicMasterEngine_.notifyAudioSourceChanged("processor_reset");
	}

bool MorePhiProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& mainOut = layouts.getMainOutputChannelSet();
    const auto& mainIn  = layouts.getMainInputChannelSet();

    const int numChannels = mainOut.size();
    // W2 FIX: clamp the main bus to mono/stereo. The mastering subsystem
    // (BrickwallLimiter, StereoFieldAnalyzer, MultibandDynamicsProcessor) and
    // the SonicMaster capture path all assume ≤2 channels. Accepting a surround
    // layout would let channels 3+ pass through un-limited, exceeding the
    // dBTP ceiling on those channels. The hosted-plugin wide-buffer path still
    // supports plugins with more internal channels via PluginHostManager.
    if (numChannels < 1 || numChannels > 2)
        return false;

    if (mainIn != mainOut)
        return false;
    // M-8 FIX: The plugin currently forces symmetric I/O (same channel count on
    // input and output). Relaxing this would require upmixing/downmixing logic
    // inside processBlock before routing to the hosted plugin.

    // Sidechain bus (index 1) can be disabled or mono/stereo
    if (layouts.inputBuses.size() > 1)
    {
        const auto& sc = layouts.inputBuses[1];
        if (!sc.isDisabled() &&
            sc != juce::AudioChannelSet::mono() &&
            sc != juce::AudioChannelSet::stereo())
            return false;
    }

    return true;
}

// C-2/M-3 AUDIT-NOTE: This function IS real-time safe despite being called from
// processBlock(). It only reads APVTS parameters via readRawFloat/readRawBool/
// readRawChoice, which opt out of the CriticalSection by accessing the underlying
// std::atomic<float>* directly (see AudioProcessorValueTreeState::getRawParameterValue).
// No APVTS CriticalSection is taken. All writes are to std::atomic<> members with
// relaxed ordering (UI→audio hints — eventual visibility is sufficient).
void MorePhiProcessor::syncStateFromAPVTS()
{
    morphX_.store(juce::jlimit(0.0f, 1.0f,
                    readRawFloat(rawParams_.morphX, morphX_.load(std::memory_order_relaxed))),
                  std::memory_order_relaxed);
    morphY_.store(juce::jlimit(0.0f, 1.0f,
                    readRawFloat(rawParams_.morphY, morphY_.load(std::memory_order_relaxed))),
                  std::memory_order_relaxed);
    faderPos_.store(juce::jlimit(0.0f, 1.0f,
                      readRawFloat(rawParams_.faderPos, faderPos_.load(std::memory_order_relaxed))),
                    std::memory_order_relaxed);

    morphSource_.store(juce::jlimit(0, 1,
                        readRawChoice(rawParams_.morphSource, morphSource_.load(std::memory_order_relaxed))),
                      std::memory_order_relaxed);

    physicsMode_.store(juce::jlimit(0, 2,
                        readRawChoice(rawParams_.physicsMode, physicsMode_.load(std::memory_order_relaxed))),
                      std::memory_order_relaxed);
    smoothingRate_.store(juce::jlimit(0.0f, 0.999f,
                         readRawFloat(rawParams_.smoothing, smoothingRate_.load(std::memory_order_relaxed))),
                         std::memory_order_relaxed);
    driftSpeed_.store(juce::jlimit(0.01f, 2.0f,
                      readRawFloat(rawParams_.driftSpeed, driftSpeed_.load(std::memory_order_relaxed))),
                      std::memory_order_relaxed);
    driftDistance_.store(juce::jlimit(0.0f, 1.0f,
                         readRawFloat(rawParams_.driftDistance, driftDistance_.load(std::memory_order_relaxed))),
                         std::memory_order_relaxed);
    driftChaos_.store(juce::jlimit(0.0f, 1.0f,
                      readRawFloat(rawParams_.driftChaos, driftChaos_.load(std::memory_order_relaxed))),
                      std::memory_order_relaxed);

    recallMode_.store(juce::jlimit(0, 1,
                       readRawChoice(rawParams_.recallMode, recallMode_.load(std::memory_order_relaxed))),
                      std::memory_order_relaxed);
    sidechainEnabled_.store(readRawBool(rawParams_.sidechainEnabled,
                           sidechainEnabled_.load(std::memory_order_relaxed)),
                           std::memory_order_relaxed);
    const float scThresholdDb = juce::jlimit(-60.0f, 0.0f,
                             readRawFloat(rawParams_.sidechainThreshold,
                                       sidechainThreshold_.load(std::memory_order_relaxed)));
    sidechainThreshold_.store(scThresholdDb, std::memory_order_relaxed);
    // ATS-5 FIX: Pre-compute linear threshold here (once per block) instead of
    // calling std::pow on the audio thread every block.
    sidechainThresholdLinear_.store(std::pow(10.0f, scThresholdDb / 20.0f),
                                     std::memory_order_relaxed);
    listenMode_.store(readRawBool(rawParams_.listenMode, listenMode_.load(std::memory_order_relaxed)),
                      std::memory_order_relaxed);
    recallToggle_.store(readRawBool(rawParams_.recallToggle, getRecallToggle()) ? 1 : 0,
                        std::memory_order_relaxed);
    linkEnabled_.store(readRawBool(rawParams_.linkMode, linkEnabled_.load(std::memory_order_relaxed)),
                       std::memory_order_relaxed);

    // ── Engine: Spectral ──────────────────────────────────────────────────
    const bool spectralActive = readRawBool(rawParams_.spectralActive, spectralEngine_.isActive());
    spectralEngine_.setActive(spectralActive);
    int desiredFFT = fftSizeFromChoiceIndex(readRawChoice(rawParams_.spectralFFTSize, 2));
    // PERF-CPU: cpuSaver halves the effective FFT size and caps oversampling
    // at x2. Reduces audio-domain CPU by ~40-60% for CPU-constrained systems.
    const bool cpuSaver = readRawBool(rawParams_.cpuSaver, false);
    cpuSaver_.store(cpuSaver, std::memory_order_relaxed);
    if (cpuSaver)
    {
        // Halve FFT size but never below 512
        if (desiredFFT > 512) desiredFFT /= 2;
    }
    if (desiredSpectralFFTSize_.exchange(desiredFFT, std::memory_order_acq_rel) != desiredFFT)
        audioDomainConfigDirty_.store(true, std::memory_order_release);
    spectralEngine_.setTransientPreserve(readRawBool(rawParams_.spectralTransient, true));
    spectralEngine_.setFormantPreserve(readRawBool(rawParams_.spectralFormant, false));

    // ── Engine: Granular ──────────────────────────────────────────────────
    const bool granularActive = readRawBool(rawParams_.granularActive, granularEngine_.isActive());
    granularEngine_.setActive(granularActive);
    granularEngine_.setGrainSize(readRawFloat(rawParams_.grainSize, 50.0f));
    granularEngine_.setGrainDensity(readRawFloat(rawParams_.grainDensity, 20.0f));
    granularEngine_.setPitchRandomization(readRawFloat(rawParams_.grainPitch, 0.0f));
    granularEngine_.setPositionRandomization(readRawFloat(rawParams_.grainScatter, 0.0f));

    // ── Engine: Hybrid Blend ──────────────────────────────────────────────
    const bool audioDomainEnabled = readRawBool(rawParams_.audioDomainEnabled, false);
    audioDomainEnabled_.store(audioDomainEnabled, std::memory_order_relaxed);

    int desiredFactor = factorFromChoiceIndex(readRawChoice(rawParams_.oversampling, 0));
    // PERF-CPU: cpuSaver caps oversampling at x2
    if (cpuSaver && desiredFactor > 2) desiredFactor = 2;
    if (desiredOversamplingFactor_.exchange(desiredFactor, std::memory_order_acq_rel) != desiredFactor)
        audioDomainConfigDirty_.store(true, std::memory_order_release);

    hybridParamWeight_.store(readRawFloat(rawParams_.blendParamWeight, 1.0f),
                             std::memory_order_relaxed);
    hybridSpectralWeight_.store(readRawFloat(rawParams_.blendSpectralWeight, 0.0f),
                                std::memory_order_relaxed);
    hybridGranularWeight_.store(readRawFloat(rawParams_.blendGranularWeight, 0.0f),
                                std::memory_order_relaxed);

    // ── Morph Alpha (shared crossfade) ────────────────────────────────────
    morphAlpha_.store(juce::jlimit(0.0f, 1.0f, readRawFloat(rawParams_.morphAlpha, 0.0f)),
                      std::memory_order_relaxed);

    // PERF-OPT opt-in flags.
    coarseParameterWrites_.store(readRawBool(rawParams_.coarseParameterWrites, false),
                                 std::memory_order_relaxed);
    disableTouchDetection_.store(readRawBool(rawParams_.disableTouchDetection, false),
                                 std::memory_order_relaxed);
    throttleParamCommits_.store(readRawBool(rawParams_.throttleParamCommits, false),
                                std::memory_order_relaxed);

    // Output protection: push the toggle + ceiling to the wet-path limiter and
    // dirty latency so updateReportedLatency() adds/removes the 4 ms lookahead.
    // The limiter setters are atomic / any-thread safe.
    const bool outputProtectOn = readRawBool(rawParams_.outputProtect, true);
    morphOutputLimiter_.setEnabled(outputProtectOn);
    if (rawParams_.outputCeiling != nullptr)
        morphOutputLimiter_.setCeiling(rawParams_.outputCeiling->load(std::memory_order_relaxed));
    if (outputProtectOn != lastOutputProtectEnabled_)
    {
        lastOutputProtectEnabled_ = outputProtectOn;
        latencyConfigDirty_.store(true, std::memory_order_release);
    }

    if (audioDomainEnabled != lastLatencyAudioDomainEnabled_
        || spectralActive != lastLatencySpectralActive_
        || granularActive != lastLatencyGranularActive_)
    {
        lastLatencyAudioDomainEnabled_ = audioDomainEnabled;
        lastLatencySpectralActive_ = spectralActive;
        lastLatencyGranularActive_ = granularActive;
        latencyConfigDirty_.store(true, std::memory_order_release);
    }

    // SonicMaster realtime neural mastering toggle (preview).
    // Uses the cached raw parameter pointer (cached in cacheRawParameterPointers)
    // to avoid dynamic_cast + string-keyed map lookup.
    sonicMasterEngine_.setActive(readRawBool(rawParams_.sonicMasterEnabled, false));

    if (audioDomainConfigDirty_.load(std::memory_order_acquire)
        || latencyConfigDirty_.load(std::memory_order_acquire))
        requestMessageThreadMaintenance();

    apvtsStateDirty_.store(false, std::memory_order_release);
}

// ── Process Block ────────────────────────────────────────────────────────────

void MorePhiProcessor::updateTransportContextSnapshot(juce::AudioPlayHead* newPlayHead) noexcept
{
    if (newPlayHead == nullptr)
    {
        transportAvailable_.store(false, std::memory_order_relaxed);
        transportPlaying_.store(false, std::memory_order_relaxed);
        transportLooping_.store(false, std::memory_order_relaxed);
        return;
    }

    const auto position = newPlayHead->getPosition();
    if (!position.hasValue())
    {
        transportAvailable_.store(false, std::memory_order_relaxed);
        return;
    }

    const auto& info = *position;
    transportAvailable_.store(true, std::memory_order_relaxed);
    transportPlaying_.store(info.getIsPlaying(), std::memory_order_relaxed);
    transportLooping_.store(info.getIsLooping(), std::memory_order_relaxed);
    transportBpm_.store(info.getBpm().orFallback(0.0), std::memory_order_relaxed);
    transportPpqPosition_.store(info.getPpqPosition().orFallback(0.0), std::memory_order_relaxed);
    transportSecondsPosition_.store(info.getTimeInSeconds().orFallback(0.0), std::memory_order_relaxed);

    const auto signature = info.getTimeSignature().orFallback(juce::AudioPlayHead::TimeSignature{});
    // M-10 FIX: Time signature is read but never used internally. Removed dead
    // atomic stores to avoid unnecessary work on the audio thread.
    juce::ignoreUnused(signature);
}

void MorePhiProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                    juce::MidiBuffer& midi) noexcept
{
    juce::ScopedNoDenormals noDenormals;

    // P2 FIX: Explicit acquire load — pairs with prepareToPlay / releaseResources
    // release stores. Avoids implicit seq_cst overhead on the hot path.
    // M-4 FIX: Single prepared check before incrementing audioThreadActive_.
    // The second check after fetch_add was redundant — the AudioGuard
    // destructor always decrements even if the config changes between check
    // and process start. Removing one atomic load per block on the hot path.
    if (!prepared.load(std::memory_order_acquire)
        || shuttingDown_.load(std::memory_order_acquire)) return;

    // H-1 FIX: Cache the block-start timestamp once per block. Every audio-thread
    // call to juce::Time::getMillisecondCounter() is a kernel round-trip
    // (timeGetTime / QueryPerformanceCounter on Windows); batching removes
    // ~3-8 µs of syscall overhead per block from the hot path.
    const juce::uint32 blockNow = juce::Time::getMillisecondCounter();
    blockTimestampMs_.store(blockNow, std::memory_order_relaxed);

    audioThreadActive_.fetch_add(1, std::memory_order_acq_rel);
    struct AudioGuard { std::atomic<int>& ref; ~AudioGuard() { ref.fetch_sub(1, std::memory_order_acq_rel); } } audioGuard{audioThreadActive_};

    // The persistent maintenance timer (started in prepareToPlay) will pick up
    // pendingStateRestore_ on its next tick — no need to allocate here.

    auto* const currentPlayHead = getPlayHead();
    hostManager.setPlayHead(currentPlayHead);
    hostManagerB_.setPlayHead(currentPlayHead);
    updateTransportContextSnapshot(currentPlayHead);

    // Keep runtime atomics coherent with APVTS automation/preset state.
    if (apvtsStateDirty_.load(std::memory_order_acquire))
        syncStateFromAPVTS();

    // PERF-M4: Cache parameter count once per block to avoid repeated virtual calls
    const int cachedParamCount = juce::jmin(paramBridge.getParameterCount(),
                                            static_cast<int>(finalOutput_.size()),
                                            static_cast<int>(lastApplied_.size()));

    // C-3 FIX: Separate command-drain gating from parameter application.
    // The commandConsumerLock_ try-lock only gates the drain (which shares
    // drainScratch_ with the assistant flush path). Parameter application
    // always proceeds — it has its own touchStateLock_ try-lock and the
    // morph output must reach the hosted plugin every block.
    juce::SpinLock::ScopedTryLockType parameterCommandGuard(commandConsumerLock_);
    const bool canDrainCommands = parameterCommandGuard.isLocked();

    const bool isBypassed = readRawBool(rawParams_.bypass, false);

    // Guard: skip morph processing while state is being restored (async plugin reload)
    if (isRestoring_.load(std::memory_order_acquire))
    {
        // Still forward audio through the host if it's loaded
        if (!isBypassed)
            hostManager.processBlock(buffer, midi);
        // ponytail: same throttle as the main path (see 7b).
        if (++analysisSkipCounter_ >= ANALYSIS_THROTTLE_BLOCKS)
        {
            analysisSkipCounter_ = 0;
            autoMasteringEngine_.analyzeBlock(buffer);
        }
        return;
    }

    // Profile total processBlock time
    MORE_PHI_PROFILE(profiler_, "processBlock_total");

    drainParameterCommandQueue(cachedParamCount, canDrainCommands, blockNow);
    processMIDIAndSidechain(midi, buffer);
    applyMorphAndParameters(buffer, cachedParamCount, /*canApplyParameters=*/true, blockNow);
    // C-5 FIX (audit): advance any active recall ramp AFTER the morph apply so
    // the ramp's linear-interpolated values win when both run in the same block
    // (the MIDI recall path starts the ramp and invalidates the apply cache so
    // the morph treats the ramped values as the new baseline). Fast no-op
    // (single acquire-load) when no ramp is active.
    paramBridge.processRecallRamp();
    applyOutputGainAndMetering(buffer, isBypassed);

    // SonicMaster analysis capture: lock-free ring write only (no locks, no
    // allocation). Early-returns when the feature is OFF or unprepared.
    // AUDIT-FIX-R3: support mono capture by duplicating channel 0 to both L and
    // R. Mono vocal/podcast tracks are a primary mastering use case.
    const int numCaptureChannels = buffer.getNumChannels();
    if (numCaptureChannels >= 1)
    {
        MORE_PHI_PROFILE(profiler_, "sonicmaster_capture");
        if (numCaptureChannels >= 2)
        {
            sonicMasterEngine_.capture(buffer.getReadPointer(0),
                                       buffer.getReadPointer(1),
                                       static_cast<std::size_t>(buffer.getNumSamples()));
        }
        else
        {
            // Mono: duplicate channel 0 into both L and R.
            sonicMasterEngine_.capture(buffer.getReadPointer(0),
                                       buffer.getReadPointer(0),
                                       static_cast<std::size_t>(buffer.getNumSamples()));
        }
    }
}

juce::AudioProcessorParameter* MorePhiProcessor::getBypassParameter() const
{
    // BP-3 FIX (audit): returning the APVTS "bypass" param tells the host to
    // drive its native bypass gesture through it. processBlock then reads the
    // bypass state (isBypassed) and runs the C-6 wet/dry crossfade — so host
    // bypass is click-free instead of the hard JUCE default.
    return bypassParameter_;
}

void MorePhiProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer,
                                            juce::MidiBuffer& midi) noexcept
{
    // BP-3 FIX (audit): with getBypassParameter() returning the bypass param,
    // JUCE routes host bypass through the param → processBlock → C-6 crossfade,
    // so this override is normally unreachable. Implement it as a safe fallback
    // for any host that calls the dedicated bypassed path directly: route back
    // through the normal pipeline (the bypass param's current value governs the
    // crossfade), preserving click-free behavior.
    processBlock(buffer, midi);
}

// ── Private helpers ─────────────────────────────────────────────────────────

void MorePhiProcessor::drainParameterCommandQueue(int cachedParamCount,
                                                  bool canDrainCommands, juce::uint32 now) noexcept
{
    MORE_PHI_PROFILE(profiler_, "command_queue_drain");
    if (canDrainCommands)
        (void) drainParameterCommandQueue(cachedParamCount, 2048, nullptr, nullptr, now);
}

void MorePhiProcessor::processMIDIAndSidechain(juce::MidiBuffer& midi,
                                               juce::AudioBuffer<float>& buffer) noexcept
{
    MORE_PHI_PROFILE(profiler_, "midi_processing");

    // 2) Process MIDI: filter trigger notes, pass rest through
    filteredMidiBuffer_.clear();
    midiRouter.processMidi(midi, filteredMidiBuffer_);
    modulationEngine_.processMIDI(filteredMidiBuffer_);

    // 2b) Process sidechain trigger (audio-driven snapshot recall)
    if (sidechainEnabled_.load(std::memory_order_relaxed))
    {
        // H-6 FIX: JUCE 8 sidechain access — getBusBuffer returns empty buffer if bus disabled
        auto scBuffer = getBusBuffer(buffer, true, 1);
        if (scBuffer.getNumChannels() > 0)
        {
            // ATS-5 FIX: Use pre-computed linear threshold (computed in syncStateFromAPVTS)
            midiRouter.setSidechainThreshold(sidechainThresholdLinear_.load(std::memory_order_relaxed));
            midiRouter.processSidechain(scBuffer);
        }
    }
}

void MorePhiProcessor::applyMorphAndParameters(juce::AudioBuffer<float>& buffer,
                                              int cachedParamCount,
                                              bool canTouchHostedParameters,
                                              juce::uint32 /*now*/) noexcept
{
    // 3) Sync physics tuning from atomics
    morphProcessor.setElasticPreset(
        static_cast<ElasticPreset>(elasticPreset_.load(std::memory_order_relaxed)));
    morphProcessor.setDriftSpeed(driftSpeed_.load(std::memory_order_relaxed));
    morphProcessor.setDriftDistance(driftDistance_.load(std::memory_order_relaxed));
    morphProcessor.setDriftChaos(driftChaos_.load(std::memory_order_relaxed));
    morphProcessor.setSmoothingRate(smoothingRate_.load(std::memory_order_relaxed));

    // Sync Listen Mode state to morph processor
    morphProcessor.setListenMode(listenMode_.load(std::memory_order_relaxed));

    // 4) Compute morph target values through physics pipeline
    {
        MORE_PHI_PROFILE(profiler_, "morph_computation");
        // M-2 FIX: Clamp paramCount to the actual finalOutput_ buffer size.
        const int paramCount = juce::jmin(cachedParamCount,
                                          static_cast<int>(finalOutput_.size()));
        if (paramCount > 0 && snapshotBank.hasAnyOccupied())
        {
            // CRITICAL: No resize here - buffer pre-allocated in prepareToPlay
            // Clear only the portion we'll use
            const size_t usedCount = static_cast<size_t>(paramCount);
            if (finalOutput_.size() >= usedCount)
                juce::FloatVectorOperations::clear(finalOutput_.data(), static_cast<int>(usedCount));

            const float dt = static_cast<float>(currentBlockSize) /
                             static_cast<float>(currentSampleRate);
            const auto source = static_cast<MorphSource>(
                morphSource_.load(std::memory_order_relaxed));
            const auto mode = static_cast<MorphMode>(
                physicsMode_.load(std::memory_order_relaxed));
            const float mx = morphX_.load(std::memory_order_relaxed);
            const float my = morphY_.load(std::memory_order_relaxed);
            const float fp = faderPos_.load(std::memory_order_relaxed);

            // Link Mode: if follower, override morph position from leader
            float linkX = mx, linkY = my;
            if (linkEnabled_.load(std::memory_order_relaxed) && !linkBroadcaster_.isLeader())
            {
                linkBroadcaster_.setEnabled(true);
                if (linkBroadcaster_.receive(linkX, linkY))
                {
                    // H7 FIX: Validate shared-memory reads before applying.
                    // Corrupted or malicious shared memory could inject NaN
                    // or out-of-range values that bypass clamping in setMorphX/Y.
                    if (!std::isfinite(linkX)) linkX = 0.5f;
                    if (!std::isfinite(linkY)) linkY = 0.5f;
                    linkX = juce::jlimit(0.0f, 1.0f, linkX);
                    linkY = juce::jlimit(0.0f, 1.0f, linkY);
                    morphX_.store(linkX, std::memory_order_relaxed);
                    morphY_.store(linkY, std::memory_order_relaxed);
                }
                else
                {
                    linkX = mx;
                    linkY = my;
                }
            }

            // PERF-C3: Early-exit when morph position is static in Direct mode.
            // Interpolation is deterministic — same position → identical output.
            // After the output stabilizes for MORPH_STABLE_THRESHOLD blocks,
            // skip the expensive compute2D/compute1D + smoothing + SIMD walks.
            // We still tick down touch cooldowns so they expire on schedule.
            const int modeInt = static_cast<int>(mode);
            const int sourceInt = static_cast<int>(source);
            const bool positionChanged = (linkX != prevMorphX_ || linkY != prevMorphY_ ||
                                          fp != prevFaderPos_ || modeInt != prevPhysicsMode_ ||
                                          sourceInt != prevMorphSource_);
            if (positionChanged)
            {
                prevMorphX_ = linkX;
                prevMorphY_ = linkY;
                prevFaderPos_ = fp;
                prevPhysicsMode_ = modeInt;
                prevMorphSource_ = sourceInt;
            }

            // PERF-C3+: Extend the steady-state skip to settled Elastic. Stability
            // is measured by ACTUAL hosted-parameter write activity this block
            // (anyWriteThisBlock, set in the apply loop below), not just the raw
            // target. This keeps Elastic's smoothing tail (which keeps writing
            // after the spring settles) from being frozen mid-ramp, and keeps
            // Drift — which writes every block — from ever skipping (its output
            // genuinely changes every block). Direct reaches the skip exactly as
            // before once output is stable. morphStableBlocks_ is updated after
            // the apply loop using anyWriteThisBlock.
            bool anyWriteThisBlock = positionChanged;

            // PERF-OPT: optional throttled commit. Compute the morph every block
            // but only push setValue updates to the hosted plugin every Nth block,
            // cutting the hosted-plugin setValue storm during continuous morphing
            // (e.g. Drift) at the cost of a few-ms parameter latency. Throttled
            // (non-commit) blocks are treated as "active" so they never feed the
            // steady-state skip — we can't know the output is stable without
            // committing it.
            constexpr int kParamCommitInterval = 4;
            const bool throttleCommits = throttleParamCommits_.load(std::memory_order_relaxed);
            const bool commitThisBlock = !throttleCommits || (paramCommitCounter_ == 0);
            if (throttleCommits)
                paramCommitCounter_ = (paramCommitCounter_ + 1) % kParamCommitInterval;
            if (!commitThisBlock)
                anyWriteThisBlock = true;

            const bool canSkipMorph = (mode == MorphMode::Direct || mode == MorphMode::Elastic)
                                   && !positionChanged
                                   && morphStableBlocks_ > MORPH_STABLE_THRESHOLD;

            if (!canSkipMorph)
            {
                morphProcessor.process(linkX, linkY, fp, source, mode, dt, finalOutput_);

                // V2: Modulation engine
                const double hostBpm = transportBpm_.load(std::memory_order_relaxed);
                modulationEngine_.setBPM(hostBpm > 0.0 ? static_cast<float>(hostBpm) : 120.0f);
                modulationEngine_.setMorphPosition(linkX, linkY, fp);
                // PERF-MOD-IDLE: Skip envelope-follower input processing and
                // source ticking when no modulation routes are published (the
                // common case). processAudioInput otherwise runs a per-sample
                // pass over the block every morphing block for nothing.
                // hasActiveRoutes() is audio-thread safe (seqlock-guarded).
                if (modulationEngine_.hasActiveRoutes())
                {
                    modulationEngine_.processAudioInput(buffer.getReadPointer(0), buffer.getNumSamples());
                    MORE_PHI_PROFILE(profiler_, "modulation_engine");
                    modulationEngine_.processBlock(finalOutput_, dt);
                }

                // H1 FIX: Snap discrete parameters to valid steps with hysteresis.
                // Fix 2.6: pass a morph progress that is meaningful in BOTH modes
                // (Fader → faderPos; XY-pad → radial cursor magnitude 0..1) so the
                // HoldSource/Crossfade strategies behave consistently, and pass the
                // real block dt so Stepwise traversal time is host-config independent.
                if (!finalOutput_.empty())
                {
                    float discreteMorphAmount = fp;
                    if (source == MorphSource::XYPad)
                    {
                        const float px = morphProcessor.getProcessedX();   // [-1,1]
                        const float py = morphProcessor.getProcessedY();   // [-1,1]
                        discreteMorphAmount = std::max(std::abs(px), std::abs(py));
                    }
                    discreteHandler_.processDiscreteParameters(
                        finalOutput_, finalOutput_, discreteMorphAmount, dt);
                }
            }

            // Link Mode: if leader, broadcast current morph position
            if (linkEnabled_.load(std::memory_order_relaxed) && linkBroadcaster_.isLeader())
            {
                linkBroadcaster_.setEnabled(true);
                linkBroadcaster_.broadcast(linkX, linkY);
            }

            // Apply interpolated values to hosted plugin
            {
                MORE_PHI_PROFILE(profiler_, "parameter_application");

                // A/B Compare override: if active, substitute the stored state
                // for the morph output so the plugin sees the captured reference
                // values instead of the live morph position.
                const bool abOverride = abCompareActive_.load(std::memory_order_relaxed);
                if (abOverride && !abCompareState_.empty() && paramCount > 0)
                {
                    const int copyCount = juce::jmin(paramCount,
                                                     static_cast<int>(abCompareState_.size()));
                    for (int i = 0; i < copyCount; ++i)
                    {
                        const float val = abCompareState_[static_cast<size_t>(i)];
                        if (val >= 0.0f)
                            finalOutput_[static_cast<size_t>(i)] = val;
                    }
                }

                // PERF-C3: When morph is static in Direct mode and output has
                // stabilized, skip parameter writes entirely. Still tick down
                // active touch cooldowns so they expire on schedule.
                if (canSkipMorph && canTouchHostedParameters)
                {
                    const bool skipTouchLock = touchStateLock_.tryEnter();
                    if (skipTouchLock)
                    {
                        const int touchSize = juce::jmin(paramCount, static_cast<int>(touchCooldown_.size()));
                        for (int i = 0; i < touchSize; ++i)
                        {
                            if (touchCooldown_[static_cast<size_t>(i)] > 0)
                                --touchCooldown_[static_cast<size_t>(i)];
                        }
                        touchStateLock_.exit();
                    }
                }
                else if (commitThisBlock && canTouchHostedParameters
                         && finalOutput_.size() >= static_cast<size_t>(paramCount))
                {
                    if (auto* plugin = hostManager.acquirePluginForUse())
                    {
                        struct ScopedRelease {
                            PluginHostManager& hm;
                            ~ScopedRelease() { hm.releasePluginFromUse(); }
                        } release{ hostManager };

                        // PERF-C2: Batch-read current hosted parameter values once,
                        // then use the cached snapshot for touch detection. Without this,
                        // each iteration calls params[i]->getValue() individually, causing
                        // up to 2048 virtual dispatches + L1 cache-pollution cycles per
                        // block — the dominant CPU cost in FL Studio with small buffers.
                        auto& pluginParams = plugin->getParameters();
                        const int pluginParamCount = juce::jmin(paramCount, static_cast<int>(pluginParams.size()));

                        // PERF-OPT opt-in flags (default OFF -> current behavior preserved).
                        const float writeDeadband = coarseParameterWrites_.load(std::memory_order_relaxed)
                                                    ? 5e-4f : 1e-5f;

                        if (disableTouchDetection_.load(std::memory_order_relaxed))
                        {
                            // PERF-OPT: touch detection disabled — pure morph output.
                            // Skips the per-block batch getValue() read (the dominant CPU
                            // cost during morphing) and all touch/hold/cooldown logic.
                            // Manual hosted-knob edits while morphing are NOT held in this
                            // mode (opt-in trade-off). lastApplied_ still tracks writes for
                            // the deadband skip, guarded by the same touchStateLock_.
                            const bool hasLock = touchStateLock_.tryEnter();
                            for (int i = 0; i < paramCount; ++i)
                            {
                                const size_t idx = static_cast<size_t>(i);
                                const float morphVal = finalOutput_[idx];
                                if (morphVal < 0.0f) continue;            // Listen Mode discrete sentinel
                                if (i >= pluginParamCount) continue;
                                if (hasLock && lastApplied_[idx] >= 0.0f
                                    && std::abs(morphVal - lastApplied_[idx]) < writeDeadband)
                                    continue;
                                const float clamped = juce::jlimit(0.0f, 1.0f, morphVal);
                                // C-1 FIX: use SEH wrapper for setValue
                                if (!ParameterBridge::setValueNoexcept(pluginParams[i], clamped, &paramBridge))
                                    continue;
                                anyWriteThisBlock = true;
                                if (hasLock) lastApplied_[idx] = morphVal;
                            }
                            if (hasLock) touchStateLock_.exit();
                        }
                        else
                        {
                        // PERF-IA: Interleaved touch sampling — only call getValue()
                        // on 1/kTouchSamplingStride params per block, rotating the offset
                        // each block. This reduces the dominant CPU cost (virtual calls
                        // through the VST3 wrapper) by ~75%. Touch detection latency
                        // increases to kTouchSamplingStride blocks (~20ms at 256 samples)
                        // — acceptable because the cooldown is already ~200ms.
                        const int touchOffset = touchSamplingPhase_;
                        touchSamplingPhase_ = (touchSamplingPhase_ + 1) % kTouchSamplingStride;

                        // AUDIT-2026-06-25: isolate the getValue() batch read — the
                        // documented dominant per-block cost — as its own leaf section
                        // so the audit can quantify it independently of the interleaved
                        // touch/setValue loop below. ~512 calls (stride 4 of 2048).
                        {
                            MORE_PHI_PROFILE(profiler_, "param_getvalue_read");
                            // Only batch-read params in the current stride window
                            for (int i = touchOffset; i < pluginParamCount; i += kTouchSamplingStride)
                            {
                                // C-1 FIX: use SEH wrapper for getValue
                                currentParamSnapshot_[static_cast<size_t>(i)] = ParameterBridge::getValueNoexcept(pluginParams[i]);
                            }
                        }

                        const bool hasTouchLock = touchStateLock_.tryEnter();

                        for (int i = 0; i < paramCount; ++i)
                        {
                            const size_t idx = static_cast<size_t>(i);
                            const float morphVal = finalOutput_[idx];

                            // Skip sentinel-marked discrete params (Listen Mode)
                            if (morphVal < 0.0f) continue;

                            // C-3 FIX: liveEditHold_ is written under touchStateLock_ in
                            // drainParameterCommandQueue (both audio-thread and assistant-flush
                            // paths). To prevent a data race when the assistant flush holds
                            // touchStateLock_ while we don't, gate this check on hasTouchLock.
                            // When hasTouchLock is false, live-edit-holds are skipped for this
                            // block — the assistant's explicit edits take priority anyway.
                            if (hasTouchLock)
                            {
                                if (i < static_cast<int>(liveEditHold_.size()) && liveEditHold_[idx] != 0)
                                {
                                    if (shouldReleaseLiveEditHold(i, linkX, linkY, fp))
                                    {
                                        liveEditHold_[idx] = 0;
                                    }
                                    else
                                    {
                                        if (i < pluginParamCount)
                                            lastApplied_[idx] = currentParamSnapshot_[idx];
                                        continue;
                                    }
                                }
                            }

                            // Touch detection: check if user manually moved this parameter.
                            // PERF-C2: Use cached snapshot instead of per-param getValue().
                            // PERF-IA: Only run touch detection on params that were sampled
                            // this block (the rest have stale snapshots). For unsampled
                            // params, morph output still proceeds below — only touch
                            // detection is deferred to the next sampling window.
                            const bool paramSampledThisBlock =
                                ((i % kTouchSamplingStride) == touchOffset);
                            if (hasTouchLock && lastApplied_[idx] >= 0.0f && paramSampledThisBlock)
                            {
                                const float currentVal = (i < pluginParamCount)
                                    ? currentParamSnapshot_[idx] : 0.0f;
                                const float lastVal = lastApplied_[idx];
                                const float userDelta = std::abs(currentVal - lastVal);

                                if (userDelta > TOUCH_THRESHOLD)
                                {
                                    touchMorphX_[idx] = linkX;
                                    touchMorphY_[idx] = linkY;
                                    lastApplied_[idx] = currentVal;
                                    touchCooldown_[idx] = touchCooldownBlocks_;
                                }
                            }

                            // If parameter is in cooldown, skip applying morph output
                            if (hasTouchLock && touchCooldown_[idx] > 0)
                            {
                                --touchCooldown_[idx];

                                const float morphDelta = std::abs(linkX - touchMorphX_[idx]) +
                                                         std::abs(linkY - touchMorphY_[idx]);
                                const bool morphMoved = morphDelta > MORPH_POS_THRESHOLD;

                                if (touchCooldown_[idx] == 0 && morphMoved)
                                {
                                    lastApplied_[idx] = -1.0f;
                                }
                                else
                                {
                                    if (i < pluginParamCount)
                                        lastApplied_[idx] = currentParamSnapshot_[idx];
                                }
                                continue;
                            }

                            // Skip if the value has not changed significantly from the last applied value.
                            if (hasTouchLock && lastApplied_[idx] >= 0.0f)
                            {
                                const float lastVal = lastApplied_[idx];
                                if (std::abs(morphVal - lastVal) < writeDeadband)
                                    continue;
                            }

                            // Apply morph output and track what we applied
                            // PERF-C2: Direct setValue() bypasses per-param
                            // acquirePluginForUse/releasePluginFromUse and throttle
                            // lock acquisition (already batch-acquired above).
                            if (i < pluginParamCount)
                            {
                                const float clamped = juce::jlimit(0.0f, 1.0f, morphVal);
                                // C-1 FIX: use SEH wrapper for setValue
                                if (!ParameterBridge::setValueNoexcept(pluginParams[i], clamped, &paramBridge))
                                    continue;
                                anyWriteThisBlock = true;
                            }
                            if (hasTouchLock)
                                lastApplied_[idx] = morphVal;
                        }

                        if (hasTouchLock)
                            touchStateLock_.exit();
                        } // else (!disableTouchDetection)
                    }
                }
            }

            // PERF-C3+: update stability for next block from this block's actual
            // hosted-parameter write activity. Elastic reaches the skip once
            // writes stop (spring settled + smoothing converged); Drift writes
            // every block so it never skips.
            if (anyWriteThisBlock)
                morphStableBlocks_ = 0;
            else
                ++morphStableBlocks_;
        }
    }

    // 5) Write drift output for DAW automation recording
    //    Gated by dawWrite toggle — when OFF, drift movement is not
    //    recorded as automation (only user/MCP moves show up).
    const bool dawWriteOn = readRawBool(rawParams_.dawWrite, true);
    if (dawWriteOn)
    {
        if (rawParams_.driftOutputX != nullptr)
            rawParams_.driftOutputX->store((morphProcessor.getProcessedX() + 1.0f) * 0.5f,
                                           std::memory_order_relaxed);
        if (rawParams_.driftOutputY != nullptr)
            rawParams_.driftOutputY->store((morphProcessor.getProcessedY() + 1.0f) * 0.5f,
                                           std::memory_order_relaxed);
    }
}

void MorePhiProcessor::applyOutputGainAndMetering(juce::AudioBuffer<float>& buffer,
                                                   bool isBypassed) noexcept
{
    // C-6 FIX (audit): Bypass wet/dry crossfade.
    // The old code hard-switched: when isBypassed, the hosted plugin was
    // skipped entirely → buffer went from wet to dry in one sample → click.
    // We now ramp a wet/dry mix (bypassMix_: 1.0 = wet, 0.0 = dry) over
    // kBypassRampBlocks and crossfade. To keep the CPU-saver property of
    // bypass (skip the hosted plugin when fully dry), the wet path only runs
    // while bypassMix_ is meaningfully above 0 (i.e. active, or mid-fade-out).
    // Once settled fully dry, the original skip behavior is restored.
    const int ns = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();
    const float bypassTarget = isBypassed ? 0.0f : 1.0f;

    float bypassMix = bypassMix_.load(std::memory_order_relaxed);
    if (!bypassMixInitialized_)
    {
        // First block: jump straight to the target (no misleading fade from 1.0).
        bypassMix = bypassTarget;
        bypassMix_.store(bypassMix, std::memory_order_relaxed);
        bypassMixInitialized_ = true;
    }

    // Per-block linear step toward the target. kBypassRampBlocks gives the
    // fade length regardless of host block size (block-counted, not timed).
    const bool transitioning = std::abs(bypassMix - bypassTarget) > 1.0f / 256.0f;
    // C-6 FIX: the wet path must run whenever the TARGET is wet (so there is a
    // wet signal to fade IN on bypass-release) OR the current mix still has
    // wet content (to fade OUT cleanly). Only fully-bypassed steady state
    // (target dry AND mix≈0) skips the hosted plugin.
    const bool wetNeeded = (bypassTarget > 0.0f) || (bypassMix > 1.0f / 256.0f);

    // Capture dry BEFORE the hosted plugin processes buffer in place, but ONLY
    // when we will need it (mid-transition, or fully dry this block). When fully
    // wet and staying wet, the dry copy is wasted work — skip it.
    const bool needDryCapture = transitioning || isBypassed;
    if (needDryCapture && ns > 0 && numCh > 0
        && dryBuffer_.getNumChannels() >= numCh && dryBuffer_.getNumSamples() >= ns)
    {
        for (int c = 0; c < numCh; ++c)
            dryBuffer_.copyFrom(c, 0, buffer, c, 0, ns);
    }

    // 6) Forward audio + filtered MIDI to hosted plugin — only when wet is
    //    needed (active, or fading out). Fully-bypassed steady state skips it.
    if (wetNeeded)
    {
        MORE_PHI_PROFILE(profiler_, "hosted_plugin_process");
        // L-2 FIX: wall-clock watchdog around the hosted plugin processBlock.
        // Uses the cached block-start timestamp for the start measurement
        // (avoids one extra getMillisecondCounter syscall), then measures the
        // end. Only one syscall per block when the plugin processes.
        const juce::uint32 tHostedStart = blockTimestampMs_.load(std::memory_order_relaxed);
        hostManager.processBlock(buffer, filteredMidiBuffer_);
        const int hostedMs = static_cast<int>(
            juce::Time::getMillisecondCounter() - tHostedStart);
        if (hostedMs >= kHostedPluginOverrunThresholdMs)
        {
            uint64_t prev = hostedPluginOverrunCount_.load(std::memory_order_relaxed);
            if (prev != std::numeric_limits<uint64_t>::max())
                hostedPluginOverrunCount_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // V2: Audio-domain morph path (C-6 FIX: gated on wetNeeded, same as the
    // hosted-plugin path, so it's skipped once fully bypassed).
    if (wetNeeded
        && audioDomainEnabled_.load(std::memory_order_relaxed)
        && hostManagerB_.hasPlugin()
        && !audioDomainReconfiguring_.load(std::memory_order_acquire))
    {
        audioDomainUsers_.fetch_add(1, std::memory_order_acq_rel);
        const bool canProcessAudioDomain = !audioDomainReconfiguring_.load(std::memory_order_acquire);

        if (canProcessAudioDomain)
        {
            MORE_PHI_PROFILE(profiler_, "audio_domain_total");
            const float alpha = morphAlpha_.load(std::memory_order_relaxed);
            const int ns = buffer.getNumSamples();

            copyBufferRegion(bufferB_, buffer, ns);
            midiCopyB_.clear();
            midiCopyB_.addEvents(filteredMidiBuffer_, 0, -1, 0);
            hostManagerB_.processBlock(bufferB_, midiCopyB_);

            const bool spectralActive = spectralEngine_.isActive();
            const bool granularActive = granularEngine_.isActive();

            if (spectralActive || granularActive)
            {
                const int activeFactor = activeOversamplingFactor_.load(std::memory_order_relaxed);

                if (activeFactor <= 1)
                {
                    copyBufferRegion(paramOut_, buffer, ns);

                    if (spectralActive)
                    {
                        MORE_PHI_PROFILE(profiler_, "spectral_engine");
                        copyBufferRegion(spectralOut_, paramOut_, ns);
                        spectralEngine_.processBlock(spectralOut_, bufferB_, alpha);
                    }

                    if (granularActive)
                    {
                        MORE_PHI_PROFILE(profiler_, "granular_engine");
                        copyBufferRegion(granularOut_, paramOut_, ns);
                        granularEngine_.processBlock(granularOut_, bufferB_, alpha);
                    }

                    if (formantEngine_.isActive())
                    {
                        MORE_PHI_PROFILE(profiler_, "formant_engine");
                        // Fix 5: Capture plugin A's pre-morph dry output (paramOut_
                        // is a copy of buffer taken above) as the formant source
                        // envelope, so formant transplant tracks plugin A's actual
                        // spectral character instead of freezing on the first
                        // morphed frame. Captured once per engagement; re-captured
                        // if formant is toggled off and back on.
                        if (!formantSourceCaptured_)
                        {
                            formantEngine_.captureFormants(paramOut_);
                            formantSourceCaptured_ = true;
                        }
                        if (spectralActive)
                            formantEngine_.processBlock(spectralOut_);
                        if (granularActive)
                            formantEngine_.processBlock(granularOut_);
                    }
                    else
                    {
                        formantSourceCaptured_ = false;
                    }

                    MORE_PHI_PROFILE(profiler_, "hybrid_blend");
                    HybridBlend::blend(buffer,
                                       paramOut_,
                                       spectralActive ? spectralOut_ : paramOut_,
                                       granularActive ? granularOut_ : paramOut_,
                                       hybridParamWeight_.load(std::memory_order_relaxed),
                                       hybridSpectralWeight_.load(std::memory_order_relaxed),
                                       hybridGranularWeight_.load(std::memory_order_relaxed));
                }
                else
                {
                    juce::dsp::AudioBlock<float> inputBlock(buffer);
                    juce::dsp::AudioBlock<float> inputBlockB(bufferB_);
                    auto osABlock = oversampling_.upsample(inputBlock);
                    auto osBBlock = oversamplingB_.upsample(inputBlockB);
                    auto osABuffer = makeBlockBuffer(osABlock, osParamPtrs_);
                    auto osBBuffer = makeBlockBuffer(osBBlock, osBPtrs_);

                    const int osSamples = osABuffer.getNumSamples();
                    copyBufferRegion(paramOut_, osABuffer, osSamples);

                    if (spectralActive)
                    {
                        MORE_PHI_PROFILE(profiler_, "spectral_engine");
                        copyBufferRegion(spectralOut_, osABuffer, osSamples);
                        spectralEngine_.processBlock(spectralOut_, osBBuffer, alpha);
                    }

                    if (granularActive)
                    {
                        MORE_PHI_PROFILE(profiler_, "granular_engine");
                        copyBufferRegion(granularOut_, osABuffer, osSamples);
                        granularEngine_.processBlock(granularOut_, osBBuffer, alpha);
                    }

                    if (formantEngine_.isActive())
                    {
                        MORE_PHI_PROFILE(profiler_, "formant_engine");
                        // Fix 5: capture plugin A's pre-morph oversampled output
                        // as the formant source envelope (see non-OS branch above).
                        if (!formantSourceCaptured_)
                        {
                            formantEngine_.captureFormants(osABuffer);
                            formantSourceCaptured_ = true;
                        }
                        if (spectralActive)
                            formantEngine_.processBlock(spectralOut_);
                        if (granularActive)
                            formantEngine_.processBlock(granularOut_);
                    }
                    else
                    {
                        formantSourceCaptured_ = false;
                    }

                    MORE_PHI_PROFILE(profiler_, "hybrid_blend");
                    HybridBlend::blend(osABuffer,
                                       paramOut_,
                                       spectralActive ? spectralOut_ : paramOut_,
                                       granularActive ? granularOut_ : paramOut_,
                                       hybridParamWeight_.load(std::memory_order_relaxed),
                                       hybridSpectralWeight_.load(std::memory_order_relaxed),
                                       hybridGranularWeight_.load(std::memory_order_relaxed));

                    juce::dsp::AudioBlock<float> outputBlock(buffer);
                    oversampling_.downsample(outputBlock);
                }
            }
        }

        audioDomainUsers_.fetch_sub(1, std::memory_order_acq_rel);
    }

    // Apply post-processing output gain to the wet signal when wet is needed.
    // M9 FIX: Ramp gain smoothly across the block to avoid zipper noise.
    // C-6 FIX: gate on wetNeeded (not !isBypassed) so the gain still tracks
    // during the fade-out, then the crossfade below blends wet↔dry.
    if (wetNeeded && rawParams_.outputGain != nullptr)
    {
        const float gainDb = rawParams_.outputGain->load(std::memory_order_relaxed);
        const float targetGainLinear = juce::Decibels::decibelsToGain(gainDb);
        if (!gainSmoothingInitialized_)
        {
            // First processed block: jump directly to the current target to avoid
            // a misleading ramp from the default 1.0f.
            smoothedGain_.store(targetGainLinear, std::memory_order_relaxed);
            gainSmoothingInitialized_ = true;
            if (targetGainLinear != 1.0f)
                buffer.applyGain(targetGainLinear);
        }
        else
        {
            const float currentGain = smoothedGain_.load(std::memory_order_relaxed);
            if (buffer.getNumSamples() > 0 && targetGainLinear != currentGain)
            {
                buffer.applyGainRamp(0, buffer.getNumSamples(), currentGain, targetGainLinear);
                smoothedGain_.store(targetGainLinear, std::memory_order_relaxed);
            }
            else if (targetGainLinear != 1.0f)
            {
                buffer.applyGain(targetGainLinear);
            }
        }
    }

    // Output protection: lookahead brickwall limiter on the main wet path.
    // Sits after the hosted plugin + HybridBlend + output gain (all potential
    // overshoot sources during morphing) and before the wet/dry crossfade, so a
    // morph into a high-gain snapshot — or a correlated-peak HybridBlend sum —
    // cannot clip past the ceiling. Default ON (outputProtect). Bypass stays
    // truly dry: this only touches the wet `buffer`; the dry copy for the
    // crossfade was captured earlier. Lookahead is reported to the DAW via
    // latencyManager_ so PDC keeps the wet/dry fade aligned.
    if (wetNeeded && rawParams_.outputProtect != nullptr
        && rawParams_.outputProtect->load(std::memory_order_relaxed) > 0.5f)
    {
        if (rawParams_.outputCeiling != nullptr)
            morphOutputLimiter_.setCeiling(rawParams_.outputCeiling->load(std::memory_order_relaxed));
        MORE_PHI_PROFILE(profiler_, "output_protect");
        morphOutputLimiter_.processBlock(buffer);
    }

    // C-6 FIX (audit): Wet/dry crossfade + mix-ramp advance.
    // The ramp is LINEAR with a CONSTANT step of 1/kBypassRampBlocks toward the
    // target (not a proportional/exponential step) so it completes in exactly
    // kBypassRampBlocks blocks and lands precisely on the target. The mix
    // advances exactly once per block (the crossfade uses the pre-advance value
    // for THIS block's blend, then advances for next block).
    if (transitioning && ns > 0 && numCh > 0
        && dryBuffer_.getNumChannels() >= numCh && dryBuffer_.getNumSamples() >= ns)
    {
        // One block's worth of constant-velocity movement toward the target.
        const float step = (bypassTarget >= bypassMix ? 1.0f : -1.0f)
                         / static_cast<float>(kBypassRampBlocks);
        // The per-sample blend ramps from the current mix across this block to
        // the post-step mix for a continuous, click-free fade.
        const float nextMix = std::clamp(bypassMix + step, 0.0f, 1.0f);
        const float invNs = 1.0f / static_cast<float>(ns);
        // M-2 FIX: Vectorized wet/dry crossfade. Compute per-sample gain
        // coefficients into scratch arrays, then use FloatVectorOperations
        // multiply+add for SIMD throughput (4-8× faster than scalar on AVX/SSE).
        // The per-sample ramp is: m(t) = bypassMix + (nextMix - bypassMix) * t
        // so wet_gain[i] = m(i/ns), dry_gain[i] = 1 - wet_gain[i].
        // Scratch arrays are stack-allocated (ns <= typical 4096 → 32 KB stack).
        const auto uns = static_cast<size_t>(ns);
        wetGainScratch_.resize(uns);
        dryGainScratch_.resize(uns);
        for (int i = 0; i < ns; ++i)
        {
            const float m = bypassMix + (nextMix - bypassMix) * (static_cast<float>(i) * invNs);
            wetGainScratch_[static_cast<size_t>(i)] = m;
            dryGainScratch_[static_cast<size_t>(i)] = 1.0f - m;
        }
        for (int c = 0; c < numCh; ++c)
        {
            float* wet = buffer.getWritePointer(c);
            const float* dry = dryBuffer_.getReadPointer(c);
            // In-place: wet[i] = wet[i] * wetGain[i] + dry[i] * dryGain[i]
            juce::FloatVectorOperations::multiply(wet, wetGainScratch_.data(), ns);
            juce::FloatVectorOperations::addWithMultiply(wet, dry, dryGainScratch_.data(), ns);
        }
        bypassMix = nextMix;
    }
    else if (transitioning)
    {
        // Transitioning but no buffer to crossfade (e.g. zero samples) — still
        // advance the mix by one constant step so the ramp completes on time.
        const float step = (bypassTarget >= bypassMix ? 1.0f : -1.0f)
                         / static_cast<float>(kBypassRampBlocks);
        bypassMix = std::clamp(bypassMix + step, 0.0f, 1.0f);
    }
    // Snap to target if within one step (avoids straddling the threshold that
    // gates `transitioning` and leaving a tiny residual forever).
    if (std::abs(bypassMix - bypassTarget) <= 1.0f / static_cast<float>(kBypassRampBlocks))
        bypassMix = bypassTarget;
    bypassMix_.store(bypassMix, std::memory_order_relaxed);

    // 7b) Non-mutating live analysis tap for MCP/Track Assistant tools.
    // ponytail: throttle — the analysis runs LUFS+FFT+true-peak+stereo, and the
    // downstream decision chain only acts every ~30s. Per-block analysis was the
    // idle-CPU cause of the host-wide lag. RMS tap below uses the same pattern.
    if (++analysisSkipCounter_ >= ANALYSIS_THROTTLE_BLOCKS)
    {
        analysisSkipCounter_ = 0;
        autoMasteringEngine_.analyzeBlock(buffer);
    }

    // 8) Peak magnitude for UI visualization — H-8(a): replaced getRMSLevel()
    // (full buffer scan) with a simple magnitude approximation to reduce CPU.
    // W3 FIX: average L/R magnitude so a hard-panned signal is metered correctly.
    // M-1 FIX: Use FloatVectorOperations::findMinAndMax for SIMD peak detection
    // instead of scalar per-sample abs+compare.
    if (buffer.getNumChannels() > 0)
    {
        ++rmsSkipCounter_;
        if (rmsSkipCounter_ >= RMS_THROTTLE_BLOCKS)
        {
            rmsSkipCounter_ = 0;
            const int nch = buffer.getNumChannels();
            const int numSamples = buffer.getNumSamples();
            const auto rangeL = juce::FloatVectorOperations::findMinAndMax(buffer.getReadPointer(0),
                                                                            numSamples);
            const float peakL = std::max(std::abs(rangeL.getStart()), std::abs(rangeL.getEnd()));
            float peakR = peakL;
            if (nch > 1)
            {
                const auto rangeR = juce::FloatVectorOperations::findMinAndMax(buffer.getReadPointer(1),
                                                                               numSamples);
                peakR = std::max(std::abs(rangeR.getStart()), std::abs(rangeR.getEnd()));
            }
            setRmsLevel(0.5f * (peakL + peakR));
        }
    }
}

// ── State Persistence ────────────────────────────────────────────────────────

void MorePhiProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // P2 FIX: Detect calling thread so DBG() (which may allocate) is skipped
    // on the audio thread during offline render/export.
    bool isAudioThread = false;
    if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
        isAudioThread = !mm->isThisTheMessageThread();

    // C-2 FIX: If called from the audio thread (e.g. Pro Tools offline
    // render or FL Studio export), return the last cached state snapshot
    // that was saved on the message thread. This avoids heavy heap
    // allocations (XML + Base64 encoding) on the real-time path.
    if (isAudioThread)
    {
        // C-2 FIX (audit, full): the original fall-through built XML on the
        // audio thread when the cache was empty (export right after load).
        // That is an RT violation. If no cache exists yet, return an empty
        // block — the host treats this as "no state to save" and the next
        // message-thread getStateInformation (user save / timer) will build
        // and cache the real state. Returning empty loses no data the host
        // wouldn't already have (the plugin was just loaded). Critically,
        // we must NOT fall through to copyState()/createXml()/Base64 here.
        juce::MemoryBlock cached;
        {
            const juce::SpinLock::ScopedLockType lock(cachedSavedStateMutex_);
            cached = cachedSavedState_;
        }
        if (cached.getSize() > 0)
            destData = std::move(cached);
        // else: leave destData empty (no allocation, no XML build).
        return;
    }

    auto state = apvts.copyState();
    auto xml = state.createXml();
    if (!xml) return;

    // 1) Embed the hosted plugin description so we can reopen it on restore
    //    CRITICAL: Use getLastDescription() which persists even after unload
    if (auto* desc = hostManager.getLastDescription())
    {
        auto descXml = desc->createXml();
        if (descXml)
        {
            descXml->setTagName("HOSTED_PLUGIN");
            xml->addChildElement(descXml.release());
            if (!isAudioThread)
            {
#if JUCE_DEBUG
                DBG("getStateInformation: Saved hosted plugin description: " + desc->name);
#endif
            }
        }
        else
        {
            if (!isAudioThread)
            {
#if JUCE_DEBUG
                DBG("getStateInformation: WARNING - Failed to create XML from plugin description");
#endif
            }
        }
    }
    else
    {
        if (!isAudioThread)
        {
#if JUCE_DEBUG
            DBG("getStateInformation: No hosted plugin description available to save");
#endif
        }
    }

    // 2) Save the hosted plugin's own opaque state (EQ curves, compressor
    //    settings, etc.) — this is the data that was being lost on export
    //
    // H-4 FIX: beginExclusivePluginUse() may block (spin-waits for active
    // audio-thread users). getStateInformation can be called from the audio
    // thread during offline render / export in some DAWs (Pro Tools, FL Studio).
    // Detect the calling thread: only block on the message thread; on the audio
    // thread, fall back to the buffered pending state copy.
    {
        bool canBlockSafely = false;
        if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
            canBlockSafely = mm->isThisTheMessageThread();

        bool capturedPluginState = false;

        if (canBlockSafely)
        {
            if (auto* plugin = hostManager.beginExclusivePluginUse(500))
            {
                juce::MemoryBlock pluginState;
                try
                {
                    plugin->getStateInformation(pluginState);
                }
                catch (...)
                {
                    if (!isAudioThread)
                    {
#if JUCE_DEBUG
                        DBG("getStateInformation: WARNING - hosted plugin threw during getStateInformation");
#endif
                    }
                    pluginState.reset();
                }

                hostManager.endExclusivePluginUse();

                if (pluginState.getSize() > 0)
                {
                    auto stateXml = std::make_unique<juce::XmlElement>("HOSTED_PLUGIN_STATE");
                    stateXml->setAttribute("data", pluginState.toBase64Encoding());
                    xml->addChildElement(stateXml.release());
                    if (!isAudioThread)
                    {
#if JUCE_DEBUG
                        DBG("getStateInformation: Saved hosted plugin state, size: "
                            + juce::String(static_cast<int>(pluginState.getSize())) + " bytes");
#endif
                    }
                    capturedPluginState = true;
                }
            }
        }
        // else: audio thread or unknown — skip exclusive use, use pending state below

        // Fallback: if plugin is not currently loaded (or we're on audio thread),
        // try to preserve any pending state from setStateInformation.
        if (!capturedPluginState)
        {
            juce::MemoryBlock pendingStateCopy;
            {
                const juce::SpinLock::ScopedLockType guard(pendingStateMutex_);
                pendingStateCopy = pendingHostedState_;
            }

            if (pendingStateCopy.getSize() > 0)
            {
                auto stateXml = std::make_unique<juce::XmlElement>("HOSTED_PLUGIN_STATE");
                stateXml->setAttribute("data", pendingStateCopy.toBase64Encoding());
                xml->addChildElement(stateXml.release());
                if (!isAudioThread)
                {
#if JUCE_DEBUG
                    DBG("getStateInformation: Saved pending hosted plugin state, size: "
                        + juce::String(static_cast<int>(pendingStateCopy.getSize())) + " bytes");
#endif
                }
            }
        }
    }

    // 3) Persist Snapshot Bank (all 12 slots of morph data)
    if (auto bankXml = snapshotBank.toXml())
        xml->addChildElement(bankXml.release());

    // 4) Persist Modulation Engine state (routes, LFO settings, envelopes)
    if (auto modXml = modulationEngine_.toXml())
    {
        modXml->setTagName("MODULATION_ENGINE");
        xml->addChildElement(modXml.release());
    }

    // 5) Persist SanityConfig safely
    auto sc = getSanityConfigCopy();
    auto sanityXml = std::make_unique<juce::XmlElement>("SANITY_CONFIG");
    sanityXml->setAttribute("enabled", sc.enabled);
    for (int idx : sc.protectedIndices)
        sanityXml->createNewChildElement("PROTECTED")->setAttribute("index", idx);
    xml->addChildElement(sanityXml.release());

    // 6) Persist RecallMode
    // State version for forward migration (single source of truth: Version.h).
    xml->setAttribute("stateVersion", juce::String(more_phi::VERSION_STRING));
    xml->setAttribute("recallMode", recallMode_.load(std::memory_order_relaxed));

    // 7) Persist morph cursor position so morph resumes from where it was
    xml->setAttribute("morphX", static_cast<double>(morphX_.load(std::memory_order_relaxed)));
    xml->setAttribute("morphY", static_cast<double>(morphY_.load(std::memory_order_relaxed)));
    xml->setAttribute("faderPos", static_cast<double>(faderPos_.load(std::memory_order_relaxed)));
    xml->setAttribute("morphSource", morphSource_.load(std::memory_order_relaxed));

    // 8) Persist UI expert mode (AUDIT-2026-06-25)
    xml->setAttribute("expertMode", isExpertMode() ? 1 : 0);

    // 9) Persist MCP identity for port reuse across export cycles
    auto mcpXml = std::make_unique<juce::XmlElement>("MCP_IDENTITY");
    mcpXml->setAttribute("port", instanceIdentity_.port);
    mcpXml->setAttribute("instanceId", instanceIdentity_.instanceId);
    mcpXml->setAttribute("morphCode", instanceIdentity_.morphCode);
    xml->addChildElement(mcpXml.release());

    // 9) Persist agent-layer autonomy level (H6). The runtime is rebuilt lazily on
    // restore, so without this a user's autonomy choice (Assist/CoPilot/Autopilot)
    // is lost across a project reload / export cycle. Versioned so future agent
    // state can be forward-migrated.
    {
        juce::String autonomyStr;
        if (auto* runtime = getAutomationRuntimeForAgents())
            autonomyStr = more_phi::toString(runtime->permissions().getAutonomyLevel());
        else
            autonomyStr = more_phi::toString(pendingAgentAutonomy_);

        auto agentXml = std::make_unique<juce::XmlElement>("AGENT_RUNTIME");
        agentXml->setAttribute("version", "1");
        agentXml->setAttribute("autonomy", autonomyStr);
        xml->addChildElement(agentXml.release());
    }

    // 10) Persist the last applied neural mastering plan across save/restore
    // (MED-11). No-op when no plan has been applied. Versioned so restore can
    // reject plans from an incompatible schema.
    autoMasteringEngine_.serializeLastPlan(*xml);

    copyXmlToBinary(*xml, destData);

    // C-2 FIX: Cache the built state on the message thread so that
    // future audio-thread callers (e.g. offline render) can
    // return it immediately without building XML or allocating memory.
    if (!isAudioThread)
    {
        const juce::SpinLock::ScopedLockType lock(cachedSavedStateMutex_);
        cachedSavedState_ = destData;
    }
}

void MorePhiProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (!xml) 
    {
        DBG("setStateInformation: ERROR - Failed to parse state XML");
        return;
    }

    DBG("setStateInformation: Restoring state, XML root tag: " + xml->getTagName());

    // Version check (M1 NOTE): intentionally forward-compatible — we LOG the
    // saved version but perform no migration. All fromXml() reads use defaulted
    // getters (getBoolAttribute(...,default)/getIntAttribute(...,default)), so a
    // missing field from an older state degrades gracefully to its default rather
    // than corrupting. Add a real `if (major < N)` migration branch here only when
    // a schema change actually breaks those defaults. Until then this is a
    // diagnostic breadcrumb, not a migration gate.
    {
        const juce::String stateVersion = xml->getStringAttribute("stateVersion", "0.0.0");
        if (stateVersion != juce::String(more_phi::VERSION_STRING))
        {
            DBG("MorePhiProcessor::setStateInformation — loading state from version " + stateVersion
                + " (current: " + juce::String(more_phi::VERSION_STRING) + "). Some settings may not be compatible.");
        }
    }

    // Block morph engine during restoration to prevent race conditions
    // ORDERING: isRestoring_ is set to true BEFORE any modification of the
    // hostedPlugin pointer so the audio thread never races with plugin teardown.
    isRestoring_.store(true, std::memory_order_release);

    // Reset retry counter for fresh state restoration
    pendingLoadAttempts_.store(0, std::memory_order_relaxed);

    // 1) Restore APVTS parameters
    // H-3 FIX: Some DAWs wrap the state XML in an additional element.
    // Do a recursive search for the APVTS state element instead of only
    // checking the root tag, which silently skips parameter restore otherwise.
    {
        const auto apvtsTag = apvts.state.getType().toString();
        std::function<juce::XmlElement*(juce::XmlElement*)> findApvtsElement =
            [&](juce::XmlElement* el) -> juce::XmlElement*
            {
                if (el == nullptr) return nullptr;
                if (el->hasTagName(apvtsTag)) return el;
                for (auto* child : el->getChildIterator())
                {
                    if (auto* found = findApvtsElement(child))
                        return found;
                }
                return nullptr;
            };

        if (auto* apvtsEl = findApvtsElement(xml.get()))
            apvts.replaceState(juce::ValueTree::fromXml(*apvtsEl));
        else
            DBG("MorePhiProcessor::setStateInformation — skipping APVTS restore: no element matching '"
                + apvtsTag + "' found in state XML (root tag: '" + xml->getTagName() + "')");
    }

    // 2) Restore Snapshot Bank
    if (auto* bankXml = xml->getChildByName("SNAPSHOT_BANK"))
        snapshotBank.fromXml(*bankXml);

    // 3) Restore Modulation Engine state
    if (auto* modXml = xml->getChildByName("MODULATION_ENGINE"))
        modulationEngine_.fromXml(*modXml);

    // 4) Restore SanityConfig
    if (auto* sanityXml = xml->getChildByName("SANITY_CONFIG"))
    {
        SanityConfig sc;
        sc.enabled = sanityXml->getBoolAttribute("enabled", false);
        for (auto* child : sanityXml->getChildIterator())
        {
            if (child->hasTagName("PROTECTED"))
                sc.protectedIndices.insert(child->getIntAttribute("index", -1));
        }
        setSanityConfig(sc);
    }

    // 5) Restore RecallMode
    if (xml->hasAttribute("recallMode"))
        recallMode_.store(xml->getIntAttribute("recallMode", 0), std::memory_order_relaxed);

    // 6) Restore morph cursor position
    if (xml->hasAttribute("morphX"))
        setMorphX(static_cast<float>(xml->getDoubleAttribute("morphX", 0.5)));
    if (xml->hasAttribute("morphY"))
        setMorphY(static_cast<float>(xml->getDoubleAttribute("morphY", 0.5)));
    if (xml->hasAttribute("faderPos"))
        setFaderPos(static_cast<float>(xml->getDoubleAttribute("faderPos", 0.0)));
    if (xml->hasAttribute("morphSource"))
        setMorphSource(xml->getIntAttribute("morphSource", 0));

    // 7) Restore UI expert mode (AUDIT-2026-06-25)
    if (xml->hasAttribute("expertMode"))
    {
        if (auto* p = apvts.getParameter("expertMode"))
        {
            const bool expert = xml->getIntAttribute("expertMode", 0) != 0;
            p->setValueNotifyingHost(expert ? 1.0f : 0.0f);
        }
    }

    // 8) Restore MCP identity (port reuse)
    if (auto* mcpXml = xml->getChildByName("MCP_IDENTITY"))
    {
        pendingIdentity_.port = mcpXml->getIntAttribute("port", 0);
        pendingIdentity_.instanceId = mcpXml->getStringAttribute("instanceId", "");
        pendingIdentity_.morphCode = mcpXml->getStringAttribute("morphCode", "");
        DBG("setStateInformation: Restored MCP identity, port: " + juce::String(pendingIdentity_.port));
    }

    // 8) Buffer the hosted plugin's opaque state for re-application after async load
    if (auto* stateXml = xml->getChildByName("HOSTED_PLUGIN_STATE"))
    {
        juce::String base64 = stateXml->getStringAttribute("data", "");
        if (base64.isNotEmpty())
        {
            const juce::SpinLock::ScopedLockType guard(pendingStateMutex_);
            pendingHostedState_.reset();
            pendingHostedState_.fromBase64Encoding(base64);
            DBG("setStateInformation: Buffered hosted plugin state, size: "
                + juce::String(static_cast<int>(pendingHostedState_.getSize())) + " bytes");
        }
    }

    // 8b) Restore agent-layer autonomy (H6). Stashed until the runtime is rebuilt
    // lazily in startAgentRuntimeIfNeeded, where it is applied to the permission
    // kernel. Defaults to Assist if absent (preserves pre-existing behavior).
    if (auto* agentXml = xml->getChildByName("AGENT_RUNTIME"))
    {
        const auto autonomyStr = agentXml->getStringAttribute("autonomy", "");
        if (autonomyStr.isNotEmpty())
            pendingAgentAutonomy_ = more_phi::autonomyLevelFromString(autonomyStr);
    }

    // 8c) Restore the last applied neural mastering plan (MED-11).
    // Safe on the message thread — the plan is only read by the SonicMaster
    // analysis engine's applyRamped, which also runs on the message thread.
    if (auto* mp = xml->getChildByName("MASTERING_PLAN"))
        autoMasteringEngine_.restoreLastPlan(*mp);

    // 9) Restore the hosted plugin — synchronous when possible, Timer fallback when not.
    //    CRITICAL: Do NOT use MessageManager::callAsync() here. Per JUCE forum findings,
    //    callAsync() may silently drop callbacks when the editor is closed (FL Studio,
    //    Linux hosts), or when "Smart disable" is active. Timers are more reliable.
    //
    //    C-1 FIX: Check isSwapping_ to avoid racing with UI-initiated plugin load.
    if (hostManager.isPluginSwapping())
    {
        DBG("setStateInformation: Plugin swap in progress, deferring hosted plugin restore");
        isRestoring_.store(false, std::memory_order_release);
        return;
    }

    if (auto* descXml = xml->getChildByName("HOSTED_PLUGIN"))
    {
        juce::PluginDescription desc;
        if (desc.loadFromXml(*descXml))
        {
            DBG("setStateInformation: Found hosted plugin description: " + desc.name 
                + " (" + desc.pluginFormatName + ")");
            
            if (juce::MessageManager::getInstance()->isThisTheMessageThread())
            {
                // Already on message thread — load synchronously (no race, no dropped callback)
                DBG("setStateInformation: On message thread, loading synchronously");
                if (!loadHostedPluginFromState(desc))
                {
                    DBG("setStateInformation: Synchronous load failed, will retry via timer");
                    {
                        const juce::SpinLock::ScopedLockType guard(pendingPluginDescLock_);
                        pendingPluginDesc_ = desc;
                    }
                    hasPendingPluginLoad_.store(true, std::memory_order_release);
                    pendingLoadAttempts_.store(1, std::memory_order_relaxed);  // Already tried once
                    startTimer(50);
                }
            }
            else
            {
                // M11 FIX: Not on message thread — use atomic flag instead of
                // callFunctionOnMessageThread to avoid blocking the audio thread.
                DBG("setStateInformation: Not on message thread, deferring to timer");
                {
                    const juce::SpinLock::ScopedLockType guard(pendingPluginDescLock_);
                    pendingPluginDesc_ = desc;
                }
                hasPendingPluginLoad_.store(true, std::memory_order_release);
                pendingLoadAttempts_.store(0, std::memory_order_relaxed);
                pendingStateRestore_.store(true, std::memory_order_release);
            }
        }
        else
        {
            DBG("setStateInformation: ERROR - Failed to load plugin description from XML");
            isRestoring_.store(false, std::memory_order_release);
        }
    }
    else
    {
        DBG("setStateInformation: No HOSTED_PLUGIN element found in state");
        isRestoring_.store(false, std::memory_order_release);
    }

    // C-2 FIX (audit, full): Seed the audio-thread save cache with the state
    // we just loaded. The loaded block is the best available snapshot of
    // "what the plugin should save next," and pre-populating it here means
    // the FIRST audio-thread getStateInformation (e.g. an export that fires
    // immediately after a load, before any message-thread save has run) can
    // return it verbatim instead of either allocating (old fall-through bug)
    // or returning empty. setStateInformation runs on the message thread, so
    // the MemoryBlock copy here is safe. See the matching consumer in
    // getStateInformation's isAudioThread branch.
    {
        const juce::SpinLock::ScopedLockType lock(cachedSavedStateMutex_);
        cachedSavedState_.setSize(static_cast<size_t>(sizeInBytes), false);
        if (sizeInBytes > 0 && data != nullptr)
            std::memcpy(cachedSavedState_.getData(), data, static_cast<size_t>(sizeInBytes));
    }
}

// ── Deferred Plugin Loading (Timer fallback) ────────────────────────────────

void MorePhiProcessor::clearHostedMasteringApplicators() noexcept
{
    autoMasteringEngine_.getChainPlanner().clearOzonePlanApplicator();
    ozonePlanApplicator_.reset();
    ozoneParamMap_.reset();
}

void MorePhiProcessor::refreshHostedMasteringApplicators(const juce::PluginDescription& desc)
{
    clearHostedMasteringApplicators();

    // FIX (2026-06-29): Removed the hard isOzone11 check. The buildFromHostedPlugin
    // function does generic name-based discovery ("EQ Band", "Dynamics", "Imager",
    // etc.) and will match any plugin with similarly named parameters — not just
    // iZotope Ozone 11. We always attempt to build the map, then only register the
    // applicator if at least one parameter was mapped (ready check).
    ozoneParamMap_ = std::make_unique<OzoneParameterMap>(OzoneParameterMap::buildFromHostedPlugin(paramBridge));

    if (!ozoneParamMap_->hasAnyMapping())
    {
        DBG("refreshHostedMasteringApplicators: no mapped parameters found for '"
            + desc.name + "' — OzonePlanApplicator NOT registered. "
            "Run ozone.audit_parameters(apply=true) explicitly if you want to "
            "attempt mapping, or check that the hosted plugin exposes standard "
            "parameter names (EQ Band X, Dynamics, Imager, Maximizer).");
        ozoneParamMap_.reset();
        return;
    }

    ozonePlanApplicator_ = std::make_unique<OzonePlanApplicator>(*this, *ozoneParamMap_);
    autoMasteringEngine_.getChainPlanner().setOzonePlanApplicator(ozonePlanApplicator_.get());
    DBG("refreshHostedMasteringApplicators: OzonePlanApplicator registered for '"
        + desc.name + "' (mapped " + juce::String(ozoneParamMap_->mappedSlotCount())
        + " slots).");
}

bool MorePhiProcessor::ensureOzonePlanApplicator()
{
    if (ozoneMappingReady())
        return true;

    auto* plugin = getHostManager().getPlugin();
    if (plugin == nullptr)
        return false;

    // Construct a temporary PluginDescription for the refresh call
    juce::PluginDescription desc;
    desc.name = plugin->getName();
    refreshHostedMasteringApplicators(desc);
    return ozoneMappingReady();
}

bool MorePhiProcessor::loadHostedPluginFromState(const juce::PluginDescription& desc)
{
    auto& host = getHostManager();
    
    // Ensure plugin formats are ready before attempting to load
    if (!ensurePluginFormatsReady())
    {
        DBG("loadHostedPluginFromState: Plugin formats not ready yet, will retry");
        return false;
    }
    
    const bool loaded = host.loadPlugin(desc);
    if (loaded)
    {
        DBG("loadHostedPluginFromState: Successfully loaded plugin: " + desc.name);

        // AUDIT-FIX-R7: flush the SonicMaster capture ring so the next inference
        // cycle starts with fresh audio from the new plugin, not stale samples
        // from the previous source.
        sonicMasterEngine_.flushCaptureRing();
        
        // Re-apply the hosted plugin's saved opaque state (EQ curves, etc.)
        if (auto* plugin = host.beginExclusivePluginUse(500))
        {
            juce::MemoryBlock stateToApply;
            {
                const juce::SpinLock::ScopedLockType guard(pendingStateMutex_);
                stateToApply = pendingHostedState_;
            }

            if (stateToApply.getSize() > 0)
            {
                try
                {
                    plugin->setStateInformation(
                        stateToApply.getData(),
                        static_cast<int>(stateToApply.getSize()));
                    DBG("loadHostedPluginFromState: Restored plugin state, size: "
                        + juce::String(static_cast<int>(stateToApply.getSize())) + " bytes");

                    const juce::SpinLock::ScopedLockType guard(pendingStateMutex_);
                    pendingHostedState_.reset();
                }
                catch (...)
                {
                    DBG("loadHostedPluginFromState: WARNING - hosted plugin threw during setStateInformation");
                }
            }
            else
            {
                DBG("loadHostedPluginFromState: No pending state to restore");
            }

            host.endExclusivePluginUse();
        }

        // Refresh discrete parameter map for the restored plugin
        // CRITICAL (Finding 5): Must happen BEFORE isRestoring_ is cleared.
        // The release store below ensures audio thread sees updated discreteMap_
        // when it observes isRestoring_==false.
        refreshDiscreteMap();
        refreshHostedMasteringApplicators(desc);

        // M-5 FIX: trim throttle states to the hosted plugin's actual parameter
        // count. This saves ~50-60 KB for typical plugins (64-512 params vs the
        // fixed 4096-entry default). Must happen before audio thread unblocking.
        paramBridge.prepare(paramBridge.getParameterCount());

        if (vst3IpcBridge_ != nullptr)
            vst3IpcBridge_->exportParameterRegistry();

        // Unblock the morph engine on success
        // Release semantics ensure all prior writes (including discreteMap_ update)
        // are visible to audio thread when it loads isRestoring_ with acquire.
        isRestoring_.store(false, std::memory_order_release);
    }
    else
    {
        DBG("MorePhiProcessor::loadHostedPluginFromState — loadPlugin failed for: " + desc.name);
        // Don't unblock the morph engine yet — we're going to retry via timer
    }
    
    return loaded;
}

bool MorePhiProcessor::ensurePluginFormatsReady()
{
    auto& host = getHostManager();
    auto& formatManager = host.getFormatManager();
    
    // Check if we have any formats registered (VST3, AU, etc.)
    if (formatManager.getNumFormats() == 0)
    {
        DBG("ensurePluginFormatsReady: No plugin formats registered yet");
        return false;
    }
    
    return true;
}

void MorePhiProcessor::requestMessageThreadMaintenance() noexcept
{
    bool expected = false;
    if (!maintenanceTimerRequested_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

    juce::WeakReference<MorePhiProcessor> weakThis(this);
    auto startMaintenanceTimer = [weakThis]() mutable
    {
        if (auto* self = weakThis.get())
            self->startTimer(50);
    };

    if (auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
        messageManager != nullptr && messageManager->isThisTheMessageThread())
        startMaintenanceTimer();
    else if (!juce::MessageManager::callAsync(startMaintenanceTimer))
        maintenanceTimerRequested_.store(false, std::memory_order_release);
}

bool MorePhiProcessor::hasPendingMessageThreadWork() const noexcept
{
    return mcpStartPending_.load(std::memory_order_acquire)
        || licenseLoadPending_.load(std::memory_order_acquire)
        || hasPendingPluginLoad_.load(std::memory_order_acquire)
        || audioDomainConfigDirty_.load(std::memory_order_acquire)
        || latencyConfigDirty_.load(std::memory_order_acquire)
        || pendingFullStateRecallGeneration_.load(std::memory_order_acquire) != appliedFullStateRecallGeneration_
        || pendingStateRestore_.load(std::memory_order_acquire)
        || morphPositionNotifyPending_.load(std::memory_order_acquire)
        || sonicMasterEngine_.hasPendingApplication();  // AUDIT-FIX-R5: Timer-deferred neural plan application
}

void MorePhiProcessor::loadCachedLicenseIfNeeded()
{
    if (!licenseLoadPending_.exchange(false, std::memory_order_acq_rel))
        return;

    if (licenseManager_ != nullptr)
        licenseManager_->loadCachedCertificate();
}

void MorePhiProcessor::refreshLicenseIfNeeded()
{
    // Runs on the message thread from timerCallback(). When the cached
    // certificate's nextOnlineCheckAtUnix has passed, kick off ONE background
    // refresh so the license renews without forcing the user to re-enter the
    // key. The activation server re-activates the same machine idempotently, so
    // a periodic refresh never burns a seat.
    if (licenseManager_ == nullptr)
        return;

    const auto& runtime = licenseManager_->getRuntimeState();
    if (!runtime.premiumFeaturesEnabled.load(std::memory_order_relaxed))
        return; // nothing to refresh when unlicensed

    const int64_t nextCheck = runtime.nextCheckUnixSeconds.load(std::memory_order_relaxed);
    if (nextCheck <= 0)
        return; // cert without an online-check deadline — nothing to do

    if (licensing::LicenseManager::nowUnixSeconds() <= nextCheck)
        return; // not due yet

    // exchange returns the PREVIOUS value. If it was already true a refresh is
    // in flight and we bail; only the caller that observed false proceeds.
    if (licenseRefreshInFlight_.exchange(true, std::memory_order_acq_rel))
        return;

    // L-5 FIX: Capture shared_ptr to extend manager lifetime past
    // processor destruction — the detached refresh thread holds a reference.
    auto manager = licenseManager_;  // shared_ptr copy
    const auto activationId = manager->lastActivationId();
    juce::Thread::launch([manager, activationId, this]()
    {
        if (activationId.isNotEmpty())
            (void) manager->refreshActivation(activationId);

        licenseRefreshInFlight_.store(false, std::memory_order_release);
    });
}

void MorePhiProcessor::startMCPServerIfNeeded()
{
    if (!mcpStartPending_.exchange(false, std::memory_order_acq_rel) || mcpServer.isRunning())
        return;

    // AUDIT-2026-06-25: the MCP server / IPC bridge are opt-in for Release
    // builds. The test suite still forces startup via startPendingMCPServerForTesting().
#if !defined(MORE_PHI_ENABLE_MCP_SERVER) || !MORE_PHI_ENABLE_MCP_SERVER
#  if !defined(MORE_PHI_TEST_MODE) || !MORE_PHI_TEST_MODE
    mcpStartPending_.store(false, std::memory_order_release);
    return;
#  endif
#endif

    if (pendingIdentity_.port > 0)
    {
        if (pendingIdentity_.instanceId.isEmpty())
            pendingIdentity_.instanceId = generateSecureRandomHexString(16);
        if (pendingIdentity_.morphCode.isEmpty())
            pendingIdentity_.morphCode = generateSecureShortCode(8);
        if (pendingIdentity_.bearerToken.isEmpty())
            pendingIdentity_.bearerToken = generateSecureRandomHexString(16);
        if (pendingIdentity_.createdAt == 0)
            pendingIdentity_.createdAt = juce::Time::currentTimeMillis();

        instanceIdentity_ = pendingIdentity_;
        pendingIdentity_ = {};
    }
    else
    {
        instanceIdentity_ = InstanceRegistry::getInstance().registerInstance();
    }

    if (instanceIdentity_.port <= 0 || instanceIdentity_.instanceId.isEmpty())
    {
        mcpServer.recordStartupFailure("No free MCP port available");
        return;
    }

    mcpServer.setIdentity(instanceIdentity_);
    mcpServer.startServer(instanceIdentity_.port);

    if (vst3IpcBridge_ == nullptr)
        vst3IpcBridge_ = std::make_unique<VST3IPCBridge>(*this, instanceIdentity_);

    vst3IpcBridge_->start();
    vst3IpcBridge_->exportParameterRegistry();

    // Spin up the multi-agent orchestration layer now that the MCP server's
    // AutomationRuntime (ledger/permissions/events/workflows/memory) exists.
    startAgentRuntimeIfNeeded();
}

void MorePhiProcessor::startAgentRuntimeIfNeeded()
{
    if (agentRuntime_ != nullptr)
        return;

    // H8 FIX: Ensure MCP server is fully up before building the agent runtime.
    // The runtime borrows AutomationRuntime from the MCP server, which must
    // be alive and fully initialized first.
    if (!mcpServer.isRunning())
    {
        DBG("startAgentRuntimeIfNeeded: MCP server not running yet, deferring");
        return;
    }

    namespace ag = more_phi::agents;

    auto& automationRuntime = mcpServer.getAutomationRuntime();

    // P2.5 (AUDIT): wire the ActionLedger to AutoMasteringEngine so neural
    // mastering writes (which bypass MCPToolHandler::handle) are auditable. The
    // MCP path already records via MCPToolHandler; this closes the gap for the
    // SonicMaster → applyValidatedPlan path. Ledger lifetime is bound to the MCP
    // server's AutomationRuntime, which outlives the agent runtime (see the
    // member-destruction-order note below).
    autoMasteringEngine_.setActionLedger(&automationRuntime.ledger());

    // Long-lived holders for the runtime's by-reference dependencies. The runtime
    // borrows these by raw reference, so they must outlive it. Member declaration
    // order in PluginProcessor.h guarantees that: the holders are declared BEFORE
    // agentRuntime_, and C++ destroys members in reverse declaration order, so
    // agentRuntime_ is destroyed FIRST. (H5: real members are agentTools_ ->
    // agentLogger_ -> agentLlm_ -> agentBlackboard_ -> agentRuntime_.)
    auto blackboardHolder = std::make_unique<ag::BlackboardBridge>(automationRuntime.events());
    auto toolHolder = std::make_unique<ag::DefaultToolInvoker>(
        // DispatchFn: route every agent tool call through MCPToolHandler::handle so
        // the existing permission/ledger/rate budgets remain the single chokepoint.
        [this, &automationRuntime](const juce::String& method, const nlohmann::json& params) -> juce::String {
            juce::var v = juce::JSON::parse(juce::String(params.dump()));
            return MCPToolHandler::handle(method, v, *this, instanceIdentity_, automationRuntime);
        },
        // CapabilityFn: resolve per-agent allowed-tools by role id prefix.
        // Returning an empty list fails closed inside DefaultToolInvoker.
        [](const juce::String& agentId) -> std::vector<juce::String> {
            const auto id = agentId.toLowerCase();
            if (id.startsWith("conductor"))    return { "workflow.submit", "workflow.execute", "workflow.cancel", "hosted_plugin.info", "analysis.get_summary" };
            // O1 (2026-06-29): analysis agents may pull a dry-run neural decision
            // (sonicmaster_decision) alongside live measurements for mastering intents.
            if (id.startsWith("analysis"))     return { "analysis.get_summary", "analysis.get_spectrum", "analysis.get_stereo_field", "analysis.capture_window", "analysis.compare_render", "sonicmaster_decision" };
            // O1 (2026-06-29): optimization agents prefer the neural one-shot apply
            // (mastering.neural_apply) for mastering goals, falling back to the
            // heuristic batch. sonicmaster_decision is granted for preview/preview-then-apply
            // flows. These must match OptimizationAgent::allowedTools().
            if (id.startsWith("optimization")) return { "mastering.plan_preview", "mastering.render_batch", "mastering.render_status", "mastering.select_candidate", "hosted_plugin.set_parameter", "mastering.neural_apply", "sonicmaster_decision" };
            // H1 FIX: CreativeAgent::execute calls find_related_parameters and
            // suggest_intermediate_snapshots (CreativeAgent.cpp:22-24). Granting
            // only mastering.plan_preview / plugin_profile.describe_semantics left
            // both invocations failing closed in DefaultToolInvoker, making the
            // entire CreativeAgent role a permanent no-op. Advisory-only agent —
            // it never populates proposedActions, so these reads can't write state.
            if (id.startsWith("creative"))     return { "find_related_parameters", "suggest_intermediate_snapshots", "mastering.plan_preview", "plugin_profile.describe_semantics" };
            // C3: RealtimeControlAgent trims the More-Phi OUTPUT GAIN via
            // more_phi.set_parameter (resolved by parameter id on the message
            // thread). It must NOT be granted hosted_plugin.set_parameter —
            // reactive trim of an arbitrary hosted index was the bug.
            if (id.startsWith("realtime"))     return { "more_phi.set_parameter" };
            if (id.startsWith("quality"))      return { "analysis.get_summary", "analysis.compare_render", "mastering.render_status" };
            if (id.startsWith("memory"))       return { "memory.list", "memory.store", "memory.recall" };
            return {};
        });

    // M2: production observability. StructuredAgentLogger appends one JSONL record
    // per agent log call to a per-run file under the shared More-Phi data dir,
    // falling back to an in-memory ring if the file can't be opened. Previously
    // production wired NullAgentLogger, leaving zero agent audit trail.
    const auto logDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                            .getChildFile("More-Phi")
                            .getChildFile("agents");
    const auto logRunId = instanceIdentity_.instanceId.isNotEmpty()
        ? instanceIdentity_.instanceId
        : juce::String("default");
    auto logHolder = std::make_unique<ag::StructuredAgentLogger>(logDir, logRunId);

    // AUDIT-FIX (close the "AI theater" gap): prefer a REAL LLM (OpenAI /
    // Anthropic / OpenAI-compatible) when the user has CONFIGURED an API key, so
    // ConductorAgent::decomposeGoal is driven by a genuine model instead of the
    // 3-keyword deterministic heuristic. Falls back to the deterministic client
    // when no provider is configured — preserving the always-works, offline-safe
    // default (Risk R1 mitigation).
    //
    // AUDIT (A1, 2026-06-25): the gate is isConfigured() (non-empty key + selected
    // model), NOT a live validation. LLMConnectionValidator::testConnectionAsync is
    // async/UI-panel-only; calling it synchronously here would stall MCP server init
    // on a network round-trip (up to its 15s timeout). A configured-but-invalid key
    // therefore wires RestLlmClient and fails lazily to http_401 on the first
    // complete() (retried then surfaced by RestLlmClient — see
    // TestRestLlmClientHardening.cpp). This is intentional; AGENTS.md documents it.
    std::unique_ptr<ag::ILlmClient> llmHolder = [this]() -> std::unique_ptr<ag::ILlmClient>
    {
        LLMSettings settings = LLMSettings::createDefault();
        juce::String loadErr;
        LLMSettingsStore store;
        if (store.load(settings, loadErr) && settings.activeProvider.has_value())
        {
            const auto id = *settings.activeProvider;
            const auto& ps = settings.getProvider(id);
            if (ag::RestLlmClient::isConfigured(ps))
            {
                DBG("Agent LLM: using REST client for provider '"
                    + toDisplayString(id) + "' / model '" + ps.selectedModel + "'");
                return std::make_unique<ag::RestLlmClient>(
                    id, ps, std::make_shared<JuceLLMHttpClient>());
            }
        }
        DBG("Agent LLM: no configured provider — using deterministic fallback");
        return std::make_unique<ag::DeterministicFallbackLlmClient>();
    }();

    auto runtime = std::make_unique<ag::AgentRuntime>(
        this,
        &instanceIdentity_,
        &automationRuntime,
        *toolHolder,
        *blackboardHolder,
        *logHolder,
        llmHolder.get());

    // Register the full cast: Conductor + 6 specialists.
    runtime->registerAgent(std::make_unique<ag::ConductorAgent>());
    runtime->registerAgent(std::make_unique<ag::AnalysisAgent>());
    runtime->registerAgent(std::make_unique<ag::OptimizationAgent>());
    runtime->registerAgent(std::make_unique<ag::CreativeAgent>());
    runtime->registerAgent(std::make_unique<ag::RealtimeControlAgent>());
    runtime->registerAgent(std::make_unique<ag::QualitySafetyAgent>());
    runtime->registerAgent(std::make_unique<ag::MemoryAgent>());

    runtime->start(2);  // two scheduler workers

    // H6: replay the persisted autonomy level onto the freshly-built permission
    // kernel so a user's Assist/CoPilot/Autopilot choice survives project reload.
    automationRuntime.permissions().setAutonomyLevel(pendingAgentAutonomy_);

    // Commit: order matters — assign the runtime LAST so the holders are already
    // in place on the processor before any worker could try to dispatch.
    agentTools_      = std::move(toolHolder);
    agentLogger_     = std::move(logHolder);
    agentLlm_        = std::move(llmHolder);
    agentBlackboard_ = std::move(blackboardHolder);
    agentRuntime_    = std::move(runtime);
}

AutomationRuntime* MorePhiProcessor::getAutomationRuntimeForAgents() noexcept
{
    if (mcpServer.isRunning())
        return &mcpServer.getAutomationRuntime();
    return nullptr;
}

void MorePhiProcessor::reconfigureAudioDomainProcessing()
{
    audioDomainReconfiguring_.store(true, std::memory_order_release);

    // P1 FIX / H-6 FIX: Bounded spin-wait with hard timeout to prevent message-thread
    // stall when the audio thread is mid-processing with large FFT sizes.
    // If the timeout expires, re-dirty the config flag and retry on the next
    // timer tick instead of blocking indefinitely.
    const auto deadline = juce::Time::getMillisecondCounter() + 50; // H-6: reduced from 100ms
    while (audioDomainUsers_.load(std::memory_order_acquire) > 0)
    {
        if (juce::Time::getMillisecondCounter() > deadline)
        {
            audioDomainReconfiguring_.store(false, std::memory_order_release);
            audioDomainConfigDirty_.store(true, std::memory_order_release);
            return;
        }
        juce::Thread::yield(); // H-6: non-blocking yield instead of sleep
    }

    const int factor = juce::jlimit(1, OversamplingWrapper::kMaxOSFactor,
                                    desiredOversamplingFactor_.load(std::memory_order_relaxed));
    const int fftSize = desiredSpectralFFTSize_.load(std::memory_order_relaxed);
    const int channels = juce::jlimit(1, OversamplingWrapper::kMaxChannels, getTotalNumOutputChannels());
    const int osBlockSize = juce::jmax(1, currentBlockSize * factor);
    const double osSampleRate = currentSampleRate * static_cast<double>(factor);

    oversampling_.setFactor(factorFromInt(factor));
    oversamplingB_.setFactor(factorFromInt(factor));
    oversampling_.prepare(currentBlockSize, channels, currentSampleRate);
    oversamplingB_.prepare(currentBlockSize, channels, currentSampleRate);

    spectralEngine_.setFFTSize(fftSize);
    spectralEngine_.prepare(osSampleRate, osBlockSize);
    granularEngine_.prepare(osSampleRate, osBlockSize);
    formantEngine_.prepare(osSampleRate, osBlockSize);

    bufferB_.setSize(channels, currentBlockSize);
    paramOut_.setSize(channels, osBlockSize);
    spectralOut_.setSize(channels, osBlockSize);
    granularOut_.setSize(channels, osBlockSize);
    // C-6 FIX (audit): keep the dry buffer at host rate (currentBlockSize).
    dryBuffer_.setSize(channels, currentBlockSize);

    activeOversamplingFactor_.store(factor, std::memory_order_release);
    activeSpectralFFTSize_.store(fftSize, std::memory_order_release);
    audioDomainConfigDirty_.store(false, std::memory_order_release);
    audioDomainReconfiguring_.store(false, std::memory_order_release);

    updateReportedLatency();
}

void MorePhiProcessor::updateReportedLatency()
{
    int hostedLatency = 0;
    if (auto* plugin = hostManager.acquirePluginForUse())
    {
        try
        {
            hostedLatency = plugin->getLatencySamples();
        }
        catch (...)
        {
            hostedLatency = 0;
        }
        hostManager.releasePluginFromUse();
    }

    const bool audioDomainActive =
        audioDomainEnabled_.load(std::memory_order_relaxed)
        && hostManagerB_.hasPlugin()
        && (spectralEngine_.isActive() || granularEngine_.isActive());

    latencyManager_.setHostedPluginLatency(hostedLatency);
    latencyManager_.setOversamplingLatency(audioDomainActive ? oversampling_.getLatencyInSamples() : 0);
    latencyManager_.setFFTWindowLatency((audioDomainActive && spectralEngine_.isActive())
                                            ? spectralEngine_.getLatencyInSamples()
                                            : 0);
    // ENHANCERS-1/PDC: report the mastering-chain lookahead (brickwall-limiter
    // + exciter 4x oversampling) so the DAW compensates. Returns 0 while the
    // chain is dormant (shipped plugin only meters), so the live reported latency
    // is unchanged until mastering is engaged.
    latencyManager_.setMasteringChainLatency(autoMasteringEngine_.getMasteringChainLatency());
    // Output-protection brickwall limiter on the main wet path: report its 4 ms
    // lookahead only while the user has outputProtect engaged, so the DAW PDC
    // compensates and bypass crossfades stay aligned. 0 when disabled.
    latencyManager_.setMorphOutputLatency(
        readRawBool(rawParams_.outputProtect, true) ? morphOutputLimiter_.getLookaheadSamples() : 0);
    setLatencySamples(latencyManager_.getTotal());
    latencyConfigDirty_.store(false, std::memory_order_release);
}

// Phase 2.3 (production readiness): real forward-migration. The previous stub
// only LOGGED the saved version and returned true, relying entirely on defaulted
// XML getters — which is safe for ADDITIVE schema changes but silently DROPS
// changed/renamed semantics. This implementation runs a versioned transform
// chain so each prior release is carried forward to the current schema.
//
// Migration policy:
//  - Parse "major.minor.patch" from `version` (missing/unparseable → 0.0.0).
//  - Apply transforms in order for every version newer than the saved one, up
//    to CURRENT_VERSION. Each transform mutates the XML in place.
//  - Versions older than 3.0.0 (the pre-versioned, structurally-different
//    format) are rejected (return false) — they cannot be safely migrated.
//  - Same/newer versions pass through unchanged.
//  - Each transform is defensive: it checks attribute/child existence before
//    touching it, so a partially-saved state still migrates cleanly.
bool MorePhiProcessor::applyStateMigration(juce::XmlElement& stateXml, const juce::String& version)
{
    // ── Parse the saved version into a comparable integer key ──────────────
    int savedMajor = 0, savedMinor = 0, savedPatch = 0;
    auto parseVersion = [](const juce::String& v, int& maj, int& min, int& pat)
    {
        const auto parts = juce::StringArray::fromTokens(v, ".", "");
        maj = parts.size() > 0 ? parts[0].getIntValue() : 0;
        min = parts.size() > 1 ? parts[1].getIntValue() : 0;
        pat = parts.size() > 2 ? parts[2].getIntValue() : 0;
    };
    parseVersion(version, savedMajor, savedMinor, savedPatch);

    const int savedKey = savedMajor * 1000000 + savedMinor * 1000 + savedPatch;

    // Current version — sourced from Version.h (single source of truth). Migrations
    // below bring the state up to this version.
    const juce::String kCurrentStateVersion = juce::String(more_phi::VERSION_STRING);
    int kCurrentMajor = 0, kCurrentMinor = 0, kCurrentPatch = 0;
    parseVersion(kCurrentStateVersion, kCurrentMajor, kCurrentMinor, kCurrentPatch);
    const int currentKey = kCurrentMajor * 1000000 + kCurrentMinor * 1000 + kCurrentPatch;

    // Newer-than-current state (e.g. a project saved with a future build) is
    // loaded best-effort via defaulted getters; we do not reverse-migrate.
    if (savedKey > currentKey)
    {
        DBG("MorePhiProcessor::applyStateMigration — state version " + version
            + " is newer than current " + juce::String(kCurrentMajor) + "."
            + juce::String(kCurrentMinor) + "." + juce::String(kCurrentPatch)
            + "; loading best-effort without reverse migration.");
        return true;
    }

    // Already current — nothing to do.
    if (savedKey == currentKey)
        return true;

    // Pre-3.0.0 state predates versioning and the structural rewrite; reject.
    if (savedMajor < 3)
    {
        DBG("MorePhiProcessor::applyStateMigration — rejecting pre-3.0.0 state (" + version + ")");
        return false;
    }

    // ── Versioned transform chain ──────────────────────────────────────────
    // Each step runs if the saved state is older than that target version.
    // Add new steps here as the schema evolves. Keep them idempotent + defensive.

    // 3.3.0 → 3.4.0: rename the legacy "more-phi" program name attribute to the
    // canonical "MorePhi" (Phase 2.4 name-consistency fix). Old saved sessions
    // that captured getName() as "More-Phi" are normalized so they don't surface
    // the stale hyphenated spelling in preset/automation identity.
    {
        const int target340 = 3 * 1000000 + 4 * 1000 + 0;
        if (savedKey < target340)
        {
            // The program-name is not stored as a dedicated attribute today, but
            // any snapshot whose NAME captured the old spelling is normalized.
            // We walk every SNAPSHOT_BANK entry and rewrite names that equal the
            // legacy literal. This is a no-op when no snapshot used the old name.
            juce::XmlElement* bank = stateXml.getChildByName("SNAPSHOT_BANK");
            if (bank != nullptr)
            {
                for (auto* snap : bank->getChildIterator())
                {
                    if (snap == nullptr) continue;
                    const auto name = snap->getStringAttribute("name", "");
                    if (name == "More-Phi")
                        snap->setAttribute("name", "MorePhi");
                }
            }
            DBG("MorePhiProcessor::applyStateMigration — applied 3.3.0 -> 3.4.0 transforms");
        }
    }

    // Mark the state as migrated to the current version so downstream code sees
    // a consistent schema and so a re-save does not re-enter the chain.
    stateXml.setAttribute("stateVersion", kCurrentStateVersion);

    return true;
}

void MorePhiProcessor::applyPendingFullStateRecall()
{
    const uint32_t generation = pendingFullStateRecallGeneration_.load(std::memory_order_acquire);
    if (generation == appliedFullStateRecallGeneration_)
        return;

    // H8 FIX: Increment retry count on every attempt and give up after max retries
    if (fullStateRecallRetryCount_.fetch_add(1, std::memory_order_relaxed) > MAX_FULL_STATE_RECALL_RETRIES)
    {
        appliedFullStateRecallGeneration_ = generation; // Give up after max retries
        return;
    }

    const int slot = pendingFullStateRecallSlot_.load(std::memory_order_relaxed);
    juce::MemoryBlock chunk;
    if (slot < 0 || slot >= SnapshotBank::NUM_SLOTS || !snapshotBank.copyStateChunk(slot, chunk))
        return;

    auto* plugin = hostManager.beginExclusivePluginUse(200);
    if (plugin == nullptr)
        return;

    // Phase 7: Smart Preset Caching — skip state chunk if the hosted plugin
    // has changed since this slot was captured. Empty cached UID means legacy
    // capture (no UID recorded); apply the chunk unconditionally for backward
    // compat.
    {
        const juce::String cachedUid = snapshotBank.getCachedPluginUID(slot);
        if (cachedUid.isNotEmpty())
        {
            const juce::PluginDescription& desc = plugin->getPluginDescription();
            const juce::String currentUid = desc.name + "|" + desc.manufacturerName;
            if (cachedUid != currentUid)
            {
                hostManager.endExclusivePluginUse();
                appliedFullStateRecallGeneration_ = generation;
                return;
            }
        }
    }

    try
    {
        plugin->setStateInformation(chunk.getData(), static_cast<int>(chunk.getSize()));
        appliedFullStateRecallGeneration_ = generation;
    }
    catch (...)
    {
        // H8 FIX: Do NOT advance the generation on failure. The timer will retry
        // on the next tick (with a bounded retry limit to avoid infinite loops).
    }

    hostManager.endExclusivePluginUse();
}

void MorePhiProcessor::timerCallback()
{
    // M11 FIX: If a state restore was deferred from the audio thread, ensure
    // the timer stays running so the deferred load can proceed.
    if (pendingStateRestore_.load(std::memory_order_acquire))
    {
        pendingStateRestore_.store(false, std::memory_order_release);
        startTimer(50);
    }

    // ── MORPH DIAGNOSTIC PROBE (temporary) ───────────────────────────────────
    // Reports the state of every gate that can silently block the morph-apply
    // loop, ~once per second from the MESSAGE thread (never audio thread, per
    // AGENTS.md logging rules). Wrapped in JUCE_DEBUG so Release emits nothing.
    // Goal: identify which of the 6 short-circuit gates is freezing "knobs
    // don't move at all". Remove after the root cause is fixed.
#if JUCE_DEBUG
    {
        constexpr int kMorphHealthReportIntervalTicks = 20;  // 20 × 50ms = ~1s
        morphHealthTick_++;
        if (morphHealthTick_ >= kMorphHealthReportIntervalTicks)
        {
            morphHealthTick_ = 0;

            const int   paramCount       = paramBridge.getParameterCount();           // gate A: ==0 blocks at line ~2054
            const bool  hasOccupied      = snapshotBank.hasAnyOccupied();              // gate B: false blocks entirely
            std::array<int, SnapshotBank::NUM_SLOTS> occupiedSlotsArr{};
            const int   occupiedCount    = snapshotBank.getOccupiedSlots(occupiedSlotsArr);
            const bool  hasPlugin        = hostManager.hasPlugin();                    // gate C prerequisite
            const bool  exclusiveFlag    = hostManager.isExclusivePluginUseRequested(); // gate C: stuck true → acquire returns null
            const int   morphStable      = morphStableBlocks_;                          // gate D: >threshold + static → process() skipped
            const float mx               = morphX_.load(std::memory_order_relaxed);    // cursor should move when dragging
            const float my               = morphY_.load(std::memory_order_relaxed);
            const float fp               = faderPos_.load(std::memory_order_relaxed);
            const int   source           = morphSource_.load(std::memory_order_relaxed); // 0=XY,1=Fader
            const int   mode             = physicsMode_.load(std::memory_order_relaxed);  // 0=Direct,1=Elastic,2=Drift

            // Gates E/F: count blocked params under the touch lock.
            int touchBlocked = 0, holdBlocked = 0;
            {
                const juce::SpinLock::ScopedTryLockType tl(touchStateLock_);
                if (tl.isLocked())
                {
                    const int n = juce::jmin(touchCooldown_.size(), liveEditHold_.size());
                    for (int i = 0; i < n; ++i)
                    {
                        if (touchCooldown_[static_cast<size_t>(i)] > 0) ++touchBlocked;  // gate E
                        if (liveEditHold_[static_cast<size_t>(i)] != 0)  ++holdBlocked;  // gate F
                    }
                }
            }

            DBG("[MorphHealth] paramCount=" + juce::String(paramCount)
                + "  hasOccupied=" + (hasOccupied ? "Y" : "N")
                + "  occupiedSlots=" + juce::String(occupiedCount)
                + "  hasPlugin=" + (hasPlugin ? "Y" : "N")
                + "  exclusiveFlag=" + (exclusiveFlag ? "STUCK" : "clear")
                + "  morphStableBlocks=" + juce::String(morphStable)
                + "  cursor[XY=(" + juce::String(mx, 3) + "," + juce::String(my, 3)
                + ") fader=" + juce::String(fp, 3) + " src=" + juce::String(source)
                + " mode=" + juce::String(mode) + ")]"
                + "  touchBlocked=" + juce::String(touchBlocked)
                + "  liveEditHoldBlocked=" + juce::String(holdBlocked));
        }
    }
#endif

    { MSG_TRACE(diagnostics_, "loadCachedLicenseIfNeeded"); loadCachedLicenseIfNeeded(); }
    { MSG_TRACE(diagnostics_, "refreshLicenseIfNeeded");   refreshLicenseIfNeeded(); }
    { MSG_TRACE(diagnostics_, "startMCPServerIfNeeded");   startMCPServerIfNeeded(); }

    // C1 FIX: Drain any hosted-plugin instances whose teardown was deferred
    // because audio-thread leases were held when unloadPlugin() timed out.
    // Destroys them now that activePluginUsers_ has returned to zero.
    { MSG_TRACE(diagnostics_, "drainDeferredDoomedPlugins"); hostManager.drainDeferredDoomedPlugins(); hostManagerB_.drainDeferredDoomedPlugins(); }

    if (audioDomainConfigDirty_.load(std::memory_order_acquire))
    { MSG_TRACE(diagnostics_, "reconfigureAudioDomainProcessing"); reconfigureAudioDomainProcessing(); }
    else if (latencyConfigDirty_.load(std::memory_order_acquire))
    { MSG_TRACE(diagnostics_, "updateReportedLatency"); updateReportedLatency(); }

    { MSG_TRACE(diagnostics_, "applyPendingFullStateRecall"); applyPendingFullStateRecall(); }

    // P2 FIX: Flush pending morph-position-to-APVTS notifications on the
    // message thread. Replaces the dropped-prone callAsync path with a
    // Timer-deferred notification that works reliably even when the editor
    // is closed (FL Studio, headless Linux hosts, etc.).
    if (morphPositionNotifyPending_.load(std::memory_order_acquire))
    {
        morphPositionNotifyPending_.store(false, std::memory_order_release);
        if (auto* p = apvts.getParameter("morphX"))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost(morphX_.load(std::memory_order_relaxed));
            p->endChangeGesture();
        }
        if (auto* p = apvts.getParameter("morphY"))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost(morphY_.load(std::memory_order_relaxed));
            p->endChangeGesture();
        }
        if (auto* p = apvts.getParameter("faderPos"))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost(faderPos_.load(std::memory_order_relaxed));
            p->endChangeGesture();
        }
        if (auto* p = apvts.getParameter("morphSource"))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost(static_cast<float>(morphSource_.load(std::memory_order_relaxed)));
            p->endChangeGesture();
        }
    }

    // AUDIT-FIX-R5: Process pending SonicMaster neural mastering plan on the
    // message thread. Replaces the old MessageManager::callAsync hop which can
    // silently drop in headless hosts. The analysis thread stores the plan and
    // sets a flag; the processor's timer applies it here.
    if (sonicMasterEngine_.hasPendingApplication())
        sonicMasterEngine_.processPendingApplication();

    // GENRE PRIOR (Stage 1 + 2, Ozone §3.2): push the genre classifier's current
    // top genre into the SonicMaster decode path. Stage 1 = target-LUFS prior
    // (precedence: explicit on-demand > closed-loop feedback > genre prior >
    // model default). Stage 2 = the genre's tonal-balance curve, blended against
    // the measured spectrum with residualBlend scaled by confidence so an
    // uncertain guess doesn't stamp a curve on the plan. Only assert a prior when
    // the classifier is confident (≥0.5); below that, clear both so the model
    // default stands. Cheap: atomic reads/writes per tick.
    // No-model caveat: GenreClassifier returns index 10 (Streaming) at conf 1.0
    // when no genre ONNX is loaded, so the prior collapses to the Streaming
    // profile until a model is wired — safe, non-destructive.
    {
        auto& classifier = autoMasteringEngine_.getGenreClassifier();
        const int topGenre = classifier.getTopGenre();
        const float conf = classifier.getTopConfidence();
        if (conf >= 0.5f)
        {
            const auto profile = more_phi::getGenreMasteringProfile(topGenre);
            sonicMasterEngine_.setGenreTargetLufs(profile.targetLufs);
            // Stage 2: curve index. Find the curve's position in the fixed table
            // so the engine can look it up; -1 if the genre maps to no curve.
            int curveIdx = -1;
            if (profile.targetCurve != nullptr)
            {
                for (std::size_t i = 0; i < more_phi::kMasteringTargetCurves.size(); ++i)
                    if (&more_phi::kMasteringTargetCurves[i] == profile.targetCurve)
                    { curveIdx = static_cast<int>(i); break; }
            }
            sonicMasterEngine_.setGenreCurveIndex(curveIdx);
            // Confidence-scaled blend: cap at 0.5 so the model's EQ still leads;
            // the prior nudges, not overrides. 0 when no curve.
            sonicMasterEngine_.setResidualBlend(curveIdx >= 0 ? 0.5f * conf : 0.0f);
        }
        else
        {
            sonicMasterEngine_.setGenreTargetLufs(more_phi::kUseModelTargetLufs);
            sonicMasterEngine_.setGenreCurveIndex(-1);
            sonicMasterEngine_.setResidualBlend(0.0f);
        }
    }

    // ── V2 feature→delta controller cycle (throttled ~2s) ────────────────────
    // Every tick: update the spectral-flux EMA for transient density tracking.
    RealtimeSpectrumAnalyzer::SpectrumSnapshot spectrumSnap;
    const bool hasSpectrum = autoMasteringEngine_.getSpectrumAnalyzer().getSnapshot(spectrumSnap);
    if (hasSpectrum && spectrumSnap.frameIndex > 0)
        v2FluxMean_ = (1.0f - kFluxEmaAlpha) * v2FluxMean_ + kFluxEmaAlpha * spectrumSnap.spectralFlux;

    // Every kNeuralMasteringV2CycleTicks: build a feature frame and feed the controller.
    if (neuralMasteringFeatureTick_ % kNeuralMasteringV2CycleTicks == 0)
    {
        const auto& lufs = autoMasteringEngine_.getLUFSMeter();
        StereoFieldAnalyzer::StereoFieldSnapshot   stereoSnap;
        const bool hasStereo   = autoMasteringEngine_.getStereoFieldAnalyzer().getSnapshot(stereoSnap);

        NeuralMasteringAnalysisSnapshot snap {};
        snap.integratedLUFS    = lufs.getIntegrated();
        snap.shortTermLUFS     = lufs.getShortTerm();
        snap.momentaryLUFS     = lufs.getMomentary();
        snap.loudnessRange     = lufs.getLRA();
        snap.truePeakDbTp      = autoMasteringEngine_.getTruePeakEstimator().getTruePeak_dBTP();
        snap.crestFactorDb     = hasSpectrum ? spectrumSnap.crestFactor : 0.0f;
        snap.spectralTilt      = hasSpectrum ? spectrumSnap.spectralTilt : 0.0f;
        snap.monoFoldDownDeltaDb = computeMonoFoldDownDeltaDb(hasStereo ? &stereoSnap : nullptr);
        snap.transientDensity  = computeTransientDensity(hasSpectrum ? &spectrumSnap : nullptr);
        snap.harmonicRisk      = computeHarmonicRisk(hasSpectrum ? &spectrumSnap : nullptr);
        snap.sourceQualityScore = 1.0f;
        snap.spectralBands     = hasSpectrum ? spectrumSnap.magnitudeDB.data() : nullptr;
        snap.stereoCorrelation = hasStereo ? stereoSnap.correlation.data() : nullptr;
        snap.midSideRatio      = hasStereo ? stereoSnap.msEnergyRatio.data() : nullptr;

        NeuralMasteringFeatureExtractor extractor;
        const auto result = extractor.extractFromAnalysis(
            getSampleRate(), getTotalNumInputChannels(),
            getBlockSize(), neuralMasteringFeatureTick_, snap);

        if (result.status == NeuralMasteringFeatureExtractionStatus::Success)
        {
            NeuralMasteringRuntimeState runtimeState;
            runtimeState.currentFrame = static_cast<std::uint64_t>(neuralMasteringFeatureTick_);
            runtimeState.sampleRate = getSampleRate();
            runtimeState.channelCount = getTotalNumInputChannels();
            runtimeState.layout = runtimeState.channelCount == 1
                ? NeuralMasteringLayout::Mono
                : NeuralMasteringLayout::Stereo;
            (void)neuralMasteringController_.processFeatureFrame(result.frame, runtimeState, true);
        }
    }
    ++neuralMasteringFeatureTick_;

    // ── Waypoint morph path (Phase 6) ────────────────────────────────────────
    // When waypoints are enabled, the waypoint engine drives the morph position
    // at BPM-synced timing. Runs on the message thread timer (~33ms resolution).
    if (rawParams_.waypointEnable != nullptr)
    {
        const bool waypointsOn = readRawBool(rawParams_.waypointEnable, false);
        if (waypointsOn)
        {
            // Sync BPM and play state from APVTS each tick
            waypointEngine_.setBPM(readRawFloat(rawParams_.waypointBPM, 120.0f));
            waypointEngine_.setPlaying(readRawBool(rawParams_.waypointPlay, false));

            float wpX, wpY;
            waypointEngine_.process(1.0f / 30.0f, wpX, wpY);

            // Push waypoint output to both atomics and APVTS (DAW sees it)
            setMorphX(wpX);
            setMorphY(wpY);

            if (auto* p = apvts.getParameter("morphX"))
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost(wpX);
                p->endChangeGesture();
            }
            if (auto* p = apvts.getParameter("morphY"))
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost(wpY);
                p->endChangeGesture();
            }
        }
    }

    // Timer is persistent (started in prepareToPlay, never stopped) so the audio
    // thread never needs to allocate a closure or WeakReference to kick it.
    if (!hasPendingPluginLoad_.load(std::memory_order_acquire))
    {
        maintenanceTimerRequested_.store(false, std::memory_order_release);
        return;
    }

    // Attempt to load — retry up to MAX_PLUGIN_LOAD_RETRIES times (500ms window) in case the host
    // is still initialising when the first tick fires (e.g. FL Studio startup scan, export rendering).
    juce::PluginDescription desc;
    {
        const juce::SpinLock::ScopedLockType guard(pendingPluginDescLock_);
        desc = pendingPluginDesc_;
    }
    DBG("timerCallback: Attempt " + juce::String(pendingLoadAttempts_.load(std::memory_order_relaxed) + 1) + " to load plugin: "
        + desc.name);
    
    const bool loaded = loadHostedPluginFromState(desc);
    if (loaded)
    {
        // Success!
        hasPendingPluginLoad_.store(false, std::memory_order_release);
        pendingLoadAttempts_.store(0, std::memory_order_relaxed);
        DBG("timerCallback: Plugin loaded successfully");
        latencyConfigDirty_.store(true, std::memory_order_release);
    }
    else if (pendingLoadAttempts_.fetch_add(1, std::memory_order_relaxed) + 1 >= MAX_PLUGIN_LOAD_RETRIES)
    {
        // Give up after max retries
        hasPendingPluginLoad_.store(false, std::memory_order_release);
#if JUCE_DEBUG
        int totalAttempts = pendingLoadAttempts_.load(std::memory_order_relaxed);
#endif
        pendingLoadAttempts_.store(0, std::memory_order_relaxed);
        
        juce::String pluginName;
        {
            const juce::SpinLock::ScopedLockType guard(pendingPluginDescLock_);
            pluginName = pendingPluginDesc_.name;
        }
#if JUCE_DEBUG
        DBG("MorePhiProcessor::timerCallback — gave up after " + juce::String(totalAttempts) + " attempts: "
            + pluginName);
#endif
        
        // Unblock the morph engine so audio can still pass through
        isRestoring_.store(false, std::memory_order_release);
    }
}

// ── VST3 Program Interface (C-5 FIX) ──────────────────────────────────────────
//
// P3 NOTE: This exposes snapshot slots as DAW "programs" for preset-browser
// integration. Empty slots appear as "Empty N" and are selectable (DAW preset
// browsers typically list all programs). Selecting an empty slot is a no-op.
// Program changes are forwarded to the command queue (recallSnapshotQueued)
// which enqueues parameter writes; this is thread-safe (multi-producer queue)
// but the DAW may call setCurrentProgram from UI or audio thread.
// Future: consider filtering to only occupied slots, or using VST3 unit
// information to expose only populated snapshots.

int MorePhiProcessor::getNumPrograms()
{
    return SnapshotBank::NUM_SLOTS;
}

int MorePhiProcessor::getCurrentProgram()
{
    return currentProgram_.load(std::memory_order_relaxed);
}

void MorePhiProcessor::setCurrentProgram(int index)
{
    if (index >= 0 && index < SnapshotBank::NUM_SLOTS)
    {
        currentProgram_.store(index, std::memory_order_relaxed);
        // C3 FIX: slot 11 is reserved for ABCompareEngine rollback (kReservedSlot,
        // ABCompareEngine.h:30). captureSnapshotToSlot() rejects writes to it
        // (PluginProcessor.cpp:429), but recall was not symmetric — a DAW
        // selecting program 12 (index 11) could recall the rollback checkpoint and
        // clobber live params. Treat it like an empty slot: selectable for DAW
        // program-index consistency, but never recalled via the morph/recall path.
        if (index == 11)
            return;
        if (snapshotBank.isOccupied(index))
            recallSnapshotQueued(index);
    }
}

const juce::String MorePhiProcessor::getProgramName(int index)
{
    if (index < 0 || index >= SnapshotBank::NUM_SLOTS)
        return {};

    if (snapshotBank.isOccupied(index))
        return "Snapshot " + juce::String(index + 1);

    return "Empty " + juce::String(index + 1);
}

// ── Editor ────────────────────────────────────────────────────────────────────────────

juce::AudioProcessorEditor* MorePhiProcessor::createEditor()
{
    try
    {
        auto editor = std::make_unique<MorePhiEditor>(*this);
        return editor.release();
    }
    catch (...)
    {
        return nullptr;
    }
}

// C-1 FIX: Report actual latency from all pipeline stages.
// AUDIT-FIX (TAIL-1): previously this returned ONLY the hosted plugin's tail,
// ignoring the plugin's own audio-domain tails (spectral overlap-add residual,
// granular in-flight grains, mastering-chain lookahead). That made offline-bounce
// tails wrong whenever audio-domain morph or mastering was engaged. Now we take
// the max across the hosted plugin and every active internal engine, matching the
// latency aggregation in updateReportedLatency().
double MorePhiProcessor::getTailLengthSeconds() const
{
    double tail = 0.0;
    // P2 FIX: hostManager is now mutable — no const_cast needed.
    if (auto* plugin = hostManager.acquirePluginForUse())
    {
        try
        {
            tail = plugin->getTailLengthSeconds();
        }
        catch (...)
        {
            // Plugin in bad state, report zero tail
        }
        hostManager.releasePluginFromUse();
    }

    // AUDIT-FIX (TAIL-1): add the plugin's own engine tails when the audio
    // domain is active. isActive() gates each engine so dormant engines add 0.
    if (audioDomainEnabled_.load(std::memory_order_relaxed))
    {
        tail = juce::jmax(tail, spectralEngine_.getTailLengthSeconds());
        tail = juce::jmax(tail, granularEngine_.getTailLengthSeconds());
    }

    // AUDIT-FIX (TAIL-1): mastering-chain lookahead tail. getMasteringChainLatency()
    // returns 0 while the chain is dormant (shipped plugin only meters), so this
    // is a no-op until mastering is engaged — the live reported tail is unchanged.
    if (currentSampleRate > 0.0)
    {
        const double masteringTailSec =
            static_cast<double>(autoMasteringEngine_.getMasteringChainLatency()) / currentSampleRate;
        tail = juce::jmax(tail, masteringTailSec);
    }

    return tail;
}

// H-5 FIX: Call this whenever an engine is toggled to update DAW latency compensation
void MorePhiProcessor::reportLatencyToHost()
{
    updateReportedLatency();
}

    // refreshDiscreteMap — called after plugin load to update discrete parameter classification
    void MorePhiProcessor::refreshDiscreteMap()
    {
        auto map = paramBridge.getDiscreteMap();
        morphProcessor.setDiscreteMap(std::move(map));
        parameterClassifier_.analyzeParameters(paramBridge);
        discreteHandler_.initialize(parameterClassifier_);
    }

} // namespace more_phi

// Plugin entry point
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new more_phi::MorePhiProcessor();
}
