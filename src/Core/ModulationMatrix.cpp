/*
 * MorphSnap — Core/ModulationMatrix.cpp
 * Route-based modulation matrix: source values → parameter delta accumulation.
 *
 * apply() inner loop:
 *   for each enabled route:
 *     delta = sourceValues[source] * depth
 *     output[dest] = clamp(output[dest] + delta, 0.0, 1.0)
 *
 * destParamIndex == -1 marks an unassigned slot (see ModulationTypes.h).
 */
#include "ModulationMatrix.h"
#include <algorithm>
#include <stdexcept>

namespace morphsnap {

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void ModulationMatrix::prepare(int maxParamCount) noexcept
{
    maxParamCount_ = maxParamCount > 0 ? maxParamCount : 0;
}

void ModulationMatrix::reset() noexcept
{
    for (auto& r : routes_)
    {
        r.source        = ModSourceId::LFO_1;
        r.destParamIndex= -1;
        r.depth         = 0.0f;
        r.enabled       = false;
    }
    activeCount_ = 0;
}

// ── Audio-thread apply ────────────────────────────────────────────────────────

void ModulationMatrix::apply(
    const std::array<float, static_cast<int>(ModSourceId::NUM_SOURCES)>& sourceValues,
    std::vector<float>& output) noexcept
{
    const int paramCount = static_cast<int>(output.size());
    if (paramCount == 0 || activeCount_ == 0) return;

    for (int i = 0; i < MAX_ROUTES; ++i)
    {
        const ModRoute& r = routes_[i];

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

// ── Route management ─────────────────────────────────────────────────────────

int ModulationMatrix::findFreeSlot() const noexcept
{
    for (int i = 0; i < MAX_ROUTES; ++i)
    {
        if (routes_[i].destParamIndex == -1)
            return i;
    }
    return -1;
}

bool ModulationMatrix::isValidRouteId(int routeId) const noexcept
{
    if (routeId < 0 || routeId >= MAX_ROUTES) return false;
    return routes_[routeId].destParamIndex != -1;
}

int ModulationMatrix::addRoute(ModSourceId source, int destParamIndex, float depth)
{
    if (destParamIndex < 0 || destParamIndex >= maxParamCount_)
        return -1;

    const int slot = findFreeSlot();
    if (slot < 0) return -1;

    routes_[slot].source         = source;
    routes_[slot].destParamIndex = destParamIndex;
    routes_[slot].depth          = std::clamp(depth, -1.0f, 1.0f);
    routes_[slot].enabled        = true;
    ++activeCount_;
    return slot;
}

void ModulationMatrix::removeRoute(int routeId)
{
    if (!isValidRouteId(routeId)) return;

    routes_[routeId].destParamIndex = -1;
    routes_[routeId].enabled        = false;
    routes_[routeId].depth          = 0.0f;
    if (activeCount_ > 0) --activeCount_;
}

void ModulationMatrix::setRouteDepth(int routeId, float depth)
{
    if (!isValidRouteId(routeId)) return;
    routes_[routeId].depth = std::clamp(depth, -1.0f, 1.0f);
}

void ModulationMatrix::setRouteEnabled(int routeId, bool enabled)
{
    if (!isValidRouteId(routeId)) return;
    routes_[routeId].enabled = enabled;
}

void ModulationMatrix::clearAll()
{
    reset();
}

const ModRoute& ModulationMatrix::getRoute(int routeId) const
{
    if (routeId < 0 || routeId >= MAX_ROUTES)
        throw std::out_of_range("ModulationMatrix::getRoute — routeId out of range");
    return routes_[routeId];
}

// ── Serialization ─────────────────────────────────────────────────────────────

std::unique_ptr<juce::XmlElement> ModulationMatrix::toXml() const
{
    auto xml = std::make_unique<juce::XmlElement>("ModulationMatrix");
    xml->setAttribute("version", 1);
    xml->setAttribute("maxParamCount", maxParamCount_);

    for (int i = 0; i < MAX_ROUTES; ++i)
    {
        const ModRoute& r = routes_[i];
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

    reset();

    forEachXmlChildElementWithTagName(xml, routeEl, "Route")
    {
        const int id   = routeEl->getIntAttribute("id", -1);
        const int src  = routeEl->getIntAttribute("source", 0);
        const int dest = routeEl->getIntAttribute("dest", -1);
        const float depth   = static_cast<float>(routeEl->getDoubleAttribute("depth", 0.0));
        const bool  enabled = routeEl->getIntAttribute("enabled", 1) != 0;

        if (id < 0 || id >= MAX_ROUTES)    continue;
        if (dest < 0 || dest >= maxParamCount_) continue;
        if (src  < 0 || src  >= static_cast<int>(ModSourceId::NUM_SOURCES)) continue;

        routes_[id].source         = static_cast<ModSourceId>(src);
        routes_[id].destParamIndex = dest;
        routes_[id].depth          = std::clamp(depth, -1.0f, 1.0f);
        routes_[id].enabled        = enabled;
        ++activeCount_;
    }
}

} // namespace morphsnap
