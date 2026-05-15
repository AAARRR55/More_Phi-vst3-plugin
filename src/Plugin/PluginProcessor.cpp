/*
 * More-Phi — Advanced Parameter Morphing Engine
 * PluginProcessor.cpp — Main VST3 Audio Processor Implementation
 */
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Core/AllocationTracker.h"
#include "Core/InterpolationEngine.h"
#include "Core/OversamplingWrapper.h"
#include "AI/InstanceRegistry.h"
#include "AI/OzoneParameterMap.h"
#include "AI/OzonePlanApplicator.h"
#include <algorithm>

namespace more_phi {

namespace {

float readRawFloat(const std::atomic<float>* p, float fallback) noexcept
{
    return p != nullptr ? p->load(std::memory_order_relaxed) : fallback;
}

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
      mcpServer(*this)
{
    // Constructor is kept minimal for FL Studio validation.
    cacheRawParameterPointers();

    // Wire MIDI router callbacks (plain C function pointers + void* context)
    midiRouter.setSnapshotCallback([](int slot, void* ctx)
    {
        auto* self = static_cast<MorePhiProcessor*>(ctx);
        if (self->snapshotBank.isOccupied(slot))
        {
            self->clearLiveEditHoldsAudioThread();

            if (self->getRecallToggle())
                self->requestFullStateRecallFromAudioThread(slot);

            // MIDI callbacks already run on the audio thread, so parameter
            // recall is applied here. Opaque state recall is deferred above.
            self->snapshotBank.recallFast(slot, self->paramBridge);
        }
    }, this);

    midiRouter.setMorphCallback([](float value, void* ctx)
    {
        auto* self = static_cast<MorePhiProcessor*>(ctx);
        self->setFaderPos(value);
        self->setMorphSource(1);  // Switch to fader mode
    }, this);

    // Attach to shared memory for Link Mode
    linkBroadcaster_.attach(0);  // Default link group
}

MorePhiProcessor::~MorePhiProcessor()
{
    stopTimer();
    aiAssistant_.reset();       // Stop AI background thread first
    linkBroadcaster_.detach();
    mcpServer.stopServer();
    InstanceRegistry::getInstance().deregisterInstance(instanceIdentity_.instanceId);
    // Unregister Ozone applicator before tearing down hosted plugin
    autoMasteringEngine_.getChainPlanner().clearOzonePlanApplicator();
    ozonePlanApplicator_.reset();
    ozoneParamMap_.reset();
    hostManager.unloadPlugin();
}

void MorePhiProcessor::cacheRawParameterPointers()
{
    rawParams_.morphX = apvts.getRawParameterValue("morphX");
    rawParams_.morphY = apvts.getRawParameterValue("morphY");
    rawParams_.faderPos = apvts.getRawParameterValue("faderPos");
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
    rawParams_.driftOutputX = apvts.getRawParameterValue("driftOutputX");
    rawParams_.driftOutputY = apvts.getRawParameterValue("driftOutputY");
}

bool MorePhiProcessor::enqueueParameterSet(int paramIndex,
                                           float normalizedValue,
                                           ParameterEditSource source,
                                           bool holdAgainstMorph)
{
    if (paramIndex < 0 || paramIndex >= MAX_PARAMETERS)
        return false;

    return commandQueue.push(ParamCommand{
        paramIndex,
        juce::jlimit(0.0f, 1.0f, normalizedValue),
        false, -1,
        source,
        holdAgainstMorph
    });
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

bool MorePhiProcessor::recallSnapshotQueued(int slot)
{
    std::vector<float> values;
    if (!snapshotBank.getSlotValuesCopy(slot, values))
        return false;

    const int count = static_cast<int>(values.size());

    // Space for values + marker
    if (static_cast<size_t>(count + 1) > commandQueue.freeSpaceApprox())
        return false;

    // Push values
    for (int i = 0; i < count; ++i)
    {
        if (!commandQueue.push(ParamCommand{
            i, juce::jlimit(0.0f, 1.0f, values[static_cast<size_t>(i)]),
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
            {
                juce::MemoryBlock chunk;
                plugin->getStateInformation(chunk);
                if (chunk.getSize() > 0)
                    snapshotBank.captureStateChunk(slot, chunk);
            }
        }
        catch (...)
        {
            hostManager.endExclusivePluginUse();
            return false;
        }

        hostManager.endExclusivePluginUse();
        return snapshotBank.isOccupied(slot);
    }

    if (hostManager.hasPlugin())
        return false;

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
}

void MorePhiProcessor::setMorphPositionExternal(float x, bool hasX,
                                                float y, bool hasY,
                                                float fader, bool hasFader,
                                                int source)
{
    if (hasX)
    {
        const float clamped = juce::jlimit(0.0f, 1.0f, x);
        morphX_.store(clamped, std::memory_order_relaxed);
        if (rawParams_.morphX != nullptr)
            rawParams_.morphX->store(clamped, std::memory_order_relaxed);
    }

    if (hasY)
    {
        const float clamped = juce::jlimit(0.0f, 1.0f, y);
        morphY_.store(clamped, std::memory_order_relaxed);
        if (rawParams_.morphY != nullptr)
            rawParams_.morphY->store(clamped, std::memory_order_relaxed);
    }

    if (hasFader)
    {
        const float clamped = juce::jlimit(0.0f, 1.0f, fader);
        faderPos_.store(clamped, std::memory_order_relaxed);
        if (rawParams_.faderPos != nullptr)
            rawParams_.faderPos->store(clamped, std::memory_order_relaxed);
    }

    morphSource_.store(source, std::memory_order_relaxed);

    juce::WeakReference<MorePhiProcessor> weakThis(this);
    juce::MessageManager::callAsync([weakThis, hasX, hasY, hasFader]() mutable
    {
        auto* self = weakThis.get();
        if (self == nullptr)
            return;

        if (hasX)
            if (auto* p = self->apvts.getParameter("morphX"))
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost(self->morphX_.load(std::memory_order_relaxed));
                p->endChangeGesture();
            }
        if (hasY)
            if (auto* p = self->apvts.getParameter("morphY"))
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost(self->morphY_.load(std::memory_order_relaxed));
                p->endChangeGesture();
            }
        if (hasFader)
            if (auto* p = self->apvts.getParameter("faderPos"))
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost(self->faderPos_.load(std::memory_order_relaxed));
                p->endChangeGesture();
            }
    });
}

void MorePhiProcessor::clearLiveEditHoldsAudioThread() noexcept
{
    std::fill(liveEditHold_.begin(), liveEditHold_.end(), uint8_t{0});
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

    return { params.begin(), params.end() };
}

// ── Audio Lifecycle ──────────────────────────────────────────────────────────

void MorePhiProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // CRITICAL (Finding 8): Set prepared=false at START, not just at end.
    // This prevents race where audio thread continues with stale state during
    // concurrent prepareToPlay() call from misbehaving host.
    prepared.store(false, std::memory_order_seq_cst);

