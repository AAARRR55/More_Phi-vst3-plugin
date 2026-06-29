/*
 * More-Phi — UI/HostedPluginWindow.h
 * DocumentWindow wrapper for displaying a hosted plugin's native editor.
 */
#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_processors/juce_audio_processors.h>

namespace more_phi {

class HostedPluginWindow : public juce::DocumentWindow
{
public:
    HostedPluginWindow(juce::AudioPluginInstance* plugin,
                       std::function<void()> onClose = nullptr,
                       std::function<void()> releasePluginCallback = nullptr)
        : DocumentWindow(plugin ? plugin->getName() : "Plugin",
                         juce::Colour(0xff1a1a2e),
                         DocumentWindow::allButtons),
          closeCallback_(std::move(onClose)),
          releasePluginCallback_(std::move(releasePluginCallback))
    {
        if (plugin)
        {
            if (auto* editor = plugin->createEditor())
            {
                setContentOwned(editor, true);
                setResizable(editor->isResizable(), false);
            }
            else
            {
                // Plugin has no custom editor — show generic
                auto* generic = new juce::GenericAudioProcessorEditor(*plugin);
                setContentOwned(generic, true);
                setResizable(true, false);
            }
        }

        setUsingNativeTitleBar(true);
        setVisible(true);
        centreWithSize(getWidth(), getHeight());
    }

    ~HostedPluginWindow() override
    {
        // Phase 4 (editor-lifetime safety): destroy the hosted editor BEFORE
        // releasing the plugin lease. The editor (set via setContentOwned) is
        // the one object that depends on the hosted plugin instance being alive.
        // The base DocumentWindow destructor would eventually delete the content
        // too, but only AFTER this destructor body runs — so if we released the
        // lease here first, activePluginUsers_ could momentarily reach zero,
        // letting PluginHostManager's deferred-doom queue destroy the plugin,
        // and THEN the editor's destructor would run against freed memory.
        // Clearing content here forces the editor to die while we still hold
        // the lease; the lease release below then drops the last reference.
        clearContentComponent();

        if (releasePluginCallback_ && !releaseCalled_)
        {
            releaseCalled_ = true;
            releasePluginCallback_();
        }
    }

    void closeButtonPressed() override
    {
        if (closeCallback_)
            closeCallback_();
        else
            setVisible(false);
    }

private:
    std::function<void()> closeCallback_;
    std::function<void()> releasePluginCallback_;
    bool releaseCalled_ = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HostedPluginWindow)
};

} // namespace more_phi
