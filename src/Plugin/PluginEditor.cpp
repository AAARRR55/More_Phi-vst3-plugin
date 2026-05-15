/*
 * More-Phi — Advanced Parameter Morphing Engine
 * PluginEditor.cpp — Main Editor Window (V2 Tabbed Layout)
 */
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "UI/EngineTabPage.h"
#include "UI/ModulationMatrixPanel.h"
#include "UI/V2PresetBrowserPanel.h"

namespace more_phi {

MorePhiEditor::MorePhiEditor(MorePhiProcessor& p)
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
        paramToggleBtn_.setButtonText(
        paramPanelVisible_
            ? juce::String::charToString(0x25C2) + juce::String(" Params")
            : juce::String("Params ") + juce::String::charToString(0x25B8));
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

    aiChatPage_ = std::make_unique<AIChatPanel>(processor);
    addChildComponent(*aiChatPage_);

    // Default to Classic tab
    switchTab(V2TabBar::Classic);

    // M-16 FIX: Reduced from 30Hz to 15Hz — sufficient for UI updates,
    // reduces CPU overhead and message-thread contention.
    startTimerHz(15);
}

MorePhiEditor::~MorePhiEditor()
{
    closePluginWindow();
    setLookAndFeel(nullptr);
}

void MorePhiEditor::paint(juce::Graphics& g)
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
    g.setFont(juce::Font(juce::FontOptions("Segoe UI", 20.0f, juce::Font::bold)));
    g.drawText("More-Phi", titleArea.reduced(14, 0).removeFromLeft(160),
               juce::Justification::centredLeft);

    // Version
    g.setColour(lnf.textDim);
    g.setFont(juce::Font(juce::FontOptions("Segoe UI", 10.0f, juce::Font::plain)));
    g.drawText("v3.3.0", titleArea.reduced(14, 0).removeFromLeft(200),
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
    g.setFont(juce::Font(juce::FontOptions("Segoe UI", 9.0f, juce::Font::plain)));
    g.drawText("OUT", meterArea.translated(-28, 0).withWidth(24),
               juce::Justification::centredRight);

    // Title bar border
    g.setColour(lnf.borderColour);
    g.drawLine(0, 44, static_cast<float>(getWidth()), 44, 1.0f);
}

