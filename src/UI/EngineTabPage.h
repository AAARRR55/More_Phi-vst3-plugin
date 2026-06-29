/*
 * More-Phi — UI/EngineTabPage.h
 * Container component that arranges the five V2 engine sub-panels
 * (HybridBlendPanel, PerformancePanel, DriftControlPanel, SpectralControlPanel,
 * GranularControlPanel) inside the "Engine" tab of the main editor.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>

namespace more_phi {

// Forward declarations — full types resolved in EngineTabPage.cpp
class MorePhiProcessor;
class SpectralControlPanel;
class GranularControlPanel;
class HybridBlendPanel;
class PerformancePanel;
class DriftControlPanel;

class EngineTabPage : public juce::Component
{
public:
    explicit EngineTabPage(MorePhiProcessor& proc);
    ~EngineTabPage() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    MorePhiProcessor& proc_;

    // Sub-panels owned by this container
    std::unique_ptr<SpectralControlPanel> spectralPanel_;
    std::unique_ptr<GranularControlPanel> granularPanel_;
    std::unique_ptr<HybridBlendPanel>     blendPanel_;
    std::unique_ptr<PerformancePanel>     perfPanel_;
    std::unique_ptr<DriftControlPanel>    driftPanel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EngineTabPage)
};

} // namespace more_phi
