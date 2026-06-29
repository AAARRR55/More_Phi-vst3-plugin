/*
 * More-Phi - Core/SnapshotBank.h
 * 12-slot snapshot storage with capture/recall.
 * Thread-safe for concurrent UI/audio access using seqlock pattern.
 *
 * LOCK-FREE DESIGN:
 * - Audio thread: Never blocks, uses seqlock read with retry
 * - UI thread: Can block briefly during writes using seqlock
 * - Seqlock allows concurrent reads without locks, writers get exclusive access
 *
 * MEMORY:
 * - slots_ is heap-allocated (unique_ptr) to avoid placing ~97 KB of raw
 *   parameter data on the plugin's stack and triggering stack-overflow in
 *   hosts that use small thread stacks (e.g. FL Studio).
 */
#pragma once

#include "ParameterState.h"
#include <juce_core/juce_core.h>
#include <array>
#include <memory>
#include <vector>
#include <atomic>

// Forward declare for Full recall mode
namespace juce { 
    class AudioPluginInstance; 
    class AudioProcessor;
}

// Platform-specific pause instruction for spin-wait loops
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#endif

namespace more_phi {

class IParameterBridge;  // forward

inline void spinPause() noexcept
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    _mm_pause();
#elif defined(__arm__) || defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

/** Controls how snapshots are recalled.
 *  Fast: normalized floats only (instant, works for most plugins)
 *  Full: also stores/restores VST3 opaque state chunk (for Kontakt, wavetable synths) */
enum class RecallMode { Fast = 0, Full = 1 };

class SnapshotBank
{
public:
    static constexpr int NUM_SLOTS = 12;
    // Maximum retries for audio thread read operations
    // Increased to 128 for better tolerance under MCP write contention
    static constexpr int MAX_READ_RETRIES = 128;

    // Heap-allocate the large slots array so it never lives on the stack
    SnapshotBank()
        : slots_(std::make_unique<std::array<ParameterState, NUM_SLOTS>>())
    {}

    // Called during prepareToPlay to set parameter upper bound for safe copies.
    void prepare(int maxParamCount);
    void capture(int slot, const IParameterBridge& bridge);
    void captureValues(int slot, const std::vector<float>& values);
    void captureValuesWithNames(int slot, const float* values, int count, const juce::StringArray& names);
    void recall(int slot, IParameterBridge& bridge) const;

    // Params-only recall: skips state chunk even in Full mode.
    // Used when Recall Toggle is off to sustain notes during snapshot switches.
    void recallFast(int slot, IParameterBridge& bridge) const;

    // AUDIT-FIX (C6): Parameter remapping recall. When a hosted plugin's
    // parameter order has changed (e.g. after a version update), the snapshot's
    // stored parameter names are used to find the correct current index for each
    // parameter. Parameters whose names cannot be found are silently skipped.
    // Falls back to the supplied slot values if no names are captured.
    void recallWithParameterMapping(int slot, IParameterBridge& bridge) const;

    // Full mode: capture/recall opaque VST3 state chunks alongside parameters
    bool captureStateChunk(int slot, juce::AudioPluginInstance* plugin);
    void captureStateChunk(int slot, const juce::MemoryBlock& chunk);
    bool recallStateChunk(int slot, juce::AudioPluginInstance* plugin) const;
    bool recallStateChunk(int slot, juce::AudioProcessor* plugin) const;
    bool copyStateChunk(int slot, juce::MemoryBlock& outChunk) const;
    bool hasStateChunk(int slot) const;

    void setRecallMode(RecallMode m)  { recallMode_.store(m, std::memory_order_release); }
    RecallMode getRecallMode() const  { return recallMode_.load(std::memory_order_acquire); }

    // ── Smart Preset Caching: plugin UID per slot (Phase 7) ──────────────────
    /** Set the cached plugin UID for a slot. Used during Full-mode capture to
     *  record which hosted plugin the state chunk came from. */
    void setCachedPluginUID(int slot, const juce::String& uid)
    {
        if (slot >= 0 && slot < NUM_SLOTS)
        {
            const juce::SpinLock::ScopedLockType chunkLock(chunksLock_);
            cachedPluginUID_[slot] = uid;
        }
    }

    /** Get the cached plugin UID for a slot. Returns empty string if none. */
    juce::String getCachedPluginUID(int slot) const
    {
        if (slot < 0 || slot >= NUM_SLOTS) return {};
        const juce::SpinLock::ScopedLockType chunkLock(chunksLock_);
        return cachedPluginUID_[slot];
    }