    currentSampleRate = sampleRate;
    currentBlockSize  = samplesPerBlock;

    // M-6 FIX: Dynamic touch cooldown — ~200ms regardless of host config.
    touchCooldownBlocks_ = std::max(1, static_cast<int>(0.2 * sampleRate / samplesPerBlock));

    hostManager.prepare(sampleRate, samplesPerBlock, getTotalNumOutputChannels());

    // Pre-allocate morph processor buffers
    morphProcessor.prepare(MAX_PARAMETERS);  // Max expected param count
    finalOutput_.resize(MAX_PARAMETERS, 0.0f);
    finalOutput_.resize(MAX_PARAMETERS, 0.0f);      // Pre-size for discrete parameter processing
    lastApplied_.resize(MAX_PARAMETERS, -1.0f);     // -1 = never applied
    touchCooldown_.resize(MAX_PARAMETERS, 0);
    touchMorphX_.resize(MAX_PARAMETERS, -1.0f);     // Morph X when touch detected
    touchMorphY_.resize(MAX_PARAMETERS, -1.0f);     // Morph Y when touch detected
    liveEditHold_.assign(MAX_PARAMETERS, uint8_t{0});
    liveEditX_.assign(MAX_PARAMETERS, 0.5f);
    liveEditY_.assign(MAX_PARAMETERS, 0.5f);
    liveEditFader_.assign(MAX_PARAMETERS, 0.0f);

    // Pre-allocate snapshot bank scratch buffers (real-time safety)
    snapshotBank.prepare(MAX_PARAMETERS);

    // Pre-allocate MIDI router buffers (real-time safety)
    midiRouter.prepare(128);  // Expected max MIDI events per block

    mcpStartPending_.store(true, std::memory_order_release);

    // Initialize AI assistant
    if (!aiAssistant_)
        aiAssistant_ = std::make_unique<AIAssistant>(*this);

    // V2: Prepare second host manager
    hostManagerB_.prepare(sampleRate, samplesPerBlock, getTotalNumOutputChannels());

    // V2: Prepare modulation engine
    modulationEngine_.prepare(sampleRate, samplesPerBlock, 2048);

