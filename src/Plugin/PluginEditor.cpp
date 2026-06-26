/*
 * More-Phi — Advanced Parameter Morphing Engine
 * PluginEditor.cpp — Main Editor Window (V2 Tabbed Layout)
 */
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "UI/EngineTabPage.h"
#include "UI/ModulationMatrixPanel.h"
#include "UI/V2PresetBrowserPanel.h"
#include "Version.h"  // AUDIT-FIX: VERSION_STRING (single source of truth for the painted version)

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
      licenseOverlay(p),
      onboardingOverlay(p)
{
    setLookAndFeel(&lnf);
    setSize(920, 760);
    setResizable(true, true);
    setResizeLimits(760, 640, 1600, 1200);
    // AUDIT-FIX (accessibility): make the editor a focus container so keyboard
    // (Tab/arrow) traversal reaches the hosted controls (MorphPad, SnapFader, …).
    setFocusContainerType(juce::Component::FocusContainerType::focusContainer);

#if JUCE_WINDOWS
    // HiDPI: scale the editor canvas to match the desktop scale factor.
    // Only on Windows/FL-Studio where JUCE's built-in HiDPI support may not
    // apply the desktop-scale transform automatically (macOS already scales).
    {
        auto& desktop = juce::Desktop::getInstance();
        if (auto* display = desktop.getDisplays().getPrimaryDisplay())
        {
            const float scale = display->scale;
            if (scale > 1.0f && scale < 3.0f)
                setTransform(juce::AffineTransform::scale(scale));
        }
    }
#endif

    paramToggleBtn_.setButtonText("All Parameters \u25B8");
    paramToggleBtn_.setTooltip("Show or hide all hosted plugin parameters with search and sliders.");
    openPluginBtn_.setTooltip("Open the hosted plugin editor in a separate window.");

    // ── Bypass (title bar) ─────────────────────────────────────────────────────
    bypassBtn_.setClickingTogglesState(true);
    bypassBtn_.setColour(juce::TextButton::buttonColourId,    lnf.surfaceColour);
    bypassBtn_.setColour(juce::TextButton::buttonOnColourId,  lnf.accentCoral);
    bypassBtn_.setColour(juce::TextButton::textColourOffId,   lnf.textPrimary);
    bypassBtn_.setColour(juce::TextButton::textColourOnId,    juce::Colours::white);
    bypassBtn_.setTooltip("Bypass: disable all More-Phi processing and pass audio through unchanged.");
    if (auto* param = processor.getAPVTS().getParameter("bypass"))
    {
        bypassBtn_.onClick = [this, param]() {
            param->setValueNotifyingHost(bypassBtn_.getToggleState() ? 1.0f : 0.0f);
        };
    }
    addAndMakeVisible(bypassBtn_);

    // AUDIT-2026-06-25: Expert mode toggle
    expertBtn_.setClickingTogglesState(true);
    expertBtn_.setColour(juce::TextButton::buttonColourId,    lnf.surfaceColour);
    expertBtn_.setColour(juce::TextButton::buttonOnColourId,  lnf.accentGreen);
    expertBtn_.setColour(juce::TextButton::textColourOffId,   lnf.textPrimary);
    expertBtn_.setColour(juce::TextButton::textColourOnId,    juce::Colours::white);
    expertBtn_.setTooltip("Show advanced tabs (Engine, Modulation, AI) and Neural Master controls.");
    if (auto* param = processor.getAPVTS().getParameter("expertMode"))
    {
        expertBtn_.onClick = [this, param]() {
            param->setValueNotifyingHost(expertBtn_.getToggleState() ? 1.0f : 0.0f);
            updateExpertModeUI();
        };
    }
    addAndMakeVisible(expertBtn_);
    updateExpertModeUI();

    // ── A/B Compare toggle ─────────────────────────────────────────────────────
    abCompareBtn_.setClickingTogglesState(true);
    abCompareBtn_.setColour(juce::TextButton::buttonColourId,    lnf.surfaceColour);
    abCompareBtn_.setColour(juce::TextButton::buttonOnColourId,  lnf.accentCoral);
    abCompareBtn_.setColour(juce::TextButton::textColourOffId,   lnf.textPrimary);
    abCompareBtn_.setColour(juce::TextButton::textColourOnId,    juce::Colours::white);
    abCompareBtn_.setTooltip(
        "A/B Compare: captures the current parameter state as the 'B' reference, "
        "then toggles between live tweaks ('A') and the captured reference ('B'). "
        "Click once to capture, click again to toggle.");
    abCompareBtn_.onClick = [this]()
    {
        const bool nowActive = processor.toggleABCompare();
        abCompareBtn_.setToggleState(nowActive, juce::dontSendNotification);
        abCompareBtn_.setButtonText(nowActive ? "B" : "A/B");
    };
    addAndMakeVisible(abCompareBtn_);

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

    // Accessibility: mark key components as accessible with explicit focus order.
    morphPad.setAccessible(true);
    morphPad.setExplicitFocusOrder(1);
    snapFader.setAccessible(true);
    snapFader.setExplicitFocusOrder(2);
    macroStrip.setAccessible(true);
    macroStrip.setExplicitFocusOrder(3);
    modeBar.setAccessible(true);
    modeBar.setExplicitFocusOrder(4);
    controlStrip.setAccessible(true);
    controlStrip.setExplicitFocusOrder(5);
    tabBar_.setAccessible(true);
    tabBar_.setExplicitFocusOrder(6);
    snapshotRing.setAccessible(true);
    snapshotRing.setExplicitFocusOrder(7);
    bypassBtn_.setExplicitFocusOrder(8);

    // Parameter panel (initially hidden)
    addChildComponent(paramPanel);
    paramPanel.setVisible(false);

    // Toggle button in plugin browser row
    paramToggleBtn_.onClick = [this]()
    {
        paramPanelVisible_ = !paramPanelVisible_;
        paramPanel.setVisible(paramPanelVisible_);
        paramToggleBtn_.setButtonText(
        paramPanelVisible_
            ? juce::String("\u25C2 All Parameters")
            : juce::String("All Parameters \u25B8"));
        if (paramPanelVisible_)
            paramPanel.rebuildForPlugin();
        resized();
    };
    addAndMakeVisible(paramToggleBtn_);

    // ── SonicMaster realtime neural mastering (preview) ───────────────────────
    // Toggle is bound to the APVTS bool; disabled when no model is available.
    // Status label is refreshed every timer tick (timerCallback).
    sonicMasterToggle_.setTooltip(
        "Continuously analyse ~6s of audio on a background thread and refresh "
        "the built-in mastering chain from a neural decision model. Preview "
        "(research-grade); off by default. Every prediction is clamped by the "
        "safety policy, so a bad frame can never push the chain unsafe.");
    sonicMasterToggle_.onClick = [this]()
    {
        if (auto* param = dynamic_cast<juce::AudioParameterBool*>(
                processor.getAPVTS().getParameter("SonicMasterAnalysisEnabled")))
            param->setValueNotifyingHost(sonicMasterToggle_.getToggleState() ? 1.0f : 0.0f);
    };
    addAndMakeVisible(sonicMasterToggle_);

    sonicMasterStatus_.setJustificationType(juce::Justification::centredLeft);
    sonicMasterStatus_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible(sonicMasterStatus_);

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

    // Onboarding overlay — shown on first launch (no snapshot data present)
    addChildComponent(onboardingOverlay);
    if (!processor.getSnapshotBank().hasAnyOccupied())
    {
        onboardingOverlay.setVisible(true);
        onboardingOverlay.toFront(false);
    }

    // ponytail: FL Studio composites an unbuffered plugin window every frame,
    // re-running paint() (incl. a ColourGradient alloc) on the shared message
    // thread — that's the host-wide UI lag. Buffering the editor to an image
    // makes FL blit the cache and only re-paint invalidated regions (meter).
    setBufferedToImage(true);

    // M-16 FIX: Reduced from 30Hz to 15Hz — sufficient for UI updates,
    // reduces CPU overhead and message-thread contention.
    startTimerHz(15);  // meter-glide + status refresh; 15Hz is plenty visible
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

    // Version — AUDIT-FIX: render from the single source of truth (Version.h)
    // instead of a hardcoded literal. Previously this was "v3.3.0" while
    // CMakeLists.txt said 3.4.0 and Version.h said 3.4.0 — four version
    // strings, two values. Now the editor, CMake, CI, and Version.h all agree.
    g.setColour(lnf.textDim);
    g.setFont(lnf.makeScaledFont(10.0f));
    g.drawText("v" + juce::String(more_phi::VERSION_STRING),
               titleTextArea.removeFromLeft(72).translated(0, 1),
               juce::Justification::centredLeft, false);

    // A/B Compare button (right side of title bar, before expert/bypass)
    auto abArea = titleArea.removeFromRight(56).reduced(6, 12);
    abCompareBtn_.setBounds(abArea);

    // Expert mode toggle (right side, before A/B)
    auto expertArea = titleArea.removeFromRight(70).reduced(6, 12);
    expertBtn_.setBounds(expertArea);

    // Bypass button (right side of title bar, before meter)
    auto bypassArea = titleArea.removeFromRight(80).reduced(6, 12);
    bypassBtn_.setBounds(bypassArea);

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

    // Tick marks at key levels — shape+position fallback for color-only meter zones
    {
        const float tickLevels[] = { 40.0f / 60.0f, 50.0f / 60.0f, 54.0f / 60.0f };  // -20, -10, -6 dB
        const juce::String tickLabels[] = { "-20", "-10", "-6" };
        g.setFont(lnf.makeScaledFont(7.0f, 7.0f));
        for (int i = 0; i < 3; ++i)
        {
            const float tx = meterArea.getX() + meterArea.getWidth() * tickLevels[i];
            g.setColour(lnf.textDim.withAlpha(0.45f));
            g.drawLine(tx, meterArea.getBottom() - 3.0f, tx, meterArea.getBottom(), 0.5f);
            g.drawText(tickLabels[i],
                       juce::Rectangle<float>(tx - 12.0f, meterArea.getBottom() + 1.0f, 24.0f, 10.0f),
                       juce::Justification::centred);
        }
    }

    // Title bar border
    g.setColour(lnf.borderColour);
    g.drawLine(0, 48, static_cast<float>(getWidth()), 48, 1.0f);

    // SonicMaster panel background
    if (sonicMasterRowBounds_.getWidth() > 0)
    {
        g.setColour(lnf.surfaceColour.withAlpha(0.85f));
        g.fillRoundedRectangle(sonicMasterRowBounds_.toFloat().reduced(4, 2), 4.0f);
        g.setColour(lnf.borderColour);
        g.drawRoundedRectangle(sonicMasterRowBounds_.toFloat().reduced(4, 2), 4.0f, 0.5f);
    }
}

