/*
 * MorphSnap — UI/EngineTabPage.h
 * Container component that arranges the three V2 engine sub-panels
 * (SpectralControlPanel, GranularControlPanel, HybridBlendPanel) inside
 * the "Engine" tab of the main editor.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>

namespace morphsnap {

// Forward declarations — full types resolved in EngineTabPage.cpp
class MorphSnapProcessor;
class SpectralControlPanel;
class GranularControlPanel;
class HybridBlendPanel;

class EngineTabPage : public juce::Component
{
public:
    explicit EngineTabPage(MorphSnapProcessor& proc);
    ~EngineTabPage() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    MorphSnapProcessor& proc_;

    // Sub-panels owned by this container
    std::unique_ptr<SpectralControlPanel> spectralPanel_;
    std::unique_ptr<GranularControlPanel> granularPanel_;
    std::unique_ptr<HybridBlendPanel>     blendPanel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EngineTabPage)
};

} // namespace morphsnap
