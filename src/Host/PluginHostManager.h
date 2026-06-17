/*
 * More-Phi — Host/PluginHostManager.h
 * Manages loading and running a hosted VST3/AU plugin instance.
 * Implements IPluginHostManager for testability.
 *
 * Stability: An exception counter tracks repeated failures from a hosted
 * plugin. When it exceeds MAX_PLUGIN_EXCEPTIONS the plugin is auto-unloaded
 * to prevent a misbehaving guest from continuously disrupting real-time audio.
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "IPluginHostManager.h"
#include <atomic>
#include <array>
#include <memory>
#include <vector>
#include <functional>

namespace more_phi {

class PluginHostManager : public IPluginHostManager
{
public:
    PluginHostManager();
    ~PluginHostManager() override;

    // IPluginHostManager implementation
    void prepare(double sampleRate, int blockSize, int numChannels) override;
    void releaseResources() override;
    bool loadPlugin(const juce::PluginDescription& desc) override;
    void unloadPlugin() override;
    bool hasPlugin() const noexcept override { return hostedPluginPtr_.load(std::memory_order_acquire) != nullptr; }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) noexcept override;

    /**
     * Forward the owning DAW playhead to the hosted plugin.
     * Some hosted processors use this to determine whether transport is
     * running and to populate VST3 process context for analysis features.
     */
    void setPlayHead(juce::AudioPlayHead* playHead) noexcept;
    
    juce::AudioPluginInstance* getPlugin() override { return hostedPluginPtr_.load(std::memory_order_acquire); }
    const juce::AudioPluginInstance* getPlugin() const override { return hostedPluginPtr_.load(std::memory_order_acquire); }

    /**
     * Acquire a stable plugin pointer for short-lived processing work.
     * Must be paired with releasePluginFromUse(). Returns nullptr when no plugin is loaded.
     */
    juce::AudioPluginInstance* acquirePluginForUse() noexcept;

    /**
     * Release a previously acquired plugin usage lease.
     */
    void releasePluginFromUse() noexcept;

    /**
     * Request exclusive non-audio access to the hosted plugin for opaque state
     * capture/restore. While requested, acquirePluginForUse() returns nullptr
     * so the audio thread skips hosted processing instead of blocking.
     */
    juce::AudioPluginInstance* beginExclusivePluginUse(int timeoutMs = 200) noexcept;
    void endExclusivePluginUse() noexcept;
    bool isExclusivePluginUseRequested() const noexcept
    {
        return exclusivePluginUseRequested_.load(std::memory_order_acquire);
    }
    
    /** C3 FIX: Set a callback that will be invoked (async on message thread) when the plugin is unloaded.
     *  Used by the editor to close any open hosted plugin window before the instance is destroyed. */
    void setWindowCloseCallback(std::function<void()> cb) { windowCloseCallback_ = std::move(cb); }
    
    const juce::PluginDescription* getLastDescription() const override;

    // Parameter metadata (delegates to hosted plugin)
    int getNumSteps(int index) const noexcept override;

    /** Number of processing exceptions since last successful load. */
    int getExceptionCount() const { return exceptionCount_.load(std::memory_order_relaxed); }

    juce::AudioPluginFormatManager& getFormatManager() override { return formatManager; }
    juce::KnownPluginList& getKnownPlugins() override { return knownPlugins; }
    void scanPluginFolders() override;

    /** Get the last loaded plugin description — available even after unload for recovery.
     *  Returns a reference to the immutable snapshot published at load time, so it is
     *  safe to read from any thread. If no snapshot exists yet, falls back to the
     *  mutable lastDescription member (only safe when no load is in progress). */
    const juce::PluginDescription& getLastDescriptionRef() const
    {
        if (auto* snap = descriptionSnapshot_.load(std::memory_order_acquire))
            return *snap;
        return lastDescription;
    }

    /**
     * Robust plugin discovery with multi-stage fallback.
     * Handles complex VST3 bundles (e.g. FabFilter Pro-Q 4) that fail basic discovery.
     *
     * Stage 1: Direct file query via findAllTypesForFile() (fast path).
     * Stage 2: PluginDirectoryScanner on the plugin's parent directory.
     *
     * Acquires MessageManagerLock when a MessageManager is available,
     * which is required by many VST3 plugins during scanning.
     *
     * @param formatManager  Registered plugin format manager
     * @param pluginFile     Path to the .vst3 file or bundle
     * @param outDescription Populated on success
     * @param errorDetails   Diagnostic details on failure
     * @param verbose        Print per-stage progress to stderr
     * @return true if a full PluginDescription was obtained
     */
    static bool discoverPlugin(juce::AudioPluginFormatManager& formatManager,
                               const juce::File& pluginFile,
                               juce::PluginDescription& outDescription,
                               juce::String& errorDetails,
                               bool verbose = false);

    /** Check if a plugin swap is currently in progress. */
    bool isPluginSwapping() const noexcept { return isSwapping_.load(std::memory_order_acquire); }