void MorePhiEditor::resized()
{
    auto area = getLocalBounds();

    // Update L&F with current width for font scaling
    lnf.setEditorWidth(static_cast<float>(getWidth()));

    // Title bar (painted) — increased from 44 to 48
    area.removeFromTop(48);
    {
        auto titleArea = getLocalBounds().removeFromTop(48);
        auto abArea = titleArea.removeFromRight(56).reduced(6, 12);
        abCompareBtn_.setBounds(abArea);
        auto expertArea = titleArea.removeFromRight(70).reduced(6, 12);
        expertBtn_.setBounds(expertArea);
        auto bypassArea = titleArea.removeFromRight(80).reduced(6, 12);
        bypassBtn_.setBounds(bypassArea);
    }

    // Deactivate License — moved to bottom bar for less visual intrusion
    // (positioned later, next to AI status)

    // Parameter panel (overlay — floats on top, doesn't push content)
    if (paramPanelVisible_)
    {
        const int pw = juce::jlimit(240, 320, getWidth() / 3);
        paramPanel.setBounds(getWidth() - pw - 8, area.getY(), pw, area.getHeight());
    }

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
    {
        auto aiArea = bottomBar.reduced(4, 2);
        // Deactivate license on the far right of the bottom bar (small, low-profile)
        deactivateBtn_.setBounds(aiArea.removeFromRight(130));
        aiPanel.setBounds(aiArea);
    }

    // ── SonicMaster neural-mastering toggle + status (just above AI bar) ───────
    sonicMasterRowBounds_ = area.removeFromBottom(30);
    sonicMasterToggle_.setBounds(sonicMasterRowBounds_.removeFromLeft(200).reduced(4, 2));
    sonicMasterStatus_.setBounds(sonicMasterRowBounds_.reduced(4, 2));

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

    // Tab bar (32px — L4: comfortable touch/high-DPI hit target; was 28)
    auto tabBarArea = area.removeFromBottom(32);
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

    // Cover the entire editor with licensing/onboarding overlay if visible
    licenseOverlay.setBounds(getLocalBounds());
    onboardingOverlay.setBounds(getLocalBounds());
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

// AUDIT-2026-06-25 (build unblock): updateExpertModeUI() was declared in the
// header and called from the Expert toggle's onClick + ctor, but its body was
// never written — an unresolved external at link time blocked the whole build.
// Standard mode shows Classic + Presets; Expert mode additionally reveals the
// Engine, Modulation and AI tabs (per V2TabBar's setVisibleTabs contract and
// the Expert button tooltip: "Show advanced tabs (Engine, Modulation, AI)...").
// We mirror the toggle's getToggleState() (kept in sync with the expertMode
// APVTS param by the onClick handler) and force Classic as the active tab when
// expert mode is switched off, so the editor never lands on a hidden tab.
void MorePhiEditor::updateExpertModeUI()
{
    const bool expert = expertBtn_.getToggleState();
    using Tab = V2TabBar;
    const int visibleMask = (1 << Tab::Classic) | (1 << Tab::Presets)
                          | (expert ? ((1 << Tab::Engine) | (1 << Tab::Modulation) | (1 << Tab::AI))
                                    : 0);
    tabBar_.setVisibleTabs(visibleMask);

    // If the active tab is now hidden, fall back to Classic.
    if (!tabBar_.isTabVisible(activeTab_))
    {
        switchTab(Tab::Classic);
    }

    resized();
    repaint();
}

void MorePhiEditor::timerCallback()
{
    MSG_TRACE(processor.getDiagnostics(), "Editor::timerCallback");
    // Check license state changes. R4: honor a session dismiss ("Continue in
    // Demo") so the overlay isn't re-spawned every tick after the user chose to
    // use the plugin without a license this session.
    const bool isLicensed = processor.getLicenseRuntimeState().premiumFeaturesEnabled.load(std::memory_order_relaxed);
    if (! licenseOverlay.isDismissedForSession()
        && licenseOverlay.isVisible() == isLicensed)
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

    // Mirror bypass state from APVTS into title-bar button
    if (auto* bp = dynamic_cast<juce::AudioParameterBool*>(
            processor.getAPVTS().getParameter("bypass")))
        bypassBtn_.setToggleState(bp->get(), juce::dontSendNotification);

    // Throttled refresh of the SonicMaster toggle + status (cheap atomic reads).
    refreshSonicMasterStatus();
}

void MorePhiEditor::refreshSonicMasterStatus()
{
    auto& engine = processor.getSonicMasterEngine();

    // Mirror the APVTS bool into the toggle so host automation / preset recall
    // keeps the button in sync (avoid feedback: don't fire onClick).
    if (auto* param = dynamic_cast<juce::AudioParameterBool*>(
            processor.getAPVTS().getParameter("SonicMasterAnalysisEnabled")))
    {
        const bool desired = param->get();
        if (sonicMasterToggle_.getToggleState() != desired)
            sonicMasterToggle_.setToggleState(desired, juce::dontSendNotification);
    }

    // Disable the toggle when no model is loaded (feature unavailable).
    sonicMasterToggle_.setEnabled(engine.isAvailable());

    juce::String text = "Neural Master: ";
    if (!engine.isAvailable())
    {
        text += "not available";
#if MORE_PHI_HAS_ONNX
        sonicMasterStatus_.setTooltip(
            "The bundled ONNX neural model file was not found. Place masteringbrain_v2_"
            "decision.onnx next to the plugin binary, or rebuild with the model staged in "
            "models/sonicmaster/, to enable this feature.");
#else
        sonicMasterStatus_.setTooltip(
            "Neural Master is not included in this build. Rebuild with "
            "-DMORE_PHI_ENABLE_ONNX=ON to enable it.");
#endif
    }
    else
    {
        sonicMasterStatus_.setTooltip({});
        switch (engine.getStatus())
        {
            case SonicMasterAnalysisEngine::Status::Disabled:           text += "off"; break;
            case SonicMasterAnalysisEngine::Status::CollectingAudio:    text += "listening\u2026"; break;
            case SonicMasterAnalysisEngine::Status::Applied:            text += "active #" + juce::String((int) engine.getLastPlanId()); break;
            case SonicMasterAnalysisEngine::Status::HeldLowConfidence:  text += "waiting for clearer signal\u2026"; break;
            case SonicMasterAnalysisEngine::Status::ErrorAutoDisabled:  text += "paused \u2014 check diagnostics"; break;
        }
    }
    sonicMasterStatus_.setText(text, juce::dontSendNotification);
}

} // namespace more_phi

