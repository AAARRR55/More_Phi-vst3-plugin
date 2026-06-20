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
      controlStrip(p),
      licenseOverlay(p)
{
    setLookAndFeel(&lnf);
    setSize(920, 760);
    setResizable(true, true);
    setResizeLimits(720, 600, 1600, 1120);

    paramToggleBtn_.setButtonText("Params >");
    paramToggleBtn_.setTooltip("Show or hide hosted plugin parameters.");
    openPluginBtn_.setTooltip("Open the hosted plugin editor in a separate window.");

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
            ? juce::String("< Params")
            : juce::String("Params >"));
        if (paramPanelVisible_)
            paramPanel.rebuildForPlugin();
        resized();
    };
    addAndMakeVisible(paramToggleBtn_);

    // Open Plugin UI button
    openPluginBtn_.onClick = [this]() { openPluginWindow(); };
    addAndMakeVisible(openPluginBtn_);

    // Deactivate License button
    deactivateBtn_.setTooltip("Deactivate and delete the current license key from this computer.");
    deactivateBtn_.onClick = [this]()
    {
        auto options = juce::MessageBoxOptions::makeOptionsOkCancel(
            juce::MessageBoxIconType::QuestionIcon,
            "Deactivate License",
            "Are you sure you want to deactivate this computer?\n\n"
            "This releases the activation seat on the license server and locks the "
            "plugin until a valid license key is entered again.",
            "Deactivate",
            "Cancel"
        );

        juce::Component::SafePointer<MorePhiEditor> safeThis(this);
        juce::AlertWindow::showAsync(options, [safeThis](int result)
        {
            if (safeThis == nullptr || result != 1)
                return;

            // Server-side deactivate (releases the seat) on a background thread,
            // then clear the local cert on the message thread. If the server is
            // unreachable we offer a force-local-clear so users are never
            // trapped unable to switch machines by a transient network failure.
            auto& processor = safeThis->processor;
            const auto activationId = processor.getLicenseManager().lastActivationId();

            juce::Thread::launch([safeThis, &processor, activationId]()
            {
                juce::String error;
                const bool serverOk = activationId.isNotEmpty()
                    && processor.getLicenseManager().deactivateActivation(activationId, &error);

                juce::MessageManager::callAsync([safeThis, serverOk, error]()
                {
                    if (safeThis == nullptr)
                        return;

                    if (serverOk)
                    {
                        safeThis->deactivateBtn_.setVisible(false);
                        safeThis->licenseOverlay.setVisible(true);
                        safeThis->licenseOverlay.toFront(false);
                        return;
                    }

                    // Server unreachable / failed — let the user choose whether to
                    // still clear locally (which would strand a server-side seat).
                    auto force = juce::MessageBoxOptions::makeOptionsOkCancel(
                        juce::MessageBoxIconType::WarningIcon,
                        "Server deactivation failed",
                        "The license server could not be reached to release this "
                        "activation seat:\n" + error + "\n\n"
                        "Clear the license on this computer anyway? The activation "
                        "may still count against your seat limit until it expires "
                        "or is released by support.",
                        "Clear locally anyway",
                        "Keep license");
                    juce::AlertWindow::showAsync(force, [safeThis](int r)
                    {
                        if (safeThis == nullptr || r != 1)
                            return;
                        juce::String localErr;
                        safeThis->processor.getLicenseManager().clearActivation(&localErr);
                        safeThis->deactivateBtn_.setVisible(false);
                        safeThis->licenseOverlay.setVisible(true);
                        safeThis->licenseOverlay.toFront(false);
                    });
                });
            });
        });
    };
    addAndMakeVisible(deactivateBtn_);

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

    // Licensing overlay
    addChildComponent(licenseOverlay);
    const bool isLicensed = processor.getLicenseRuntimeState().premiumFeaturesEnabled.load(std::memory_order_relaxed);
    licenseOverlay.setVisible(!isLicensed);
    deactivateBtn_.setVisible(isLicensed);
    if (!isLicensed)
        licenseOverlay.toFront(false);

    // M-16 FIX: Reduced from 30Hz to 15Hz — sufficient for UI updates,
    // reduces CPU overhead and message-thread contention.
    startTimerHz(30);  // 30 FPS for smooth meter-glide animation
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
    auto titleArea = getLocalBounds().removeFromTop(48);

    g.setGradientFill(juce::ColourGradient(
        lnf.surfaceColour.brighter(0.03f), 0, static_cast<float>(titleArea.getY()),
        lnf.surfaceColour.darker(0.02f),   0, static_cast<float>(titleArea.getBottom()), false));
    g.fillRect(titleArea);

    auto titleTextArea = titleArea.reduced(14, 0);

    // Logo
    g.setColour(lnf.accentCoral);
    g.setFont(lnf.makeScaledFont(20.0f, juce::Font::bold));
    g.drawText("More-Phi", titleTextArea.removeFromLeft(96),
               juce::Justification::centredLeft, false);

    // Version
    g.setColour(lnf.textDim);
    g.setFont(lnf.makeScaledFont(10.0f));
    g.drawText("v3.3.0", titleTextArea.removeFromLeft(72).translated(0, 1),
               juce::Justification::centredLeft, false);

    // RMS meter — uses the eased level updated in timerCallback() for a smooth glide
    auto meterArea = titleArea.removeFromRight(120).reduced(10, 14);
    float dbLevel = juce::jlimit(0.0f, 1.0f, smoothedDbLevel_);

    g.setColour(lnf.padBackground);
    g.fillRoundedRectangle(meterArea.toFloat(), 3.0f);

    if (dbLevel > 0.0f)
    {
        auto fillArea = meterArea.toFloat().withWidth(meterArea.getWidth() * dbLevel);
        juce::Colour meterColour = dbLevel > 0.9f ? juce::Colour(0xffef4444) :
                                    dbLevel > 0.7f ? lnf.accentCoral :
                                                     lnf.accentGreen;
        // Neon glow underneath the fill (matches the mockup's glowing meter).
        g.setColour(meterColour.withAlpha(0.35f));
        g.fillRoundedRectangle(fillArea.expanded(1.5f), 4.0f);
        g.setColour(meterColour);
        g.fillRoundedRectangle(fillArea, 3.0f);
    }

    g.setColour(lnf.textDim);
    g.setFont(lnf.makeScaledFont(9.0f, MorePhiLookAndFeel::kMinValueLabel));
    g.drawText("OUT", meterArea.translated(-28, 0).withWidth(24),
               juce::Justification::centredRight);

    // Title bar border
    g.setColour(lnf.borderColour);
    g.drawLine(0, 48, static_cast<float>(getWidth()), 48, 1.0f);
}

