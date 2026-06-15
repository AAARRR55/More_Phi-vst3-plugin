/*
 * More-Phi — Core/ModulationMatrix.h
 * Routes modulation source values to parameter destinations.
 *
 * The matrix is a flat array of up to MAX_ROUTES ModRoute entries.
 * apply() is called on the audio thread; all other methods are called on
 * the message thread (route management, serialization).
 *
 * noexcept guarantee: apply() is noexcept because:
 * - Iterates a fixed std::array (no heap access)
 * - Only performs clamped arithmetic on vector elements
 * - vector::operator[] is noexcept when index is in range (guarded by prepare())
 *
 * Thread safety — double-buffer swap pattern:
 *
 *   Two RouteBuffer instances (buffers_[0] and buffers_[1]) are maintained.
 *   readIndex_ (std::atomic<int>) names the buffer the audio thread reads.
 *   writeIndex_ (int, message-thread-only) names the buffer being edited.
 *
 *   Message-thread write protocol (addRoute, removeRoute, setRouteDepth,
 *   setRouteEnabled, clearAll):
 *     1. Mutate buffers_[writeIndex_].
 *     2. Publish: readIndex_.store(writeIndex_, memory_order_release).
 *     3. Flip writeIndex_ = 1 - writeIndex_.
 *     4. Mirror: copy the newly published buffer into the new write buffer so
 *        the next edit starts from the current committed state.
 *
 *   Audio-thread read protocol (apply):
 *     1. idx = readIndex_.load(memory_order_acquire).
 *     2. Read buffers_[idx] — never writes to it.
 *
 *   Correctness guarantee: the audio thread only ever reads from the buffer
 *   that was last published by the message thread. The message thread never
 *   touches buffers_[readIndex_] after publishing, because it immediately
 *   flips writeIndex_ to the other slot and copies state there.
 *
 *   Query methods (getRoute, getAssignedRouteCount, findFreeSlot,
 *   isValidRouteId, toXml, fromXml) are message-thread-only and access
 *   buffers_[writeIndex_] — the live "editor" view of the routes.
 */
#pragma once

#include "ModulationTypes.h"
#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <vector>
#include <memory>

namespace more_phi {

class ModulationMatrix
{
public:
    static constexpr int MAX_ROUTES = 128;
    // Max seqlock read retries in apply() before accepting the last snapshot
    // read (extremely rare — only under sustained concurrent publishing).
    static constexpr int kMaxReadRetries = 64;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /**
     * Prepare for the maximum number of parameters the hosted plugin can have.
     * Must be called before apply() and before adding routes.
     * Not noexcept: may be called from prepareToPlay (non-audio thread).
     */
    void prepare(int maxParamCount) noexcept;

    /** Clear all routes and reset internal state. */
    void reset() noexcept;

    // ── Audio-thread API ──────────────────────────────────────────────────────

    /**
     * Accumulate modulation from all active routes into 'output'.
     * sourceValues: per-source current values, indexed by ModSourceId.
     * output: the morph parameter vector to modify in-place; values are
     *         clamped to [0, 1] after accumulation.
     * noexcept: Only arithmetic on fixed-size arrays and pre-validated indices.
     */
    void apply(const std::array<float, static_cast<int>(ModSourceId::NUM_SOURCES)>& sourceValues,
               std::vector<float>& output) noexcept;

    // ── Route management (message thread) ─────────────────────────────────────

    /**
     * Add a new route. Returns the routeId (slot index) or -1 if full.
     * Caller must validate that destParamIndex < maxParamCount.
     * Publishes the updated buffer to the audio thread atomically.
     */
    int  addRoute(ModSourceId source, int destParamIndex, float depth);

    /**
     * Disable and free a route by its routeId.
     * No-op if routeId is out of range or already inactive.
     * Publishes the updated buffer to the audio thread atomically.
     */
    void removeRoute(int routeId);

