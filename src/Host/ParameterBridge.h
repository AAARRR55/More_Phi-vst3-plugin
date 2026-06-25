/*
 * More-Phi — Host/ParameterBridge.h
 * Read/write hosted plugin parameters by normalized index.
 * Implements IParameterBridge for testability.
 *
 * PERF-H2: cachedConcreteHost_ eliminates per-call dynamic_cast.
 * ATS-H1: Throttle state uses tryEnter instead of blocking SpinLock.
 * PERF-M2: Single lock acquisition in batch applyParameterState.
 */
#pragma once

#include "IPluginHostManager.h"
#include "Core/ParameterState.h"   // MAX_PARAMETERS for the recall-ramp buffers
#include <atomic>
#include <limits>
#include <vector>
#include <utility>
#include <cstdint>
#include <juce_core/juce_core.h>

namespace more_phi {

class PluginHostManager;

// AUDIT (E2, 2026-06-25): forward-declared so ParameterBridge can stamp the
// source of each hosted-parameter write without depending on PluginProcessor.h
// (which would create a circular include — PluginProcessor owns ParameterBridge).
// The drain loop (PluginProcessor.cpp) calls noteWriteSource() with the
// ParameterEditSource carried by each ParamCommand.
enum class HostedWriteSource : uint8_t
{
    Unknown = 0,
    UI,
    Assistant,
    MCP,        // manual hosted edit (set_parameter / hosted_plugin.set_parameter)
    Snapshot,
    Neural      // automated SonicMaster plan write (OzonePlanApplicator)
};

class ParameterBridge : public IParameterBridge
{
public:
    struct ParameterDescriptor
    {
        int index = -1;
        juce::String stableId;
        juce::String name;
        float value = 0.0f;
        juce::String displayValue;
        juce::String label;
        bool discrete = false;
        bool boolean = false;
        int numSteps = 0;
        float defaultValue = 0.0f;
    };

    struct ParameterResolution
    {
        int index = -1;
        bool success = false;
        bool ambiguous = false;
        juce::String error;
    };

    explicit ParameterBridge(IPluginHostManager& host);

    int getParameterCount() const noexcept override;
    float getParameterNormalized(int index) const noexcept override;
    float getParameterNormalized(juce::AudioPluginInstance& plugin, int index) const noexcept;
    void setParameterNormalized(int index, float value) noexcept override;
    void setParameterNormalized(juce::AudioPluginInstance& plugin, int index, float value) noexcept;

    // AUDIT-FIX (Fix 5): setParameterNormalized variant that SNAPS the value to the
    // nearest valid step for discrete/boolean parameters before writing. The raw
    // setValue path writes a continuous float regardless of isDiscrete/isBoolean,
    // relying on the hosted plugin to self-snap — which is unreliable and undetectable
    // on readback (the autonomous SonicMaster path has no round-trip check). This
    // helper computes round(value*(numSteps-1))/(numSteps-1) for discrete params and
    // 0/1 for booleans, then delegates to setParameterNormalized for the existing
    // clamp + throttle + setValue + exception-accounting. Continuous params pass
    // through unchanged. No-op if index is out of range.
    void setParameterNormalizedSnapped(int index, float value) noexcept;
    // Pure helper (no write): returns the value that setParameterNormalizedSnapped
    // would write for the given index. Exposed for the OzonePlanApplicator to
    // enqueue the already-snapped normalized value, and for readback verification.
    float snapNormalizedToStep(int index, float value) const noexcept;
    juce::String getParameterName(int index) const override;

    void applyParameterState(const float* values, int count) noexcept override;
    void applyParameterState(const std::vector<float>& values) noexcept override;

