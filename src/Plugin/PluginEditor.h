/*
 * MorphSnap — Advanced Parameter Morphing Engine
 * PluginEditor.h — Main Editor Window (V2 Tabbed Layout)
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
#include "UI/V2TabBar.h"

namespace morphsnap {

class MorphSnapProcessor;

// Forward declarations for V2 tab pages
class EngineTabPage;
class ModulationMatrixPanel;
class V2PresetBrowserPanel;

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
    void switchTab(int tabIndex);

    MorphSnapProcessor& processor;
    MorphSnapLookAndFeel lnf;

    // ── V1 UI components (Classic tab) ─────────────────────────────────────────
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
    juce::TextButton paramToggleBtn_{"Params \xe2\x96\xb8"};
    bool paramPanelVisible_ = false;

    // Hosted plugin window (detached)
    std::unique_ptr<HostedPluginWindow> hostedWindow_;
    juce::TextButton openPluginBtn_{"Open Plugin UI"};
    void openPluginWindow();
    void closePluginWindow();

    // ── V2 Tab System ──────────────────────────────────────────────────────────
    V2TabBar tabBar_;
    int activeTab_ = V2TabBar::Classic;

    // V2 tab pages (created lazily)
    std::unique_ptr<EngineTabPage>        enginePage_;
    std::unique_ptr<ModulationMatrixPanel> modulationPage_;
    std::unique_ptr<V2PresetBrowserPanel>  presetPage_;

    // Classic tab child components (grouped for show/hide)
    void setClassicTabVisible(bool visible);
    void setEngineTabVisible(bool visible);
    void setModulationTabVisible(bool visible);
    void setPresetsTabVisible(bool visible);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MorphSnapEditor)
};

} // namespace morphsnap