    /** Update depth for an existing route.
     *  Publishes the updated buffer to the audio thread atomically. */
    void setRouteDepth(int routeId, float depth);

    /** Enable or disable a route without removing it.
     *  Publishes the updated buffer to the audio thread atomically. */
    void setRouteEnabled(int routeId, bool enabled);

    /** Remove all routes.
     *  Publishes the updated buffer to the audio thread atomically. */
    void clearAll();

    // ── Query (message thread) ────────────────────────────────────────────────

    /** Returns the number of currently assigned (enabled or disabled) routes.
     *  Reads from the write buffer — message thread only. */
    int getAssignedRouteCount() const { return buffers_[writeIndex_].assignedCount; }

    /**
     * Return a const reference to the route at routeId.
     * Reads from the write buffer — message thread only.
     * Throws std::out_of_range if routeId is invalid.
     */
    const ModRoute& getRoute(int routeId) const;

    // ── Serialization ─────────────────────────────────────────────────────────

    /** Serialise all routes to an XML element. Caller owns the returned pointer.
     *  Reads from the write buffer — message thread only. */
    std::unique_ptr<juce::XmlElement> toXml() const;

    /**
     * Restore routes from a previously serialised XML element.
     * Invalid or out-of-range entries are silently skipped.
     * Publishes the restored buffer to the audio thread atomically.
     */
    void fromXml(const juce::XmlElement& xml);

private:
    // ── Double-buffer state ───────────────────────────────────────────────────

    struct RouteBuffer
    {
        std::array<ModRoute, MAX_ROUTES> routes{};
        // Track which slots are occupied (destParamIndex == -1 means unassigned).
        int assignedCount = 0;
    };

    // Two buffers: audio thread reads buffers_[readIndex_],
    // message thread writes to buffers_[writeIndex_].
    RouteBuffer      buffers_[2];
    std::atomic<int> readIndex_{ 0 };  // audio thread reads this index
    int              writeIndex_ = 1;  // message thread writes this index (never 0 initially)

    int maxParamCount_ = 0;

    // MODULATION-1 FIX: seqlock so the audio-thread reader can snapshot the
    // route buffer without racing publishAndMirror()'s mirror copy — which
    // overwrites the exact buffer a reader that captured the previous
    // readIndex_ may still be iterating. Mirrors SnapshotBank's seqlock.
    // seq_ odd  => a write is in progress (readers retry);
    // seq_ even => buffers_ / readIndex_ are stable.
    // writeLock_ serializes any concurrent message-thread writers (UI + MCP).
    mutable juce::SpinLock          writeLock_;
    mutable std::atomic<uint32_t>  seq_{ 0 };

    class WriteScope
    {
    public:
        explicit WriteScope(ModulationMatrix& m) noexcept : m_(m) { m_.beginWrite(); }
        ~WriteScope() { m_.endWrite(); }
    private:
        ModulationMatrix& m_;
    };
    void beginWrite() noexcept
    {
        seq_.fetch_add(1, std::memory_order_acq_rel);  // -> odd (write in progress)
    }
    void endWrite() noexcept
    {
        std::atomic_thread_fence(std::memory_order_release);
        seq_.fetch_add(1, std::memory_order_release);  // -> even (stable)
    }

    // ── Helpers (message thread — operate on buffers_[writeIndex_]) ───────────

    /** Find the first unassigned slot in the write buffer; returns -1 if full. */
    int findFreeSlot() const noexcept;

    /** Return true if routeId names an assigned slot in the write buffer. */
    bool isValidRouteId(int routeId) const noexcept;

    /** Reset body without acquiring writeLock_ — for callers already holding it. */
    void resetLocked() noexcept;

    /**
     * Atomically publish writeIndex_ as the new read buffer, then flip
     * writeIndex_ to the other slot and copy the committed state into it so
     * the next edit begins from a consistent baseline.
     * Must be called at the end of every mutation on the message thread.
     */
    void publishAndMirror() noexcept;
};

} // namespace more_phi
