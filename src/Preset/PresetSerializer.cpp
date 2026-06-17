/* More-Phi — Preset/PresetSerializer.cpp
 * JSON serialization for meta preset storage.
 * Format: { "version": 1, "name": "...", "apvts": {...}, "snapshots": [...] }
 */
#include "PresetSerializer.h"
#include "Core/ParameterState.h"
#include "Host/ParameterBridge.h"
#include "Host/PluginHostManager.h"

namespace more_phi {

juce::var PresetSerializer::serialize(const SnapshotBank& bank,
                                      juce::AudioProcessorValueTreeState& apvts,
                                      PluginHostManager* hostManager)
{
    auto* root = new juce::DynamicObject();
    root->setProperty("version", 1);

    // Serialize APVTS state as XML string
    auto state = apvts.copyState();
    auto xml = state.createXml();
    if (xml)
        root->setProperty("apvts", xml->toString());

    // Serialize snapshot slots
    auto snapshots = juce::Array<juce::var>();

    // Read snapshot data through the seqlock
    if (!bank.tryReadLocked([&](const std::array<ParameterState, SnapshotBank::NUM_SLOTS>& slots)
    {
        for (int s = 0; s < SnapshotBank::NUM_SLOTS; ++s)
        {
            auto* slotObj = new juce::DynamicObject();
            slotObj->setProperty("occupied", slots[s].occupied);
            slotObj->setProperty("paramCount", slots[s].parameterCount);
            slotObj->setProperty("name", juce::String(slots[s].name));

            if (slots[s].occupied && slots[s].parameterCount > 0)
            {
                auto values = juce::Array<juce::var>();
                for (int i = 0; i < slots[s].parameterCount; ++i)
                    values.add(static_cast<double>(slots[s].values[i]));
                slotObj->setProperty("values", values);
            }

            // FIX C6: Persist per-slot opaque state chunk for Full recall mode.
            juce::MemoryBlock chunk;
            if (bank.copyStateChunk(s, chunk) && chunk.getSize() > 0)
                slotObj->setProperty("stateChunk", chunk.toBase64Encoding());

            snapshots.add(juce::var(slotObj));
        }
    }))
    {
        juce::Logger::writeToLog("PresetSerializer::serialize — tryReadLocked failed");
        delete root;
        return juce::var{};
    }

    root->setProperty("snapshots", snapshots);

    // Optional hosted plugin info/state for preset parity with DAW export state.
    if (hostManager != nullptr)
    {
        if (const auto* desc = hostManager->getLastDescription())
        {
            if (auto descXml = desc->createXml())
                root->setProperty("hostedPlugin", descXml->toString());
        }

        if (auto* plugin = hostManager->acquirePluginForUse())
        {
            juce::MemoryBlock pluginState;
            try
            {
                plugin->getStateInformation(pluginState);
            }
            catch (const std::exception& e)
            {
                juce::Logger::writeToLog("PresetSerializer::serialize — getStateInformation failed: "
                    + juce::String(e.what()));
                pluginState.reset();
            }
            catch (...)
            {
                juce::Logger::writeToLog("PresetSerializer::serialize — getStateInformation failed: unknown exception");
                pluginState.reset();
            }

            hostManager->releasePluginFromUse();

            if (pluginState.getSize() > 0)
                root->setProperty("hostedPluginState", pluginState.toBase64Encoding());
        }
    }

    return juce::var(root);
}

bool PresetSerializer::deserialize(const juce::var& json,
                                    SnapshotBank& bank,
                                    juce::AudioProcessorValueTreeState& apvts,
                                    PluginHostManager* hostManager)
{
    if (!json.isObject()) return false;

    auto version = json.getProperty("version", 0);
    const int ver = static_cast<int>(version);
    if (ver < 1)
        return false;
    if (ver > 1)
    {
        juce::Logger::writeToLog("PresetSerializer::deserialize — preset version "
            + juce::String(ver) + " is newer than expected (1); attempting migration");
        // Future: call migrateFromV1() or version-specific migration here
        // For now, proceed with best-effort deserialization
    }

    // Restore APVTS from XML string
    auto apvtsXml = json.getProperty("apvts", {});
    if (apvtsXml.isString())
    {
        auto xml = juce::parseXML(apvtsXml.toString());
        if (xml && xml->hasTagName(apvts.state.getType()))
            apvts.replaceState(juce::ValueTree::fromXml(*xml));
    }

    // Restore snapshot slots
    auto* snapArr = json.getProperty("snapshots", {}).getArray();
    if (snapArr)
    {
        bank.clearAll();
        for (int s = 0; s < juce::jmin(static_cast<int>(snapArr->size()),
                                         SnapshotBank::NUM_SLOTS); ++s)
        {
            const auto& slotVar = (*snapArr)[s];
            if (!slotVar.isObject()) continue;

            bool occupied = slotVar.getProperty("occupied", false);
            if (!occupied) continue;

            int paramCount = slotVar.getProperty("paramCount", 0);
            auto* valArr = slotVar.getProperty("values", {}).getArray();
            if (!valArr || paramCount <= 0) continue;

            std::vector<float> values;
            values.reserve(static_cast<size_t>(paramCount));
            for (int i = 0; i < juce::jmin(paramCount, static_cast<int>(valArr->size())); ++i)
                values.push_back(static_cast<float>(static_cast<double>((*valArr)[i])));

            bank.captureValues(s, values);

            // FIX C6: Restore per-slot opaque state chunk (Kontakt/wavetable synths).
            juce::String stateBase64 = slotVar.getProperty("stateChunk", {}).toString();
            if (stateBase64.isNotEmpty())
            {
                juce::MemoryBlock chunk;
                if (chunk.fromBase64Encoding(stateBase64))
                    bank.captureStateChunk(s, chunk);
            }

            // Restore parameter names for VST3-H1 remapping.
            auto* nameArr = slotVar.getProperty("names", {}).getArray();
            if (nameArr != nullptr)
            {
                juce::StringArray names;
                for (int i = 0; i < nameArr->size(); ++i)
                    names.add((*nameArr)[i].toString());
                bank.captureValuesWithNames(s, values.data(), static_cast<int>(values.size()), names);
            }
        }
    }

    if (hostManager != nullptr)
    {
        bool pluginReady = hostManager->hasPlugin();

        const auto hostedPluginVar = json.getProperty("hostedPlugin", juce::var{});
        if (hostedPluginVar.isString())
        {
            if (auto descXml = juce::parseXML(hostedPluginVar.toString()))
            {
                juce::PluginDescription desc;
                if (desc.loadFromXml(*descXml))
                    pluginReady = hostManager->loadPlugin(desc);
            }
        }

        juce::MemoryBlock hostedState;
        const auto hostedStateVar = json.getProperty("hostedPluginState", juce::var{});
        if (hostedStateVar.isString())
        {
            if (!hostedState.fromBase64Encoding(hostedStateVar.toString()))
                hostedState.reset(); // H11 FIX: clear on base64 decode failure
        }

        if (pluginReady && hostedState.getSize() > 0)
        {
            if (auto* plugin = hostManager->acquirePluginForUse())
            {
                try
                {
                    plugin->setStateInformation(hostedState.getData(),
                                                static_cast<int>(hostedState.getSize()));
                }
                catch (const std::exception& e)
                {
                    juce::Logger::writeToLog("PresetSerializer::deserialize — setStateInformation failed: "
                        + juce::String(e.what()));
                }
                catch (...)
                {
                    juce::Logger::writeToLog("PresetSerializer::deserialize — setStateInformation failed: unknown exception");
                }
                hostManager->releasePluginFromUse();
            }
        }
    }

    return true;
}

} // namespace more_phi
