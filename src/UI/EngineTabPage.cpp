/*
 * More-Phi — UI/EngineTabPage.cpp
 * Layout container for the Engine tab.
 *
 * Layout (top → bottom, full width unless noted):
 *
 *   ┌─────────────────────────────────────────────────────────┐
 *   │  HybridBlendPanel     (full width, 56px)                │
 *   ├─────────────────────────────────────────────────────────┤
 *   │  PerformancePanel     (full width, 36px)                │
 *   ├─────────────────────────────────────────────────────────┤
 *   │  DriftControlPanel    (full width, 60px)                │
 *   ├──────────────────────────┬──────────────────────────────┤
 *   │  SpectralControlPanel    │  GranularControlPanel        │
 *   │  (50% width)             │  (50% width)                 │
 *   └──────────────────────────┴──────────────────────────────┘
 */
#include "EngineTabPage.h"
#include "SpectralControlPanel.h"
#include "GranularControlPanel.h"
#include "HybridBlendPanel.h"
#include "PerformancePanel.h"
#include "DriftControlPanel.h"
#include "Plugin/PluginProcessor.h"

namespace more_phi {

// ── Constants ────────────────────────────────────────────────────────────────

static constexpr int kBlendPanelHeight = 56;
static constexpr int kGapBelowBlend    =  2;
static constexpr int kPerfPanelHeight  = 36;
static constexpr int kGapBelowPerf     =  2;
static constexpr int kDriftPanelHeight = 60;
static constexpr int kGapBelowDrift    =  2;
static constexpr int kGapBetweenSides  =  2;

// ── Constructor / Destructor ─────────────────────────────────────────────────

EngineTabPage::EngineTabPage(MorePhiProcessor& proc)
    : proc_(proc)
    , spectralPanel_(std::make_unique<SpectralControlPanel>(proc_))
    , granularPanel_(std::make_unique<GranularControlPanel>(proc_))
    , blendPanel_   (std::make_unique<HybridBlendPanel>    (proc_))
    , perfPanel_    (std::make_unique<PerformancePanel>  (proc_))
    , driftPanel_   (std::make_unique<DriftControlPanel>   (proc_))
{
    addAndMakeVisible(*blendPanel_);
    addAndMakeVisible(*perfPanel_);
    addAndMakeVisible(*driftPanel_);
    addAndMakeVisible(*spectralPanel_);
    addAndMakeVisible(*granularPanel_);
}

EngineTabPage::~EngineTabPage() = default;

// ── Paint ─────────────────────────────────────────────────────────────────────

void EngineTabPage::paint(juce::Graphics& g)
{
    // Subtle surface fill — matches the rest of the editor's dark theme.
    g.setColour(juce::Colour(0xff0d0d10));
    g.fillRect(getLocalBounds());
}

// ── Layout ────────────────────────────────────────────────────────────────────

void EngineTabPage::resized()
{
    auto area = getLocalBounds();
    const bool compact = area.getWidth() < 760;

    // Top row: HybridBlendPanel — full width, fixed height.
    blendPanel_->setBounds(area.removeFromTop(kBlendPanelHeight));
    area.removeFromTop(kGapBelowBlend);

    // Second row: PerformancePanel — opt-in CPU toggles, full width.
    perfPanel_->setBounds(area.removeFromTop(kPerfPanelHeight));
    area.removeFromTop(kGapBelowPerf);

    // Third row: DriftControlPanel — Drift physics-mode params, full width.
    driftPanel_->setBounds(area.removeFromTop(kDriftPanelHeight));
    area.removeFromTop(kGapBelowDrift);

    if (compact)
    {
        auto spectralArea = area.removeFromTop(area.getHeight() / 2);
        area.removeFromTop(kGapBetweenSides);
        spectralPanel_->setBounds(spectralArea);
        granularPanel_->setBounds(area);
        return;
    }

    // Bottom row: Spectral (left) and Granular (right) — FlexBox with equal flex
    juce::FlexBox bottom;
    bottom.flexDirection = juce::FlexBox::Direction::row;
    bottom.items.add(juce::FlexItem(*spectralPanel_).withFlex(1));
    bottom.items.add(juce::FlexItem().withWidth(static_cast<float>(kGapBetweenSides)));
    bottom.items.add(juce::FlexItem(*granularPanel_).withFlex(1));
    bottom.performLayout(area);
}

} // namespace more_phi