    desiredOversamplingFactor_.store(
        factorFromChoiceIndex(readRawChoice(rawParams_.oversampling, 0)),
        std::memory_order_relaxed);
    desiredSpectralFFTSize_.store(
        fftSizeFromChoiceIndex(readRawChoice(rawParams_.spectralFFTSize, 2)),
        std::memory_order_relaxed);

    reconfigureAudioDomainProcessing();
    updateReportedLatency();

    // V2: Pre-allocate scratch buffers
    bufferB_.setSize(getTotalNumOutputChannels(), samplesPerBlock);
    const int scratchSamples = samplesPerBlock * activeOversamplingFactor_.load(std::memory_order_relaxed);
    paramOut_.setSize(getTotalNumOutputChannels(), scratchSamples);
    spectralOut_.setSize(getTotalNumOutputChannels(), scratchSamples);
    granularOut_.setSize(getTotalNumOutputChannels(), scratchSamples);

    prepared = true;
    requestMessageThreadMaintenance();
}

void MorePhiProcessor::releaseResources()
{
    prepared = false;
    hostManager.releaseResources();
    hostManagerB_.releaseResources();
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
    spectralEngine_.reset();
    granularEngine_.reset();
    formantEngine_.reset();
    oversampling_.reset();
    oversamplingB_.reset();
    modulationEngine_.reset();
}

bool MorePhiProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& mainOut = layouts.getMainOutputChannelSet();
    const auto& mainIn  = layouts.getMainInputChannelSet();

    if (mainOut != juce::AudioChannelSet::stereo() &&
        mainOut != juce::AudioChannelSet::mono())
        return false;

    if (mainIn != mainOut)
        return false;

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
    const int desiredFFT = fftSizeFromChoiceIndex(readRawChoice(rawParams_.spectralFFTSize, 2));
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

    const int desiredFactor = factorFromChoiceIndex(readRawChoice(rawParams_.oversampling, 0));
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

    if (audioDomainEnabled != lastLatencyAudioDomainEnabled_
        || spectralActive != lastLatencySpectralActive_
        || granularActive != lastLatencyGranularActive_)
    {
        lastLatencyAudioDomainEnabled_ = audioDomainEnabled;
        lastLatencySpectralActive_ = spectralActive;
        lastLatencyGranularActive_ = granularActive;
        latencyConfigDirty_.store(true, std::memory_order_release);
    }

    if (audioDomainConfigDirty_.load(std::memory_order_acquire)
        || latencyConfigDirty_.load(std::memory_order_acquire))
        requestMessageThreadMaintenance();
}

// ── Process Block ────────────────────────────────────────────────────────────

void MorePhiProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                       juce::MidiBuffer& midi) noexcept
{
    // Allocation tracking guard (zero overhead in release)
    ScopedAudioCallback allocGuard;
    juce::ScopedNoDenormals noDenormals;

    if (!prepared) return;

    // Keep runtime atomics coherent with APVTS automation/preset state.
    syncStateFromAPVTS();

    // PERF-M4: Cache parameter count once per block to avoid repeated virtual calls
    const int cachedParamCount = juce::jmin(paramBridge.getParameterCount(),
                                            static_cast<int>(finalOutput_.size()),
                                            static_cast<int>(lastApplied_.size()));

    const bool isBypassed = readRawBool(rawParams_.bypass, false);

    // Guard: skip morph processing while state is being restored (async plugin reload)
    if (isRestoring_.load(std::memory_order_acquire))
    {
        // Still forward audio through the host if it's loaded
        if (!isBypassed)
            hostManager.processBlock(buffer, midi);
        return;
    }

    // Profile total processBlock time
    MORE_PHI_PROFILE(profiler_, "processBlock_total");

    // 1) Drain command queue (MCP/LLM → audio thread)
    {
        MORE_PHI_PROFILE(profiler_, "command_queue_drain");
        ParamCommand cmd;
        constexpr int kMaxCommandDrainsPerBlock = 2048; // Increased for snapshot batching
        int drainedCommands = 0;

        // BUG-5 FIX: lastApplied_ and touchCooldown_ are written ONLY by the
        // audio thread (here) and read by the UI thread under touchStateLock_.
        // The previous code conditioned all writes on hasTouchLock, creating a
        // tracking blackout whenever the UI held the lock (e.g. setSanityConfig).
        // During a blackout every queued MCP command was silently untracked, so
        // the morph engine would later mis-classify those parameters as "manually
        // moved" and freeze them.
        //
        // Fix: write tracking arrays unconditionally (audio-thread-only writer,
        // no race).  The lock is still tried so the UI reader gets a consistent
        // snapshot when it does hold it, but tracking correctness no longer
        // depends on winning the tryEnter race.
        const bool hasTouchLock = touchStateLock_.tryEnter();

        while (drainedCommands < kMaxCommandDrainsPerBlock && commandQueue.pop(cmd))
        {
            if (cmd.isSnapshotMarker)
            {
                // Finalize snapshot recall: update cursor and touch state
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

                clearLiveEditHoldsAudioThread();

                // Always update — sole writer is this audio thread
                const int paramCount = juce::jmin(cachedParamCount,
                                                  static_cast<int>(touchCooldown_.size()));
                for (int i = 0; i < paramCount; ++i)
                {
                    lastApplied_[static_cast<size_t>(i)] = paramBridge.getParameterNormalized(i);
                    touchCooldown_[static_cast<size_t>(i)] = 0;
                }
            }
            else
            {
                paramBridge.setParameterNormalized(cmd.paramIndex, cmd.value);

                // Always update — sole writer is this audio thread
                if (cmd.paramIndex >= 0 &&
                    static_cast<size_t>(cmd.paramIndex) < lastApplied_.size())
                {
                    const auto paramIndex = static_cast<size_t>(cmd.paramIndex);
                    lastApplied_[paramIndex] = cmd.value;
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
            ++drainedCommands;
        }

        if (hasTouchLock)
            touchStateLock_.exit();
    }

    // 2) Process MIDI: filter trigger notes, pass rest through
    filteredMidiBuffer_.clear();
    midiRouter.processMidi(midi, filteredMidiBuffer_);
    modulationEngine_.processMIDI(filteredMidiBuffer_);

    // 2b) Process sidechain trigger (audio-driven snapshot recall)
    if (sidechainEnabled_.load(std::memory_order_relaxed))
    {
        // H-6 FIX: Check bus pointer and enabled state before accessing sidechain
        auto* scBus = getBus(true, 1);
        if (scBus != nullptr && scBus->isEnabled())
        {
            auto scBuffer = getBusBuffer(buffer, true, 1);
            if (scBuffer.getNumChannels() > 0)
            {
                // ATS-5 FIX: Use pre-computed linear threshold (computed in syncStateFromAPVTS)
                midiRouter.setSidechainThreshold(sidechainThresholdLinear_.load(std::memory_order_relaxed));
                midiRouter.processSidechain(scBuffer);
            }
        }
    }

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
        // Previously used COMMAND_QUEUE_CAPACITY (8192) which was larger than the
        // finalOutput_ allocation — could index out-of-bounds if plugin had >2048 params.
        const int paramCount = juce::jmin(cachedParamCount,
                                          static_cast<int>(finalOutput_.size()));
        if (paramCount > 0 && snapshotBank.hasAnyOccupied())
        {
            // CRITICAL: No resize here - buffer pre-allocated in prepareToPlay
            // Clear only the portion we'll use
            const size_t usedCount = static_cast<size_t>(paramCount);
            if (finalOutput_.size() >= usedCount)
                std::fill(finalOutput_.begin(), finalOutput_.begin() + usedCount, 0.0f);

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
                    morphX_.store(linkX, std::memory_order_relaxed);
                    morphY_.store(linkY, std::memory_order_relaxed);
                }
                else
                {
                    linkX = mx;
                    linkY = my;
                }
            }

            morphProcessor.process(linkX, linkY, fp, source, mode, dt, finalOutput_);

        // V2: Modulation engine — applies LFO/envelope/velocity modulation
        // to the finalOutput_ vector before parameter apply. Pass-through in MVP.
        modulationEngine_.setMorphPosition(linkX, linkY, fp);
        modulationEngine_.processAudioInput(buffer.getReadPointer(0), buffer.getNumSamples());
        modulationEngine_.processBlock(finalOutput_, dt);

        // Link Mode: if leader, broadcast current morph position
        if (linkEnabled_.load(std::memory_order_relaxed) && linkBroadcaster_.isLeader())
        {
            linkBroadcaster_.setEnabled(true);
            linkBroadcaster_.broadcast(linkX, linkY);
        }

        // Apply interpolated values to hosted plugin
        // When Listen Mode is active, sentinel values (-1.0f) mark discrete params to skip
        {
            MORE_PHI_PROFILE(profiler_, "parameter_application");
            if (finalOutput_.size() >= static_cast<size_t>(paramCount))
            {
                // CRITICAL (Finding 1): Use tryEnter for touch state lock on audio thread.
            // If message thread is writing (rare, during snapshot recall), skip touch
            // detection for this block - acceptable tradeoff for real-time safety.
            const bool hasTouchLock = touchStateLock_.tryEnter();

            for (int i = 0; i < paramCount; ++i)
            {
                const float morphVal = finalOutput_[static_cast<size_t>(i)];

                // Skip sentinel-marked discrete params (Listen Mode)
                if (morphVal < 0.0f) continue;

                if (i < static_cast<int>(liveEditHold_.size()) && liveEditHold_[static_cast<size_t>(i)] != 0)
                {
                    if (shouldReleaseLiveEditHold(i, linkX, linkY, fp))
                    {
                        liveEditHold_[static_cast<size_t>(i)] = 0;
                    }
                    else
                    {
                        if (hasTouchLock)
                            lastApplied_[static_cast<size_t>(i)] = paramBridge.getParameterNormalized(i);
                        continue;
                    }
                }

                // Touch detection: check if user manually moved this parameter
                // Only perform if we acquired the lock (avoid data race with message thread)
                if (hasTouchLock && lastApplied_[static_cast<size_t>(i)] >= 0.0f)
                {
                    const float currentVal = paramBridge.getParameterNormalized(i);
                    const float lastVal = lastApplied_[static_cast<size_t>(i)];
                    const float userDelta = std::abs(currentVal - lastVal);

                    if (userDelta > TOUCH_THRESHOLD)
                    {
                        // User manually changed this param — record morph position and cooldown
                        touchMorphX_[static_cast<size_t>(i)] = linkX;
                        touchMorphY_[static_cast<size_t>(i)] = linkY;
                        lastApplied_[static_cast<size_t>(i)] = currentVal;
                        touchCooldown_[static_cast<size_t>(i)] = touchCooldownBlocks_;
                    }
                }

                // If parameter is in cooldown, skip applying morph output
                if (hasTouchLock && touchCooldown_[static_cast<size_t>(i)] > 0)
                {
                    --touchCooldown_[static_cast<size_t>(i)];

                    // Check if morph position has changed significantly since touch
                    const float morphDelta = std::abs(linkX - touchMorphX_[static_cast<size_t>(i)]) +
                                             std::abs(linkY - touchMorphY_[static_cast<size_t>(i)]);
                    const bool morphMoved = morphDelta > MORPH_POS_THRESHOLD;

                    if (touchCooldown_[static_cast<size_t>(i)] == 0 && morphMoved)
                    {
                        // Cooldown expired AND user moved morph pad — resume morph writes
                        // Reset tracking so next morph value is applied fresh
                        lastApplied_[static_cast<size_t>(i)] = -1.0f;
                    }
                    else
                    {
                        // Still in cooldown OR morph hasn't moved — keep user's values
                        lastApplied_[static_cast<size_t>(i)] = paramBridge.getParameterNormalized(i);
                    }
                    continue;
                }

                // Apply morph output and track what we applied
                paramBridge.setParameterNormalized(i, morphVal);
                if (hasTouchLock)
                    lastApplied_[static_cast<size_t>(i)] = morphVal;
            }

            if (hasTouchLock)
                touchStateLock_.exit();
            }
        }
    }
    }

    // 5) Write drift output for DAW automation recording
    // VST3-M1: Write directly to the raw atomic (no host callback on audio thread)
    if (rawParams_.driftOutputX != nullptr)
        rawParams_.driftOutputX->store((morphProcessor.getProcessedX() + 1.0f) * 0.5f,
                                       std::memory_order_relaxed);
    if (rawParams_.driftOutputY != nullptr)
        rawParams_.driftOutputY->store((morphProcessor.getProcessedY() + 1.0f) * 0.5f,
                                       std::memory_order_relaxed);

    // 6) Forward audio + filtered MIDI to hosted plugin
    if (!isBypassed)
        hostManager.processBlock(buffer, filteredMidiBuffer_);

    // V2: Audio-domain morph path. Reconfiguration is message-thread-only;
    // the audio thread skips this layer while a reprepare is pending.
    if (!isBypassed
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
                        if (spectralActive)
                            formantEngine_.processBlock(spectralOut_);
                        if (granularActive)
                            formantEngine_.processBlock(granularOut_);
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
                        if (spectralActive)
                            formantEngine_.processBlock(spectralOut_);
                        if (granularActive)
                            formantEngine_.processBlock(granularOut_);
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

    // Apply post-processing output gain when not bypassed.
    if (!isBypassed)
    {
        if (rawParams_.outputGain != nullptr)
        {
            const float gainDb = rawParams_.outputGain->load(std::memory_order_relaxed);
            const float gainLinear = juce::Decibels::decibelsToGain(gainDb);
            if (gainLinear != 1.0f)
                buffer.applyGain(gainLinear);
        }
    }

    // 8) RMS for UI visualization — M-2 FIX: throttle to every N blocks
    if (buffer.getNumChannels() > 0)
    {
        ++rmsSkipCounter_;
        if (rmsSkipCounter_ >= RMS_THROTTLE_BLOCKS)
        {
            rmsSkipCounter_ = 0;
            setRmsLevel(buffer.getRMSLevel(0, 0, buffer.getNumSamples()));
        }
    }
}

