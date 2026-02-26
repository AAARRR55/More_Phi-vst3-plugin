/* MorphSnap — Preset/CloudSyncClient.h
 * Cloud sync client — stub implementation for V2.
 * Real HTTP implementation will be added in a future release.
 * This establishes the API contract so the rest of the system can be built.
 * MESSAGE THREAD ONLY. */
#pragma once

#include "PresetEntry.h"
#include <functional>
#include <string>
#include <vector>

namespace morphsnap {

class CloudSyncClient
{
public:
    enum class SyncStatus
    {
        Idle,
        Syncing,
        Success,
        Failed,
        NotConfigured
    };

    CloudSyncClient();
    ~CloudSyncClient();

    // Non-copyable
    CloudSyncClient(const CloudSyncClient&) = delete;
    CloudSyncClient& operator=(const CloudSyncClient&) = delete;

    // ── Configuration ────────────────────────────────────────────────────────

    /** Set the base URL of the sync endpoint, e.g. "https://api.morphsnap.io/v2". */
    void setEndpoint(const std::string& url);

    /** Set the API key used to authenticate requests. */
    void setApiKey(const std::string& key);

    /** True when both endpoint and API key have been set. */
    bool isConfigured() const;

    // ── Async sync operations ────────────────────────────────────────────────

    /** Callback type: called on completion of any async operation.
     *  message provides a human-readable description of the result or error. */
    using SyncCallback = std::function<void(SyncStatus, const std::string& message)>;

    /** Stub: immediately invokes callback with SyncStatus::NotConfigured (or
     *  SyncStatus::Success if configured, for future-proofing).
     *  In a real implementation this would POST the preset JSON to the endpoint. */
    void uploadPreset(const PresetEntry& preset, SyncCallback callback);

    /** Stub: immediately invokes callback with SyncStatus::NotConfigured (or
     *  SyncStatus::Success).  In a real implementation this would GET the preset
     *  JSON from the endpoint and deserialise it. */
    void downloadPreset(const std::string& presetId, SyncCallback callback);

    /** Stub: immediately invokes callback.
     *  In a real implementation this would perform a full two-way sync. */
    void syncAll(SyncCallback callback);

    // ── Cloud browse ─────────────────────────────────────────────────────────

    /** Stub: always returns an empty vector.
     *  In a real implementation this would query the cloud preset catalogue. */
    std::vector<PresetEntry> browseCloud(const PresetSearchQuery& query);

    // ── Status ───────────────────────────────────────────────────────────────

    SyncStatus getStatus() const;

private:
    std::string endpoint_;
    std::string apiKey_;
    SyncStatus  status_ = SyncStatus::NotConfigured;
};

} // namespace morphsnap
