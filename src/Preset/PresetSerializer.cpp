/* MorphSnap — Preset/PresetSerializer.cpp
 * JSON serialization for meta preset storage.
 * Format: { "version": 1, "name": "...", "apvts": {...}, "snapshots": [...] }
 */
#include "PresetSerializer.h"
#include "Core/ParameterState.h"
#include "Host/ParameterBridge.h"

namespace morphsnap {

juce::var PresetSerializer::serialize(const SnapshotBank& bank,
                                      juce::AudioProcessorValueTreeState& apvts)
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
    bank.tryReadLocked([&](const std::array<ParameterState, SnapshotBank::NUM_SLOTS>& slots)
    {
        for (int s = 0; s < SnapshotBank::NUM_SLOTS; ++s)
        {
            auto* slotObj = new juce::DynamicObject();
            slotObj->setProperty("occupied", slots[s].occupied);
            slotObj->setProperty("paramCount", slots[s].parameterCount);

            if (slots[s].occupied && slots[s].parameterCount > 0)
            {
                auto values = juce::Array<juce::var>();
                for (int i = 0; i < slots[s].parameterCount; ++i)
                    values.add(static_cast<double>(slots[s].values[i]));
                slotObj->setProperty("values", values);
            }

            snapshots.add(juce::var(slotObj));
        }
    });

    root->setProperty("snapshots", snapshots);

    return juce::var(root);
}

bool PresetSerializer::deserialize(const juce::var& json,
                                    SnapshotBank& bank,
                                    juce::AudioProcessorValueTreeState& apvts)
{
    if (!json.isObject()) return false;

    auto version = json.getProperty("version", 0);
    if (static_cast<int>(version) != 1) return false;

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
        }
    }

    return true;
}

} // namespace morphsnap
