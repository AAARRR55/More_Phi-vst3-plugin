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
 * - slots_ is heap-allocated (unique_ptr) to avoid placing ~384 KB of raw
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

    // Full mode: capture/recall opaque VST3 state chunks alongside parameters
    void captureStateChunk(int slot, juce::AudioPluginInstance* plugin);
    void captureStateChunk(int slot, const juce::MemoryBlock& chunk);
    void recallStateChunk(int slot, juce::AudioPluginInstance* plugin) const;
    void recallStateChunk(int slot, juce::AudioProcessor* plugin) const;
    bool copyStateChunk(int slot, juce::MemoryBlock& outChunk) const;
    bool hasStateChunk(int slot) const;

    void setRecallMode(RecallMode m)  { recallMode_.store(m, std::memory_order_release); }
    RecallMode getRecallMode() const  { return recallMode_.load(std::memory_order_acquire); }

    bool isOccupied(int slot) const noexcept;
    bool hasAnyOccupied() const noexcept;
    int  getOccupiedSlots(std::array<int, NUM_SLOTS>& occupiedSlots) const;
    bool getSlotValuesCopy(int slot, std::vector<float>& outValues) const;

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
        const auto& names = paramNames_[slot];
        int idx = names.indexOf(paramName);
        return (idx >= 0) ? idx : -1;
    }

    // ── State persistence ───────────────────────────────────────────────────
    /** Serialize all occupied snapshot slots to an XML element for DAW state save. */
    std::unique_ptr<juce::XmlElement> toXml() const
    {
        auto xml = std::make_unique<juce::XmlElement>("SNAPSHOT_BANK");

        for (int i = 0; i < NUM_SLOTS; ++i)
        {
            // Seqlock read for thread safety
            for (int retry = 0; retry < MAX_READ_RETRIES; ++retry)
            {
                uint32_t seq1 = seqlock_.load(std::memory_order_acquire);
                if ((seq1 & 1) != 0) continue;

                bool occupied = (*slots_)[i].occupied;
                int count = (*slots_)[i].parameterCount;

                // C-4 FIX: Read state chunk under its own mutex, NOT under seqlock.
                // MemoryBlock copy can heap-allocate, violating seqlock contract.
                juce::MemoryBlock chunkCopy;
                {
                    const juce::SpinLock::ScopedLockType chunkLock(chunksLock_);
                    if (occupied) {
                        chunkCopy = stateChunks_[i];
                    }
                }

                uint32_t seq2 = seqlock_.load(std::memory_order_acquire);
                if (seq1 != seq2) continue;

                if (occupied)
                {
                    auto slotXml = std::make_unique<juce::XmlElement>("SLOT");
                    slotXml->setAttribute("id", i);
                    slotXml->setAttribute("paramCount", count);
                    slotXml->setAttribute("name", juce::String((*slots_)[i].name));

                    if (count > 0) {
                        // Encode float values as base64 for compact, lossless storage
                        juce::MemoryBlock block((*slots_)[i].values.data(),
                                                static_cast<size_t>(count) * sizeof(float));
                        slotXml->setAttribute("values", block.toBase64Encoding());

                        // Store parameter names for forward compatibility (VST3-H1)
                        // Allows parameter remapping when hosted plugin order changes
                        {
                            auto namesXml = slotXml->createNewChildElement("PARAM_NAMES");
                            const auto& names = paramNames_[i];
                            const int nameCount = juce::jmin(names.size(), count);
                            for (int p = 0; p < nameCount; ++p)
                                namesXml->setAttribute("p" + juce::String(p), names[p]);
                        }
                    }
                    
                    if (chunkCopy.getSize() > 0) {
                        slotXml->setAttribute("stateChunk", chunkCopy.toBase64Encoding());
                    }

                    xml->addChildElement(slotXml.release());
                }
                break;  // Success
            }
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
        std::array<juce::StringArray, NUM_SLOTS> tmpNames;

        for (auto* child : xml.getChildIterator())
        {
            if (!child->hasTagName("SLOT")) continue;

            int slot = child->getIntAttribute("id", -1);
            int count = child->getIntAttribute("paramCount", 0);
            if (slot < 0 || slot >= NUM_SLOTS) continue;

            juce::String name = child->getStringAttribute("name", "");
            juce::String base64 = child->getStringAttribute("values", "");
            juce::String stateBase64 = child->getStringAttribute("stateChunk", "");

            if (base64.isNotEmpty() && count > 0)
            {
                juce::MemoryBlock block;
                if (block.fromBase64Encoding(base64))
                {
                    int safeCount = juce::jmin(count, MAX_PARAMETERS);
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
                (*tmpSlots)[slot].occupied = true;
            }

            // Restore parameter names (VST3-H1)
            if (auto* namesEl = child->getChildByName("PARAM_NAMES"))
            {
                for (int p = 0; p < count; ++p)
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
                stateChunks_[i] = std::move(tmpChunks[i]);
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
                // Spin pause hint for better performance on x86
                #if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
                _mm_pause();
                #elif defined(__arm__) || defined(__aarch64__)
                __asm__ __volatile__("yield" ::: "memory");
                #endif
                continue;
            }

            // Copy data - this is the critical read section
            fn(*slots_);

            // Prevent the compiler from moving non-atomic slot reads below the
            // second sequence check. The seqlock only works if validation
            // happens after the copy in the generated code.
            std::atomic_signal_fence(std::memory_order_seq_cst);

            // Load sequence again - acquire to ensure we see consistent state
            uint32_t seq2 = seqlock_.load(std::memory_order_acquire);

            // If sequence unchanged and even, read was consistent
            if (seq1 == seq2)
                return true;

            // Sequence changed - data was modified during read, retry
        }

        // Exhausted retries - extremely rare, indicates heavy write contention
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
    // Seqlock requires a single writer. Multiple non-audio writers (UI + MCP)
    // must serialize writes to keep sequence transitions valid.
    mutable juce::SpinLock writeLock_;

    // C-4 FIX: Separate mutex for stateChunks_ — MemoryBlock copy assignment
    // can heap-allocate, which violates the seqlock's side-effect-free read contract.
    mutable juce::SpinLock chunksLock_;

    // Heap-allocated to keep SnapshotBank (and its owner MorePhiProcessor)
    // off the stack. Each ParameterState holds 2048 floats; 12 slots = ~384 KB.
    std::unique_ptr<std::array<ParameterState, NUM_SLOTS>> slots_;
    std::atomic<int> preparedParamCount_{0};
    std::atomic<RecallMode> recallMode_{RecallMode::Fast};

    // State chunks for Full recall mode (one per slot)
    std::array<juce::MemoryBlock, NUM_SLOTS> stateChunks_;

    // Parameter names per slot — populated during capture(), used for forward
    // compatibility when hosted plugin parameter order changes between versions.
    // Not read on audio thread, so juce::StringArray (heap-allocating) is safe.
    std::array<juce::StringArray, NUM_SLOTS> paramNames_;

    // Pre-allocated scratch buffer for recall/recallFast — avoids per-call stack
    // allocation of 8 KB (std::array<float, 2048>) on the audio thread.
    mutable std::array<float, MAX_PARAMETERS> recallScratch_{};

    // Pre-allocated scratch buffer for capture/captureValues — avoids per-call stack
    // allocation of 8 KB (std::array<float, 2048>) on the calling thread.
    mutable std::array<float, MAX_PARAMETERS> captureScratch_{};

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
                #if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
                _mm_pause();
                #elif defined(__arm__) || defined(__aarch64__)
                __asm__ __volatile__("yield" ::: "memory");
                #endif
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

            std::atomic_signal_fence(std::memory_order_seq_cst);

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
            if ((seq1 & 1) != 0) continue;
            occupied = (*slots_)[slot].occupied;
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
