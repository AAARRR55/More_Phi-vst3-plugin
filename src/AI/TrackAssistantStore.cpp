#include "TrackAssistantStore.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>

namespace more_phi {

using json = nlohmann::json;

namespace {

constexpr const char* kDefaultArtist = "Unknown Artist";
constexpr const char* kPendingReview = "pending_review";
constexpr const char* kInMastering = "in_mastering";
constexpr const char* kMasteringComplete = "mastering_complete";

std::mutex& storeMutex()
{
    static std::mutex mutex;
    return mutex;
}

juce::File& overrideDirectory()
{
    static juce::File directory;
    return directory;
}

bool& hasOverrideDirectory()
{
    static bool enabled = false;
    return enabled;
}

juce::File getStoreDirectoryUnlocked()
{
    if (hasOverrideDirectory())
        return overrideDirectory();

#if defined(_MSC_VER)
    char* envBuffer = nullptr;
    size_t envLength = 0;
    if (_dupenv_s(&envBuffer, &envLength, "MORE_PHI_TRACK_ASSISTANT_STORE_DIR") == 0
        && envBuffer != nullptr)
    {
        const juce::String directory(envBuffer);
        std::free(envBuffer);
        if (directory.isNotEmpty())
            return juce::File(directory);
    }
#else
    if (const char* env = std::getenv("MORE_PHI_TRACK_ASSISTANT_STORE_DIR"))
    {
        const juce::String directory(env);
        if (directory.isNotEmpty())
            return juce::File(directory);
    }
#endif

    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("More-Phi")
        .getChildFile("track_assistant");
}

juce::File getStoreFileUnlocked()
{
    return getStoreDirectoryUnlocked().getChildFile("tracks.json");
}

std::string toStdString(const juce::String& value)
{
    return value.toStdString();
}

juce::String nowIso()
{
    return juce::Time::getCurrentTime().toISO8601(true);
}

bool isAlnum(juce::juce_wchar c) noexcept
{
    return (c >= static_cast<juce::juce_wchar>('0') && c <= static_cast<juce::juce_wchar>('9'))
        || (c >= static_cast<juce::juce_wchar>('a') && c <= static_cast<juce::juce_wchar>('z'))
        || (c >= static_cast<juce::juce_wchar>('A') && c <= static_cast<juce::juce_wchar>('Z'));
}

juce::String makeTrackId(const juce::String& seed)
{
    auto body = (juce::String::toHexString(seed.hashCode64())
              + juce::String::toHexString((seed + "|morephi-track").hashCode64()))
                    .retainCharacters("0123456789abcdefABCDEF")
                    .toLowerCase();

    while (body.length() < 20)
        body += "0";

    return "trk_" + body.substring(0, 20);
}

juce::String normaliseStatus(const juce::String& status)
{
    return status.trim().toLowerCase();
}

json emptyRoot()
{
    return json{{"schema_version", 1}, {"tracks", json::array()}};
}

json loadRootUnlocked()
{
    const auto file = getStoreFileUnlocked();
    if (!file.existsAsFile())
        return emptyRoot();

    try
    {
        auto root = json::parse(file.loadFileAsString().toStdString());
        if (!root.is_object() || !root.contains("tracks") || !root["tracks"].is_array())
            return emptyRoot();
        if (!root.contains("schema_version"))
            root["schema_version"] = 1;
        return root;
    }
    catch (...)
    {
        return emptyRoot();
    }
}

bool saveRootUnlocked(const json& root)
{
    const auto directory = getStoreDirectoryUnlocked();
    if (!directory.exists() && directory.createDirectory().failed())
        return false;

    return getStoreFileUnlocked().replaceWithText(juce::String(root.dump(2)));
}

json summaryForTrack(const json& track)
{
    json summary{
        {"track_id", track.value("track_id", "")},
        {"title", track.value("title", "")},
        {"artist", track.value("artist", kDefaultArtist)},
        {"status", track.value("status", kPendingReview)},
        {"created_at", track.value("created_at", "")},
        {"updated_at", track.value("updated_at", "")}
    };

    if (track.contains("latest_render_job_id"))
        summary["latest_render_job_id"] = track["latest_render_job_id"];
    if (track.contains("selected_candidate_id"))
        summary["selected_candidate_id"] = track["selected_candidate_id"];

    return summary;
}

int findTrackIndex(const json& root, const juce::String& trackId)
{
    const auto target = trackId.toStdString();
    const auto& tracks = root["tracks"];
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        if (tracks[i].value("track_id", "") == target)
            return static_cast<int>(i);
    }
    return -1;
}

int findTrackIndexByRenderJob(const json& root, const juce::String& renderJobId)
{
    const auto target = renderJobId.toStdString();
    const auto& tracks = root["tracks"];
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        if (tracks[i].value("latest_render_job_id", "") == target)
            return static_cast<int>(i);
    }
    return -1;
}

void appendHistory(json& track, const juce::String& status, const juce::String& reason)
{
    if (!track.contains("history") || !track["history"].is_array())
        track["history"] = json::array();

    json entry{
        {"status", toStdString(status)},
        {"at", toStdString(nowIso())}
    };

    if (reason.isNotEmpty())
        entry["reason"] = toStdString(reason);

    track["history"].push_back(entry);
}

