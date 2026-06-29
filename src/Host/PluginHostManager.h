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
#include "DAWTransportForwarder.h"
#include "PluginHealthMonitor.h"
#include <atomic>
#include <array>
#include <memory>
#include <vector>
#include <functional>
#include <mutex>

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
     * W-10 FIX (audit): const-qualified so getNumSteps() (a const host-called
     * method) can take a ref-counted lease instead of raw-loading the plugin
     * pointer (which raced unloadPlugin → use-after-free). The methods only
     * touch atomics (refcounting is logically const), mirroring the documented
     * "hostManager is mutable" pattern for const host callbacks.
     */
    juce::AudioPluginInstance* acquirePluginForUse() const noexcept;

    /**
     * Release a previously acquired plugin usage lease.
     */
    void releasePluginFromUse() const noexcept;

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
    
    const juce::PluginDescription* getLastDescription() const override;

    // Parameter metadata (delegates to hosted plugin)
    int getNumSteps(int index) const noexcept override;

    /** Number of processing exceptions since last successful load. */
    int getExceptionCount() const { return healthMonitor_.getConsecutiveFailureCount(); }

    /** Set a callback invoked (async on the message thread) when the plugin is
     *  unloaded, so the UI can close any open hosted-plugin editor window
     *  before the instance is destroyed. The UI-owned HostedPluginWindow holds
     *  a ref-counted lease that guarantees editor-dies-before-instance. */
    void setWindowCloseCallback(std::function<void()> cb) { windowCloseCallback_ = std::move(cb); }

    /** Report the runtime health of the hosted plugin. */
    PluginHealthState getHealthState() const { return healthMonitor_.getState(); }
    PluginHealthMonitor::Snapshot getHealthSnapshot() const { return healthMonitor_.getSnapshot(); }

    juce::AudioPluginFormatManager& getFormatManager() override { return formatManager; }
    juce::KnownPluginList& getKnownPlugins() override
    {
        // CAVEAT: The caller must either be on the message thread (where the
        // scanner is not concurrently mutating knownPlugins) or must hold
        // knownPluginsLock_ themselves. Returning a reference under a
        // short-lived RAII lock is a TOCTOU race — the lock releases before
        // the caller reads the object. The lock here is retained for API
        // compatibility but callers should prefer getKnownPluginsSnapshot().
        return knownPlugins;
    }

    /** Thread-safe snapshot: returns a copy of the KnownPluginList types array.
     *  Safe to call from any thread. Use this instead of getKnownPlugins()
     *  when the caller is not on the message thread or needs a stable snapshot. */
    juce::Array<juce::PluginDescription> getKnownPluginsSnapshot() const
    {
        const juce::SpinLock::ScopedLockType lock(knownPluginsLock_);
        return knownPlugins.getTypes();
    }
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

    /**
     * C1 FIX: Destroy any plugin instances whose teardown was deferred because
     * audio-thread leases were still held when unloadPlugin() timed out.
     * Safe to call repeatedly; no-op when nothing is pending. Call from the
     * message thread (e.g. on the maintenance timer) so destruction can run
     * without blocking real-time audio.
     */
    void drainDeferredDoomedPlugins();

    // Core unload logic without the isSwapping_ guard — called by both
    // the public unloadPlugin() and by loadPlugin() (which already holds
    // the swap flag).
    void unloadPluginInternal();

    // PERF-BROWSER (browser-lag fix, 2026-06-28): persistent KnownPluginList cache.
    // loadKnownPluginsCache() is called from the constructor so the browser can
    // show a previously-discovered plugin list INSTANTLY on GUI open without
    // re-instantiating every installed VST3. saveKnownPluginsCache() is called at
    // the end of scanPluginFolders() after a successful scan. Uses JUCE's built-in
    // KnownPluginList::createXml()/recreateFromXml(). Cache file lives under the
    // user app-data "MorePhi/" folder (same convention as the genre-classifier
    // model path). Stale-cache fallback: if load fails or the file is absent, the
    // browser falls back to the slow background scan as before.
    juce::File getKnownPluginsCacheFile() const;
    void loadKnownPluginsCache();
    void saveKnownPluginsCache() const;

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
    // W-10 FIX (audit): mutable so const host callbacks (getNumSteps,
    // getTailLengthSeconds) can take ref-counted leases via the now-const
    // acquirePluginForUse()/releasePluginFromUse().
    mutable std::atomic<uint32_t> activePluginUsers_{0};
    mutable std::atomic<bool> exclusivePluginUseRequested_{false};
    // L-1 FIX: WaitableEvent signaled by releasePluginFromUse() when the
    // ref-count drops to zero. beginExclusivePluginUse() waits on this instead
    // of Thread::sleep(1) spin — avoids 1 ms worst-case latency spikes and
    // needless wake-ups when the audio thread releases promptly.
    juce::WaitableEvent usersZeroEvent_;

    // C-1 FIX: Guards against concurrent load/unload calls. Set during
    // loadPlugin() and unloadPlugin(), checked by callers to prevent
    // UI-initiated swaps from racing with state restoration.
    std::atomic<bool> isSwapping_{false};
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

    // ------------------------------------------------------------------------
    //  New: Plugin Health Monitor — state-machine driven crash isolation
    // ------------------------------------------------------------------------
    PluginHealthMonitor healthMonitor_;

    // Window-close callback: invoked (async on the message thread) when the
    // plugin is unloaded, so the UI can tear down its editor window before the
    // instance is destroyed. The UI-owned HostedPluginWindow holds a lease that
    // guarantees editor-dies-before-instance; this is the out-of-band teardown
    // hook for plugin swaps not initiated by the editor UI.
    std::function<void()> windowCloseCallback_;

    // ------------------------------------------------------------------------
    //  New: DAW Transport Forwarder — lock-free audio→message thread relay
    // ------------------------------------------------------------------------
    DAWTransportForwarder transportForwarder_;

    // C1 FIX: Plugin instances detached from live use but not yet destroyed
    // because audio-thread leases were held at unload time. Drained from the
    // message thread by drainDeferredDoomedPlugins() once users reach zero.
    // A mutex is acceptable here: only touched on the message thread, and
    // the drain is best-effort (a missed drain just defers to the destructor).
    std::mutex deferredDoomMutex_;
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> deferredDoomedPlugins_;
};

} // namespace more_phi
