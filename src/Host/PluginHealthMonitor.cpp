/*
 * More-Phi — Host/PluginHealthMonitor.cpp
 * State-machine driven crash isolation for the hosted plugin.
 *
 * Tracks consecutive failures, transitions through degraded/suspended states,
 * and orchestrates automatic recovery.
 */
#include "PluginHealthMonitor.h"
#include <chrono>

namespace more_phi {

PluginHealthMonitor::PluginHealthMonitor() = default;

void PluginHealthMonitor::reportSuccess() noexcept
{
    consecutiveFailures_.store(0, std::memory_order_relaxed);

    auto current = state_.load(std::memory_order_relaxed);
    // Success in Degraded → Healthy
    // Success in Recovering → Healthy
    // Success in Healthy → no-op
    // Success in Suspended/Terminated → no-op (recovery must resolve these)
    if (current == PluginHealthState::Degraded
        || current == PluginHealthState::Recovering)
    {
        state_.store(PluginHealthState::Healthy, std::memory_order_relaxed);
        recoveryAttempts_.store(0, std::memory_order_relaxed);
    }
}

void PluginHealthMonitor::reportFailure() noexcept
{
    const int count = consecutiveFailures_.fetch_add(1, std::memory_order_relaxed) + 1;
    const int threshold = getMaxConsecutiveFailures();

    // Transition: Healthy → Degraded on first failure.
    // If the failure count has reached the threshold, go to Suspended.
    // This handles both the cumulative case (e.g. threshold=20) and the
    // edge case where threshold=1 (single failure → immediate Suspended).

    if (count >= threshold)
    {
        // CAS loop: only transition to Suspended from Healthy/Degraded.
        // Don't overwrite Recovering/Suspended/Terminated.
        PluginHealthState expected = state_.load(std::memory_order_relaxed);
        while (expected == PluginHealthState::Healthy
            || expected == PluginHealthState::Degraded)
        {
            if (state_.compare_exchange_weak(expected, PluginHealthState::Suspended,
                                              std::memory_order_relaxed))
            {
                suspendedAtMs_.store(getCurrentTimeMs(), std::memory_order_relaxed);
                break;
            }
            // expected is reloaded by compare_exchange_weak on failure
        }
    }
    else
    {
        // First failures before threshold: Healthy → Degraded
        PluginHealthState expected = PluginHealthState::Healthy;
        state_.compare_exchange_strong(expected, PluginHealthState::Degraded,
                                        std::memory_order_relaxed);
    }
}

bool PluginHealthMonitor::shouldProcess() const noexcept
{
    auto current = state_.load(std::memory_order_relaxed);
    return current == PluginHealthState::Healthy
        || current == PluginHealthState::Degraded;
}

bool PluginHealthMonitor::shouldRecover() const noexcept
{
    auto current = state_.load(std::memory_order_relaxed);
    if (current != PluginHealthState::Suspended)
        return false;

    // Enforce the recovery delay
    const int64_t suspendedAt = suspendedAtMs_.load(std::memory_order_relaxed);
    const int64_t now = getCurrentTimeMs();
    const int delay = getRecoveryDelayMs();
    return (now - suspendedAt) >= delay;
}

bool PluginHealthMonitor::beginRecovery() noexcept
{
    auto current = state_.load(std::memory_order_relaxed);

    if (current == PluginHealthState::Terminated)
        return false;

    if (current == PluginHealthState::Recovering)
    {
        // Already in recovery — check if attempts are exhausted. If so,
        // transition to Terminated (the caller will see false and the state
        // change signals that recovery is no longer possible).
        const int attempts = recoveryAttempts_.load(std::memory_order_relaxed);
        const int maxAttempts = getMaxRecoveryAttempts();
        if (attempts >= maxAttempts)
        {
            state_.store(PluginHealthState::Terminated, std::memory_order_relaxed);
        }
        return false;
    }

    if (current != PluginHealthState::Suspended)
        return false;

    const int attempts = recoveryAttempts_.load(std::memory_order_relaxed);
    const int maxAttempts = getMaxRecoveryAttempts();

    if (attempts >= maxAttempts)
    {
        // Exhausted all recovery attempts — terminate the plugin.
        state_.store(PluginHealthState::Terminated, std::memory_order_relaxed);
        return false;
    }

    recoveryAttempts_.fetch_add(1, std::memory_order_relaxed);
    state_.store(PluginHealthState::Recovering, std::memory_order_relaxed);
    return true;
}

void PluginHealthMonitor::endRecovery(bool succeeded) noexcept
{
    auto current = state_.load(std::memory_order_relaxed);

    if (current != PluginHealthState::Recovering)
        return;

    if (succeeded)
    {
        state_.store(PluginHealthState::Healthy, std::memory_order_relaxed);
        consecutiveFailures_.store(0, std::memory_order_relaxed);
        recoveryAttempts_.store(0, std::memory_order_relaxed);
    }
    else
    {
        // Recovery failed — go back to Suspended and record the time
        // so shouldRecover() enforces the delay before next attempt.
        state_.store(PluginHealthState::Suspended, std::memory_order_relaxed);
        suspendedAtMs_.store(getCurrentTimeMs(), std::memory_order_relaxed);

        // Check if we've exhausted recovery attempts
        const int attempts = recoveryAttempts_.load(std::memory_order_relaxed);
        const int maxAttempts = getMaxRecoveryAttempts();
        if (attempts >= maxAttempts)
        {
            state_.store(PluginHealthState::Terminated, std::memory_order_relaxed);
        }
    }
}

PluginHealthState PluginHealthMonitor::getState() const noexcept
{
    return state_.load(std::memory_order_relaxed);
}

int PluginHealthMonitor::getConsecutiveFailureCount() const noexcept
{
    return consecutiveFailures_.load(std::memory_order_relaxed);
}

void PluginHealthMonitor::setMaxConsecutiveFailures(int maxFailures) noexcept
{
    maxConsecutiveFailures_ = maxFailures;
}

void PluginHealthMonitor::setRecoveryDelayMs(int delayMs) noexcept
{
    recoveryDelayMs_ = delayMs;
}

void PluginHealthMonitor::setMaxRecoveryAttempts(int maxAttempts) noexcept
{
    maxRecoveryAttempts_ = maxAttempts;
}

void PluginHealthMonitor::reset() noexcept
{
    state_.store(PluginHealthState::Healthy, std::memory_order_relaxed);
    consecutiveFailures_.store(0, std::memory_order_relaxed);
    recoveryAttempts_.store(0, std::memory_order_relaxed);
    suspendedAtMs_.store(0, std::memory_order_relaxed);
}

PluginHealthMonitor::Snapshot PluginHealthMonitor::getSnapshot() const noexcept
{
    return Snapshot{
        state_.load(std::memory_order_relaxed),
        consecutiveFailures_.load(std::memory_order_relaxed),
        recoveryAttempts_.load(std::memory_order_relaxed)
    };
}

int64_t PluginHealthMonitor::getCurrentTimeMs() noexcept
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace more_phi
