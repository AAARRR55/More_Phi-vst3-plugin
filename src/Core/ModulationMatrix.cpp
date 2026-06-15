/*
 * More-Phi — Core/ModulationMatrix.cpp
 * Route-based modulation matrix: source values → parameter delta accumulation.
 *
 * apply() inner loop:
 *   for each enabled route:
 *     delta = sourceValues[source] * depth
 *     output[dest] = clamp(output[dest] + delta, 0.0, 1.0)
 *
 * destParamIndex == -1 marks an unassigned slot (see ModulationTypes.h).
 *
 * Double-buffer protocol:
 *   All mutations operate on buffers_[writeIndex_], then call publishAndMirror()
 *   to atomically make the updated state visible to the audio thread and copy
 *   it into the new write buffer for subsequent edits.
 */
#include "ModulationMatrix.h"
#include <algorithm>
#include <stdexcept>

namespace more_phi {

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void ModulationMatrix::prepare(int maxParamCount) noexcept
{
    maxParamCount_ = maxParamCount > 0 ? maxParamCount : 0;
}

void ModulationMatrix::reset() noexcept
{
    const juce::SpinLock::ScopedLockType lock(writeLock_);
    WriteScope ws(*this);
    resetLocked();
}

void ModulationMatrix::resetLocked() noexcept
{
    // Reset both buffers so neither contains stale data.
    for (auto& buf : buffers_)
    {
        for (auto& r : buf.routes)
        {
            r.source         = ModSourceId::LFO_1;
            r.destParamIndex = -1;
            r.depth          = 0.0f;
            r.enabled        = false;
        }
        buf.assignedCount = 0;
    }
    // After reset, audio thread should read buffer 0; message thread writes to buffer 1.
    readIndex_.store(0, std::memory_order_release);
    writeIndex_ = 1;
}

// ── Audio-thread apply ────────────────────────────────────────────────────────

void ModulationMatrix::apply(
    const std::array<float, static_cast<int>(ModSourceId::NUM_SOURCES)>& sourceValues,
    std::vector<float>& output) noexcept
{
    const int paramCount = static_cast<int>(output.size());
    if (paramCount == 0) return;

    // MODULATION-1 FIX: seqlock read. Snapshot the active route buffer to a
    // local copy so a concurrent publishAndMirror() (which mirrors into the
    // buffer a reader that captured the previous readIndex_ may still hold)
    // cannot tear the routes mid-iteration. Retry if seq changed parity during
    // the copy. Writers are serialized by writeLock_, so this is the standard
    // single-writer seqlock; a torn read is impossible regardless of timing.
    RouteBuffer local{};
    bool snapshotOk = false;
    for (int attempt = 0; attempt < kMaxReadRetries; ++attempt)
    {
        const uint32_t s1 = seq_.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0u) continue;  // writer in progress

        const int idx = readIndex_.load(std::memory_order_acquire);
        local = buffers_[idx];          // full RouteBuffer copy (~2 KB, trivially copyable)

        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t s2 = seq_.load(std::memory_order_acquire);
        if (s1 == s2) { snapshotOk = true; break; }
    }

    if (!snapshotOk || local.assignedCount == 0) return;

    // Apply from the local snapshot — no concurrent writer can touch it.
    for (int i = 0; i < MAX_ROUTES; ++i)
    {
        const ModRoute& r = local.routes[i];

        // Skip unassigned or disabled routes
        if (!r.enabled || r.destParamIndex < 0 || r.destParamIndex >= paramCount)
            continue;

        const int srcIdx = static_cast<int>(r.source);
        if (srcIdx < 0 || srcIdx >= static_cast<int>(ModSourceId::NUM_SOURCES))
            continue;

        const float srcValue = sourceValues[static_cast<size_t>(srcIdx)];
        const float delta    = srcValue * r.depth;

        output[static_cast<size_t>(r.destParamIndex)] =
            std::clamp(output[static_cast<size_t>(r.destParamIndex)] + delta, 0.0f, 1.0f);
    }
}

// ── Route management helpers (message thread) ─────────────────────────────────

int ModulationMatrix::findFreeSlot() const noexcept
{
    const RouteBuffer& buf = buffers_[writeIndex_];
    for (int i = 0; i < MAX_ROUTES; ++i)
    {
        if (buf.routes[i].destParamIndex == -1)
            return i;
    }
    return -1;
}

bool ModulationMatrix::isValidRouteId(int routeId) const noexcept
{
    if (routeId < 0 || routeId >= MAX_ROUTES) return false;
    return buffers_[writeIndex_].routes[routeId].destParamIndex != -1;
}

void ModulationMatrix::publishAndMirror() noexcept
{
    // 1. Publish: make writeIndex_ the new read buffer.
    readIndex_.store(writeIndex_, std::memory_order_release);
    // 2. Flip writeIndex_ to the other slot.
    //    After this line, (1 - writeIndex_) is the index that was just published.
    writeIndex_ = 1 - writeIndex_;
    // 3. Mirror: copy the just-published buffer into the new write slot so the
    //    next mutation starts from the current committed state.
    //    (1 - writeIndex_) is the published index because we already flipped above.
    buffers_[writeIndex_] = buffers_[1 - writeIndex_];
}