    // C-5 FIX (audit): Ramped recall. Instead of snapping every hosted
    // parameter to the snapshot value in one block (an audible click on
    // continuous params like gain/cutoff), startRecallRamp() captures the
    // plugin's CURRENT values as the ramp start and stashes the target. The
    // audio-thread per-block loop then calls processRecallRamp(dt), which
    // advances a linear ramp toward the target over ~kRecallRampSeconds.
    // RT-safe: buffers are fixed std::array (no allocation), no locks beyond
    // the existing throttle try-lock. Returns false if no hosted plugin is
    // available (caller should fall back to applyParameterState).
    bool startRecallRamp(const float* targetValues, int count) noexcept;
    // Advances the active recall ramp by one block. No-op if none active.
    // dtSeconds is the block duration; the ramp resolves in a fixed number of
    // blocks regardless of block size by counting blocks, not seconds, so the
    // feel is consistent (kRecallRampBlocks).
    void processRecallRamp() noexcept;
    [[nodiscard]] bool isRecallRampActive() const noexcept
    {
        return recallRampActive_.load(std::memory_order_acquire);
    }
    // Public read-only access to the ramp length (blocks) for diagnostics/tests.
    static constexpr int recallRampBlocks() noexcept { return kRecallRampBlocks; }

    std::vector<float> captureParameterState() const noexcept override;
    void captureAllNormalized(float* outValues, int count) const noexcept override;
    void captureAllNames(juce::StringArray& outNames, int count) const override;

    bool isDiscrete(int index) const noexcept override;
    std::vector<bool> getDiscreteMap() const noexcept override;

    juce::String getParameterLabel(int index) const override;
    juce::String getParameterDisplayValue(int index) const override;
    juce::String getParameterDisplayValueAtNormalized(int index, float normalizedValue) const;
    float getParameterDefault(int index) const noexcept override;
    juce::StringArray getParameterValueStrings(int index) const override;
    juce::String getParameterStableID(int index) const override;
    int getParameterNumSteps(int index) const noexcept override;
    bool isBoolean(int index) const noexcept;
    ParameterDescriptor getParameterDescriptor(int index) const;
    std::vector<ParameterDescriptor> getParameterDescriptors() const;

#if MORE_PHI_TEST_MODE
    void setParameterDescriptorsForTesting(std::vector<ParameterDescriptor> descriptors)
    {
        testDescriptors_ = std::move(descriptors);
    }

    void clearParameterDescriptorsForTesting()
    {
        testDescriptors_.clear();
    }
#endif

    int findParameterIndex(const juce::String& name) const;
    ParameterResolution resolveParameter(const juce::String& stableId,
                                         int index,
                                         const juce::String& name) const;

    // B4 FIX: number of times a hosted-plugin setValue() threw during apply.
    // Apply is on the audio thread (silent catch), so without this counter a
    // hosted plugin that throws on every write would no-op forever with no
    // diagnostic. Saturating to avoid wraparound. Safe to read from any thread.
    uint64_t getApplyExceptionCount() const noexcept
    {
        return applyExceptionCount_.load(std::memory_order_relaxed);
    }
    void resetApplyExceptionCount() noexcept
    {
        applyExceptionCount_.store(0, std::memory_order_relaxed);
    }

    // AUDIT (E2, 2026-06-25): per-parameter write-source stamp + write-precedence
    // conflict counter. noteWriteSource() is called from the audio-thread drain
    // for every hosted write, carrying the ParamCommand's source. If a write
    // from a DIFFERENT source arrives within kWritePrecedenceSettleMs of the
    // previous write to the same index, writePrecedenceConflictCount_ is
    // incremented (audio-safe saturating counter). Read getWritePrecedenceConflicts()
    // from the message thread for diagnostics / ActionLedger recording.
    //
    // Rationale: the audit's original E2 premise ("two uncoordinated hosted
    // writers") is incorrect — both hosted writers (Neural + MCP) share one FIFO
    // command queue, so they are already serialized. This stamp does NOT
    // arbitrate; it makes a same-parameter, different-source edit burst OBSERVABLE
    // for debugging (e.g. a user manually tweaking a control the neural engine is
    // also driving). Audio-safe: fixed std::array + relaxed atomics, no locks.
    static constexpr int kWritePrecedenceSettleMs = 250;
    void noteWriteSource(int index, HostedWriteSource source) noexcept;
    uint64_t getWritePrecedenceConflicts() const noexcept
    {
        return writePrecedenceConflictCount_.load(std::memory_order_relaxed);
    }
    void resetWritePrecedenceConflicts() noexcept
    {
        writePrecedenceConflictCount_.store(0, std::memory_order_relaxed);
    }
    HostedWriteSource getLastWriteSource(int index) const noexcept;

private:
    struct ThrottleState
    {
        float lastValue = -1.0f;
        juce::uint32 lastUpdateTime = 0;
    };

