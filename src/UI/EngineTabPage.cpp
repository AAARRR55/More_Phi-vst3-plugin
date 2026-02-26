/*
 * MorphSnap — UI/EngineTabPage.cpp
 * Layout container for the Engine tab.
 *
 * Layout (180px total height):
 *
 *   ┌─────────────────────────────────────────────────────────┐
 *   │  HybridBlendPanel  (full width, 56px)                   │
 *   ├──────────────────────────┬──────────────────────────────┤
 *   │  SpectralControlPanel    │  GranularControlPanel        │
 *   │  (50% width, ~120px)     │  (50% width, ~120px)         │
 *   └──────────────────────────┴──────────────────────────────┘
 */
#include "EngineTabPage.h"
#include "SpectralControlPanel.h"
#include "GranularControlPanel.h"
#include "HybridBlendPanel.h"
#include "Plugin/PluginProcessor.h"

namespace morphsnap {

// ── Constants ────────────────────────────────────────────────────────────────

static constexpr int kBlendPanelHeight = 56;
static constexpr int kGapBelowBlend    =  2;
static constexpr int kGapBetweenSides  =  2;

// ── Constructor / Destructor ─────────────────────────────────────────────────

EngineTabPage::EngineTabPage(MorphSnapProcessor& proc)
    : proc_(proc)
    , spectralPanel_(std::make_unique<SpectralControlPanel>(proc_))
    , granularPanel_(std::make_unique<GranularControlPanel>(proc_))
    , blendPanel_   (std::make_unique<HybridBlendPanel>    (proc_))
{
    addAndMakeVisible(*blendPanel_);
    addAndMakeVisible(*spectralPanel_);
    addAndMakeVisible(*granularPanel_);
}

EngineTabPage::~EngineTabPage() = default;

// ── Paint ─────────────────────────────────────────────────────────────────────

void EngineTabPage::paint(juce::Graphics& g)
{
    // Subtle surface fill — matches the rest of the editor's dark theme.
    g.setColour(juce::Colour(0xff16213e));
    g.fillRect(getLocalBounds());
}

// ── Layout ────────────────────────────────────────────────────────────────────

void EngineTabPage::resized()
{
    auto area = getLocalBounds();

    // Top row: HybridBlendPanel — full width, fixed height.
    blendPanel_->setBounds(area.removeFromTop(kBlendPanelHeight));
    area.removeFromTop(kGapBelowBlend);

    // Bottom row: Spectral (left) and Granular (right), split evenly.
    const int halfWidth = (area.getWidth() - kGapBetweenSides) / 2;

    spectralPanel_->setBounds(area.removeFromLeft(halfWidth));
    area.removeFromLeft(kGapBetweenSides);
    granularPanel_->setBounds(area);
}

} // namespace morphsnap
