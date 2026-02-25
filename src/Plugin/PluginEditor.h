/*
 * MorphSnap — Advanced Parameter Morphing Engine
 * PluginEditor.h — Main Editor Window
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "UI/MorphSnapLookAndFeel.h"
#include "UI/MorphPad.h"
#include "UI/SnapFader.h"
#include "UI/SnapshotRing.h"
#include "UI/PluginBrowserPanel.h"
#include "UI/MacroKnobStrip.h"
#include "UI/AIStatusPanel.h"
#include "UI/BreedingPanel.h"
#include "UI/ModeBar.h"
#include "UI/ParameterMapPanel.h"
#include "UI/BottomControlStrip.h"
#include "UI/HostedPluginWindow.h"

namespace morphsnap {

class MorphSnapProcessor;

class MorphSnapEditor : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    explicit MorphSnapEditor(MorphSnapProcessor&);
    ~MorphSnapEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    MorphSnapProcessor& processor;
    MorphSnapLookAndFeel lnf;

    // UI components
    MorphPad          morphPad;
    SnapFader         snapFader;
    SnapshotRing      snapshotRing;
    PluginBrowserPanel pluginBrowser;
    MacroKnobStrip    macroStrip;
    AIStatusPanel     aiPanel;
    BreedingPanel     breedingPanel;
    ModeBar           modeBar;
    ParameterMapPanel paramPanel;
    BottomControlStrip controlStrip;

    // Parameter panel toggle
    juce::TextButton paramToggleBtn_{"Params ▸"};
    bool paramPanelVisible_ = false;

    // Hosted plugin window (detached)
    std::unique_ptr<HostedPluginWindow> hostedWindow_;
    juce::TextButton openPluginBtn_{"Open Plugin UI"};
    void openPluginWindow();
    void closePluginWindow();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MorphSnapEditor)
};

} // namespace morphsnap
