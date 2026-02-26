/*
 * MorphSnap — Core/ModulationMatrix.h
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
 * Thread safety: apply() reads routes_/activeCount_ without a lock.
 * Route management methods must only be called from the message thread.
 * A future improvement could add a double-buffer swap for lock-free updates;
 * for now the contract mirrors ModulationState from ModulationTypes.h.
 */
#pragma once

#include "ModulationTypes.h"
#include <juce_core/juce_core.h>
#include <array>
#include <vector>
#include <memory>

namespace morphsnap {

class ModulationMatrix
{
public:
    static constexpr int MAX_ROUTES = 128;

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
     * Add a new route. Returns the routeId (index into routes_) or -1 if full.
     * Caller must validate that destParamIndex < maxParamCount.
     */
    int  addRoute(ModSourceId source, int destParamIndex, float depth);

    /**
     * Disable and free a route by its routeId.
     * No-op if routeId is out of range or already inactive.
     */
    void removeRoute(int routeId);

    /** Update depth for an existing route. */
    void setRouteDepth(int routeId, float depth);

    /** Enable or disable a route without removing it. */
    void setRouteEnabled(int routeId, bool enabled);

    /** Remove all routes. */
    void clearAll();

    // ── Query (message thread) ────────────────────────────────────────────────

    /** Returns the number of currently assigned (enabled or disabled) routes. */
    int getActiveRouteCount() const { return activeCount_; }

    /**
     * Return a copy of the route at routeId.
     * Throws std::out_of_range if routeId is invalid.
     */
    const ModRoute& getRoute(int routeId) const;

    // ── Serialization ─────────────────────────────────────────────────────────

    /** Serialise all routes to an XML element. Caller owns the returned pointer. */
    std::unique_ptr<juce::XmlElement> toXml() const;

    /**
     * Restore routes from a previously serialised XML element.
     * Invalid or out-of-range entries are silently skipped.
     */
    void fromXml(const juce::XmlElement& xml);

private:
    std::array<ModRoute, MAX_ROUTES> routes_{};
    // Track which slots are occupied (enabled flag inside ModRoute serves as
    // "assigned" flag; destParamIndex == -1 means unassigned).
    int activeCount_   = 0;
    int maxParamCount_ = 0;

    /** Find the first unassigned slot; returns -1 if all slots are taken. */
    int findFreeSlot() const noexcept;

    /** Return true if routeId names an assigned route slot. */
    bool isValidRouteId(int routeId) const noexcept;
};

} // namespace morphsnap
