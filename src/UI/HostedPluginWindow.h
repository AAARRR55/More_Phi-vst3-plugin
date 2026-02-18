/*
 * MorphSnap — UI/HostedPluginWindow.h
 * DocumentWindow wrapper for displaying a hosted plugin's native editor.
 */
#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_processors/juce_audio_processors.h>

namespace morphsnap {

class HostedPluginWindow : public juce::DocumentWindow
{
public:
    HostedPluginWindow(juce::AudioPluginInstance* plugin,
                       std::function<void()> onClose = nullptr)
        : DocumentWindow(plugin ? plugin->getName() : "Plugin",
                         juce::Colour(0xff1a1a2e),
                         DocumentWindow::allButtons),
          closeCallback_(std::move(onClose))
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

    void closeButtonPressed() override
    {
        if (closeCallback_)
            closeCallback_();
        else
            setVisible(false);
    }

private:
    std::function<void()> closeCallback_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HostedPluginWindow)
};

} // namespace morphsnap
