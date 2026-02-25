/*
 * MorphSnap — Advanced Parameter Morphing Engine
 * PluginProcessor.cpp — Main VST3 Audio Processor Implementation
 */
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Core/AllocationTracker.h"
#include "Core/InterpolationEngine.h"
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
        {
            // Recall Toggle: when off, only apply float params (no state chunks)
            // This lets notes sustain across snapshot switches
            if (getRecallToggle())
                snapshotBank.recall(slot, paramBridge);      // Full recall
            else
                snapshotBank.recallFast(slot, paramBridge);  // Params-only
        }
    });

    midiRouter.setMorphCallback([this](float value)
    {
        setFaderPos(value);
        setMorphSource(1);  // Switch to fader mode
    });

    // Attach to shared memory for Link Mode
    linkBroadcaster_.attach(0);  // Default link group
}

MorphSnapProcessor::~MorphSnapProcessor()
{
    linkBroadcaster_.detach();
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

    // Apply parameter values directly to the hosted plugin
    // (bypasses command queue → avoids morph engine overwrite)
    paramBridge.applyParameterState(values);

    // Sync touch detection state: mark all applied values as "last applied"
    // so the morph engine doesn't falsely detect them as manual user touches
    // and enter cooldown (which would block XY pad morphing).
    const size_t count = std::min(values.size(), lastApplied_.size());
    for (size_t i = 0; i < count; ++i)
    {
        lastApplied_[i] = values[i];
        touchCooldown_[i] = 0;
    }

    // Move the morph cursor to this slot's clock position so the
    // morph engine's continuous output matches what we just applied.
    auto positions = InterpolationEngine::getClockPositions();
    if (slot >= 0 && slot < static_cast<int>(positions.size()))
    {
        float nx = (positions[slot].x + 1.0f) * 0.5f;
        float ny = (positions[slot].y + 1.0f) * 0.5f;
        morphX_.store(nx, std::memory_order_relaxed);
        morphY_.store(ny, std::memory_order_relaxed);
    }

    return true;
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
    lastApplied_.resize(2048, -1.0f);     // -1 = never applied
    touchCooldown_.resize(2048, 0);

    // Pre-allocate snapshot bank scratch buffers (real-time safety)
    snapshotBank.prepare(2048);

    // Auto-start MCP server — reuse saved identity if available, else register new
    if (!mcpServer.isRunning())
    {
        if (pendingIdentity_.port > 0 && !pendingIdentity_.bearerToken.isEmpty())
        {
            // Reuse the port/token from a previous session (preserved across export)
            instanceIdentity_ = pendingIdentity_;
            pendingIdentity_ = {};  // Clear to avoid stale reuse
        }
        else
        {
            instanceIdentity_ = InstanceRegistry::getInstance().registerInstance();
        }
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

// ── Process Block ────────────────────────────────────────────────────────────

void MorphSnapProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                       juce::MidiBuffer& midi)
{
    // Allocation tracking guard (zero overhead in release)
    ScopedAudioCallback allocGuard;
    juce::ScopedNoDenormals noDenormals;

    if (!prepared) return;

    // Guard: skip morph processing while state is being restored (async plugin reload)
    if (isRestoring_.load(std::memory_order_acquire))
    {
        // Still forward audio through the host if it's loaded
        hostManager.processBlock(buffer, midi);
        return;
    }

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

    // 2b) Process sidechain trigger (audio-driven snapshot recall)
    if (sidechainEnabled_.load(std::memory_order_relaxed))
    {
        auto scBus = getBusBuffer(buffer, true, 1);  // sidechain input bus
        if (scBus.getNumChannels() > 0)
        {
            midiRouter.setSidechainThreshold(
                sidechainThreshold_.load(std::memory_order_relaxed));
            midiRouter.processSidechain(scBus);
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

        morphProcessor.process(linkX, linkY, fp, source, mode, dt, morphOutput);

        // Link Mode: if leader, broadcast current morph position
        if (linkEnabled_.load(std::memory_order_relaxed) && linkBroadcaster_.isLeader())
        {
            linkBroadcaster_.setEnabled(true);
            linkBroadcaster_.broadcast(linkX, linkY);
        }

        // Apply interpolated values to hosted plugin
        // When Listen Mode is active, sentinel values (-1.0f) mark discrete params to skip
        if (morphOutput.size() >= static_cast<size_t>(paramCount))
        {
            for (int i = 0; i < paramCount; ++i)
            {
                const float morphVal = morphOutput[static_cast<size_t>(i)];

                // Skip sentinel-marked discrete params (Listen Mode)
                if (morphVal < 0.0f) continue;

                // Touch detection: check if user manually moved this parameter
                if (lastApplied_[static_cast<size_t>(i)] >= 0.0f)
                {
                    const float currentVal = paramBridge.getParameterNormalized(i);
                    const float lastVal = lastApplied_[static_cast<size_t>(i)];
                    const float userDelta = std::abs(currentVal - lastVal);

                    if (userDelta > TOUCH_THRESHOLD)
                    {
                        // User manually changed this param — start cooldown
                        touchCooldown_[static_cast<size_t>(i)] = TOUCH_COOLDOWN_BLOCKS;
                    }
                }

                // If parameter is in cooldown, skip applying morph output
                if (touchCooldown_[static_cast<size_t>(i)] > 0)
                {
                    --touchCooldown_[static_cast<size_t>(i)];
                    lastApplied_[static_cast<size_t>(i)] = -1.0f; // Reset tracking
                    continue;
                }

                // Apply morph output and track what we applied
                paramBridge.setParameterNormalized(i, morphVal);
                lastApplied_[static_cast<size_t>(i)] = morphVal;
            }
        }
    }

    // 5) Write drift output for DAW automation recording
    if (auto* px = apvts.getRawParameterValue("driftOutputX"))
        *px = (morphProcessor.getProcessedX() + 1.0f) * 0.5f;
    if (auto* py = apvts.getRawParameterValue("driftOutputY"))
        *py = (morphProcessor.getProcessedY() + 1.0f) * 0.5f;

    // 6) Forward audio + filtered MIDI to hosted plugin
    hostManager.processBlock(buffer, filteredMidi);

    // 7) RMS for UI visualization
    if (buffer.getNumChannels() > 0)
        setRmsLevel(buffer.getRMSLevel(0, 0, buffer.getNumSamples()));
}

// ── State Persistence ────────────────────────────────────────────────────────

void MorphSnapProcessor::getStateInformation(juce::MemoryBlock& destData)
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
    if (auto* plugin = hostManager.getPlugin())
    {
        juce::MemoryBlock pluginState;
        plugin->getStateInformation(pluginState);
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
        std::lock_guard<std::mutex> guard(pendingStateMutex_);
        if (pendingHostedState_.getSize() > 0)
        {
            auto stateXml = std::make_unique<juce::XmlElement>("HOSTED_PLUGIN_STATE");
            stateXml->setAttribute("data", pendingHostedState_.toBase64Encoding());
            xml->addChildElement(stateXml.release());
            DBG("getStateInformation: Saved pending hosted plugin state, size: "
                + juce::String(static_cast<int>(pendingHostedState_.getSize())) + " bytes");
        }
    }

    // 3) Persist Snapshot Bank (all 12 slots of morph data)
    if (auto bankXml = snapshotBank.toXml())
        xml->addChildElement(bankXml.release());

    // 4) Persist SanityConfig
    auto sanityXml = std::make_unique<juce::XmlElement>("SANITY_CONFIG");
    sanityXml->setAttribute("enabled", sanityConfig_.enabled);
    for (int idx : sanityConfig_.protectedIndices)
        sanityXml->createNewChildElement("PROTECTED")->setAttribute("index", idx);
    xml->addChildElement(sanityXml.release());

    // 5) Persist RecallMode
    xml->setAttribute("recallMode", recallMode_.load(std::memory_order_relaxed));

    // 6) Persist morph cursor position so morph resumes from where it was
    xml->setAttribute("morphX", static_cast<double>(morphX_.load(std::memory_order_relaxed)));
    xml->setAttribute("morphY", static_cast<double>(morphY_.load(std::memory_order_relaxed)));
    xml->setAttribute("faderPos", static_cast<double>(faderPos_.load(std::memory_order_relaxed)));
    xml->setAttribute("morphSource", morphSource_.load(std::memory_order_relaxed));

    // 7) Persist MCP identity for port reuse across export cycles
    auto mcpXml = std::make_unique<juce::XmlElement>("MCP_IDENTITY");
    mcpXml->setAttribute("port", instanceIdentity_.port);
    mcpXml->setAttribute("token", instanceIdentity_.bearerToken);
    mcpXml->setAttribute("instanceId", instanceIdentity_.instanceId);
    mcpXml->setAttribute("morphCode", instanceIdentity_.morphCode);
    xml->addChildElement(mcpXml.release());

    copyXmlToBinary(*xml, destData);
}

void MorphSnapProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (!xml) 
    {
        DBG("setStateInformation: ERROR - Failed to parse state XML");
        return;
    }

    DBG("setStateInformation: Restoring state, XML root tag: " + xml->getTagName());

    // Block morph engine during restoration to prevent race conditions
    isRestoring_.store(true, std::memory_order_release);
    
    // Reset retry counter for fresh state restoration
    pendingLoadAttempts_ = 0;

    // 1) Restore APVTS parameters
    if (xml->hasTagName(apvts.state.getType()))
        apvts.replaceState(juce::ValueTree::fromXml(*xml));
    else
        DBG("MorphSnapProcessor::setStateInformation — skipping APVTS restore: root tag '" +
            xml->getTagName() + "' does not match '" + apvts.state.getType().toString() + "'");

    // 2) Restore Snapshot Bank
    if (auto* bankXml = xml->getChildByName("SNAPSHOT_BANK"))
        snapshotBank.fromXml(*bankXml);

    // 3) Restore SanityConfig
    if (auto* sanityXml = xml->getChildByName("SANITY_CONFIG"))
    {
        sanityConfig_.enabled = sanityXml->getBoolAttribute("enabled", false);
        sanityConfig_.protectedIndices.clear();
        for (auto* child : sanityXml->getChildIterator())
        {
            if (child->hasTagName("PROTECTED"))
                sanityConfig_.protectedIndices.insert(child->getIntAttribute("index", -1));
        }
    }

    // 4) Restore RecallMode
    if (xml->hasAttribute("recallMode"))
        recallMode_.store(xml->getIntAttribute("recallMode", 0), std::memory_order_relaxed);

    // 5) Restore morph cursor position
    if (xml->hasAttribute("morphX"))
        morphX_.store(static_cast<float>(xml->getDoubleAttribute("morphX", 0.5)), std::memory_order_relaxed);
    if (xml->hasAttribute("morphY"))
        morphY_.store(static_cast<float>(xml->getDoubleAttribute("morphY", 0.5)), std::memory_order_relaxed);
    if (xml->hasAttribute("faderPos"))
        faderPos_.store(static_cast<float>(xml->getDoubleAttribute("faderPos", 0.0)), std::memory_order_relaxed);
    if (xml->hasAttribute("morphSource"))
        morphSource_.store(xml->getIntAttribute("morphSource", 0), std::memory_order_relaxed);

    // 6) Restore MCP identity (port reuse)
    if (auto* mcpXml = xml->getChildByName("MCP_IDENTITY"))
    {
        pendingIdentity_.port = mcpXml->getIntAttribute("port", 0);
        pendingIdentity_.bearerToken = mcpXml->getStringAttribute("token", "");
        pendingIdentity_.instanceId = mcpXml->getStringAttribute("instanceId", "");
        pendingIdentity_.morphCode = mcpXml->getStringAttribute("morphCode", "");
        DBG("setStateInformation: Restored MCP identity, port: " + juce::String(pendingIdentity_.port));
    }

    // 7) Buffer the hosted plugin's opaque state for re-application after async load
    if (auto* stateXml = xml->getChildByName("HOSTED_PLUGIN_STATE"))
    {
        juce::String base64 = stateXml->getStringAttribute("data", "");
        if (base64.isNotEmpty())
        {
            std::lock_guard<std::mutex> guard(pendingStateMutex_);
            pendingHostedState_.reset();
            pendingHostedState_.fromBase64Encoding(base64);
            DBG("setStateInformation: Buffered hosted plugin state, size: "
                + juce::String(static_cast<int>(pendingHostedState_.getSize())) + " bytes");
        }
    }

    // 8) Restore the hosted plugin — synchronous when possible, Timer fallback when not.
    //    CRITICAL: Do NOT use MessageManager::callAsync() here. Per JUCE forum findings,
    //    callAsync() may silently drop callbacks when the editor is closed (FL Studio,
    //    Linux hosts), or when "Smart disable" is active. Timers are more reliable.
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
                    hasPendingPluginLoad_ = true;
                    pendingLoadAttempts_ = 1;  // Already tried once
                    startTimer(50);
                }
            }
            else
            {
                // Not on message thread — store description and use Timer to poll.
                // CRITICAL: startTimer() must be called from the message thread.
                // We use callAsync to hop to the message thread first, then start the timer.
                DBG("setStateInformation: Not on message thread, deferring to timer");
                pendingPluginDesc_ = desc;
                hasPendingPluginLoad_ = true;
                pendingLoadAttempts_ = 0;
                juce::MessageManager::callAsync([this] { startTimer(50); });
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

bool MorphSnapProcessor::loadHostedPluginFromState(const juce::PluginDescription& desc)
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
        if (auto* plugin = host.getPlugin())
        {
            std::lock_guard<std::mutex> guard(pendingStateMutex_);
            if (pendingHostedState_.getSize() > 0)
            {
                plugin->setStateInformation(
                    pendingHostedState_.getData(),
                    static_cast<int>(pendingHostedState_.getSize()));
                DBG("loadHostedPluginFromState: Restored plugin state, size: "
                    + juce::String(static_cast<int>(pendingHostedState_.getSize())) + " bytes");
                pendingHostedState_.reset();
            }
            else
            {
                DBG("loadHostedPluginFromState: No pending state to restore");
            }
        }

        // Refresh discrete parameter map for the restored plugin
        refreshDiscreteMap();
        
        // Unblock the morph engine on success
        isRestoring_.store(false, std::memory_order_release);
    }
    else
    {
        DBG("MorphSnapProcessor::loadHostedPluginFromState — loadPlugin failed for: " + desc.name);
        // Don't unblock the morph engine yet — we're going to retry via timer
    }
    
    return loaded;
}

