/*
 * More-Phi - UI/ChatDisplay.h
 * Lightweight in-plugin assistant transcript view.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

namespace more_phi {

class ChatDisplay final : public juce::Component
{
public:
    enum class Role
    {
        System,
        User,
        Assistant
    };

    struct Message
    {
        Role role = Role::Assistant;
        juce::String text;
    };

    ChatDisplay();

    void addMessage(Role role, juce::String text);
    void clearMessages();

    void paint(juce::Graphics& g) override;

private:
    static juce::String roleLabel(Role role);
    static juce::Colour roleColour(Role role);

    std::vector<Message> messages_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChatDisplay)
};

} // namespace more_phi
