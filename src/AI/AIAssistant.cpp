/*
 * More-Phi — AI/AIAssistant.cpp
 */
#include "AIAssistant.h"
#include "Plugin/PluginProcessor.h"

namespace more_phi {

AIAssistant::AIAssistant(MorePhiProcessor& processor)
    : processor_(processor)
{
}

AIAssistant::~AIAssistant() = default;

void AIAssistant::stagePendingChanges(std::vector<ParamChange> changes)
{
    pendingChanges_ = std::move(changes);
    previewActive_ = false;
}

void AIAssistant::clearPendingChanges()
{
    pendingChanges_.clear();
    previewActive_ = false;
}

bool AIAssistant::applyPreview(juce::String* errorMessage)
{
    if (pendingChanges_.empty())
    {
        if (errorMessage != nullptr)
            *errorMessage = "no pending changes";
        return false;
    }

    for (const auto& change : pendingChanges_)
    {
        const int index = canonicalIndex(change);
        if (index < 0)
        {
            if (errorMessage != nullptr)
                *errorMessage = "invalid parameter index";
            return false;
        }

        if (!processor_.enqueueParameterSet(index,
                                            juce::jlimit(0.0f, 1.0f, change.newValue),
                                            MorePhiProcessor::ParameterEditSource::Assistant,
                                            true))
        {
            if (errorMessage != nullptr)
                *errorMessage = "parameter command queue is full";
            return false;
        }
    }

    previewActive_ = true;
    return true;
}

void AIAssistant::rejectPreview()
{
    clearPendingChanges();
}

void AIAssistant::commitPreview()
{
    clearPendingChanges();
}

int AIAssistant::canonicalIndex(const ParamChange& change) noexcept
{
    return change.index >= 0 ? change.index : change.paramIndex;
}

} // namespace more_phi
