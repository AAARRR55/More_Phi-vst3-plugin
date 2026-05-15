#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>
#include <vector>

namespace more_phi {

class TrackAssistantStore
{
public:
    static juce::File getStoreDirectory();
    static juce::File getStoreFile();

    static bool isValidTrackId(const juce::String& trackId) noexcept;
    static bool isValidStatus(const juce::String& status) noexcept;
    static bool isValidAnalysisProfile(const juce::String& profile) noexcept;
    static bool isValidDate(const juce::String& date) noexcept;

    static nlohmann::json ensureCurrentSessionTrack(const juce::String& instanceId);
    static nlohmann::json upsertFileTrack(const juce::File& sourceFile,
                                          const juce::String& renderJobId);

    static nlohmann::json search(const juce::String& query,
                                 const std::vector<juce::String>& statusFilter,
                                 const juce::String& dateFrom,
                                 const juce::String& dateTo,
                                 int page,
                                 int pageSize);

    static nlohmann::json getInfo(const juce::String& trackId, bool includeHistory);
    static nlohmann::json updateStatus(const juce::String& trackId,
                                       const juce::String& newStatus,
                                       const juce::String& reason);
    static nlohmann::json setAnalysis(const juce::String& trackId,
                                      const nlohmann::json& analysis);
    static nlohmann::json selectCandidateForRenderJob(const juce::String& renderJobId,
                                                       const juce::String& candidateId,
                                                       const juce::String& outputPath);

    static juce::String findTrackIdByRenderJob(const juce::String& renderJobId);
    static void setStoreDirectoryOverrideForTests(const juce::File& directory);
    static void clearStoreDirectoryOverrideForTests();
};

} // namespace more_phi
