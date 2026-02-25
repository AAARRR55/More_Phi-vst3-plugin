/*
 * MorphSnap — UI/PluginBrowserPanel.cpp
 */
#include "PluginBrowserPanel.h"
#include "Plugin/PluginProcessor.h"

namespace morphsnap {

PluginBrowserPanel::PluginBrowserPanel(MorphSnapProcessor& proc)
    : proc_(proc), host_(proc.getHostManager())
{
    addAndMakeVisible(loadBtn_);
    addAndMakeVisible(showBtn_);
    addAndMakeVisible(captureBtn_);
    addAndMakeVisible(pluginNameLabel_);

    loadBtn_.addListener(this);
    showBtn_.addListener(this);
    captureBtn_.addListener(this);

    showBtn_.setEnabled(false);
    captureBtn_.setEnabled(false);

    pluginNameLabel_.setText("No plugin loaded", juce::dontSendNotification);
    pluginNameLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff888888));
    pluginNameLabel_.setJustificationType(juce::Justification::centredLeft);

    // Poll at 10 Hz to sync label/buttons after state restore or async plugin reload.
    // This is what makes the UI show the plugin name after project open/export.
    startTimerHz(10);
}

PluginBrowserPanel::~PluginBrowserPanel()
{
    closePluginEditor();
}

void PluginBrowserPanel::resized()
{
    auto b = getLocalBounds().reduced(4, 2);
    loadBtn_.setBounds(b.removeFromLeft(120));
    b.removeFromLeft(6);
    showBtn_.setBounds(b.removeFromLeft(100));
    b.removeFromLeft(6);
    captureBtn_.setBounds(b.removeFromLeft(80));
    b.removeFromLeft(10);
    pluginNameLabel_.setBounds(b);
}

void PluginBrowserPanel::paint(juce::Graphics& g)
{
    g.setColour(juce::Colour(0xff16213e));
    g.fillRect(getLocalBounds());
    g.setColour(juce::Colour(0xff0f3460));
    g.drawLine(0, static_cast<float>(getHeight()), static_cast<float>(getWidth()),
               static_cast<float>(getHeight()), 1.0f);
}

void PluginBrowserPanel::buttonClicked(juce::Button* b)
{
    if (b == &loadBtn_)
        showPluginListDialog();
    else if (b == &showBtn_)
        openPluginEditor();
    else if (b == &captureBtn_)
        captureToNextSlot();
}

void PluginBrowserPanel::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // Scan completed or plugin list changed
}

void PluginBrowserPanel::timerCallback()
{
    // Auto-refresh label and button states after async plugin reload / state restore.
    // This is the fix for 'No plugin loaded' persisting after project open or export.
    const bool hasPlugin = host_.hasPlugin();
    showBtn_.setEnabled(hasPlugin);
    captureBtn_.setEnabled(hasPlugin);

    if (hasPlugin)
    {
        const juce::String currentName = host_.getLastDescriptionRef().name;
        if (currentName != lastKnownPluginName_)
        {
            lastKnownPluginName_ = currentName;
            pluginNameLabel_.setText(currentName, juce::dontSendNotification);
            pluginNameLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffe0e0e0));
        }
    }
    else if (lastKnownPluginName_.isNotEmpty())
    {
        // Plugin was unloaded — reset label
        lastKnownPluginName_ = {};
        pluginNameLabel_.setText("No plugin loaded", juce::dontSendNotification);
        pluginNameLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff888888));
    }
}

void PluginBrowserPanel::showPluginListDialog()
{
    // Start a scan if not already done
    if (!scanDone_)
    {
        scanDone_ = true;
        auto safeThis = juce::Component::SafePointer<PluginBrowserPanel>(this);
        // Scan in background thread to avoid blocking UI
        juce::Thread::launch([safeThis]()
        {
            if (safeThis == nullptr)
                return;

            safeThis->host_.scanPluginFolders();

            // After scan completes, show the list on the message thread
            juce::MessageManager::callAsync([safeThis]()
            {
                if (safeThis != nullptr)
                    safeThis->showPluginListDialog();
            });
        });
        pluginNameLabel_.setText("Scanning plugins...", juce::dontSendNotification);
        return;
    }

    // Create a dialog with JUCE's built-in PluginListComponent
    auto& knownPlugins = host_.getKnownPlugins();
    auto types = knownPlugins.getTypes();

    if (types.isEmpty())
    {
        pluginNameLabel_.setText("No plugins found", juce::dontSendNotification);
        return;
    }

    // Build a popup menu of discovered plugins
    juce::PopupMenu menu;
    for (int i = 0; i < types.size(); ++i)
    {
        const auto& desc = types.getReference(i);
        menu.addItem(i + 1, desc.name + " (" + desc.pluginFormatName + ")");
    }

    auto safeThis = juce::Component::SafePointer<PluginBrowserPanel>(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(loadBtn_),
        [safeThis, types](int result)
        {
            if (safeThis != nullptr && result > 0 && result <= types.size())
                safeThis->loadSelectedPlugin(types.getReference(result - 1));
        });
}

void PluginBrowserPanel::loadSelectedPlugin(const juce::PluginDescription& desc)
{
    closePluginEditor();

    if (host_.loadPlugin(desc))
    {
        pluginNameLabel_.setText(desc.name, juce::dontSendNotification);
        pluginNameLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffe0e0e0));
        showBtn_.setEnabled(true);
        captureBtn_.setEnabled(true);

        // Clear old snapshots since parameter layout changed
        proc_.getSnapshotBank().clearAll();
    }
    else
    {
        pluginNameLabel_.setText("Failed to load: " + desc.name, juce::dontSendNotification);
        pluginNameLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffe94560));
    }
}

void PluginBrowserPanel::openPluginEditor()
{
    if (!host_.hasPlugin()) return;

    if (pluginWindow_)
    {
        pluginWindow_->toFront(true);
        return;
    }

    pluginWindow_ = std::make_unique<HostedPluginWindow>(
        host_.getPlugin(),
        [this]() { closePluginEditor(); });
}

void PluginBrowserPanel::closePluginEditor()
{
    pluginWindow_.reset();
}

void PluginBrowserPanel::captureToNextSlot()
{
    auto& bank = proc_.getSnapshotBank();
    // Find first empty slot
    for (int i = 0; i < SnapshotBank::NUM_SLOTS; ++i)
    {
        if (!bank.isOccupied(i))
        {
            bank.capture(i, proc_.getParameterBridge());
            pluginNameLabel_.setText("Captured → Slot " + juce::String(i + 1),
                                     juce::dontSendNotification);
            return;
        }
    }
    // All slots full — overwrite slot 0
    bank.capture(0, proc_.getParameterBridge());
    pluginNameLabel_.setText("Captured → Slot 1 (overwrite)", juce::dontSendNotification);
}

} // namespace morphsnap
