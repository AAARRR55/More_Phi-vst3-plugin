/*
 * MorphSnap — UI/ModulationMatrixPanel.h
 * Modulation tab panel: route list (left) + LFO controls (right).
 *
 * Layout: 180px tall, two sections side by side.
 *   Left  (60%): scrollable route list with source/dest/depth per route.
 *   Right (40%): four LFO mini-panels (shape + rate).
 *
 * A 5 Hz timer keeps the displayed route list in sync with engine state
 * (routes may be added/removed via MCP or automation between repaints).
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <memory>

namespace morphsnap {

class MorphSnapProcessor;

// ---------------------------------------------------------------------------
// RouteRow — one 24px-tall row inside the route list
// ---------------------------------------------------------------------------

class RouteRow final : public juce::Component
{
public:
    RouteRow(int routeId, MorphSnapProcessor& proc);

    void resized() override;

    // Populate widgets from the engine's current route data.
    void refresh();

    // Rebuild the destination combo from the current ParameterBridge state.
    // Call once after construction (and again if the hosted plugin changes).
    void rebuildDestCombo();

    int getRouteId() const noexcept { return routeId_; }

private:
    void onEnabledToggled();
    void onSourceChanged();
    void onDestChanged();
    void onDepthChanged();

    // Prevent recursive callbacks while refreshing from engine state.
    bool syncing_ = false;

    int routeId_;
    MorphSnapProcessor& proc_;

    juce::ToggleButton  enabledToggle_;
    juce::ComboBox      sourceCombo_;
    juce::ComboBox      destCombo_;
    juce::Slider        depthKnob_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RouteRow)
};

// ---------------------------------------------------------------------------
// LFOPanel — compact mini-panel for one LFO (shape + rate)
// ---------------------------------------------------------------------------

class LFOPanel final : public juce::Component
{
public:
    LFOPanel(int lfoIndex, MorphSnapProcessor& proc);

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void onShapeChanged();
    void onRateChanged();

    int lfoIndex_;
    MorphSnapProcessor& proc_;

    juce::Label    label_;
    juce::ComboBox shapeCombo_;
    juce::Slider   rateKnob_;
    juce::Label    rateLabel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LFOPanel)
};

// ---------------------------------------------------------------------------
// ModulationMatrixPanel — top-level panel for the "Modulation" tab
// ---------------------------------------------------------------------------

class ModulationMatrixPanel final : public juce::Component,
                                    private juce::Timer
{
public:
    explicit ModulationMatrixPanel(MorphSnapProcessor& proc);
    ~ModulationMatrixPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    // juce::Timer — 5 Hz sync
    void timerCallback() override;

    // Rebuild RouteRow list to match engine state.
    void rebuildRouteRows();

    // Refresh existing rows without rebuilding.
    void refreshRouteRows();

    // Update the "Routes: N/128" label.
    void updateRouteCountLabel();

    // Button callbacks
    void onAddRoute();
    void onRemoveRoute();
    void onClearAll();

    // ---------------------------------------------------------------------------
    // Left section — route list
    // ---------------------------------------------------------------------------

    juce::Label    routeListHeader_;
    juce::Viewport routeViewport_;
    juce::Component routeContainer_;           // lives inside the viewport
    std::vector<std::unique_ptr<RouteRow>> routeRows_;

    juce::TextButton addRouteBtn_    { "Add Route" };
    juce::TextButton removeRouteBtn_ { "Remove"    };
    juce::TextButton clearAllBtn_    { "Clear All" };
    juce::Label      routeCountLabel_;

    // Currently selected route (for Remove).
    int selectedRouteId_ = -1;

    // ---------------------------------------------------------------------------
    // Right section — LFO controls
    // ---------------------------------------------------------------------------

    juce::Label lfoHeader_;
    std::array<std::unique_ptr<LFOPanel>, 4> lfoPanels_;

    // ---------------------------------------------------------------------------
    // Processor reference
    // ---------------------------------------------------------------------------

    MorphSnapProcessor& proc_;

    // Last known route count — used to detect external route changes.
    int cachedRouteCount_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModulationMatrixPanel)
};

} // namespace morphsnap
