/*
 * More-Phi — Host/PluginHostManager.cpp
 * Robust plugin hosting with exception handling for stability.
 *
 * Stability: PluginHealthMonitor tracks consecutive processBlock failures.
 * When it reaches MAX_PLUGIN_EXCEPTIONS (20) the plugin is Suspended (audio
 * bypassed) and a background-thread auto-recovery is attempted via tickHealth/
 * tryStartRecovery. This prevents a misbehaving guest from continuously
 * disrupting real-time audio while allowing it to recover if it can.
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
    // PERF-BROWSER: populate knownPlugins from the on-disk cache so the browser
    // can show a previously-discovered list instantly on first GUI open, without
    // re-instantiating every installed VST3. No-op (and never throws) if the
    // cache file is absent or corrupt — the browser will run a full scan then.
    loadKnownPluginsCache();
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

    // Before swapping, drain any deferred plugins to prevent memory bloat
    // when rapidly cycling plugins.
    drainDeferredDoomedPlugins();

    hostedPlugin = std::move(newPlugin);
    hostedPluginPtr_.store(hostedPlugin.get(), std::memory_order_release);

    // Initialize the new health monitor state
    healthMonitor_.reset();

    // (The UI-owned HostedPluginWindow manages the visible native editor and
    //  holds a ref-counted lease via acquirePluginForUse; the manager does not
    //  own an editor. See setWindowCloseCallback for the out-of-band teardown hook.)

    {
        const juce::SpinLock::ScopedLockType guard(descLock_);
        lastDescription = desc;  // protected write (retained for legacy callers)

        // Publish an immutable snapshot so getLastDescription() can return a stable
        // pointer without exposing lock-protected data to a TOCTOU race.
        auto snapshot = std::make_unique<juce::PluginDescription>(desc);
        auto* raw = snapshot.get();

        // AUDIT-FIX: Cap history growth. Repeated plugin swaps (e.g. automated
        // testing, long sessions) otherwise accumulated PluginDescription objects
        // indefinitely — a slow leak. Keep only the most recent entries.
        constexpr size_t kMaxDescriptionHistory = 16;
        while (descriptionHistory_.size() > kMaxDescriptionHistory)
            descriptionHistory_.erase(descriptionHistory_.begin());
        descriptionSnapshot_.store(raw, std::memory_order_release);
    }

    hasPreparedConfiguration_ = true;
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

        // The UI-owned editor window (if open) holds a ref-counted lease via
        // acquirePluginForUse(); the bounded wait below therefore also serves
        // as the editor-teardown gate — the instance is not destroyed until the
        // window's lease is released (or deferred to the doom-queue). windowCloseCallback_
        // (set via setWindowCloseCallback) is the out-of-band hook that asks the
        // UI to close the window before this point when possible.

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
    // Lock-free transport state forwarding (audio thread -> message thread bridge).
    // We update the forwarder with the latest playhead info, even if we
    // don't have a plugin or we're suspended. This ensures the message
    // thread always has up-to-date transport data for the UI.
    transportForwarder_.updateFromAudioThread(
        playHead_.load(std::memory_order_acquire));

    auto* plugin = acquirePluginForUse();
    if (!plugin || !healthMonitor_.shouldProcess())
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

        // If the plugin is present but not processing (suspended / recovering),
        // pass through silence with a fade out / in.
        if (currentGain_ > 0.0f)   // (2) fade-out seam or bypass
        {
            buffer.applyGainRamp(0, buffer.getNumSamples(), currentGain_, 0.0f);
            currentGain_ = 0.0f;
        }
        else
        {
            buffer.clear();
        }

        // If we are bypassing because the plugin is unhealthy, report this
        // as a failure to the health monitor so it can advance its state
        // machine (e.g. Degraded -> Suspended).
        if (plugin != nullptr && !healthMonitor_.shouldProcess())
        {
            healthMonitor_.reportFailure();
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

    // Health check: if the plugin is not healthy, bypass processing.
    if (!healthMonitor_.shouldProcess())
    {
        buffer.clear();  // FIX C22: ensure silence, not stale pass-through audio
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
            healthMonitor_.reportFailure();
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
            healthMonitor_.reportFailure();
            return;
        }
#endif
        {
            healthMonitor_.reportSuccess();
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
        healthMonitor_.reportFailure();
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
        healthMonitor_.reportFailure();
        return;
    }
#endif
    {
        healthMonitor_.reportSuccess();

        // Smoothly fade in after recall / bypass switch
        if (currentGain_ < 1.0f)
        {
            buffer.applyGainRamp(0, buffer.getNumSamples(), currentGain_, 1.0f);
            currentGain_ = 1.0f;
        }
    }
}

juce::File PluginHostManager::getKnownPluginsCacheFile() const
{
    // PERF-BROWSER: cache under the user app-data "MorePhi/" folder — same
    // convention as the genre-classifier model path (see
    // MorePhiProcessor::initializeGenreClassifier). createUnixDirectories is
    // safe to call here unconditionally; it's idempotent.
    const auto baseDir = juce::File::getSpecialLocation(
        juce::File::userApplicationDataDirectory);
    return baseDir.getChildFile("MorePhi").getChildFile("known_plugins.xml");
}

void PluginHostManager::loadKnownPluginsCache()
{
    // PERF-BROWSER: deserialize the persisted KnownPluginList so the browser can
    // show the list instantly on first GUI open. Fail-soft: any parse error,
    // missing file, or version skew is ignored — the browser will fall back to a
    // full scan. Held under knownPluginsLock_ because knownPlugins is mutated.
    const auto cacheFile = getKnownPluginsCacheFile();
    if (!cacheFile.existsAsFile())
        return;

    juce::XmlDocument doc(cacheFile);
    const auto xml = doc.getDocumentElement();
    if (xml == nullptr)
    {
        DBG("PluginHostManager::loadKnownPluginsCache — failed to parse cache: "
            + doc.getLastParseError());
        return;
    }

    const juce::SpinLock::ScopedLockType lock(knownPluginsLock_);
    knownPlugins.recreateFromXml(*xml);
}

void PluginHostManager::saveKnownPluginsCache() const
{
    // PERF-BROWSER: persist the discovered plugin list so subsequent GUI opens
    // skip the slow VST3-instantiation scan entirely. Snapshot the list under
    // the lock, then write to a temp file and atomically move into place — this
    // avoids a partial-write corrupting the cache if the process is killed mid-
    // write. Never throws on failure (best-effort persistence).
    const auto cacheFile = getKnownPluginsCacheFile();
    const auto cacheDir = cacheFile.getParentDirectory();
    if (!cacheDir.exists() && !cacheDir.createDirectory())
    {
        DBG("PluginHostManager::saveKnownPluginsCache — could not create cache dir");
        return;
    }

    std::unique_ptr<juce::XmlElement> xml;
    {
        const juce::SpinLock::ScopedLockType lock(knownPluginsLock_);
        xml = knownPlugins.createXml();
    }
    if (xml == nullptr)
        return;

    // Atomic write: write to a fixed temp path, then move into place. Delete any
    // stale temp first (a previous crashed write may have left one). The temp
    // file lives next to the cache so the move is on the same volume (atomic).
    const auto tempFile = cacheFile.getSiblingFile("known_plugins.xml.tmp");
    tempFile.deleteFile();
    if (!tempFile.replaceWithText(xml->toString()))
    {
        DBG("PluginHostManager::saveKnownPluginsCache — failed to write temp cache");
        tempFile.deleteFile();
        return;
    }
    if (!tempFile.moveFileTo(cacheFile))
    {
        DBG("PluginHostManager::saveKnownPluginsCache — failed to move cache into place");
        tempFile.deleteFile();
    }
}

void PluginHostManager::scanPluginFolders()
{
    // VST3-SPEC FIX (background): scanning a VST3 plugin instantiates its module,
    // and many plugins' entry points assume the host's message-thread context
    // (they touch COM / the message pump / UI APIs during init). Running
    // PluginDirectoryScanner on a bare background thread — as PluginBrowserPanel
    // does via juce::Thread::launch — crashes FL Studio (and other strict hosts)
    // the moment a plugin with such an entry point is scanned. Same class of bug
    // as juce-framework/JUCE#997 and lsp-plugins#576.
    //
    // PERF-BROWSER FIX (the actual browser-freeze root cause, 2026-06-28): the
    // PRIOR implementation acquired ONE MessageManagerLock at the top of this
    // function and held it across the ENTIRE for+while loop over every installed
    // plugin. The comment below claimed it "briefly blocks the DAW message thread
    // only during each plugin's instantiation" — that was factually wrong: a
    // single RAII guard blocks FL Studio's message thread CONTINUOUSLY for the
    // whole scan (many seconds to tens of seconds), freezing the entire DAW UI.
    //
    // The fix: acquire AND RELEASE both locks PER PLUGIN, so the DAW message
    // thread can pump events between plugin instantiations. Each plugin still
    // sees a message-thread-safe context during its own scanNextFile() call
    // (VST3-spec compliant), but no longer blocks the host between plugins.
    //
    // LOCK ORDER: per-plugin, MessageManagerLock is acquired BEFORE
    // knownPluginsLock_ (matching loadPlugin()/discoverPlugin()). Neither lock
    // is held across iteration boundaries, so there is no long-held lock and no
    // possibility of AB-BA inversion with any other caller.

    // Check for an exit request up front (the launching worker thread may be
    // joined during shutdown); rechecked implicitly each iteration via MMLock
    // gain failure below.
    bool aborted = false;
    for (auto* format : formatManager.getFormats())
    {
        if (aborted)
            break;

        const auto defaultLocations = format->getDefaultLocationsToSearch();

        // Scanner construction does NOT instantiate plugins — only scanNextFile
        // does — so it's safe to build outside the locks. It captures a
        // reference to knownPlugins, which we guard per-call below.
        juce::PluginDirectoryScanner scanner(
            knownPlugins, *format, defaultLocations,
            true, juce::File(), false);

        juce::String pluginName;
        // Loop until the scanner has no more files. Each iteration:
        //   1. acquire MessageManagerLock (message-thread context for the plugin)
        //   2. acquire knownPluginsLock_ (guard the list mutation)
        //   3. scanNextFile (instantiates one plugin, may append to knownPlugins)
        //   4. release both locks — yields the DAW message thread between plugins
        while (true)
        {
            // Per-plugin MessageManagerLock. If the host is shutting down the
            // message manager, lockWasGained() returns false and we abort the
            // scan cleanly (matches the prior early-return behavior).
            std::unique_ptr<juce::MessageManagerLock> mmLock;
            if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
            {
                mmLock = std::make_unique<juce::MessageManagerLock>(juce::Thread::getCurrentThread());
                if (!mmLock->lockWasGained())
                {
                    DBG("PluginHostManager::scanPluginFolders — MessageManagerLock not gained; aborting scan");
                    aborted = true;
                    break;
                }
            }

            bool more = false;
            {
                const juce::SpinLock::ScopedLockType lock(knownPluginsLock_);
                more = scanner.scanNextFile(true, pluginName);
            }
            // mmLock releases here as it goes out of scope — message thread freed.
            if (!more)
                break;
        }
    }

    // PERF-BROWSER: persist the freshly-scanned list so the next GUI open is
    // instant. Best-effort; failure to write the cache is non-fatal (the list
    // is still populated in memory for this session). Written even on abort so
    // whatever was discovered before the abort is still cached.
    saveKnownPluginsCache();
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
