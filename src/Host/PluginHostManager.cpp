/*
 * More-Phi — Host/PluginHostManager.cpp
 * Robust plugin hosting with exception handling for stability.
 *
 * Stability: exceptionCount_ tracks consecutive processBlock failures.
 * When it reaches MAX_PLUGIN_EXCEPTIONS the plugin is auto-unloaded to
 * prevent a misbehaving guest from continuously disrupting real-time audio.
 */
#include "PluginHostManager.h"
#include <exception>

#if defined(_MSC_VER)
// Helper to call plugin processBlock with SEH guard (hardware exception safety).
// Must be a separate function from the caller because MSVC prohibits mixing SEH
// (__try/__except) with C++ EH (try/catch) or RAII destructors in the same function.
// Returns true if processBlock completed normally, false on hardware exception.
#include <windows.h>
static inline bool safePluginProcessBlock(juce::AudioPluginInstance* plugin,
                                           juce::AudioBuffer<float>& buffer,
                                           juce::MidiBuffer& midi) noexcept
{
    __try
    {
        plugin->processBlock(buffer, midi);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
#endif

namespace more_phi {

PluginHostManager::PluginHostManager()
{
    formatManager.addDefaultFormats();  // VST3 + AU
}

PluginHostManager::~PluginHostManager()
{
    unloadPlugin();
    // C1 FIX: final drain — anything still deferred (audio leases held at the
    // very end) is destroyed now. By this point the AudioProcessor is gone and
    // no processBlock caller can exist.
    drainDeferredDoomedPlugins();
}

void PluginHostManager::drainDeferredDoomedPlugins()
{
    // Message-thread only. Destroy any plugin whose teardown was deferred
    // because audio-thread leases were still held when unloadPlugin() timed
    // out. If a lease is STILL held (host misbehaving), leave it for next
    // drain or the destructor rather than creating a use-after-free.
    if (activePluginUsers_.load(std::memory_order_acquire) > 0)
        return;

    std::vector<std::unique_ptr<juce::AudioPluginInstance>> toDestroy;
    {
        std::lock_guard<std::mutex> guard(deferredDoomMutex_);
        toDestroy.swap(deferredDoomedPlugins_);
    }
    // Destroy outside the lock — releaseResources() may be slow.
    for (auto& doomed : toDestroy)
    {
        if (doomed)
        {
            try { doomed->releaseResources(); }
            catch (...) { /* silent during teardown */ }
            doomed.reset();
        }
    }
}

juce::AudioPluginInstance* PluginHostManager::acquirePluginForUse() const noexcept
{
    if (exclusivePluginUseRequested_.load(std::memory_order_acquire))
        return nullptr;

    activePluginUsers_.fetch_add(1, std::memory_order_acq_rel);

    if (exclusivePluginUseRequested_.load(std::memory_order_acquire))
    {
        activePluginUsers_.fetch_sub(1, std::memory_order_acq_rel);
        return nullptr;
    }

    auto* plugin = hostedPluginPtr_.load(std::memory_order_acquire);
    if (!plugin)
    {
        activePluginUsers_.fetch_sub(1, std::memory_order_acq_rel);
        return nullptr;
    }
    return plugin;
}

void PluginHostManager::releasePluginFromUse() const noexcept
{
    // A1a FIX: underflow guard. A double-release (or a release without a
    // matching acquire) used to drive activePluginUsers_ negative, after which
    // unloadPlugin()'s `while (activePluginUsers_ > 0)` spin would never exit —
    // freezing teardown. Decrement only while the count is strictly positive.
    // No current caller double-releases; this is defensive against future
    // imbalance and converts a silent deadlock into a one-off leaked lease
    // (unloadPlugin's bounded wait + deferredDoomedPlugins_ queue handles it).
    // ponytail: CAS loop, O(1), no allocation.
    uint32_t expected = activePluginUsers_.load(std::memory_order_acquire);
    while (expected > 0)
    {
        if (activePluginUsers_.compare_exchange_weak(expected, expected - 1,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            // L-1 FIX: signal the event when the last user releases, so
            // beginExclusivePluginUse can wake immediately instead of polling.
            if (expected == 1)  // was 1, now 0
                usersZeroEvent_.signal();
            return;
        }
        // expected refreshed by compare_exchange_weak on failure; loop.
    }
    // expected == 0 : underflow — nothing to release. Intentionally a no-op.
}

juce::AudioPluginInstance* PluginHostManager::beginExclusivePluginUse(int timeoutMs) noexcept
{
    bool expected = false;
    if (!exclusivePluginUseRequested_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return nullptr;

    const auto start = juce::Time::getMillisecondCounter();
    while (activePluginUsers_.load(std::memory_order_acquire) > 0)
    {
        const int elapsed = static_cast<int>(juce::Time::getMillisecondCounter() - start);
        if (timeoutMs >= 0 && elapsed >= timeoutMs)
        {
            exclusivePluginUseRequested_.store(false, std::memory_order_release);
            return nullptr;
        }

        // L-1 FIX: wait on the event instead of Thread::sleep(1). The event
        // is signaled by releasePluginFromUse() when the count hits zero, so
        // this wake-ups only when there's actually a state change, not every
        // 1 ms. Remaining wait time is used as the timeout so we don't
        // overshoot the caller's deadline.
        const int waitRemaining = (timeoutMs >= 0) ? (timeoutMs - elapsed) : 100;
        usersZeroEvent_.wait(std::max(waitRemaining, 1));
    }

    auto* plugin = hostedPluginPtr_.load(std::memory_order_acquire);
    if (plugin == nullptr)
        exclusivePluginUseRequested_.store(false, std::memory_order_release);

    return plugin;
}

void PluginHostManager::endExclusivePluginUse() noexcept
{
    exclusivePluginUseRequested_.store(false, std::memory_order_release);
}

void PluginHostManager::setPlayHead(juce::AudioPlayHead* playHead) noexcept
{
    const auto previousPlayHead = playHead_.exchange(playHead, std::memory_order_acq_rel);
    if (previousPlayHead == playHead)
        return;

    if (auto* plugin = acquirePluginForUse())
    {
        try {
            plugin->setPlayHead(playHead);
        } catch (...) {
            // Hosted plugin playhead update must not escape this noexcept boundary.
        }
        releasePluginFromUse();
    }
}

void PluginHostManager::prepare(double sampleRate, int blockSize, int numChannels)
{
    const bool configurationChanged = !hasPreparedConfiguration_
        || currentSampleRate != sampleRate
        || currentBlockSize != blockSize
        || currentNumChannels != numChannels;

    currentSampleRate = sampleRate;
    currentBlockSize = blockSize;
    currentNumChannels = numChannels;

    // H11 FIX: Set preparing_ while resizing wideBuffer_ to prevent audio-thread race
    preparing_.store(true, std::memory_order_release);

    // kMaxHostChannels is declared in the class header (PluginHostManager.h).
    // Pre-allocate worst-case wide buffer to avoid audio-thread heap allocation
    // when hosted plugin requires more channels than the incoming buffer.
    wideBuffer_.setSize(kMaxHostChannels, currentBlockSize, false, true, true);

    preparing_.store(false, std::memory_order_release);

    if (hostedPlugin && configurationChanged)
    {
        try
        {
            hostedPlugin->setPlayHead(playHead_.load(std::memory_order_acquire));
            hostedPlugin->enableAllBuses();
            hostedPlugin->prepareToPlay(sampleRate, blockSize);
            hasPreparedConfiguration_ = true;  // M4 FIX: Only set after prepareToPlay succeeds
        }
        catch (const std::exception& e)
        {
            juce::ignoreUnused(e);
            // Keep plugin loaded, will attempt recovery on next process
        }
        catch (...)
        {
            // Silent — cannot log on audio path
        }
    }
}

void PluginHostManager::releaseResources()
{
    if (hostedPlugin)
    {
        try
        {
            hostedPlugin->releaseResources();
        }
        catch (...)
        {
            // Silently handle - plugin may be in bad state
        }
    }
}

bool PluginHostManager::loadPlugin(const juce::PluginDescription& desc)
{
    // C-1 FIX: Prevent concurrent load/unload calls from racing.
    bool expected = false;
    if (!isSwapping_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        DBG("loadPlugin: rejected — swap already in progress");
        return false;
    }

    // RAII guard to clear isSwapping_ on all exit paths
    struct SwapGuard {
        std::atomic<bool>& flag;
        ~SwapGuard() { flag.store(false, std::memory_order_release); }
    } swapGuard{ isSwapping_ };

    // M-5 FIX: Some VST3 plugins require the MessageManagerLock during
    // creation (especially when hosted from a plugin context like FL Studio).
    // Acquire it if a MessageManager is available.
    std::unique_ptr<juce::MessageManagerLock> mmLock;
    if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
    {
        mmLock = std::make_unique<juce::MessageManagerLock>(
            juce::Thread::getCurrentThread());
        if (!mmLock->lockWasGained())
        {
            DBG("loadPlugin: Failed to acquire MessageManagerLock");
            mmLock.reset();
            // Continue anyway — many plugins don't need it
        }
    }

    // Create the new instance BEFORE destroying the old one.
    // If creation fails, the current plugin remains untouched.
    juce::String errorMessage;
    auto newPlugin = formatManager.createPluginInstance(
        desc, currentSampleRate, currentBlockSize, errorMessage);

    // Release the lock before initialization (init doesn't need it)
    mmLock.reset();

    if (!newPlugin)
    {
        DBG("Failed to load plugin: " + errorMessage);
        // B1 FIX: replaced std::cerr with DBG (AGENTS.md: DBG is the only
        // approved logging macro). This is the message-thread load path.
        DBG("[PluginHostManager] createPluginInstance failed: " + errorMessage);
        // IMPORTANT: do NOT unload the current plugin — keep it running
        return false;
    }

    try
    {
        // Enable all buses first so the plugin can configure its full I/O layout,
        // then prepare. For bridged plugins (yabridge), disabling buses can cause
        // mismatches between the host-side channel expectations and the Wine-side
        // shared memory layout, leading to memcpy crashes during processBlock.
        newPlugin->setPlayHead(playHead_.load(std::memory_order_acquire));
        newPlugin->enableAllBuses();
        newPlugin->prepareToPlay(currentSampleRate, currentBlockSize);
    }
    catch (const std::exception& e)
    {
        juce::ignoreUnused(e);
        DBG("Exception during plugin initialization: " + juce::String(e.what()));
        // newPlugin goes out of scope and is destroyed; old plugin stays.
        return false;
    }
    catch (...)
    {
        DBG("Unknown exception during plugin initialization");
        return false;
    }

    // New plugin is valid — now swap it in.
    unloadPlugin();
    hostedPlugin = std::move(newPlugin);
    hostedPluginPtr_.store(hostedPlugin.get(), std::memory_order_release);

    {
        const juce::SpinLock::ScopedLockType guard(descLock_);
        lastDescription = desc;  // protected write (retained for legacy callers)

        // Publish an immutable snapshot so getLastDescription() can return a stable
        // pointer without exposing lock-protected data to a TOCTOU race.
        auto snapshot = std::make_unique<juce::PluginDescription>(desc);
        auto* raw = snapshot.get();
        descriptionHistory_.push_back(std::move(snapshot));
        // AUDIT-FIX: Cap history growth. Repeated plugin swaps (e.g. automated
        // testing, long sessions) otherwise accumulated PluginDescription objects
        // indefinitely — a slow leak. Keep only the most recent entries.
        constexpr size_t kMaxDescriptionHistory = 16;
        while (descriptionHistory_.size() > kMaxDescriptionHistory)
            descriptionHistory_.erase(descriptionHistory_.begin());
        descriptionSnapshot_.store(raw, std::memory_order_release);
    }

    hasPreparedConfiguration_ = true;
    exceptionCount_.store(0, std::memory_order_relaxed);
    suspended_.store(false, std::memory_order_relaxed);
    recoveryGracePeriod_.store(0, std::memory_order_relaxed);
    return true;
}

void PluginHostManager::unloadPlugin()
{
    // H2 FIX: Guard unloadPlugin with the same isSwapping_ CAS to prevent racing
    // with loadPlugin. Without this, a concurrent unload can reset the unique_ptr
    // after loadPlugin has moved newPlugin into it but before it publishes the
    // new hostedPluginPtr_, causing a dangling pointer publication.
    bool expected = false;
    if (!isSwapping_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        DBG("unloadPlugin: rejected — swap already in progress");
        return;
    }
    struct SwapGuard {
        std::atomic<bool>& flag;
        ~SwapGuard() { flag.store(false, std::memory_order_release); }
    } swapGuard{ isSwapping_ };

    // DAW-M2: Execute directly without message-thread dispatch.
    // The callFunctionOnMessageThread dispatch was removed to prevent
    // re-entrancy and lifetime issues when called from destructors or
    // state-restore paths.
    if (hostedPlugin)
    {
        // H-3 FIX: Bounded wait for an in-flight exclusive state capture/restore.
        // The previous unbounded `while (...) yield()` could hang the destructor
        // forever if a beginExclusivePluginUse() caller faulted without calling
        // endExclusivePluginUse() — which hangs the DAW on track removal. After a
        // 200 ms grace window (exclusive ops are sub-millisecond in practice),
        // force-release the flag and proceed. NOTE: the activePluginUsers_ wait
        // below is intentionally still unbounded — proceeding past a live audio
        // lease would be a use-after-free, so we must wait for audio to drain.
        for (int i = 0; i < 200 && exclusivePluginUseRequested_.load(std::memory_order_acquire); ++i)
            juce::Thread::sleep(1);
        exclusivePluginUseRequested_.store(false, std::memory_order_release);

        hostedPluginPtr_.store(nullptr, std::memory_order_release);

        // Notify any open editor window to close before destroying the plugin.
        // BP-4 FIX (audit): callAsync() can silently drop in headless hosts or
        // when no editor is open, orphaning a hosted plugin's window. Invoke
        // directly when we're already on the message thread (the common case —
        // unloadPlugin runs here), and only fall back to callAsync for the rare
        // non-message-thread caller.
        if (windowCloseCallback_)
        {
            auto* mm = juce::MessageManager::getInstanceWithoutCreating();
            if (mm != nullptr && mm->isThisTheMessageThread())
                windowCloseCallback_();
            else
                juce::MessageManager::callAsync([cb = windowCloseCallback_]() { cb(); });
        }

        // FIX C2/C1: Bounded wait for active audio-thread leases. An unbounded
        // spin here can hang the DAW forever if a lease holder is stalled. After
        // a 500 ms grace window we detach the plugin from live use
        // (hostedPluginPtr_ is already nulled above, so new acquirePluginForUse()
        // calls return nullptr) but we do NOT destroy it while a lease is held —
        // that would be a use-after-free. Instead we move it to the deferred-doom
        // queue, which the message-thread maintenance timer drains once
        // activePluginUsers_ returns to zero (see drainDeferredDoomedPlugins).
        const auto waitStart = juce::Time::getMillisecondCounter();
        const bool leasesDrained = [&, this]() {
            while (activePluginUsers_.load(std::memory_order_acquire) > 0)
            {
                if (static_cast<int>(juce::Time::getMillisecondCounter() - waitStart) > 500)
                {
                    DBG("PluginHostManager::unloadPlugin — timeout waiting for audio thread lease; deferring destruction");
                    return false;
                }
                juce::Thread::sleep(1);
            }
            return true;
        }();

        if (leasesDrained)
        {
            try
            {
                hostedPlugin->releaseResources();
            }
            catch (...)
            {
                // Silently handle during unload
            }
            hostedPlugin.reset();
        }
        else
        {
            // C1 FIX: leases still held. Move the instance to the deferred-doom
            // queue. It stays alive until all in-flight audio leases drop, then
            // the maintenance timer (or destructor) destroys it. No UAF.
            std::unique_ptr<juce::AudioPluginInstance> doomed = std::move(hostedPlugin);
            // hostedPlugin is now nullptr. Queue for later teardown.
            {
                std::lock_guard<std::mutex> guard(deferredDoomMutex_);
                deferredDoomedPlugins_.push_back(std::move(doomed));
            }
        }
    }

    hasPreparedConfiguration_ = false;
}

void PluginHostManager::processBlock(juce::AudioBuffer<float>& buffer,
                                      juce::MidiBuffer& midi) noexcept
{
    auto* plugin = acquirePluginForUse();
    if (!plugin)
    {
        // Distinguish two cases where acquirePluginForUse() returns nullptr:
        //
        //  (1) No plugin is loaded at all (hostedPluginPtr_ == nullptr).
        //      → Pass the dry signal through unchanged. This preserves the
        //        original passthrough behavior so the analysis tap and output
        //        chain still see real audio.
        //
        //  (2) A plugin IS loaded but exclusive access is requested
        //      (exclusivePluginUseRequested_ == true) — i.e. an opaque state
        //      chunk recall is in progress on the message thread.
        //      → Fade the buffer to silence over this block to prevent an
        //        audible click/pop at the seam, then keep it silent until the
        //        plugin becomes available again (fade-in resumes on re-acquire).
        if (hostedPluginPtr_.load(std::memory_order_acquire) == nullptr)
            return;  // (1) dry passthrough

        if (currentGain_ > 0.0f)   // (2) fade-out seam
        {
            buffer.applyGainRamp(0, buffer.getNumSamples(), currentGain_, 0.0f);
            currentGain_ = 0.0f;
        }
        else
        {
            buffer.clear();
        }
        return;
    }

    struct ScopedPluginUse
    {
        explicit ScopedPluginUse(PluginHostManager& owner) : owner_(owner) {}
        ~ScopedPluginUse() { owner_.releasePluginFromUse(); }
        PluginHostManager& owner_;
    } scopedUse(*this);

    // H9 FIX: Cache last playhead pointer to avoid calling setPlayHead every block.
    // Some VST3 plugins acquire internal locks or post messages on this call,
    // causing priority inversion when called from the real-time thread.
    juce::AudioPlayHead* currentPlayHead = playHead_.load(std::memory_order_acquire);
    if (currentPlayHead != lastPlayHeadSent_)
    {
        lastPlayHeadSent_ = currentPlayHead;
        try {
            plugin->setPlayHead(currentPlayHead);
        } catch (...) {
            // Cannot let hosted plugin exceptions escape the noexcept audio path.
        }
    }

    // If suspended, pass-through silence — do NOT unload.
    if (suspended_.load(std::memory_order_relaxed))
    {
        buffer.clear();  // FIX C22: ensure silence, not stale pass-through audio
        // Periodically attempt recovery (every ~100 blocks ≈ once per second)
        const int count = exceptionCount_.load(std::memory_order_relaxed);
        if (count > 0 && (count % 100) == 0)
        {
            try
            {
                plugin->processBlock(buffer, midi);
                // If we get here, the plugin recovered!
                // m-5 FIX: Set a grace period before fully re-enabling.
                // This prevents immediate re-suspend from a burst of exceptions.
                suspended_.store(false, std::memory_order_relaxed);
                exceptionCount_.store(0, std::memory_order_relaxed);
                recoveryGracePeriod_.store(10, std::memory_order_relaxed);
                return;
            }
            catch (...)
            {
                // C12 FIX: Saturated unsigned increment in suspended recovery path.
            const uint32_t c = exceptionCount_.load(std::memory_order_relaxed);
            if (c < static_cast<uint32_t>(MAX_PLUGIN_EXCEPTIONS) + 1)
                exceptionCount_.fetch_add(1, std::memory_order_relaxed);
            // AUDIT-FIX: recovery attempt failed — count this failure and return.
            // Previously fell through to the unconditional increment below,
            // double-counting every failed recovery probe (every 100th block).
            return;
            }
        }
        // C12 FIX: Saturated unsigned increment in suspended path.
        const uint32_t c = exceptionCount_.load(std::memory_order_relaxed);
        if (c < static_cast<uint32_t>(MAX_PLUGIN_EXCEPTIONS) + 1)
            exceptionCount_.fetch_add(1, std::memory_order_relaxed);
        return;  // Output silence (buffer unchanged = pass-through)
    }

    // Match channel count: the plugin may need more channels than the caller
    // provides (e.g. FabFilter Pro-Q 4 has a sidechain bus requiring 4 input
    // channels even though only 2 carry audio). Pad to the maximum of input/
    // output channel counts so yabridge's shared-memory IPC doesn't overrun.
    const int pluginInputChannels = plugin->getTotalNumInputChannels();
    const int pluginOutputChannels = plugin->getTotalNumOutputChannels();
    // DAW-M3: Clamp channel count to kMaxHostChannels to prevent wide buffer overrun
    const int requiredChannels = juce::jmin(juce::jmax(pluginInputChannels, pluginOutputChannels),
                                            kMaxHostChannels);

    if (buffer.getNumChannels() < requiredChannels)
    {
        // H11 FIX: Skip wide path if prepare() is concurrently resizing wideBuffer_
        if (preparing_.load(std::memory_order_acquire))
            return; // dry passthrough while prepare() is in progress

        // C11 FIX: Clamp every wide-buffer operation to the actual allocation.
        // A misbehaving host may call processBlock with more samples than prepare()
        // advertised (or before prepare() at all). Without this clamp, clear/copy
        // and the temporary subBuffer would reference memory past wideBuffer_.
        const int safeSamples = juce::jmin(buffer.getNumSamples(), wideBuffer_.getNumSamples());
        jassert(buffer.getNumSamples() <= currentBlockSize);

        // Use the pre-allocated wide buffer — zero heap allocation on the audio thread.
        // Only clear the channels we will actually use; the rest remain from last block
        // but will not be read back (we only copy buffer.getNumChannels() back out).
        for (int ch = 0; ch < requiredChannels; ++ch)
            wideBuffer_.clear(ch, 0, safeSamples);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            wideBuffer_.copyFrom(ch, 0, buffer, ch, 0, safeSamples);

        juce::AudioBuffer<float> subBuffer(wideBuffer_.getArrayOfWritePointers(),
                                           requiredChannels, safeSamples);

        // H-4: Wrap hosted plugin processBlock in SEH/C++ handler to survive
        // hardware exceptions (access violations, stack overflows) from
        // buggy third-party plugins.
#if defined(_MSC_VER)
        if (!safePluginProcessBlock(plugin, subBuffer, midi))
        {
            currentGain_ = 0.0f;
            applyExceptionGracePeriod(buffer);
            return;
        }
#else
        try
        {
            plugin->processBlock(subBuffer, midi);
        }
        catch (...)
        {
            currentGain_ = 0.0f;
            applyExceptionGracePeriod(buffer);
            return;
        }
#endif
        {
            exceptionCount_.store(0, std::memory_order_relaxed);
            // m-5 FIX: Decrement grace period
            int grace = recoveryGracePeriod_.load(std::memory_order_relaxed);
            if (grace > 0)
                recoveryGracePeriod_.store(grace - 1, std::memory_order_relaxed);
        }

        // Copy processed audio back to the caller's buffer (only up to safeSamples)
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.copyFrom(ch, 0, wideBuffer_, ch, 0, safeSamples);

        // Smoothly fade in after recall / bypass switch
        if (currentGain_ < 1.0f)
        {
            buffer.applyGainRamp(0, buffer.getNumSamples(), currentGain_, 1.0f);
            currentGain_ = 1.0f;
        }
        return;
    }

    if (buffer.getNumChannels() > requiredChannels)
    {
        for (int ch = requiredChannels; ch < buffer.getNumChannels(); ++ch)
            buffer.clear(ch, 0, buffer.getNumSamples());
    }

    // H-4: Wrap hosted plugin processBlock in SEH/C++ handler to survive
    // hardware exceptions from buggy third-party plugins.
#if defined(_MSC_VER)
    if (!safePluginProcessBlock(plugin, buffer, midi))
    {
        currentGain_ = 0.0f;
        applyExceptionGracePeriod(buffer);
        return;
    }
#else
    try
    {
        plugin->processBlock(buffer, midi);
    }
    catch (...)
    {
        currentGain_ = 0.0f;
        applyExceptionGracePeriod(buffer);
        return;
    }
#endif
    {
        exceptionCount_.store(0, std::memory_order_relaxed);  // reset on success
        // m-5 FIX: Decrement grace period. If an exception occurs during grace,
        // re-suspend immediately rather than waiting for 20 more exceptions.
        int grace = recoveryGracePeriod_.load(std::memory_order_relaxed);
        if (grace > 0)
            recoveryGracePeriod_.store(grace - 1, std::memory_order_relaxed);

        // Smoothly fade in after recall / bypass switch
        if (currentGain_ < 1.0f)
        {
            buffer.applyGainRamp(0, buffer.getNumSamples(), currentGain_, 1.0f);
            currentGain_ = 1.0f;
        }
    }
}

// ---------------------------------------------------------------------------
//  H12 FIX: Deduplicated exception grace-period helper
// ---------------------------------------------------------------------------

bool PluginHostManager::applyExceptionGracePeriod(juce::AudioBuffer<float>& buffer) noexcept
{
    buffer.clear();
    if (recoveryGracePeriod_.load(std::memory_order_relaxed) > 0)
    {
        suspended_.store(true, std::memory_order_relaxed);
        recoveryGracePeriod_.store(0, std::memory_order_relaxed);
        return true;
    }
    // C12 FIX: Saturated unsigned increment — never wraps, never overflows.
    const uint32_t maxSat = static_cast<uint32_t>(MAX_PLUGIN_EXCEPTIONS) + 1;
    uint32_t count = exceptionCount_.load(std::memory_order_relaxed);
    if (count < maxSat)
        count = exceptionCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count >= static_cast<uint32_t>(MAX_PLUGIN_EXCEPTIONS))
        suspended_.store(true, std::memory_order_relaxed);
    return false;
}

void PluginHostManager::scanPluginFolders()
{
    // VST3-SPEC FIX: scanning a VST3 plugin instantiates its module, and many
    // plugins' entry points assume the host's message-thread context (they touch
    // COM / the message pump / UI APIs during init). Running PluginDirectoryScanner
    // on a bare background thread — as PluginBrowserPanel does via juce::Thread::launch
    // — crashes FL Studio (and other strict hosts) the moment a plugin with such an
    // entry point is scanned. This is the same class of bug documented in
    // juce-framework/JUCE#997 and lsp-plugins#576.
    //
    // The fix matches the guard already applied by discoverPlugin() and loadPlugin()
    // in this file: acquire MessageManagerLock on the scanning thread so each
    // plugin's instantiation sees a message-thread-safe context. The lock is held on
    // THIS (worker) thread; it briefly blocks the DAW message thread only during
    // each plugin's instantiation, identical to loadPlugin's behavior. Scan is only
    // ever invoked from a worker thread by the browser, so this never deadlocks the
    // message thread against itself.
    //
    // LOCK ORDER: MessageManagerLock is acquired BEFORE knownPluginsLock_, matching
    // loadPlugin() (MMLock at the top, then the data lock). This prevents an
    // AB-BA inversion: if a future message-thread caller held knownPluginsLock_ and
    // then needed the message pump, it would not deadlock against this scan.
    std::unique_ptr<juce::MessageManagerLock> mmLock;
    if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
    {
        mmLock = std::make_unique<juce::MessageManagerLock>(juce::Thread::getCurrentThread());
        if (!mmLock->lockWasGained())
        {
            DBG("PluginHostManager::scanPluginFolders — MessageManagerLock not gained; aborting scan");
            return;
        }
    }

    const juce::SpinLock::ScopedLockType lock(knownPluginsLock_);

    // Scan default VST3 and AU locations
    for (auto* format : formatManager.getFormats())
    {
        auto defaultLocations = format->getDefaultLocationsToSearch();
        juce::PluginDirectoryScanner scanner(
            knownPlugins, *format, defaultLocations,
            true, juce::File(), false);

        juce::String pluginName;
        while (scanner.scanNextFile(true, pluginName))
        {
            // Progress reported via pluginName
        }
    }
}

const juce::PluginDescription* PluginHostManager::getLastDescription() const
{
    // Return a pointer to the immutable snapshot published when the plugin was
    // loaded. The snapshot is never mutated, so callers can safely use the
    // pointer after this function returns without racing with loadPlugin().
    auto* snap = descriptionSnapshot_.load(std::memory_order_acquire);
    return snap;
}

int PluginHostManager::getNumSteps(int index) const noexcept
{
    // W-10 FIX (audit): take a ref-counted lease instead of raw-loading
    // hostedPluginPtr_. The old code dereferenced the raw pointer with no
    // refcount, racing unloadPlugin() — a small but real use-after-free
    // window if the plugin was swapped mid-call. With the lease, unloadPlugin's
    // bounded wait + deferredDoomedPlugins_ queue guarantees the instance stays
    // alive for the duration of this call.
    auto* plugin = acquirePluginForUse();
    if (plugin == nullptr) return 0;

    struct ScopedRelease
    {
        const PluginHostManager* host;
        ~ScopedRelease() { if (host != nullptr) host->releasePluginFromUse(); }
    } release{ this };
    juce::ignoreUnused(release);

    auto& params = plugin->getParameters();
    if (index < 0 || index >= static_cast<int>(params.size())) return 0;

    auto* param = params[index];
    if (param == nullptr) return 0;

    // For JUCE AudioParameterChoice, choices.size() gives the exact step count.
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(param))
        return static_cast<int>(choice->choices.size());

    // Default to JUCE RangedAudioParameter::getNumSteps().
    return param->getNumSteps();
}

// ---------------------------------------------------------------------------
//  Static multi-stage plugin discovery
// ---------------------------------------------------------------------------

bool PluginHostManager::discoverPlugin(juce::AudioPluginFormatManager& formatManager,
                                       const juce::File& pluginFile,
                                       juce::PluginDescription& outDescription,
                                       juce::String& errorDetails,
                                       bool verbose)
{
    const auto pluginPath = pluginFile.getFullPathName();

    // B1 FIX: discovery progress used to go to std::cerr, violating the
    // AGENTS.md "DBG is the only approved logging macro" rule. All verbose
    // diagnostics now route through DBG. `verbose` still gates output so the
    // default behavior is silent.
    auto verboseLog = [&](const juce::String& message)
    {
        if (verbose)
        {
#if JUCE_DEBUG
            DBG("[Discovery] " + message);
#else
            // Release: DBG expands to nothing; reference message to avoid
            // C4100/C4390 (unreferenced param / empty controlled statement).
            juce::ignoreUnused(message);
#endif
        }
        else
        {
            juce::ignoreUnused(message);
        }
    };

    // Helper: acquire MessageManagerLock if a MessageManager exists.
    // Many VST3 plugins require the message pump during scanning/creation.
    auto withMessageLock = [](auto&& fn) -> bool
    {
        if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
        {
            juce::MessageManagerLock mmLock(juce::Thread::getCurrentThread());
            if (!mmLock.lockWasGained())
                return false;
            return fn();
        }
        return fn();
    };

    // ------------------------------------------------------------------
    //  Stage 1: Direct file query — fast path for well-behaved plugins
    // ------------------------------------------------------------------
    verboseLog("Stage 1: findAllTypesForFile for " + pluginPath);

    bool stage1Success = false;

    try
    {
        stage1Success = withMessageLock([&]() -> bool
        {
            for (auto* format : formatManager.getFormats())
            {
                if (!format->fileMightContainThisPluginType(pluginPath))
                    continue;

                juce::OwnedArray<juce::PluginDescription> descriptions;
                format->findAllTypesForFile(descriptions, pluginPath);

                if (!descriptions.isEmpty())
                {
                    outDescription = *descriptions.getFirst();
                    verboseLog("Stage 1 succeeded: " + outDescription.name
                               + " (" + format->getName() + ")");
                    return true;
                }

                verboseLog("Stage 1: " + format->getName()
                           + " recognised file but found 0 types");
            }
            return false;
        });
    }
    catch (const std::exception& e)
    {
        verboseLog(juce::String("Stage 1 exception: ") + e.what());
    }
    catch (...)
    {
        verboseLog("Stage 1 unknown exception");
    }

    if (stage1Success)
        return true;

    // ------------------------------------------------------------------
    //  Stage 2: PluginDirectoryScanner on the plugin's parent directory.
    //  This is more thorough and can discover complex bundles (e.g.
    //  FabFilter Pro-Q 4) that fail the simpler findAllTypesForFile.
    // ------------------------------------------------------------------
    verboseLog("Stage 2: PluginDirectoryScanner for parent of " + pluginPath);

    bool stage2Success = false;

    try
    {
        stage2Success = withMessageLock([&]() -> bool
        {
            const auto parentDir = pluginFile.getParentDirectory();
            juce::FileSearchPath searchPath(parentDir.getFullPathName());

            for (auto* format : formatManager.getFormats())
            {
                juce::KnownPluginList tempList;
                juce::PluginDirectoryScanner scanner(
                    tempList, *format, searchPath,
                    /*recursive=*/true, juce::File(), /*allowAsync=*/false);

                juce::String nameBeingScanned;
                while (scanner.scanNextFile(true, nameBeingScanned))
                {
                    verboseLog("Stage 2: scanning " + nameBeingScanned);
                }

                // Look for a match in the scanned results
                for (const auto& desc : tempList.getTypes())
                {
                    if (desc.fileOrIdentifier == pluginPath
                        || desc.fileOrIdentifier.endsWithIgnoreCase(
                               pluginFile.getFileName()))
                    {
                        outDescription = desc;
                        verboseLog("Stage 2 succeeded: " + outDescription.name);
                        return true;
                    }
                }

                if (!tempList.getTypes().isEmpty())
                {
                    verboseLog("Stage 2: " + format->getName()
                               + " found " + juce::String(tempList.getTypes().size())
                               + " plugin(s) but none matched target path");
                }
            }
            return false;
        });
    }
    catch (const std::exception& e)
    {
        verboseLog(juce::String("Stage 2 exception: ") + e.what());
    }
    catch (...)
    {
        verboseLog("Stage 2 unknown exception");
    }

    if (stage2Success)
        return true;

    // ------------------------------------------------------------------
    //  All stages failed — populate a minimal fallback description so the
    //  caller can still attempt loading (may succeed for simple bundles).
    // ------------------------------------------------------------------
    outDescription.fileOrIdentifier = pluginPath;
    outDescription.pluginFormatName =
        pluginFile.hasFileExtension(".vst3") ? "VST3" : juce::String();
    outDescription.name = pluginFile.getFileNameWithoutExtension();

    errorDetails = "All discovery stages failed for: " + pluginPath;
    verboseLog(errorDetails);
    return false;
}

} // namespace more_phi