    IPluginHostManager& host_;
    PluginHostManager* const cachedConcreteHost_;

    mutable juce::SpinLock throttleMutex_;
    mutable std::vector<ThrottleState> throttleStates_;
    std::vector<ParameterDescriptor> testDescriptors_;

    // AUDIT (E2): per-parameter last-writer stamp. Fixed array (no allocation on
    // the audio path), one atomic per slot — mirrors the recallRamp* pattern.
    // lastWriteSource_ holds the source of the most recent write; lastWriteMs_
    // holds its millisecond timestamp. Relaxed ordering is sufficient: the only
    // cross-thread invariant is "observe who wrote last", no dependent data.
    std::array<std::atomic<HostedWriteSource>, MAX_PARAMETERS> lastWriteSource_{};
    std::array<std::atomic<juce::uint32>, MAX_PARAMETERS>      lastWriteMs_{};
    std::atomic<uint64_t> writePrecedenceConflictCount_{0};

    // B4 FIX: saturating counter for hosted-plugin setValue() exceptions on the
    // apply path (audio thread can't log). Stops at uint64 max to avoid wrap.
    mutable std::atomic<uint64_t> applyExceptionCount_{0};

public:
    // C-5 FIX (audit): public so unit tests can pin the ramp length. This is a
    // compile-time tuning constant (no encapsulated state); the ramp *state*
    // (recallRampActive_/Count_/Step_) stays private below.
    static constexpr int kRecallRampBlocks = 8; // ~8 blocks ≈ 8..170 ms by block size

private:
    // C-5 FIX (audit): Recall ramp state. Fixed arrays (no allocation on the
    // audio thread). recallRampActive_ uses acq_rel so the message-thread
    // startRecallRamp() and the audio-thread processRecallRamp() agree on the
    // populated buffers. Only ONE ramp may be active at a time; starting a new
    // ramp while one is active replaces it (the target updates, current values
    // are re-captured as the new start).
    std::atomic<bool> recallRampActive_ { false };
    std::atomic<int>  recallRampCount_  { 0 };     // populated params in the ramp
    std::atomic<int>  recallRampStep_   { 0 };     // current block index [0..kRecallRampBlocks]
    // start/target are written by the message thread (startRecallRamp) under
    // the active-flag handoff and read by the audio thread (processRecallRamp).
    // They are not mutated mid-ramp except by a new startRecallRamp, which
    // itself happens-before publishing active_=true via release.
    std::array<float, MAX_PARAMETERS> recallRampStart_  {};
    std::array<float, MAX_PARAMETERS> recallRampTarget_ {};

    bool shouldThrottle(int index, float newValue, juce::uint32 now) const;
    void updateThrottleState(int index, float value, juce::uint32 now);

    // B4 FIX: saturating increment of applyExceptionCount_ (audio-safe).
    void bumpApplyException() const noexcept
    {
        uint64_t cur = applyExceptionCount_.load(std::memory_order_relaxed);
        if (cur != std::numeric_limits<uint64_t>::max())
            applyExceptionCount_.fetch_add(1, std::memory_order_relaxed);
    }

    template<typename Ret, typename Fn>
    static Ret withPlugin(IPluginHostManager& host, PluginHostManager* cachedHost,
                          const char* context, Ret defaultValue, Fn&& fn);
};

} // namespace more_phi
