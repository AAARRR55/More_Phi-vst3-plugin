/*
 * More-Phi — UI/PluginBrowserPanel.h
 * Top bar: Load plugin, show plugin editor, snapshot capture/recall buttons.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "Host/PluginHostManager.h"
#include "HostedPluginWindow.h"
#include <memory>

namespace more_phi {

class MorePhiProcessor;

class PluginBrowserPanel : public juce::Component,
                           private juce::Button::Listener,
                           private juce::Timer
{
public:
    explicit PluginBrowserPanel(MorePhiProcessor& proc);
    ~PluginBrowserPanel() override;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void buttonClicked(juce::Button* b) override;
    void timerCallback() override;

    void showPluginListDialog();
    void loadSelectedPlugin(const juce::PluginDescription& desc);
    // R2: the actual host load + (destructive) snapshot clear. Split out so the
    // confirm dialog can invoke it from its async callback.
    void performPluginLoad(const juce::PluginDescription& desc);
    void openPluginEditor();
    void closePluginEditor();
    void captureToNextSlot();

    MorePhiProcessor& proc_;
    PluginHostManager& host_;

    juce::TextButton loadBtn_{"Load Plugin..."};
    juce::TextButton showBtn_{"Show Editor"};
    juce::TextButton captureBtn_{"Capture \u2192"};
    juce::Label pluginNameLabel_;

    std::unique_ptr<HostedPluginWindow> pluginWindow_;
    bool scanDone_ = false;
    juce::String lastKnownPluginName_;  // tracks last label state to avoid redundant updates
};

} // namespace more_phi