private:
    // Suspend (bypass audio) a misbehaving plugin after this many consecutive
    // exceptions. Raised from 5 to tolerate short DAW reconfiguration bursts.
    static constexpr int MAX_PLUGIN_EXCEPTIONS = 20;

    // Maximum channel count for the pre-allocated wide buffer. Prevents overrun
    // when a hosted plugin reports an unusually high channel count.
    static constexpr int kMaxHostChannels = 16;

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    mutable juce::SpinLock knownPluginsLock_;  // M14 FIX: guards knownPlugins
    std::unique_ptr<juce::AudioPluginInstance> hostedPlugin;
    std::atomic<juce::AudioPluginInstance*> hostedPluginPtr_{nullptr};
    juce::PluginDescription lastDescription;
    mutable juce::SpinLock  descLock_;    // guards lastDescription and descriptionHistory_
    std::vector<std::unique_ptr<juce::PluginDescription>> descriptionHistory_;
    std::atomic<juce::PluginDescription*> descriptionSnapshot_{nullptr};

    // Number of active short-lived plugin users (audio thread processing and
    // parameter-bridge operations). unloadPlugin() waits for this to reach 0
    // after publishing hostedPluginPtr_=nullptr.
    std::atomic<uint32_t> activePluginUsers_{0};
    std::atomic<bool> exclusivePluginUseRequested_{false};

    // C12 FIX: Use unsigned to prevent signed-overflow UB. Cap at MAX+1 to avoid
    // wrap-around which would falsely reset the suspension counter.
    std::atomic<uint32_t> exceptionCount_{0};

    // When true, plugin is suspended (audio bypassed) but NOT destroyed.
    // Recovery is attempted automatically when processBlock succeeds.
    std::atomic<bool> suspended_{false};

    // C-1 FIX: Guards against concurrent load/unload calls. Set during
    // loadPlugin() and unloadPlugin(), checked by callers to prevent
    // UI-initiated swaps from racing with state restoration.
    std::atomic<bool> isSwapping_{false};

    // m-5 FIX: Grace period after recovery — requires this many consecutive
    // successful processBlock calls before fully re-enabling the plugin.
    // Prevents immediate re-suspend from burst exceptions.
    std::atomic<int> recoveryGracePeriod_{0};

    // RT-safe exception tracking — audio thread increments, message thread reads
    std::atomic<int> lastExceptionCode_{0};  // 0=none, 1=std::exception, 2=unknown
    static constexpr int MAX_EXCEPTION_LOG_ENTRIES = 4;
    std::atomic<int> exceptionLogCursor_{0};
    std::array<std::atomic<const char*>, MAX_EXCEPTION_LOG_ENTRIES> exceptionLog_{};
    std::atomic<juce::AudioPlayHead*> playHead_{nullptr};
    juce::AudioPlayHead* lastPlayHeadSent_ = nullptr;  // H9 FIX: cache to avoid per-block setPlayHead

    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;
    int currentNumChannels = 2;
    bool hasPreparedConfiguration_ = false;
    std::atomic<bool> preparing_{false};  // H11 FIX: prevents prepare/processBlock race on wideBuffer_

    // Pre-allocated wide buffer to avoid audio-thread heap allocation when the
    // hosted plugin requires more channels than the incoming buffer provides.
    juce::AudioBuffer<float> wideBuffer_;

    // Smooth gain factor to prevent clicks during preset recalls / bypass switches
    float currentGain_{1.0f};

    // H12 FIX: Deduplicated exception-handling grace-period logic
    bool applyExceptionGracePeriod(juce::AudioBuffer<float>& buffer) noexcept;

    // C3 FIX: Callback invoked when plugin is unloaded so editor can close UI windows
    std::function<void()> windowCloseCallback_;
};

} // namespace more_phi
