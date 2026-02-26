/*
 * MorphSnap — Advanced Parameter Morphing Engine
 * PluginEditor.cpp — Main Editor Window (V2 Tabbed Layout)
 */
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "UI/EngineTabPage.h"
#include "UI/ModulationMatrixPanel.h"
#include "UI/V2PresetBrowserPanel.h"

namespace morphsnap {

MorphSnapEditor::MorphSnapEditor(MorphSnapProcessor& p)
    : AudioProcessorEditor(p),
      processor(p),
      morphPad(p),
      snapFader(p),
      snapshotRing(p),
      pluginBrowser(p),
      macroStrip(p),
      aiPanel(p),
      breedingPanel(p),
      modeBar(p),
      paramPanel(p),
      controlStrip(p)
{
    setLookAndFeel(&lnf);
    setSize(920, 760);
    setResizable(true, true);
    setResizeLimits(720, 600, 1600, 1120);

    // ── Always-visible components ──────────────────────────────────────────────
    addAndMakeVisible(morphPad);
    addAndMakeVisible(snapFader);
    addAndMakeVisible(snapshotRing);
    addAndMakeVisible(pluginBrowser);
    addAndMakeVisible(aiPanel);

    // ── Classic tab components ─────────────────────────────────────────────────
    addAndMakeVisible(macroStrip);
    addAndMakeVisible(breedingPanel);
    addAndMakeVisible(modeBar);
    addAndMakeVisible(controlStrip);

    // Parameter panel (initially hidden)
    addChildComponent(paramPanel);

    // Toggle button in plugin browser row
    paramToggleBtn_.onClick = [this]()
    {
        paramPanelVisible_ = !paramPanelVisible_;
        paramPanel.setVisible(paramPanelVisible_);
        paramToggleBtn_.setButtonText(paramPanelVisible_ ? "\xe2\x97\x82 Params" : "Params \xe2\x96\xb8");
        if (paramPanelVisible_)
            paramPanel.rebuildForPlugin();
        resized();
    };
    addAndMakeVisible(paramToggleBtn_);

    // Open Plugin UI button
    openPluginBtn_.onClick = [this]() { openPluginWindow(); };
    addAndMakeVisible(openPluginBtn_);

    // ── V2 Tab Bar ─────────────────────────────────────────────────────────────
    addAndMakeVisible(tabBar_);
    tabBar_.onTabChanged = [this](int tab) { switchTab(tab); };

    // ── V2 Tab Pages (lazy creation) ───────────────────────────────────────────
    enginePage_ = std::make_unique<EngineTabPage>(processor);
    addChildComponent(*enginePage_);

    modulationPage_ = std::make_unique<ModulationMatrixPanel>(processor);
    addChildComponent(*modulationPage_);

    presetPage_ = std::make_unique<V2PresetBrowserPanel>(processor);
    addChildComponent(*presetPage_);

    // Default to Classic tab
    switchTab(V2TabBar::Classic);

    startTimerHz(30);
}

MorphSnapEditor::~MorphSnapEditor()
{
    closePluginWindow();
    setLookAndFeel(nullptr);
}

void MorphSnapEditor::paint(juce::Graphics& g)
{
    g.fillAll(lnf.backgroundDark);

    // ── Title bar ────────────────────────────────────────────────────────────
    auto titleArea = getLocalBounds().removeFromTop(44);

    g.setGradientFill(juce::ColourGradient(
        lnf.surfaceColour.brighter(0.03f), 0, static_cast<float>(titleArea.getY()),
        lnf.surfaceColour.darker(0.02f),   0, static_cast<float>(titleArea.getBottom()), false));
    g.fillRect(titleArea);

    // Logo
    g.setColour(lnf.accentCoral);
    g.setFont(juce::Font(juce::FontOptions(20.0f, juce::Font::bold)));
    g.drawText("MorphSnap", titleArea.reduced(14, 0).removeFromLeft(160),
               juce::Justification::centredLeft);

    // Version
    g.setColour(lnf.textDim);
    g.setFont(juce::Font(juce::FontOptions(10.0f)));
    g.drawText("v2.0", titleArea.reduced(14, 0).removeFromLeft(200),
               juce::Justification::centredLeft);

    // RMS meter
    auto meterArea = titleArea.removeFromRight(120).reduced(10, 12);
    float rms = processor.getRmsLevel();
    float dbLevel = juce::jlimit(0.0f, 1.0f,
        (juce::Decibels::gainToDecibels(rms) + 60.0f) / 60.0f);

    g.setColour(lnf.padBackground);
    g.fillRoundedRectangle(meterArea.toFloat(), 3.0f);

    if (dbLevel > 0.0f)
    {
        auto fillArea = meterArea.toFloat().withWidth(meterArea.getWidth() * dbLevel);
        juce::Colour meterColour = dbLevel > 0.9f ? juce::Colour(0xffef4444) :
                                    dbLevel > 0.7f ? lnf.accentCoral :
                                                     lnf.accentGreen;
        g.setColour(meterColour);
        g.fillRoundedRectangle(fillArea, 3.0f);
    }

    g.setColour(lnf.textDim);
    g.setFont(juce::Font(juce::FontOptions(9.0f)));
    g.drawText("OUT", meterArea.translated(-28, 0).withWidth(24),
               juce::Justification::centredRight);

    // Title bar border
    g.setColour(lnf.borderColour);
    g.drawLine(0, 44, static_cast<float>(getWidth()), 44, 1.0f);
}

void MorphSnapEditor::resized()
{
    auto area = getLocalBounds();

    // Title bar (painted)
    area.removeFromTop(44);

    // Parameter panel (right side, togglable)
    const int paramWidth = paramPanelVisible_ ? 320 : 0;
    if (paramPanelVisible_)
        paramPanel.setBounds(area.removeFromRight(paramWidth));

    // Plugin browser row
    auto browserRow = area.removeFromTop(38);
    paramToggleBtn_.setBounds(browserRow.removeFromRight(80));
    openPluginBtn_.setBounds(browserRow.removeFromRight(110));
    pluginBrowser.setBounds(browserRow);

    // ── Bottom: AI status bar ──────────────────────────────────────────────────
    auto bottomBar = area.removeFromBottom(32);
    aiPanel.setBounds(bottomBar);

    // ── Tab bar ────────────────────────────────────────────────────────────────
    // Sits between the pad area and tab content
    // We need to calculate from the bottom up first to know where the tab content goes

    // Tab content area (210px — same height as original V1 bottom section)
    constexpr int tabContentHeight = 210;
    auto tabContent = area.removeFromBottom(tabContentHeight);

    // Tab bar (28px, above tab content)
    auto tabBarArea = area.removeFromBottom(28);
    tabBar_.setBounds(tabBarArea);

    // ── Main area: Snap fader + MorphPad ───────────────────────────────────────
    auto leftCol = area.removeFromLeft(52);
    snapFader.setBounds(leftCol.reduced(6, 10));

    auto padArea = area.reduced(8);
    morphPad.setBounds(padArea);
    snapshotRing.setBounds(padArea);

    // ── Tab content layout ─────────────────────────────────────────────────────
    // Classic tab: stack V1 controls in tabContent area
    if (activeTab_ == V2TabBar::Classic)
    {
        auto classicArea = tabContent;
        controlStrip.setBounds(classicArea.removeFromTop(48));
        modeBar.setBounds(classicArea.removeFromTop(32));
        macroStrip.setBounds(classicArea.removeFromTop(60));
        breedingPanel.setBounds(classicArea.removeFromTop(38));
        // Remaining space is padding
    }

    // Engine tab
    if (enginePage_)
        enginePage_->setBounds(tabContent);

    // Modulation tab
    if (modulationPage_)
        modulationPage_->setBounds(tabContent);

    // Presets tab
    if (presetPage_)
        presetPage_->setBounds(tabContent);
}

void MorphSnapEditor::switchTab(int tabIndex)
{
    activeTab_ = tabIndex;

    setClassicTabVisible(tabIndex == V2TabBar::Classic);
    setEngineTabVisible(tabIndex == V2TabBar::Engine);
    setModulationTabVisible(tabIndex == V2TabBar::Modulation);
    setPresetsTabVisible(tabIndex == V2TabBar::Presets);

    resized();
    repaint();
}

void MorphSnapEditor::setClassicTabVisible(bool visible)
{
    controlStrip.setVisible(visible);
    modeBar.setVisible(visible);
    macroStrip.setVisible(visible);
    breedingPanel.setVisible(visible);
}

void MorphSnapEditor::setEngineTabVisible(bool visible)
{
    if (enginePage_)
        enginePage_->setVisible(visible);
}

void MorphSnapEditor::setModulationTabVisible(bool visible)
{
    if (modulationPage_)
        modulationPage_->setVisible(visible);
}

void MorphSnapEditor::setPresetsTabVisible(bool visible)
{
    if (presetPage_)
        presetPage_->setVisible(visible);
}

void MorphSnapEditor::timerCallback()
{
    morphPad.repaint();
    repaint(0, 0, getWidth(), 44);  // RMS meter
}

} // namespace morphsnap

void morphsnap::MorphSnapEditor::openPluginWindow()
{
    if (hostedWindow_) return;  // Already open

    auto* plugin = processor.getHostManager().getPlugin();
    if (!plugin) return;

    hostedWindow_ = std::make_unique<HostedPluginWindow>(
        plugin,
        [this]() { closePluginWindow(); }  // on-close callback
    );
}

void morphsnap::MorphSnapEditor::closePluginWindow()
{
    hostedWindow_.reset();
}
