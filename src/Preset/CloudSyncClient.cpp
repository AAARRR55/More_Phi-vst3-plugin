/* More-Phi — Preset/CloudSyncClient.cpp
 * Stub implementation for cloud sync.
 * All operations complete immediately without any network activity.
 * Replace method bodies with real HTTP calls in a future release. */
#include "CloudSyncClient.h"

namespace more_phi {

// ── Constructor / Destructor ─────────────────────────────────────────────────

CloudSyncClient::CloudSyncClient() = default;
CloudSyncClient::~CloudSyncClient() = default;

// ── Configuration ─────────────────────────────────────────────────────────────

void CloudSyncClient::setEndpoint(const std::string& url)
{
    endpoint_ = url;
    // Update status: if both endpoint and key are now set, transition to Idle.
    if (!endpoint_.empty() && !apiKey_.empty())
        status_ = SyncStatus::Idle;
    else
        status_ = SyncStatus::NotConfigured;
}

void CloudSyncClient::setApiKey(const std::string& key)
{
    apiKey_ = key;
    if (!endpoint_.empty() && !apiKey_.empty())
        status_ = SyncStatus::Idle;
    else
        status_ = SyncStatus::NotConfigured;
}

bool CloudSyncClient::isConfigured() const
{
    return !endpoint_.empty() && !apiKey_.empty();
}

// ── Status ────────────────────────────────────────────────────────────────────

CloudSyncClient::SyncStatus CloudSyncClient::getStatus() const
{
    return status_;
}

// ── Stub sync operations ──────────────────────────────────────────────────────
//
// Each stub immediately invokes the callback on the calling thread.
// Real implementations would dispatch asynchronous HTTP requests and invoke
// the callback on the message thread upon completion.

void CloudSyncClient::uploadPreset(const PresetEntry& /*preset*/,
                                    SyncCallback callback)
{
    if (!isConfigured())
    {
        if (callback)
            callback(SyncStatus::NotConfigured,
                     "CloudSyncClient is not configured. "
                     "Call setEndpoint() and setApiKey() first.");
        return;
    }

    // Stub: pretend success.
    if (callback)
        callback(SyncStatus::Success,
                 "Upload stub — no network request was made.");
}

void CloudSyncClient::downloadPreset(const std::string& /*presetId*/,
                                      SyncCallback callback)
{
    if (!isConfigured())
    {
        if (callback)
            callback(SyncStatus::NotConfigured,
                     "CloudSyncClient is not configured. "
                     "Call setEndpoint() and setApiKey() first.");
        return;
    }

    // Stub: pretend success (no preset data is actually returned).
    if (callback)
        callback(SyncStatus::Success,
                 "Download stub — no network request was made.");
}

void CloudSyncClient::syncAll(SyncCallback callback)
{
    if (!isConfigured())
    {
        if (callback)
            callback(SyncStatus::NotConfigured,
                     "CloudSyncClient is not configured. "
                     "Call setEndpoint() and setApiKey() first.");
        return;
    }

    // Stub: pretend success.
    if (callback)
        callback(SyncStatus::Success,
                 "Sync-all stub — no network requests were made.");
}

// ── Cloud browse ──────────────────────────────────────────────────────────────

std::vector<PresetEntry> CloudSyncClient::browseCloud(
    const PresetSearchQuery& /*query*/)
{
    // Stub: always returns an empty result set.
    // Real implementation would issue a paginated GET request to the cloud API.
    return {};
}

} // namespace more_phi