bool MorphSnapProcessor::ensurePluginFormatsReady()
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

void MorphSnapProcessor::timerCallback()
{
    // Timer fires on the message thread — exactly where we need to load plugins.
    if (!hasPendingPluginLoad_)
    {
        stopTimer();
        return;
    }

    // Attempt to load — retry up to MAX_PLUGIN_LOAD_RETRIES times (500ms window) in case the host
    // is still initialising when the first tick fires (e.g. FL Studio startup scan, export rendering).
    DBG("timerCallback: Attempt " + juce::String(pendingLoadAttempts_ + 1) + " to load plugin: "
        + pendingPluginDesc_.name);
    
    const bool loaded = loadHostedPluginFromState(pendingPluginDesc_);
    if (loaded)
    {
        // Success!
        hasPendingPluginLoad_ = false;
        pendingLoadAttempts_  = 0;
        stopTimer();
        DBG("timerCallback: Plugin loaded successfully after " + juce::String(pendingLoadAttempts_ + 1) + " attempt(s)");
    }
    else if (++pendingLoadAttempts_ >= MAX_PLUGIN_LOAD_RETRIES)
    {
        // Give up after max retries
        hasPendingPluginLoad_ = false;
        int totalAttempts = pendingLoadAttempts_;
        pendingLoadAttempts_ = 0;
        stopTimer();
        
        DBG("MorphSnapProcessor::timerCallback — gave up after " + juce::String(totalAttempts) + " attempts: "
            + pendingPluginDesc_.name);
        
        // Unblock the morph engine so audio can still pass through
        isRestoring_.store(false, std::memory_order_release);
        
        juce::ignoreUnused(totalAttempts);  // Used in debug builds
    }
    // else: stay running; next tick will retry
}

// ── Editor ────────────────────────────────────────────────────────────────────────────

juce::AudioProcessorEditor* MorphSnapProcessor::createEditor()
{
    return new MorphSnapEditor(*this);
}

} // namespace morphsnap

// refreshDiscreteMap — called after plugin load to update discrete parameter classification
void morphsnap::MorphSnapProcessor::refreshDiscreteMap()
{
    auto map = paramBridge.getDiscreteMap();
    morphProcessor.setDiscreteMap(std::move(map));
}

// Plugin entry point
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new morphsnap::MorphSnapProcessor();
}