void more_phi::MorePhiEditor::openPluginWindow()
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());

    if (hostedWindow_) return;  // Already open

    auto* plugin = processor.getHostManager().acquirePluginForUse();
    if (!plugin) return;

    // Phase 4 (lease-lifetime safety): RAII guard so that if the HostedPluginWindow
    // constructor throws (e.g. the hosted plugin's createEditor() throws, or an
    // allocation fails), the lease acquired above is always released. Without this,
    // a thrown ctor left activePluginUsers_ > 0, causing unloadPlugin() to spin to
    // its 500 ms timeout every time. The guard is disarmed only after the window
    // is successfully constructed — on success the window's own destructor takes
    // over lease release via its releasePluginCallback.
    bool windowConstructed = false;
    struct LeaseGuard
    {
        PluginHostManager& host;
        bool& armed;
        ~LeaseGuard() { if (armed) host.releasePluginFromUse(); }
    } guard{ processor.getHostManager(), windowConstructed };

    juce::Component::SafePointer<MorePhiEditor> safeThis(this);
    processor.getHostManager().setWindowCloseCallback([safeThis]() {
        juce::MessageManager::callAsync([safeThis]() {
            if (safeThis) safeThis->closePluginWindow();
        });
    });

    hostedWindow_ = std::make_unique<HostedPluginWindow>(
        plugin,
        [safeThis]() {
            if (safeThis) safeThis->closePluginWindow();
        },
        [safeThis, this]() {
            processor.getHostManager().releasePluginFromUse();
        }  // release-on-close (HostedPluginWindow dtor)
    );

    windowConstructed = true;  // disarm guard — window now owns the lease release
}

void more_phi::MorePhiEditor::closePluginWindow()
{
    if (hostedWindow_)
    {
        processor.getHostManager().setWindowCloseCallback(nullptr);
        hostedWindow_.reset();
    }
}
