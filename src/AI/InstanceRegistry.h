/*
 * More-Phi — AI/InstanceRegistry.h
 * Thread-safe singleton managing active plugin instances.
 * Handles port allocation and instance lifecycle across all MorePhi instances.
 */
#pragma once

#include "InstanceIdentity.h"
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace more_phi {

class InstanceRegistry
{
public:
    static InstanceRegistry& getInstance()
    {
        static InstanceRegistry registry;
        return registry;
    }

    /** Register a new instance — allocates port, generates identity. */
    InstanceIdentity registerInstance();

    /** Deregister an instance by ID — frees its port. */
    void deregisterInstance(const juce::String& instanceId);

    /** Get a specific instance's identity (empty if not found). */
    std::optional<InstanceIdentity> getIdentity(const juce::String& instanceId) const;

    /** Get all active instances (snapshot, thread-safe). */
    std::vector<InstanceIdentity> getAllInstances() const;

    /** Number of active instances. */
    int getActiveCount() const;

    static constexpr int BASE_PORT = 30001;
    static constexpr int MAX_INSTANCES = 64;

private:
    InstanceRegistry() = default;
    ~InstanceRegistry() = default;

    InstanceRegistry(const InstanceRegistry&) = delete;
    InstanceRegistry& operator=(const InstanceRegistry&) = delete;

    int findAvailablePort();

    mutable std::mutex mutex_;
    std::map<juce::String, InstanceIdentity> instances_;
};

} // namespace more_phi