    /** Returns true if the cached UID for @a slot matches @a uid.
     *  Used during recall to skip state-chunk application when the hosted
     *  plugin has changed since capture. */
    bool pluginUIDMatches(int slot, const juce::String& uid) const
    {
        if (slot < 0 || slot >= NUM_SLOTS) return false;
        const juce::SpinLock::ScopedLockType chunkLock(chunksLock_);
        return cachedPluginUID_[slot] == uid;
    }

    bool isOccupied(int slot) const noexcept;
    bool hasAnyOccupied() const noexcept;
    int  getOccupiedSlots(std::array<int, NUM_SLOTS>& occupiedSlots) const;
    uint32_t getSeqlockExhaustionCount() const noexcept { return seqlockExhaustionCount_.load(std::memory_order_relaxed); }
    bool getSlotValuesCopy(int slot, std::vector<float>& outValues) const;

    // ── Gravity Wells: per-snapshot mass weight ───────────────────────────
    /** Set the mass weight for a slot. Clamped to [0.1, 3.0].
     *  Thread-safe: uses WriteScope (seqlock + writeLock). */
    void setMass(int slot, float mass);
    /** Get the mass weight for a slot. Thread-safe: uses seqlock read. */
    float getMass(int slot) const noexcept;
    /** Bulk read all 12 masses. Thread-safe: uses seqlock read. */
    void getMasses(std::array<float, NUM_SLOTS>& masses) const noexcept;

    void clearSlot(int slot);
    void clearAll();

    // ── Parameter name remapping (VST3-H1) ─────────────────────────────────
    /** Look up a parameter index by name within a captured snapshot slot.
     *  Returns -1 if the slot is unoccupied, has no names, or the name was not found.
     *  Used during recall to remap parameter indices when a hosted plugin's
     *  parameter order has changed between versions. */
    int findParameterIndex(int slot, const juce::String& paramName) const
    {
        if (slot < 0 || slot >= NUM_SLOTS) return -1;
        // M-7 FIX: Use try-lock to avoid blocking the audio thread during
        // MIDI-triggered recall. If a writer holds writeLock_, return -1
        // (caller falls back to index-based recall).
        const juce::SpinLock::ScopedTryLockType tryLock(writeLock_);
        if (! tryLock.isLocked())
            return -1;
        const auto& names = paramNames_[slot];
        int idx = names.indexOf(paramName);
        return (idx >= 0) ? idx : -1;
    }

    // ── State persistence ───────────────────────────────────────────────────
    /** Serialize all occupied snapshot slots to an XML element for DAW state save. */
    std::unique_ptr<juce::XmlElement> toXml() const
    {
        auto xml = std::make_unique<juce::XmlElement>("SNAPSHOT_BANK");
        xml->setAttribute("version", "1");

        for (int i = 0; i < NUM_SLOTS; ++i)
        {
            // B3 FIX: read the lock-protected side data ONCE, outside the
            // seqlock retry loop. paramNames_ (writeLock_) and stateChunks_
            // (chunksLock_) are independent of the seqlock sequence —
            // re-acquiring their locks on every retry iteration starved MCP
            // write traffic that needs writeLock_ to capture. The seqlock only
            // protects the slot struct itself (occupied/count/name/values).
            juce::StringArray namesBuf;
            {
                const juce::SpinLock::ScopedLockType lock(writeLock_);
                namesBuf = paramNames_[i];
            }
            juce::MemoryBlock chunkBuf;
            juce::String uidBuf;
            {
                const juce::SpinLock::ScopedLockType chunkLock(chunksLock_);
                chunkBuf = stateChunks_[i];   // copy unconditionally; sized check below
                uidBuf = cachedPluginUID_[i];
            }

            // Read slot data into local buffers under seqlock, then construct XML outside.
            bool occupied = false;
            int count = 0;
            char nameBuf[64] = {};
            std::array<float, MAX_PARAMETERS> valuesBuf{};
            float localMass = 1.0f;  // H-6: read INSIDE the seqlock window
            bool readOk = false;

            for (int retry = 0; retry < MAX_READ_RETRIES; ++retry)
            {
                uint32_t seq1 = seqlock_.load(std::memory_order_acquire);
                if ((seq1 & 1) != 0) { spinPause(); continue; }

                occupied = (*slots_)[i].occupied;
                count = (*slots_)[i].parameterCount;
                std::memcpy(nameBuf, (*slots_)[i].name, sizeof(nameBuf));
                static_assert(sizeof(nameBuf) == sizeof(ParameterState::name),
                              "SnapshotBank::toXml nameBuf must match ParameterState::name size");
                if (occupied && count > 0)
                    std::copy_n((*slots_)[i].values.begin(), count, valuesBuf.begin());
                localMass = (*slots_)[i].mass;  // H-6: read inside seqlock window

                std::atomic_thread_fence(std::memory_order_acquire);
                uint32_t seq2 = seqlock_.load(std::memory_order_acquire);
                if (seq1 == seq2)
                {
                    readOk = true;

                    if (occupied)
                    {
                        auto slotXml = std::make_unique<juce::XmlElement>("SLOT");
                        slotXml->setAttribute("id", i);
                        slotXml->setAttribute("paramCount", count);
                        slotXml->setAttribute("name", juce::String(nameBuf));
                        // Gravity Well mass (omit if default 1.0)
                        if (std::abs(localMass - 1.0f) > 1e-6f)
                            slotXml->setAttribute("mass", static_cast<double>(localMass));

                        if (count > 0) {
                            juce::MemoryBlock block(valuesBuf.data(),
                                                    static_cast<size_t>(count) * sizeof(float));
                            slotXml->setAttribute("values", block.toBase64Encoding());

                            auto namesXml = slotXml->createNewChildElement("PARAM_NAMES");
                            const int nameCount = juce::jmin(namesBuf.size(), count);
                            for (int p = 0; p < nameCount; ++p)
                                namesXml->setAttribute("p" + juce::String(p), namesBuf[p]);
                        }

                        if (chunkBuf.getSize() > 0)
                            slotXml->setAttribute("stateChunk", chunkBuf.toBase64Encoding());

                        if (uidBuf.isNotEmpty())
                            slotXml->setAttribute("pluginUID", uidBuf);

                        xml->addChildElement(slotXml.release());
                    }

                    break;
                }
            }

            if (!readOk) continue;
        }
        return xml;
    }