// ── State Persistence ────────────────────────────────────────────────────────

void MorePhiProcessor::getStateInformation(juce::MemoryBlock& destData)
{
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
            DBG("getStateInformation: Saved hosted plugin description: " + desc->name);
        }
        else
        {
            DBG("getStateInformation: WARNING - Failed to create XML from plugin description");
        }
    }
    else
    {
        DBG("getStateInformation: No hosted plugin description available to save");
    }

    // 2) Save the hosted plugin's own opaque state (EQ curves, compressor
    //    settings, etc.) — this is the data that was being lost on export
    if (auto* plugin = hostManager.beginExclusivePluginUse(500))
    {
        juce::MemoryBlock pluginState;
        try
        {
            plugin->getStateInformation(pluginState);
        }
        catch (...)
        {
            DBG("getStateInformation: WARNING - hosted plugin threw during getStateInformation");
            pluginState.reset();
        }

        hostManager.endExclusivePluginUse();

        if (pluginState.getSize() > 0)
        {
            auto stateXml = std::make_unique<juce::XmlElement>("HOSTED_PLUGIN_STATE");
            stateXml->setAttribute("data", pluginState.toBase64Encoding());
            xml->addChildElement(stateXml.release());
            DBG("getStateInformation: Saved hosted plugin state, size: " 
                + juce::String(static_cast<int>(pluginState.getSize())) + " bytes");
        }
    }
    else
    {
        // Even if plugin is not currently loaded, try to preserve any pending state
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
            DBG("getStateInformation: Saved pending hosted plugin state, size: "
                + juce::String(static_cast<int>(pendingStateCopy.getSize())) + " bytes");
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
    // State version for forward migration
    xml->setAttribute("stateVersion", "3.3.0");
    xml->setAttribute("recallMode", recallMode_.load(std::memory_order_relaxed));

    // 7) Persist morph cursor position so morph resumes from where it was
    xml->setAttribute("morphX", static_cast<double>(morphX_.load(std::memory_order_relaxed)));
    xml->setAttribute("morphY", static_cast<double>(morphY_.load(std::memory_order_relaxed)));
    xml->setAttribute("faderPos", static_cast<double>(faderPos_.load(std::memory_order_relaxed)));
    xml->setAttribute("morphSource", morphSource_.load(std::memory_order_relaxed));

    // 8) Persist MCP identity for port reuse across export cycles
    auto mcpXml = std::make_unique<juce::XmlElement>("MCP_IDENTITY");
    mcpXml->setAttribute("port", instanceIdentity_.port);
    mcpXml->setAttribute("instanceId", instanceIdentity_.instanceId);
    mcpXml->setAttribute("morphCode", instanceIdentity_.morphCode);
    xml->addChildElement(mcpXml.release());

    copyXmlToBinary(*xml, destData);
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

    // Version check for forward migration
    const juce::String stateVersion = xml->getStringAttribute("stateVersion", "0.0.0");
    juce::ignoreUnused(stateVersion);  // Future: use for migration logic

    // Block morph engine during restoration to prevent race conditions
    // ORDERING: isRestoring_ is set to true BEFORE any modification of the
    // hostedPlugin pointer so the audio thread never races with plugin teardown.
    isRestoring_.store(true, std::memory_order_release);

    // Reset retry counter for fresh state restoration
    pendingLoadAttempts_.store(0, std::memory_order_relaxed);

    // 1) Restore APVTS parameters
    if (xml->hasTagName(apvts.state.getType()))
        apvts.replaceState(juce::ValueTree::fromXml(*xml));
    else
        DBG("MorePhiProcessor::setStateInformation — skipping APVTS restore: root tag '" +
            xml->getTagName() + "' does not match '" + apvts.state.getType().toString() + "'");

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
        morphX_.store(static_cast<float>(xml->getDoubleAttribute("morphX", 0.5)), std::memory_order_relaxed);
    if (xml->hasAttribute("morphY"))
        morphY_.store(static_cast<float>(xml->getDoubleAttribute("morphY", 0.5)), std::memory_order_relaxed);
    if (xml->hasAttribute("faderPos"))
        faderPos_.store(static_cast<float>(xml->getDoubleAttribute("faderPos", 0.0)), std::memory_order_relaxed);
    if (xml->hasAttribute("morphSource"))
        morphSource_.store(xml->getIntAttribute("morphSource", 0), std::memory_order_relaxed);

    // 7) Restore MCP identity (port reuse)
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
                    pendingPluginDesc_ = desc;
                    hasPendingPluginLoad_.store(true, std::memory_order_release);
                    pendingLoadAttempts_.store(1, std::memory_order_relaxed);  // Already tried once
                    startTimer(50);
                }
            }
            else
            {
                // Not on message thread — store description and use Timer to poll.
                // CRITICAL: startTimer() must be called from the message thread.
                // callFunctionOnMessageThread synchronously hops to the message thread;
                // unlike callAsync it cannot silently drop the callback.
                DBG("setStateInformation: Not on message thread, deferring to timer");
                pendingPluginDesc_ = desc;
                hasPendingPluginLoad_.store(true, std::memory_order_release);
                pendingLoadAttempts_.store(0, std::memory_order_relaxed);
                // BUG-1 FIX: callAsync can silently drop callbacks on FL Studio /
                // Linux hosts when the editor is closed (see comment above at line 957).
                // callFunctionOnMessageThread blocks the calling thread briefly but
                // guarantees execution — no silent drops.
                juce::MessageManager::getInstance()->callFunctionOnMessageThread(
                    [](void* ctx) -> void* {
                        static_cast<MorePhiProcessor*>(ctx)->startTimer(50);
                        return nullptr;
                    }, this);
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
}