void MorePhiEditor::resized()
{
    auto area = getLocalBounds();

    // Update L&F with current width for font scaling
    lnf.setEditorWidth(static_cast<float>(getWidth()));

    // Title bar (painted) — increased from 44 to 48
    area.removeFromTop(48);

    // Position Deactivate License button in the title bar next to the level meter
    deactivateBtn_.setBounds(getWidth() - 250, 10, 120, 28);

    // Parameter panel (right side, togglable)
    const int paramWidth = paramPanelVisible_
        ? juce::jlimit(240, 320, getWidth() / 3)
        : 0;
    if (paramPanelVisible_)
        paramPanel.setBounds(area.removeFromRight(paramWidth));

    // Plugin browser row (FlexBox row) — 42px tall
    {
        auto browserRow = area.removeFromTop(42);
        juce::FlexBox fb;
        fb.flexDirection = juce::FlexBox::Direction::row;
        fb.items.add(juce::FlexItem(pluginBrowser).withFlex(1).withMargin({ 2, 4, 2, 4 }));
        fb.items.add(juce::FlexItem(openPluginBtn_).withWidth(110).withMargin(2));
        fb.items.add(juce::FlexItem(paramToggleBtn_).withWidth(80).withMargin(2));
        fb.performLayout(browserRow);
    }

    // ── Bottom: AI status bar ──────────────────────────────────────────────────
    auto bottomBar = area.removeFromBottom(32);
    aiPanel.setBounds(bottomBar);

    // ── Tab bar ────────────────────────────────────────────────────────────────
    const bool compactWidth = area.getWidth() < 760;
    const int baseTabContentHeight = compactWidth ? 300 : 260;
    const int aiTabContentHeight = compactWidth
        ? juce::jmax(baseTabContentHeight, area.getHeight() - 220)
        : juce::jmax(baseTabContentHeight, area.getHeight() - 180);
    const int tabContentHeight = activeTab_ == V2TabBar::AI
        ? juce::jlimit(baseTabContentHeight, area.getHeight() - 120, aiTabContentHeight)
        : baseTabContentHeight;
    auto tabContent = area.removeFromBottom(tabContentHeight);

    // 3px gap between tab content and tab bar
    area.removeFromBottom(3);

    // Tab bar (28px, above gap)
    auto tabBarArea = area.removeFromBottom(28);
    tabBar_.setBounds(tabBarArea);

    // ── Main area: Snap fader + square MorphPad ────────────────────────────────
    {
        auto mainArea = area.reduced(compactWidth ? 6 : 8, 6);

        const int faderWidth = compactWidth ? 48 : 56;
        snapFader.setBounds(mainArea.removeFromLeft(faderWidth).reduced(4, 0));
        mainArea.removeFromLeft(compactWidth ? 6 : 10);

        const int availableSide = juce::jmin(mainArea.getWidth(), mainArea.getHeight());
        const int padSide = juce::jlimit(96, 320, availableSide);
        const auto padBounds = juce::Rectangle<int>(0, 0, padSide, padSide)
            .withCentre(mainArea.getCentre());

        morphPad.setBounds(padBounds);
        snapshotRing.setBounds(padBounds);
    }

    // ── Tab content layout (FlexBox column with gaps) ──────────────────────────
    if (activeTab_ == V2TabBar::Classic)
    {
        const bool compactClassic = tabContent.getWidth() < 760;
        juce::FlexBox classic;
        classic.flexDirection = juce::FlexBox::Direction::column;
        classic.items.add(juce::FlexItem(controlStrip)
            .withHeight(compactClassic ? 104.0f : 64.0f)
            .withMargin(juce::FlexItem::Margin(4, 6, 2, 6)));
        classic.items.add(juce::FlexItem(modeBar)
            .withHeight(compactClassic ? 64.0f : 38.0f)
            .withMargin(juce::FlexItem::Margin(2, 6, 2, 6)));
        classic.items.add(juce::FlexItem(macroStrip)
            .withHeight(compactClassic ? 56.0f : 64.0f)
            .withMargin(juce::FlexItem::Margin(2, 6, 2, 6)));
        classic.items.add(juce::FlexItem(breedingPanel)
            .withHeight(compactClassic ? 42.0f : 48.0f)
            .withMargin(juce::FlexItem::Margin(2, 6, 4, 6)));
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

    // Cover the entire editor with licensing overlay if visible
    licenseOverlay.setBounds(getLocalBounds());
}

void MorePhiEditor::switchTab(int tabIndex)
{
    activeTab_ = tabIndex;
    tabBar_.setSelectedTab(tabIndex);

    setClassicTabVisible(tabIndex == V2TabBar::Classic);
    setEngineTabVisible(tabIndex == V2TabBar::Engine);
    setModulationTabVisible(tabIndex == V2TabBar::Modulation);
    setPresetsTabVisible(tabIndex == V2TabBar::Presets);
    setAITabVisible(tabIndex == V2TabBar::AI);

    resized();
    // Only repaint the content area below title bar and browser row, not the full editor
    repaint(0, 48, getWidth(), getHeight() - 48);
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
    // Check license state changes
    const bool isLicensed = processor.getLicenseRuntimeState().premiumFeaturesEnabled.load(std::memory_order_relaxed);
    if (licenseOverlay.isVisible() == isLicensed)
    {
        licenseOverlay.setVisible(!isLicensed);
        if (!isLicensed)
            licenseOverlay.toFront(false);
    }

    if (deactivateBtn_.isVisible() != isLicensed)
        deactivateBtn_.setVisible(isLicensed);

    // OUT meter: ease the raw RMS toward its target for a smooth glide
    // (mirrors the mockup's `transition-[width] duration-300 ease-out`).
    float currentRms = processor.getRmsLevel();
    float dbLevel = juce::jlimit(0.0f, 1.0f,
        (juce::Decibels::gainToDecibels(currentRms) + 60.0f) / 60.0f);

    // Asymmetric easing: rise quickly on transients, fall back slowly.
    const float attack = 0.6f;   // toward a louder target
    const float release = 0.18f; // toward a quieter target
    const float coeff = dbLevel > smoothedDbLevel_ ? attack : release;
    smoothedDbLevel_ += (dbLevel - smoothedDbLevel_) * coeff;

    if (std::abs(smoothedDbLevel_ - lastDbLevel_) > 0.004f)
    {
        lastDbLevel_ = smoothedDbLevel_;
        repaint(0, 0, getWidth(), 48);
    }
}

} // namespace more_phi

void more_phi::MorePhiEditor::openPluginWindow()
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());

    if (hostedWindow_) return;  // Already open

    auto* plugin = processor.getHostManager().acquirePluginForUse();
    if (!plugin) return;

    juce::Component::SafePointer<MorePhiEditor> safeThis(this);
    processor.getHostManager().setWindowCloseCallback([safeThis]() {
        juce::MessageManager::callAsync([safeThis]() {
            if (safeThis) safeThis->closePluginWindow();
        });
    });

    hostedWindow_ = std::make_unique<HostedPluginWindow>(
        plugin,
        [safeThis, this]() {
            processor.getHostManager().releasePluginFromUse();
            if (safeThis) safeThis->closePluginWindow();
        }  // on-close callback
    );
}

void more_phi::MorePhiEditor::closePluginWindow()
{
    if (hostedWindow_)
    {
        processor.getHostManager().setWindowCloseCallback(nullptr);
        hostedWindow_.reset();
    }
}
