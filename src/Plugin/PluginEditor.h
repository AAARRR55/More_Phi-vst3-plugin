/*
 * More-Phi — Advanced Parameter Morphing Engine
 * PluginEditor.h — Main Editor Window (V2 Tabbed Layout)
 */
#pragma once

#include <cstring>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "UI/MorePhiLookAndFeel.h"
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
#include "UI/AIChatPanel.h"
#include "UI/LicenseActivationOverlay.h"

namespace more_phi {

class MorePhiProcessor;

// Forward declarations for V2 tab pages
class EngineTabPage;
class ModulationMatrixPanel;
class V2PresetBrowserPanel;

class MorePhiEditor : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    explicit MorePhiEditor(MorePhiProcessor&);
    ~MorePhiEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

#if MORE_PHI_TEST_MODE
    void selectTabForTests(int tabIndex) { switchTab(tabIndex); }
    juce::Rectangle<int> getAIChatBoundsForTests() const { return aiChatPage_ != nullptr ? aiChatPage_->getBounds() : juce::Rectangle<int>(); }
    juce::Rectangle<int> getControlStripBoundsForTests() const { return controlStrip.getBounds(); }
#endif

private:
    void timerCallback() override;
    void switchTab(int tabIndex);

    MorePhiProcessor& processor;
    MorePhiLookAndFeel lnf;

    // AUDIT-FIX (accessibility): Without a TooltipWindow, setTooltip() calls
    // never display in most host contexts. Uses nullptr parent (desktop-native
    // window) so it doesn't become a child of the editor — which avoids the
    // headless test catching its empty (0×0) bounds. In a DAW this creates a
    // lightweight native popup, which works identically.
    juce::TooltipWindow tooltipWindow_ { nullptr, 700 };

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
    juce::TextButton paramToggleBtn_;
    bool paramPanelVisible_ = false;

    // Bypass button in title bar
    juce::TextButton bypassBtn_ { "Bypass" };

    // SonicMaster realtime neural mastering (preview) — toggle + live status.
    juce::ToggleButton sonicMasterToggle_ { "Neural Master (Preview)" };
    juce::Label        sonicMasterStatus_;
    void refreshSonicMasterStatus();

    // Hosted plugin window (detached)
    std::unique_ptr<HostedPluginWindow> hostedWindow_;
    juce::AudioPluginInstance* hostedWindowPlugin_ = nullptr; // pointer used at creation; only compared, never dereferenced after unload
    juce::TextButton openPluginBtn_{"Open Plugin UI"};
    juce::TextButton deactivateBtn_{"Deactivate License"};
    void openPluginWindow();
    void closePluginWindow();

    // ── V2 Tab System ──────────────────────────────────────────────────────────
    V2TabBar tabBar_;
    int activeTab_ = V2TabBar::Classic;

    // V2 tab pages (created lazily)
    std::unique_ptr<EngineTabPage>        enginePage_;
    std::unique_ptr<ModulationMatrixPanel> modulationPage_;
    std::unique_ptr<V2PresetBrowserPanel>  presetPage_;

    // AI chat tab page
    std::unique_ptr<AIChatPanel>           aiChatPage_;

    // Classic tab child components (grouped for show/hide)
    void setClassicTabVisible(bool visible);
    void setEngineTabVisible(bool visible);
    void setModulationTabVisible(bool visible);
    void setPresetsTabVisible(bool visible);
    void setAITabVisible(bool visible);

    float lastDbLevel_ = -1.0f;     // RMS meter throttle state (per-instance)
    float smoothedDbLevel_ = 0.0f;  // Eased OUT-meter level for smooth glide animation
    juce::Rectangle<int> sonicMasterRowBounds_;

    LicenseActivationOverlay licenseOverlay;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MorePhiEditor)
};

} // namespace more_phi