// ── Deferred Plugin Loading (Timer fallback) ────────────────────────────────

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

    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        startMaintenanceTimer();
    else if (!juce::MessageManager::callAsync(startMaintenanceTimer))
        maintenanceTimerRequested_.store(false, std::memory_order_release);
}

bool MorePhiProcessor::hasPendingMessageThreadWork() const noexcept
{
    return mcpStartPending_.load(std::memory_order_acquire)
        || hasPendingPluginLoad_.load(std::memory_order_acquire)
        || audioDomainConfigDirty_.load(std::memory_order_acquire)
        || latencyConfigDirty_.load(std::memory_order_acquire)
        || pendingFullStateRecallGeneration_.load(std::memory_order_acquire) != appliedFullStateRecallGeneration_;
}

void MorePhiProcessor::startMCPServerIfNeeded()
{
    if (!mcpStartPending_.exchange(false, std::memory_order_acq_rel) || mcpServer.isRunning())
        return;

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
}

void MorePhiProcessor::reconfigureAudioDomainProcessing()
{
    audioDomainReconfiguring_.store(true, std::memory_order_release);

    while (audioDomainUsers_.load(std::memory_order_acquire) > 0)
        juce::Thread::yield();

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
    setLatencySamples(latencyManager_.getTotal());
    latencyConfigDirty_.store(false, std::memory_order_release);
}