bool containsLower(const std::string& haystack, const juce::String& needle)
{
    if (needle.isEmpty())
        return true;

    return juce::String(haystack).toLowerCase().contains(needle.toLowerCase());
}

bool statusAllowed(const json& track, const std::vector<juce::String>& filters)
{
    if (filters.empty())
        return true;

    const auto status = juce::String(track.value("status", ""));
    return std::any_of(filters.begin(), filters.end(), [&](const juce::String& filter)
    {
        return status == normaliseStatus(filter);
    });
}

bool dateAllowed(const json& track, const juce::String& dateFrom, const juce::String& dateTo)
{
    const auto created = juce::String(track.value("created_at", "")).substring(0, 10);
    if (dateFrom.isNotEmpty() && created.compare(dateFrom) < 0)
        return false;
    if (dateTo.isNotEmpty() && created.compare(dateTo) > 0)
        return false;
    return true;
}

} // namespace

juce::File TrackAssistantStore::getStoreDirectory()
{
    const std::lock_guard<std::mutex> guard(storeMutex());
    return getStoreDirectoryUnlocked();
}

juce::File TrackAssistantStore::getStoreFile()
{
    const std::lock_guard<std::mutex> guard(storeMutex());
    return getStoreFileUnlocked();
}

bool TrackAssistantStore::isValidTrackId(const juce::String& trackId) noexcept
{
    if (!trackId.startsWith("trk_") || trackId.length() != 24)
        return false;

    for (int i = 4; i < trackId.length(); ++i)
        if (!isAlnum(trackId[i]))
            return false;

    return true;
}

bool TrackAssistantStore::isValidStatus(const juce::String& status) noexcept
{
    const auto s = normaliseStatus(status);
    return s == kPendingReview || s == kInMastering || s == kMasteringComplete
        || s == "approved" || s == "rejected" || s == "on_hold";
}

bool TrackAssistantStore::isValidAnalysisProfile(const juce::String& profile) noexcept
{
    const auto p = profile.trim().toLowerCase();
    return p == "standard" || p == "streaming" || p == "vinyl_master" || p == "broadcast";
}

bool TrackAssistantStore::isValidDate(const juce::String& date) noexcept
{
    if (date.isEmpty())
        return true;
    if (date.length() != 10 || date[4] != '-' || date[7] != '-')
        return false;
    for (int i = 0; i < date.length(); ++i)
    {
        if (i == 4 || i == 7)
            continue;
        const auto c = date[i];
        if (c < static_cast<juce::juce_wchar>('0') || c > static_cast<juce::juce_wchar>('9'))
            return false;
    }
    return true;
}

json TrackAssistantStore::ensureCurrentSessionTrack(const juce::String& instanceId)
{
    const std::lock_guard<std::mutex> guard(storeMutex());
    auto root = loadRootUnlocked();

    const auto seed = "current-session:" + (instanceId.isNotEmpty() ? instanceId : "default");
    const auto trackId = makeTrackId(seed);
    const int existing = findTrackIndex(root, trackId);
    if (existing >= 0)
        return root["tracks"][static_cast<size_t>(existing)];

    const auto now = nowIso();
    json track{
        {"track_id", toStdString(trackId)},
        {"title", "Current More-Phi Session"},
        {"artist", kDefaultArtist},
        {"source_path", ""},
        {"status", kPendingReview},
        {"created_at", toStdString(now)},
        {"updated_at", toStdString(now)},
        {"history", json::array()}
    };
    appendHistory(track, kPendingReview, "session created");
    root["tracks"].push_back(track);
    saveRootUnlocked(root);
    return track;
}

json TrackAssistantStore::upsertFileTrack(const juce::File& sourceFile, const juce::String& renderJobId)
{
    const std::lock_guard<std::mutex> guard(storeMutex());
    auto root = loadRootUnlocked();

    const auto trackId = makeTrackId(sourceFile.getFullPathName());
    const auto now = nowIso();
    const int existing = findTrackIndex(root, trackId);

    if (existing >= 0)
    {
        auto& track = root["tracks"][static_cast<size_t>(existing)];
        track["source_path"] = toStdString(sourceFile.getFullPathName());
        track["latest_render_job_id"] = toStdString(renderJobId);
        track["updated_at"] = toStdString(now);
        if (track.value("status", kPendingReview) == std::string(kPendingReview))
        {
            track["status"] = kInMastering;
            appendHistory(track, kInMastering, "render job queued");
        }
        saveRootUnlocked(root);
        return track;
    }

    json track{
        {"track_id", toStdString(trackId)},
        {"title", toStdString(sourceFile.getFileNameWithoutExtension())},
        {"artist", kDefaultArtist},
        {"source_path", toStdString(sourceFile.getFullPathName())},
        {"status", kInMastering},
        {"created_at", toStdString(now)},
        {"updated_at", toStdString(now)},
        {"latest_render_job_id", toStdString(renderJobId)},
        {"history", json::array()}
    };
    appendHistory(track, kInMastering, "render job queued");
    root["tracks"].push_back(track);
    saveRootUnlocked(root);
    return track;
}

