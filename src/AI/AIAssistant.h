/*
 * More-Phi — AI/AIAssistant.h
 * Minimal assistant edit-state bridge used by MCP EQ tooling.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace more_phi {

class MorePhiProcessor;

struct ParamChange
{
    int paramIndex = -1;
    int index = -1;
    juce::String stableId;
    juce::String name;
    float currentValue = 0.0f;
    float newValue = 0.0f;
};

class AIAssistant
{
public:
    explicit AIAssistant(MorePhiProcessor& processor);
    ~AIAssistant();

    void stagePendingChanges(std::vector<ParamChange> changes);
    const std::vector<ParamChange>& getPendingChanges() const noexcept { return pendingChanges_; }
    void clearPendingChanges();

    bool applyPreview(juce::String* errorMessage = nullptr);
    void rejectPreview();
    void commitPreview();
    bool isPreviewActive() const noexcept { return previewActive_; }

private:
    static int canonicalIndex(const ParamChange& change) noexcept;

    MorePhiProcessor& processor_;
    std::vector<ParamChange> pendingChanges_;
    bool previewActive_ = false;
};

} // namespace more_phi