void MorePhiProcessor::applyPendingFullStateRecall()
{
    const uint32_t generation = pendingFullStateRecallGeneration_.load(std::memory_order_acquire);
    if (generation == appliedFullStateRecallGeneration_)
        return;

    const int slot = pendingFullStateRecallSlot_.load(std::memory_order_relaxed);
    juce::MemoryBlock chunk;
    if (slot < 0 || slot >= SnapshotBank::NUM_SLOTS || !snapshotBank.copyStateChunk(slot, chunk))
    {
        appliedFullStateRecallGeneration_ = generation;
        return;
    }

    auto* plugin = hostManager.beginExclusivePluginUse(200);
    if (plugin == nullptr)
        return;

    try
    {
        plugin->setStateInformation(chunk.getData(), static_cast<int>(chunk.getSize()));
        appliedFullStateRecallGeneration_ = generation;
    }
    catch (...)
    {
        appliedFullStateRecallGeneration_ = generation;
    }

    hostManager.endExclusivePluginUse();
}

void MorePhiProcessor::timerCallback()
{
    startMCPServerIfNeeded();

    if (audioDomainConfigDirty_.load(std::memory_order_acquire))
        reconfigureAudioDomainProcessing();
    else if (latencyConfigDirty_.load(std::memory_order_acquire))
        updateReportedLatency();

    applyPendingFullStateRecall();

    // Timer fires on the message thread — exactly where we need to load plugins.
    if (!hasPendingPluginLoad_.load(std::memory_order_acquire))
    {
        if (!hasPendingMessageThreadWork())
        {
            maintenanceTimerRequested_.store(false, std::memory_order_release);
            stopTimer();
        }
        return;
    }

    // Attempt to load — retry up to MAX_PLUGIN_LOAD_RETRIES times (500ms window) in case the host
    // is still initialising when the first tick fires (e.g. FL Studio startup scan, export rendering).
    DBG("timerCallback: Attempt " + juce::String(pendingLoadAttempts_.load(std::memory_order_relaxed) + 1) + " to load plugin: "
        + pendingPluginDesc_.name);
    
    const bool loaded = loadHostedPluginFromState(pendingPluginDesc_);
    if (loaded)
    {
        // Success!
        hasPendingPluginLoad_.store(false, std::memory_order_release);
        pendingLoadAttempts_.store(0, std::memory_order_relaxed);
        DBG("timerCallback: Plugin loaded successfully");
        latencyConfigDirty_.store(true, std::memory_order_release);

        // Wire up Ozone 11 mastering integration if applicable
        if (OzoneParameterMap::isOzone11(pendingPluginDesc_.name))
        {
            ozoneParamMap_         = std::make_unique<OzoneParameterMap>(OzoneParameterMap::buildForOzone11());
            ozonePlanApplicator_   = std::make_unique<OzonePlanApplicator>(*this, *ozoneParamMap_);
            autoMasteringEngine_.getChainPlanner().setOzonePlanApplicator(ozonePlanApplicator_.get());
            DBG("timerCallback: Ozone 11 detected — OzonePlanApplicator registered");
        }
    }
    else if (pendingLoadAttempts_.fetch_add(1, std::memory_order_relaxed) + 1 >= MAX_PLUGIN_LOAD_RETRIES)
    {
        // Give up after max retries
        hasPendingPluginLoad_.store(false, std::memory_order_release);
        int totalAttempts = pendingLoadAttempts_.load(std::memory_order_relaxed);
        pendingLoadAttempts_.store(0, std::memory_order_relaxed);
        
        DBG("MorePhiProcessor::timerCallback — gave up after " + juce::String(totalAttempts) + " attempts: "
            + pendingPluginDesc_.name);
        
        // Unblock the morph engine so audio can still pass through
        isRestoring_.store(false, std::memory_order_release);
        
        juce::ignoreUnused(totalAttempts);  // Used in debug builds
    }

    if (!hasPendingMessageThreadWork())
    {
        maintenanceTimerRequested_.store(false, std::memory_order_release);
        stopTimer();
    }
}