// ── Route management (message thread) ────────────────────────────────────────

int ModulationMatrix::addRoute(ModSourceId source, int destParamIndex, float depth)
{
    if (destParamIndex < 0 || destParamIndex >= maxParamCount_)
        return -1;

    const juce::SpinLock::ScopedLockType lock(writeLock_);
    WriteScope ws(*this);

    const int slot = findFreeSlot();
    if (slot < 0) return -1;

    RouteBuffer& buf = buffers_[writeIndex_];
    buf.routes[slot].source         = source;
    buf.routes[slot].destParamIndex = destParamIndex;
    buf.routes[slot].depth          = std::clamp(depth, -1.0f, 1.0f);
    buf.routes[slot].enabled        = true;
    ++buf.assignedCount;

    publishAndMirror();
    return slot;
}

void ModulationMatrix::removeRoute(int routeId)
{
    const juce::SpinLock::ScopedLockType lock(writeLock_);
    if (!isValidRouteId(routeId)) return;
    WriteScope ws(*this);

    RouteBuffer& buf = buffers_[writeIndex_];
    buf.routes[routeId].destParamIndex = -1;
    buf.routes[routeId].enabled        = false;
    buf.routes[routeId].depth          = 0.0f;
    if (buf.assignedCount > 0) --buf.assignedCount;

    publishAndMirror();
}

void ModulationMatrix::setRouteDepth(int routeId, float depth)
{
    const juce::SpinLock::ScopedLockType lock(writeLock_);
    if (!isValidRouteId(routeId)) return;
    WriteScope ws(*this);
    buffers_[writeIndex_].routes[routeId].depth = std::clamp(depth, -1.0f, 1.0f);
    publishAndMirror();
}

void ModulationMatrix::setRouteEnabled(int routeId, bool enabled)
{
    const juce::SpinLock::ScopedLockType lock(writeLock_);
    if (!isValidRouteId(routeId)) return;
    WriteScope ws(*this);
    buffers_[writeIndex_].routes[routeId].enabled = enabled;
    publishAndMirror();
}

void ModulationMatrix::clearAll()
{
    reset();
}

const ModRoute& ModulationMatrix::getRoute(int routeId) const
{
    if (routeId < 0 || routeId >= MAX_ROUTES)
        throw std::out_of_range("ModulationMatrix::getRoute — routeId out of range");
    return buffers_[writeIndex_].routes[routeId];
}

// ── Serialization ─────────────────────────────────────────────────────────────

std::unique_ptr<juce::XmlElement> ModulationMatrix::toXml() const
{
    auto xml = std::make_unique<juce::XmlElement>("ModulationMatrix");
    xml->setAttribute("version", 1);
    xml->setAttribute("maxParamCount", maxParamCount_);

    const RouteBuffer& buf = buffers_[writeIndex_];
    for (int i = 0; i < MAX_ROUTES; ++i)
    {
        const ModRoute& r = buf.routes[i];
        if (r.destParamIndex == -1) continue;  // unassigned

        auto* routeEl = xml->createNewChildElement("Route");
        routeEl->setAttribute("id",      i);
        routeEl->setAttribute("source",  static_cast<int>(r.source));
        routeEl->setAttribute("dest",    r.destParamIndex);
        routeEl->setAttribute("depth",   static_cast<double>(r.depth));
        routeEl->setAttribute("enabled", r.enabled ? 1 : 0);
    }

    return xml;
}

void ModulationMatrix::fromXml(const juce::XmlElement& xml)
{
    if (!xml.hasTagName("ModulationMatrix")) return;

    const juce::SpinLock::ScopedLockType lock(writeLock_);
    WriteScope ws(*this);

    resetLocked();

    RouteBuffer& buf = buffers_[writeIndex_];

    for (auto* routeEl : xml.getChildWithTagNameIterator("Route"))
    {
        const int id   = routeEl->getIntAttribute("id", -1);
        const int src  = routeEl->getIntAttribute("source", 0);
        const int dest = routeEl->getIntAttribute("dest", -1);
        const float depth   = static_cast<float>(routeEl->getDoubleAttribute("depth", 0.0));
        const bool  enabled = routeEl->getIntAttribute("enabled", 1) != 0;

        if (id < 0 || id >= MAX_ROUTES)              continue;
        if (dest < 0 || dest >= maxParamCount_)       continue;
        if (src  < 0 || src  >= static_cast<int>(ModSourceId::NUM_SOURCES)) continue;

        buf.routes[id].source         = static_cast<ModSourceId>(src);
        buf.routes[id].destParamIndex = dest;
        buf.routes[id].depth          = std::clamp(depth, -1.0f, 1.0f);
        buf.routes[id].enabled        = enabled;
        ++buf.assignedCount;
    }

    publishAndMirror();
}

} // namespace more_phi
