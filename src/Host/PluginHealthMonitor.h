/*
 * More-Phi — Host/PluginHealthMonitor.h
 * State-machine driven crash isolation for the hosted plugin.
 * Tracks consecutive failures, transitions through degraded/suspended states,
 * and orchestrates automatic recovery.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <chrono>
#include <juce_core/juce_core.h>

namespace more_phi {

/** Runtime health of a hosted plugin. */
enum class PluginHealthState
{
    Healthy,     /**< Plugin is processing normally. */
    Degraded,    /**< One or more consecutive failures detected. */
    Suspended,   /**< Too many failures — audio is bypassed. */
    Recovering,  /**< Attempting to reinitialize the plugin. */
    Terminated   /**< Recovery failed too many times — plugin is dead. */
};

/**
 * Monitors the health of the hosted plugin and governs its lifecycle.
 *
 * - call reportSuccess() / reportFailure() from the audio thread.
 * - call shouldProcess() to decide whether processBlock runs the plugin.
 * - call shouldRecover() / beginRecovery() / endRecovery() from a timer
 *   or background thread to drive the auto-recovery pipeline.
 */
class PluginHealthMonitor
{
public:
    PluginHealthMonitor();

    /** Call when a processBlock succeeds (audio thread). */
    void reportSuccess() noexcept;
    /** Call when a processBlock or prepareToPlay fails (audio thread). */
    void reportFailure() noexcept;

    /** Is the plugin allowed to participate in processing? */
    bool shouldProcess() const noexcept;
    /** Is it time to attempt a recovery from the Suspended state? */
    bool shouldRecover() const noexcept;
    /** Initiate a recovery attempt (returns false if not in Suspended). */
    bool beginRecovery() noexcept;
    /** Mark the current recovery attempt as finished. true = success. */
    void endRecovery(bool succeeded) noexcept;

    /** Current health state (message thread use). */
    PluginHealthState getState() const noexcept;

    /** How many consecutive failures have occurred since last success? */
    int getConsecutiveFailureCount() const noexcept;

    /** Configuration: max failures before suspension (default 20, matching existing). */
    void setMaxConsecutiveFailures(int maxFailures) noexcept;
    /** Configuration: ms to wait in Suspended before recovery (default 5000). */
    void setRecoveryDelayMs(int delayMs) noexcept;
    /** Configuration: max recovery attempts before Terminated (default 3). */
    void setMaxRecoveryAttempts(int maxAttempts) noexcept;

    /** Reset the monitor to a pristine Healthy state. Call on successful plugin load. */
    void reset() noexcept;

    struct Snapshot
    {
        PluginHealthState state;
        int consecutiveFailures;
        int recoveryAttempts;
    };
    /** Thread-safe snapshot of current state. */
    Snapshot getSnapshot() const noexcept;

private:
    std::atomic<PluginHealthState> state_{PluginHealthState::Healthy};
    std::atomic<int> consecutiveFailures_{0};
    std::atomic<int> recoveryAttempts_{0};

    int maxConsecutiveFailures_ = 20;
    int recoveryDelayMs_ = 5000;
    int maxRecoveryAttempts_ = 3;

    // Timestamp (milliseconds) when we entered the Suspended state.
    // Used by shouldRecover() to enforce the recovery delay.
    std::atomic<int64_t> suspendedAtMs_{0};

    // Cached clock for message-thread checks.
    static int64_t getCurrentTimeMs() noexcept;

    int getMaxConsecutiveFailures() const noexcept { return maxConsecutiveFailures_; }
    int getRecoveryDelayMs() const noexcept { return recoveryDelayMs_; }
    int getMaxRecoveryAttempts() const noexcept { return maxRecoveryAttempts_; }
};

} // namespace more_phi
