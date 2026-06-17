/*
 * More-Phi — AI/InstanceRegistry.cpp
 * Thread-safe singleton managing active plugin instances.
 */
#include "InstanceRegistry.h"

namespace more_phi {

InstanceIdentity InstanceRegistry::registerInstance()
{
    std::lock_guard<std::mutex> lock(mutex_);

    int port = findAvailablePort();
    if (port <= 0)
    {
        DBG("InstanceRegistry: no free MCP port found; registration failed");
        return {};
    }

    auto identity = InstanceIdentity::generate(port);

    instances_[identity.instanceId] = identity;

    DBG("InstanceRegistry: registered instance [" + identity.morphCode
        + "] on port " + juce::String(port)
        + " (total: " + juce::String(static_cast<int>(instances_.size())) + ")");

    return identity;
}

void InstanceRegistry::deregisterInstance(const juce::String& instanceId)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = instances_.find(instanceId);
    if (it != instances_.end())
    {
        DBG("InstanceRegistry: deregistered instance [" + it->second.morphCode
            + "] from port " + juce::String(it->second.port));
        instances_.erase(it);
    }
}

std::optional<InstanceIdentity> InstanceRegistry::getIdentity(const juce::String& instanceId) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = instances_.find(instanceId);
    if (it != instances_.end())
        return it->second;
    return std::nullopt;
}

std::vector<InstanceIdentity> InstanceRegistry::getAllInstances() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<InstanceIdentity> result;
    result.reserve(instances_.size());
    for (const auto& [id, identity] : instances_)
        result.push_back(identity);
    return result;
}

int InstanceRegistry::getActiveCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(instances_.size());
}

int InstanceRegistry::findAvailablePort()
{
    // Must be called with mutex_ held
    const int64_t now = juce::Time::currentTimeMillis();
    constexpr int64_t TTL_MS = 5 * 60 * 1000;

    // C14 FIX: Evict zombie instances older than 5 minutes whose port is no longer in use.
    for (auto it = instances_.begin(); it != instances_.end(); )
    {
        if (now - it->second.createdAt > TTL_MS)
        {
            juce::StreamingSocket probe;
            if (probe.createListener(it->second.port, "127.0.0.1"))
            {
                probe.close();
                DBG("InstanceRegistry: evicted zombie instance [" + it->second.morphCode
                    + "] from port " + juce::String(it->second.port));
                it = instances_.erase(it);
                continue;
            }
        }
        ++it;
    }

    const auto isRegisteredPortInUse = [this](int candidatePort)
    {
        for (const auto& [id, identity] : instances_)
        {
            juce::ignoreUnused(id);
            if (identity.port == candidatePort)
                return true;
        }

        return false;
    };

    for (int port = BASE_PORT; port < BASE_PORT + MAX_INSTANCES; ++port)
    {
        if (!isRegisteredPortInUse(port))
            return port;
    }

    // Fallback: extend search window for externally-occupied ports.
    for (int port = BASE_PORT + MAX_INSTANCES;
         port < BASE_PORT + MAX_INSTANCES + 256; ++port)
    {
        if (!isRegisteredPortInUse(port))
            return port;
    }

    // Last resort: search IANA dynamic/private port range.
    for (int port = 49152; port <= 65535; ++port)
    {
        if (!isRegisteredPortInUse(port))
            return port;
    }

    return -1;
}

} // namespace more_phi