// ── VST3 Program Interface (C-5 FIX) ──────────────────────────────────────────

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
        if (snapshotBank.isOccupied(index))
            recallSnapshotQueued(index);
    }
}

const juce::String MorePhiProcessor::getProgramName(int index)
{
    if (index < 0 || index >= SnapshotBank::NUM_SLOTS)
        return {};

    // Read slot name from snapshot bank (best-effort, no lock)
    std::array<int, SnapshotBank::NUM_SLOTS> occupied;
    int numOccupied = snapshotBank.getOccupiedSlots(occupied);
    juce::ignoreUnused(numOccupied);

    if (snapshotBank.isOccupied(index))
        return "Snapshot " + juce::String(index + 1);

    return "Empty " + juce::String(index + 1);
}

// ── Editor ────────────────────────────────────────────────────────────────────────────

juce::AudioProcessorEditor* MorePhiProcessor::createEditor()
{
    try
    {
        return new MorePhiEditor(*this);
    }
    catch (...)
    {
        return nullptr;
    }
}

// C-1 FIX: Report actual latency from all pipeline stages
double MorePhiProcessor::getTailLengthSeconds() const
{
    double tail = 0.0;
    auto& hostMgr = const_cast<PluginHostManager&>(hostManager);
    if (auto* plugin = hostMgr.acquirePluginForUse())
    {
        try
        {
            tail = plugin->getTailLengthSeconds();
        }
        catch (...)
        {
            // Plugin in bad state, report zero tail
        }
        hostMgr.releasePluginFromUse();
    }
    return tail;
}

// H-5 FIX: Call this whenever an engine is toggled to update DAW latency compensation
void MorePhiProcessor::reportLatencyToHost()
{
    updateReportedLatency();
}

} // namespace more_phi

// refreshDiscreteMap — called after plugin load to update discrete parameter classification
void more_phi::MorePhiProcessor::refreshDiscreteMap()
{
    auto map = paramBridge.getDiscreteMap();
    morphProcessor.setDiscreteMap(std::move(map));
}

// Plugin entry point
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new more_phi::MorePhiProcessor();
}