json TrackAssistantStore::search(const juce::String& query,
                                 const std::vector<juce::String>& statusFilter,
                                 const juce::String& dateFrom,
                                 const juce::String& dateTo,
                                 int page,
                                 int pageSize)
{
    const std::lock_guard<std::mutex> guard(storeMutex());
    const auto root = loadRootUnlocked();

    page = juce::jmax(1, page);
    pageSize = juce::jlimit(1, 50, pageSize);

    json matches = json::array();
    for (const auto& track : root["tracks"])
    {
        const auto title = track.value("title", "");
        const auto artist = track.value("artist", "");
        if (!containsLower(title, query) && !containsLower(artist, query))
            continue;
        if (!statusAllowed(track, statusFilter))
            continue;
        if (!dateAllowed(track, dateFrom, dateTo))
            continue;

        matches.push_back(summaryForTrack(track));
    }

    const int total = static_cast<int>(matches.size());
    const int start = (page - 1) * pageSize;
    json results = json::array();
    for (int i = start; i < juce::jmin(total, start + pageSize); ++i)
        results.push_back(matches[static_cast<size_t>(i)]);

    return json{
        {"success", true},
        {"total", total},
        {"page", page},
        {"page_size", pageSize},
        {"results", results}
    };
}

json TrackAssistantStore::getInfo(const juce::String& trackId, bool includeHistory)
{
    const std::lock_guard<std::mutex> guard(storeMutex());
    const auto root = loadRootUnlocked();
    const int index = findTrackIndex(root, trackId);
    if (index < 0)
        return json{{"success", false}, {"error", "track_not_found"}, {"track_id", toStdString(trackId)}};

    json track = root["tracks"][static_cast<size_t>(index)];
    track["success"] = true;
    if (!includeHistory)
        track.erase("history");
    return track;
}

json TrackAssistantStore::updateStatus(const juce::String& trackId,
                                       const juce::String& newStatus,
                                       const juce::String& reason)
{
    const std::lock_guard<std::mutex> guard(storeMutex());
    auto root = loadRootUnlocked();
    const int index = findTrackIndex(root, trackId);
    if (index < 0)
        return json{{"success", false}, {"error", "track_not_found"}, {"track_id", toStdString(trackId)}};

    auto& track = root["tracks"][static_cast<size_t>(index)];
    const auto status = normaliseStatus(newStatus);
    track["status"] = toStdString(status);
    track["updated_at"] = toStdString(nowIso());
    appendHistory(track, status, reason);
    saveRootUnlocked(root);

    json result = track;
    result["success"] = true;
    return result;
}

json TrackAssistantStore::setAnalysis(const juce::String& trackId, const json& analysis)
{
    const std::lock_guard<std::mutex> guard(storeMutex());
    auto root = loadRootUnlocked();
    const int index = findTrackIndex(root, trackId);
    if (index < 0)
        return json{{"success", false}, {"error", "track_not_found"}, {"track_id", toStdString(trackId)}};

    auto& track = root["tracks"][static_cast<size_t>(index)];
    track["analysis"] = analysis;
    track["updated_at"] = toStdString(nowIso());
    saveRootUnlocked(root);

    json result = track;
    result["success"] = true;
    return result;
}

json TrackAssistantStore::selectCandidateForRenderJob(const juce::String& renderJobId,
                                                       const juce::String& candidateId,
                                                       const juce::String& outputPath)
{
    const std::lock_guard<std::mutex> guard(storeMutex());
    auto root = loadRootUnlocked();
    const int index = findTrackIndexByRenderJob(root, renderJobId);
    if (index < 0)
        return json{{"success", false}, {"error", "track_not_found_for_render_job"}};

    auto& track = root["tracks"][static_cast<size_t>(index)];
    track["selected_candidate_id"] = toStdString(candidateId);
    track["selected_output_path"] = toStdString(outputPath);
    track["status"] = kMasteringComplete;
    track["updated_at"] = toStdString(nowIso());
    appendHistory(track, kMasteringComplete, "candidate selected");
    saveRootUnlocked(root);

    json result = track;
    result["success"] = true;
    return result;
}

juce::String TrackAssistantStore::findTrackIdByRenderJob(const juce::String& renderJobId)
{
    const std::lock_guard<std::mutex> guard(storeMutex());
    const auto root = loadRootUnlocked();
    const int index = findTrackIndexByRenderJob(root, renderJobId);
    if (index < 0)
        return {};

    return juce::String(root["tracks"][static_cast<size_t>(index)].value("track_id", ""));
}

void TrackAssistantStore::setStoreDirectoryOverrideForTests(const juce::File& directory)
{
    const std::lock_guard<std::mutex> guard(storeMutex());
    overrideDirectory() = directory;
    hasOverrideDirectory() = true;
}

void TrackAssistantStore::clearStoreDirectoryOverrideForTests()
{
    const std::lock_guard<std::mutex> guard(storeMutex());
    overrideDirectory() = juce::File{};
    hasOverrideDirectory() = false;
}

} // namespace more_phi
