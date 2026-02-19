/*
 * MorphSnap — Advanced Parameter Morphing Engine
 * PluginProcessor.cpp — Main VST3 Audio Processor Implementation
 */
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Core/AllocationTracker.h"
#include "AI/InstanceRegistry.h"

namespace morphsnap {

MorphSnapProcessor::MorphSnapProcessor()
    : AudioProcessor(BusesProperties()
          .withInput ("Input",    juce::AudioChannelSet::stereo(), true)
          .withOutput("Output",   juce::AudioChannelSet::stereo(), true)
          .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts(*this, nullptr, "MORPHSNAP_STATE", createParameterLayout()),
      paramBridge(hostManager),
      morphProcessor(snapshotBank),
      mcpServer(*this)
{
    // Constructor is kept minimal for FL Studio validation.

    // Wire MIDI router callbacks
    midiRouter.setSnapshotCallback([this](int slot)
    {
        if (snapshotBank.isOccupied(slot))
            snapshotBank.recall(slot, paramBridge);
    });

    midiRouter.setMorphCallback([this](float value)
    {
        setFaderPos(value);
        setMorphSource(1);  // Switch to fader mode
    });
}

MorphSnapProcessor::~MorphSnapProcessor()
{
    mcpServer.stopServer();
    InstanceRegistry::getInstance().deregisterInstance(instanceIdentity_.instanceId);
    hostManager.unloadPlugin();
}

bool MorphSnapProcessor::enqueueParameterSet(int paramIndex, float normalizedValue)
{
    const std::lock_guard<std::mutex> guard(commandQueueProducerMutex_);
    return commandQueue.push(ParamCommand{
        paramIndex,
        juce::jlimit(0.0f, 1.0f, normalizedValue)
    });
}

int MorphSnapProcessor::enqueueParameterState(const std::vector<float>& normalizedValues)
{
    const std::lock_guard<std::mutex> guard(commandQueueProducerMutex_);
    const int count = static_cast<int>(normalizedValues.size());
    if (count <= 0)
        return 0;

    if (static_cast<size_t>(count) > commandQueue.freeSpaceApprox())
        return 0;

    int queued = 0;
    for (int i = 0; i < count; ++i)
    {
        if (!commandQueue.push(ParamCommand{
                i,
                juce::jlimit(0.0f, 1.0f, normalizedValues[static_cast<size_t>(i)])
            }))
            return queued;
        ++queued;
    }
    return queued;
}

bool MorphSnapProcessor::recallSnapshotQueued(int slot)
{
    std::vector<float> values;
    if (!snapshotBank.getSlotValuesCopy(slot, values))
        return false;

    return enqueueParameterState(values) == static_cast<int>(values.size());
}

// ── Parameter Layout ─────────────────────────────────────────────────────────

juce::AudioProcessorValueTreeState::ParameterLayout
MorphSnapProcessor::createParameterLayout()
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
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"outputGain", 1}, "Output Gain",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"bypass", 1}, "Bypass", false));

    return { params.begin(), params.end() };
}

// ── Audio Lifecycle ──────────────────────────────────────────────────────────

void MorphSnapProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentBlockSize  = samplesPerBlock;

    hostManager.prepare(sampleRate, samplesPerBlock, getTotalNumOutputChannels());

    // Pre-allocate morph processor buffers
    morphProcessor.prepare(2048);  // Max expected param count
    morphOutput.resize(2048, 0.0f);

    // Pre-allocate snapshot bank scratch buffers (real-time safety)
    snapshotBank.prepare(2048);

    // Auto-start MCP server with unique port from InstanceRegistry
    if (!mcpServer.isRunning())
    {
        instanceIdentity_ = InstanceRegistry::getInstance().registerInstance();
        mcpServer.setIdentity(instanceIdentity_);
        mcpServer.startServer(instanceIdentity_.port);
    }

    prepared = true;
}

void MorphSnapProcessor::releaseResources()
{
    prepared = false;
    hostManager.releaseResources();
}

bool MorphSnapProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& mainOut = layouts.getMainOutputChannelSet();
    const auto& mainIn  = layouts.getMainInputChannelSet();

    if (mainOut != juce::AudioChannelSet::stereo() &&
        mainOut != juce::AudioChannelSet::mono())
        return false;

    return mainIn == mainOut;
}

// ── Process Block ────────────────────────────────────────────────────────────

void MorphSnapProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                       juce::MidiBuffer& midi)
{
    // Allocation tracking guard (zero overhead in release)
    ScopedAudioCallback allocGuard;
    juce::ScopedNoDenormals noDenormals;

    if (!prepared) return;

    // 1) Drain command queue (MCP/LLM → audio thread)
    ParamCommand cmd;
    constexpr int kMaxCommandDrainsPerBlock = 1024;
    int drainedCommands = 0;
    while (drainedCommands < kMaxCommandDrainsPerBlock && commandQueue.pop(cmd))
    {
        paramBridge.setParameterNormalized(cmd.paramIndex, cmd.value);
        ++drainedCommands;
    }

    // 2) Process MIDI: filter trigger notes, pass rest through
    juce::MidiBuffer filteredMidi;
    midiRouter.processMidi(midi, filteredMidi);

    // 3) Sync physics tuning from atomics
    morphProcessor.setElasticPreset(
        static_cast<ElasticPreset>(elasticPreset_.load(std::memory_order_relaxed)));
    morphProcessor.setDriftSpeed(driftSpeed_.load(std::memory_order_relaxed));
    morphProcessor.setDriftDistance(driftDistance_.load(std::memory_order_relaxed));
    morphProcessor.setDriftChaos(driftChaos_.load(std::memory_order_relaxed));
    morphProcessor.setSmoothingRate(smoothingRate_.load(std::memory_order_relaxed));

    // 4) Compute morph target values through physics pipeline
    const int paramCount = paramBridge.getParameterCount();
    if (paramCount > 0 && snapshotBank.hasAnyOccupied())
    {
        // CRITICAL: No resize here - buffer pre-allocated in prepareToPlay
        // Clear only the portion we'll use
        const size_t usedCount = static_cast<size_t>(paramCount);
        if (morphOutput.size() >= usedCount)
            std::fill(morphOutput.begin(), morphOutput.begin() + usedCount, 0.0f);

        const float dt = static_cast<float>(currentBlockSize) /
                         static_cast<float>(currentSampleRate);
        const auto source = static_cast<MorphSource>(
            morphSource_.load(std::memory_order_relaxed));
        const auto mode = static_cast<MorphMode>(
            physicsMode_.load(std::memory_order_relaxed));
        const float mx = morphX_.load(std::memory_order_relaxed);
        const float my = morphY_.load(std::memory_order_relaxed);
        const float fp = faderPos_.load(std::memory_order_relaxed);

        morphProcessor.process(mx, my, fp, source, mode, dt, morphOutput);

        // Apply interpolated values to hosted plugin (bounded by actual param count)
        if (morphOutput.size() >= static_cast<size_t>(paramCount))
            paramBridge.applyParameterState(morphOutput);
    }

    // 5) Forward audio + filtered MIDI to hosted plugin
    hostManager.processBlock(buffer, filteredMidi);

    // 6) RMS for UI visualization
    if (buffer.getNumChannels() > 0)
        setRmsLevel(buffer.getRMSLevel(0, 0, buffer.getNumSamples()));
}

// ── State Persistence ────────────────────────────────────────────────────────

void MorphSnapProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    auto xml = state.createXml();
    if (!xml) return;

    // Embed the hosted plugin description so we can reopen it on restore
    if (auto* desc = hostManager.getLastDescription())
    {
        auto descXml = desc->createXml();
        if (descXml)
        {
            descXml->setTagName("HOSTED_PLUGIN");
            xml->addChildElement(descXml.release());
        }
    }

    copyXmlToBinary(*xml, destData);
}

void MorphSnapProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (!xml) return;

    // Restore APVTS parameters
    if (xml->hasTagName(apvts.state.getType()))
        apvts.replaceState(juce::ValueTree::fromXml(*xml));
    else
        DBG("MorphSnapProcessor::setStateInformation — skipping APVTS restore: root tag '" +
            xml->getTagName() + "' does not match '" + apvts.state.getType().toString() + "'");

    // Restore the hosted plugin — must happen on message thread
    if (auto* descXml = xml->getChildByName("HOSTED_PLUGIN"))
    {
        juce::PluginDescription desc;
        if (desc.loadFromXml(*descXml))
        {
            // Defer to message thread to avoid audio-thread plugin loading
            juce::WeakReference<MorphSnapProcessor> weakThis(this);
            juce::MessageManager::callAsync([weakThis, desc]()
            {
                if (weakThis != nullptr)
                    weakThis->getHostManager().loadPlugin(desc);
            });
        }
    }
}

// ── Editor ────────────────────────────────────────────────────────────────────────────

juce::AudioProcessorEditor* MorphSnapProcessor::createEditor()
{
    return new MorphSnapEditor(*this);
}

} // namespace morphsnap

// Plugin entry point
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new morphsnap::MorphSnapProcessor();
}