    /** Restore snapshot slots from a previously saved XML element.
     *  C-4 FIX: Parse into temporary storage first, then swap on success.
     *  If any element is malformed, the original snapshot data is preserved.
     */
    void fromXml(const juce::XmlElement& xml)
    {
        // Phase 1: Parse into temporary storage (no side effects on live data)
        auto tmpSlots = std::make_unique<std::array<ParameterState, NUM_SLOTS>>();
        std::array<juce::MemoryBlock, NUM_SLOTS> tmpChunks;
        std::array<juce::String, NUM_SLOTS> tmpUIDs;
        std::array<juce::StringArray, NUM_SLOTS> tmpNames;

        // AUDIT-FIX (C5): Check XML version for forward compatibility.
        // Version 1 (or absent) = current format. Future versions can branch here.
        const int xmlVersion = xml.getIntAttribute("version", 1);
        juce::ignoreUnused(xmlVersion);  // reserved for future schema changes

        for (auto* child : xml.getChildIterator())
        {
            if (!child->hasTagName("SLOT")) continue;

            int slot = child->getIntAttribute("id", -1);
            int count = child->getIntAttribute("paramCount", 0);
            int safeCount = juce::jlimit(0, MAX_PARAMETERS, count);
            if (slot < 0 || slot >= NUM_SLOTS) continue;

            juce::String name = child->getStringAttribute("name", "");
            juce::String base64 = child->getStringAttribute("values", "");
            juce::String stateBase64 = child->getStringAttribute("stateChunk", "");

            // Gravity Well mass (default 1.0 for backward compat)
            const float slotMass = static_cast<float>(
                child->getDoubleAttribute("mass", 1.0));

            if (base64.isNotEmpty() && safeCount > 0)
            {
                juce::MemoryBlock block;
                if (block.fromBase64Encoding(base64))
                {
                    int expectedBytes = safeCount * static_cast<int>(sizeof(float));
                    if (static_cast<int>(block.getSize()) >= expectedBytes)
                    {
                        (*tmpSlots)[slot].capture(static_cast<const float*>(block.getData()), safeCount);
                        (*tmpSlots)[slot].setName(name.toRawUTF8());
                    }
                }
            }
            else {
                (*tmpSlots)[slot].setName(name.toRawUTF8());
                (*tmpSlots)[slot].occupied = false;
            }
            (*tmpSlots)[slot].mass = std::clamp(slotMass, 0.1f, 3.0f);

            // Restore parameter names (VST3-H1)
            if (auto* namesEl = child->getChildByName("PARAM_NAMES"))
            {
                for (int p = 0; p < safeCount; ++p)
                {
                    juce::String paramName = namesEl->getStringAttribute("p" + juce::String(p), "");
                    if (paramName.isNotEmpty())
                        tmpNames[slot].add(paramName);
                    else
                        tmpNames[slot].add("");  // Preserve index alignment
                }
            }

            if (stateBase64.isNotEmpty()) {
                juce::MemoryBlock chunkBlock;
                if (chunkBlock.fromBase64Encoding(stateBase64))
                    tmpChunks[slot] = std::move(chunkBlock);
            }

            // Smart Preset Caching (Phase 7): restore plugin UID
            tmpUIDs[slot] = child->getStringAttribute("pluginUID", "");
        }

        // Phase 2: Swap parsed data into live slots under seqlock write
        {
            WriteScope write(*this);

            for (int i = 0; i < NUM_SLOTS; ++i)
            {
                if ((*tmpSlots)[i].occupied || (*tmpSlots)[i].name[0] != '\0')
                    (*slots_)[i] = std::move((*tmpSlots)[i]);
                else
                    (*slots_)[i].clear();
            }

            for (int i = 0; i < NUM_SLOTS; ++i)
                paramNames_[i] = std::move(tmpNames[i]);

            const juce::SpinLock::ScopedLockType chunkLock(chunksLock_);
            for (int i = 0; i < NUM_SLOTS; ++i)
            {
                stateChunks_[i] = std::move(tmpChunks[i]);
                cachedPluginUID_[i] = std::move(tmpUIDs[i]);
            }
        }
    }

