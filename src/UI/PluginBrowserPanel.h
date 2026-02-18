/*
 * MorphSnap — UI/PluginBrowserPanel.h
 * Top bar: Load plugin, show plugin editor, snapshot capture/recall buttons.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "Host/PluginHostManager.h"
#include "HostedPluginWindow.h"
#include <memory>

namespace morphsnap {

class MorphSnapProcessor;

class PluginBrowserPanel : public juce::Component,
                           private juce::Button::Listener,
                           private juce::ChangeListener
{
public:
    explicit PluginBrowserPanel(MorphSnapProcessor& proc);
    ~PluginBrowserPanel() override;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void buttonClicked(juce::Button* b) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    void showPluginListDialog();
    void loadSelectedPlugin(const juce::PluginDescription& desc);
    void openPluginEditor();
    void closePluginEditor();
    void captureToNextSlot();

    MorphSnapProcessor& proc_;
    PluginHostManager& host_;

    juce::TextButton loadBtn_{"Load Plugin..."};
    juce::TextButton showBtn_{"Show Editor"};
    juce::TextButton captureBtn_{"Capture"};
    juce::Label pluginNameLabel_;

    std::unique_ptr<HostedPluginWindow> pluginWindow_;
    bool scanDone_ = false;
};

} // namespace morphsnap
