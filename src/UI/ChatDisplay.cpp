/*
 * More-Phi - UI/ChatDisplay.cpp
 */
#include "ChatDisplay.h"

namespace more_phi {

ChatDisplay::ChatDisplay()
{
    messages_.push_back({Role::System, "Assistant ready."});
}

void ChatDisplay::addMessage(Role role, juce::String text)
{
    text = text.trim();
    if (text.isEmpty())
        return;

    messages_.push_back({role, std::move(text)});

    constexpr size_t maxMessages = 128;
    if (messages_.size() > maxMessages)
        messages_.erase(messages_.begin(), messages_.begin() + static_cast<std::ptrdiff_t>(messages_.size() - maxMessages));

    repaint();
}

void ChatDisplay::clearMessages()
{
    messages_.clear();
    repaint();
}

void ChatDisplay::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.fillAll(juce::Colour(0xff171a1f));

    g.setColour(juce::Colour(0xff303640));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 4.0f, 1.0f);

    auto area = getLocalBounds().reduced(10, 8);
    constexpr int rowGap = 6;
    constexpr int labelWidth = 72;

    g.setFont(juce::Font(juce::FontOptions(12.0f)));

    int y = area.getY();
    for (const auto& message : messages_)
    {
        auto text = message.text;
        auto availableWidth = juce::jmax(1, area.getWidth() - labelWidth - 8);
        auto textHeight = juce::jmax(20, g.getCurrentFont().getStringWidthFloat(text) > static_cast<float>(availableWidth)
                                             ? 38
                                             : 20);

        if (y + textHeight > area.getBottom())
            break;

        auto row = juce::Rectangle<int>(area.getX(), y, area.getWidth(), textHeight);
        g.setColour(roleColour(message.role));
        g.drawText(roleLabel(message.role), row.removeFromLeft(labelWidth), juce::Justification::centredLeft);

        g.setColour(juce::Colour(0xffe6e9ef));
        g.drawFittedText(text, row, juce::Justification::centredLeft, 2);

        y += textHeight + rowGap;
    }
}

juce::String ChatDisplay::roleLabel(Role role)
{
    switch (role)
    {
        case Role::System:    return "System";
        case Role::User:      return "You";
        case Role::Assistant: return "Assistant";
    }

    return {};
}

juce::Colour ChatDisplay::roleColour(Role role)
{
    switch (role)
    {
        case Role::System:    return juce::Colour(0xff9aa4b2);
        case Role::User:      return juce::Colour(0xff80bfff);
        case Role::Assistant: return juce::Colour(0xffffb870);
    }

    return juce::Colours::white;
}

} // namespace more_phi