    // Lock-free read access for audio thread
    // Returns false if read failed after MAX_READ_RETRIES (very rare)
    // Fn signature: void(const std::array<ParameterState, NUM_SLOTS>&)
    template <typename Fn>
    bool tryReadLocked(Fn&& fn) const
    {
        // Seqlock read pattern: retry if sequence changed during read
        for (int retry = 0; retry < MAX_READ_RETRIES; ++retry)
        {
            // Load sequence - acquire ensures we see all writes before seqlock increment
            uint32_t seq1 = seqlock_.load(std::memory_order_acquire);

            // If odd, a write is in progress - retry immediately
            if ((seq1 & 1) != 0)
            {
                spinPause();
                continue;
            }

            // Copy data - this is the critical read section
            fn(*slots_);

            // Prevent the compiler from moving non-atomic slot reads below the
            // second sequence check. The seqlock only works if validation
            // happens after the copy in the generated code.
            // Acquire fence pairs with the writer's release fence in endWrite().
            // Unlike atomic_signal_fence (which emits NO hardware barrier and only
            // constrains the compiler within a thread), atomic_thread_fence
            // prevents the non-atomic slot reads above from being reordered past
            // the seq2 validation load below on weakly-ordered CPUs (ARM/Apple
            // Silicon). Without this, a torn read can validate as consistent.
            std::atomic_thread_fence(std::memory_order_acquire);

            // Load sequence again - acquire to ensure we see consistent state
            uint32_t seq2 = seqlock_.load(std::memory_order_acquire);

            // If sequence unchanged and even, read was consistent
            if (seq1 == seq2)
                return true;

            // Sequence changed - data was modified during read, retry
        }

        // Exhausted retries - extremely rare, indicates heavy write contention
        seqlockExhaustionCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

private:
    // RAII helper to guarantee balanced begin/end write markers.
    class WriteScope
    {
    public:
        explicit WriteScope(SnapshotBank& owner) : owner_(owner) { owner_.beginWrite(); }
        ~WriteScope() { owner_.endWrite(); }

    private:
        SnapshotBank& owner_;
    };

    // Seqlock sequence counter:
    // - Even value: no write in progress, data is stable
    // - Odd value: write in progress
    // - Incremented at write start and end, so readers can detect modification
    mutable std::atomic<uint32_t> seqlock_{0};
    // Diagnostics: incremented each time tryReadLocked exhausts all retries.
    // Read via getSeqlockExhaustionCount(); exposed in profiling report.
    mutable std::atomic<uint32_t> seqlockExhaustionCount_{0};
    // Seqlock requires a single writer. Multiple non-audio writers (UI + MCP)
    // must serialize writes to keep sequence transitions valid.
    mutable juce::SpinLock writeLock_;

    // C-4 FIX: Separate mutex for stateChunks_ — MemoryBlock copy assignment
    // can heap-allocate, which violates the seqlock's side-effect-free read contract.
    mutable juce::SpinLock chunksLock_;

    // Heap-allocated to keep SnapshotBank (and its owner MorePhiProcessor)
    // off the stack. Each ParameterState holds 2048 floats; 12 slots ≈ 96.8 KB
    // (12 × 2048 × 4 B = 98,304 B = 96.0 KB + per-slot overhead → 96.8 KB
    // measured via sizeof in the benchmark memory test). NOTE: was previously
    // documented as ~384 KB, stale from when MAX_PARAMETERS was larger.
    std::unique_ptr<std::array<ParameterState, NUM_SLOTS>> slots_;
    std::atomic<int> preparedParamCount_{0};
    std::atomic<RecallMode> recallMode_{RecallMode::Fast};

    // State chunks for Full recall mode (one per slot)
    std::array<juce::MemoryBlock, NUM_SLOTS> stateChunks_;

    // Smart Preset Caching (Phase 7): plugin UID per slot, recorded at capture
    // time so recall can skip state-chunk apply when the hosted plugin changed.
    // Protected alongside stateChunks_ by chunksLock_.
    std::array<juce::String, NUM_SLOTS> cachedPluginUID_;

    // Parameter names per slot — populated during capture(), used for forward
    // compatibility when hosted plugin parameter order changes between versions.
    // Not read on audio thread, so juce::StringArray (heap-allocating) is safe.
    std::array<juce::StringArray, NUM_SLOTS> paramNames_;



    // Begin write section - increments seqlock to odd
    void beginWrite()
    {
        writeLock_.enter();
        // acq_rel ensures all previous writes are visible and sequence update is ordered
        seqlock_.fetch_add(1, std::memory_order_acq_rel);
    }

    // End write section - increments seqlock to even
    void endWrite()
    {
        // Ensure all data writes complete before seqlock goes even
        std::atomic_thread_fence(std::memory_order_release);
        seqlock_.fetch_add(1, std::memory_order_release);
        writeLock_.exit();
    }

    // Helper to copy a single slot with seqlock protection.
    // Used by recall()/recallFast() on the audio path.
    bool copySlotValues(int slot, float* outValues, int& outCount) const
    {
        for (int retry = 0; retry < MAX_READ_RETRIES; ++retry)
        {
            uint32_t seq1 = seqlock_.load(std::memory_order_acquire);
            if ((seq1 & 1) != 0)
            {
                spinPause();
                continue;
            }

            // Copy slot data
            bool occupied = (*slots_)[slot].occupied;
            int count = (*slots_)[slot].parameterCount;

            if (!occupied || count <= 0)
            {
                // Still need to check sequence before returning
                uint32_t seq2 = seqlock_.load(std::memory_order_acquire);
                if (seq1 == seq2)
                {
                    outCount = 0;
                    return false;
                }
                continue;
            }

            // Copy values
            std::copy_n((*slots_)[slot].values.begin(),
                        static_cast<size_t>(count),
                        outValues);
            outCount = count;

            // Acquire fence pairs with the writer's release fence in endWrite().
            // Unlike atomic_signal_fence (which emits NO hardware barrier and only
            // constrains the compiler within a thread), atomic_thread_fence
            // prevents the non-atomic slot reads above from being reordered past
            // the seq2 validation load below on weakly-ordered CPUs (ARM/Apple
            // Silicon). Without this, a torn read can validate as consistent.
            std::atomic_thread_fence(std::memory_order_acquire);

            uint32_t seq2 = seqlock_.load(std::memory_order_acquire);
            if (seq1 == seq2)
                return true;
        }
        return false;
    }
    
    // Helper to copy a state chunk safely
    bool copyStateChunkInternal(int slot, juce::MemoryBlock& outChunk) const
    {
        // ATS-M5: Read occupied under seqlock for correctness (occupied is
        // protected by the seqlock, not chunksLock_).
        bool occupied = false;
        bool readOccupied = false;
        for (int retry = 0; retry < MAX_READ_RETRIES; ++retry)
        {
            uint32_t seq1 = seqlock_.load(std::memory_order_acquire);
            if ((seq1 & 1) != 0) { spinPause(); continue; }
            occupied = (*slots_)[slot].occupied;
            // C-1: acquire fence pairs with writer's release fence.
            std::atomic_thread_fence(std::memory_order_acquire);
            uint32_t seq2 = seqlock_.load(std::memory_order_acquire);
            if (seq1 == seq2)
            {
                readOccupied = true;
                break;
            }
        }
        if (!readOccupied || !occupied) return false;

        // C-4 FIX: Use chunksLock_ for state chunk reads — MemoryBlock
        // copy-assignment can heap-allocate, violating seqlock contract.
        const juce::SpinLock::ScopedLockType lock(chunksLock_);
        outChunk = stateChunks_[slot];
        return outChunk.getSize() > 0;
    }
};

} // namespace more_phi