void MorePhiEditor::resized()
{
    auto area = getLocalBounds();

    // Title bar (painted)
    area.removeFromTop(44);

    // Parameter panel (right side, togglable)
    const int paramWidth = paramPanelVisible_ ? 320 : 0;
    if (paramPanelVisible_)
        paramPanel.setBounds(area.removeFromRight(paramWidth));

    // Plugin browser row (FlexBox row)
    {
        auto browserRow = area.removeFromTop(38);
        juce::FlexBox fb;
        fb.flexDirection = juce::FlexBox::Direction::row;
        fb.items.add(juce::FlexItem(pluginBrowser).withFlex(1));
        fb.items.add(juce::FlexItem(openPluginBtn_).withWidth(110).withMargin(2));
        fb.items.add(juce::FlexItem(paramToggleBtn_).withWidth(80).withMargin(2));
        fb.performLayout(browserRow);
    }

    // ── Bottom: AI status bar ──────────────────────────────────────────────────
    auto bottomBar = area.removeFromBottom(32);
    aiPanel.setBounds(bottomBar);

    // ── Tab bar ────────────────────────────────────────────────────────────────
    // Sits between the pad area and tab content
    // We need to calculate from the bottom up first to know where the tab content goes

    // Tab content area (210px — same height as original V1 bottom section)
    constexpr int tabContentHeight = 210;
    auto tabContent = area.removeFromBottom(tabContentHeight);

    // 3px gap between tab content and tab bar (prevents accent bleed)
    area.removeFromBottom(3);

    // Tab bar (28px, above gap)
    auto tabBarArea = area.removeFromBottom(28);
    tabBar_.setBounds(tabBarArea);

    // ── Main area: Snap fader + MorphPad (FlexBox row) ──────────────────────────
    {
        juce::FlexBox mainRow;
        mainRow.flexDirection = juce::FlexBox::Direction::row;
        mainRow.items.add(juce::FlexItem(snapFader)
            .withWidth(52.0f)
            .withMargin(juce::FlexItem::Margin(10, 0, 10, 6)));
        mainRow.items.add(juce::FlexItem(morphPad)
            .withFlex(1)
            .withMargin(8));
        mainRow.performLayout(area);
        snapshotRing.setBounds(morphPad.getBounds());
    }

    // ── Tab content layout (FlexBox) ───────────────────────────────────────────
    if (activeTab_ == V2TabBar::Classic)
    {
        juce::FlexBox classic;
        classic.flexDirection = juce::FlexBox::Direction::column;
        classic.items.add(juce::FlexItem(controlStrip).withHeight(60.0f));
        classic.items.add(juce::FlexItem(modeBar).withHeight(32.0f));
        classic.items.add(juce::FlexItem(macroStrip).withHeight(60.0f));
        classic.items.add(juce::FlexItem(breedingPanel).withHeight(48.0f));
        classic.items.add(juce::FlexItem().withFlex(1)); // padding
        classic.performLayout(tabContent);
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

    // AI chat tab
    if (aiChatPage_)
        aiChatPage_->setBounds(tabContent);
}

void MorePhiEditor::switchTab(int tabIndex)
{
    activeTab_ = tabIndex;

    setClassicTabVisible(tabIndex == V2TabBar::Classic);
    setEngineTabVisible(tabIndex == V2TabBar::Engine);
    setModulationTabVisible(tabIndex == V2TabBar::Modulation);
    setPresetsTabVisible(tabIndex == V2TabBar::Presets);
    setAITabVisible(tabIndex == V2TabBar::AI);

    resized();
    // Only repaint the content area below title bar and browser row, not the full editor
    repaint(0, 44, getWidth(), getHeight() - 44);
}

void MorePhiEditor::setClassicTabVisible(bool visible)
{
    controlStrip.setVisible(visible);
    modeBar.setVisible(visible);
    macroStrip.setVisible(visible);
    breedingPanel.setVisible(visible);
}

void MorePhiEditor::setEngineTabVisible(bool visible)
{
    if (enginePage_)
        enginePage_->setVisible(visible);
}

void MorePhiEditor::setModulationTabVisible(bool visible)
{
    if (modulationPage_)
        modulationPage_->setVisible(visible);
}

void MorePhiEditor::setPresetsTabVisible(bool visible)
{
    if (presetPage_)
        presetPage_->setVisible(visible);
}

void MorePhiEditor::setAITabVisible(bool visible)
{
    if (aiChatPage_)
        aiChatPage_->setVisible(visible);
}

void MorePhiEditor::timerCallback()
{
    // M-3 FIX: Only repaint title bar when RMS level visually changes
    float currentRms = processor.getRmsLevel();
    float dbLevel = juce::jlimit(0.0f, 1.0f,
        (juce::Decibels::gainToDecibels(currentRms) + 60.0f) / 60.0f);
    if (std::abs(dbLevel - lastDbLevel_) > 0.02f)
    {
        lastDbLevel_ = dbLevel;
        repaint(0, 0, getWidth(), 44);
    }
}

} // namespace more_phi

void more_phi::MorePhiEditor::openPluginWindow()
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());

    if (hostedWindow_) return;  // Already open

    auto* plugin = processor.getHostManager().getPlugin();
    if (!plugin) return;

    juce::Component::SafePointer<MorePhiEditor> safeThis(this);
    hostedWindow_ = std::make_unique<HostedPluginWindow>(
        plugin,
        [safeThis]() { if (safeThis) safeThis->closePluginWindow(); }  // on-close callback
    );
}

void more_phi::MorePhiEditor::closePluginWindow()
{
    hostedWindow_.reset();
}
