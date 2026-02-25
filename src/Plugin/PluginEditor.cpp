/*
 * MorphSnap — Advanced Parameter Morphing Engine
 * PluginEditor.cpp — Main Editor Window (Premium Layout)
 */
#include "PluginEditor.h"
#include "PluginProcessor.h"

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
    setSize(920, 710);
    setResizable(true, true);
    setResizeLimits(720, 560, 1600, 1080);

    addAndMakeVisible(morphPad);
    addAndMakeVisible(snapFader);
    addAndMakeVisible(snapshotRing);
    addAndMakeVisible(pluginBrowser);
    addAndMakeVisible(macroStrip);
    addAndMakeVisible(aiPanel);
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
        paramToggleBtn_.setButtonText(paramPanelVisible_ ? "◂ Params" : "Params ▸");
        if (paramPanelVisible_)
            paramPanel.rebuildForPlugin();
        resized();
    };
    addAndMakeVisible(paramToggleBtn_);

    // Open Plugin UI button
    openPluginBtn_.onClick = [this]() { openPluginWindow(); };
    addAndMakeVisible(openPluginBtn_);

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
    g.drawText("v1.0", titleArea.reduced(14, 0).removeFromLeft(200),
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

    // Bottom bar
    auto bottomBar = area.removeFromBottom(32);
    aiPanel.setBounds(bottomBar);

    // Breeding panel
    auto breedRow = area.removeFromBottom(38);
    breedingPanel.setBounds(breedRow);

    // Macro strip
    auto macroRow = area.removeFromBottom(60);
    macroStrip.setBounds(macroRow);

    // Mode bar
    auto modeRow = area.removeFromBottom(32);
    modeBar.setBounds(modeRow);

    // Bottom control strip (new Stitch-enhanced controls)
    auto controlRow = area.removeFromBottom(48);
    controlStrip.setBounds(controlRow);

    // Snap fader (left)
    auto leftCol = area.removeFromLeft(52);
    snapFader.setBounds(leftCol.reduced(6, 10));

    // XY Pad + snapshot ring
    auto padArea = area.reduced(8);
    morphPad.setBounds(padArea);
    snapshotRing.setBounds(padArea);
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
