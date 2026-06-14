/*
 * More-Phi - UI/ChatDisplay.cpp
 */
#include "ChatDisplay.h"

#include <cmath>

namespace more_phi {

// ── Layout constants ────────────────────────────────────────────────────────
static constexpr int   kPadX      = 12;
static constexpr int   kPadY      = 10;
static constexpr int   kGap       = 10;
static constexpr int   kLabelW    = 70;
static constexpr int   kLabelGap  = 8;
static constexpr float kFontSize  = 12.5f;
static constexpr int   kMinRowH   = 18;

// ── Helpers ─────────────────────────────────────────────────────────────────
static juce::String roleLabel(ChatDisplay::Role role)
{
    switch (role)
    {
        case ChatDisplay::Role::System:    return "System";
        case ChatDisplay::Role::User:      return "You";
        case ChatDisplay::Role::Assistant: return "Assistant";
    }
    return {};
}

static juce::Colour roleLabelColour(ChatDisplay::Role role)
{
    switch (role)
    {
        case ChatDisplay::Role::System:    return juce::Colour(0xff7a8492);
        case ChatDisplay::Role::User:      return juce::Colour(0xff80bfff);
        case ChatDisplay::Role::Assistant: return juce::Colour(0xffffb870);
    }
    return juce::Colours::white;
}

static juce::Colour roleBubbleColour(ChatDisplay::Role role)
{
    switch (role)
    {
        case ChatDisplay::Role::System:    return juce::Colour(0x00000000); // transparent
        case ChatDisplay::Role::User:      return juce::Colour(0xff1e2635);
        case ChatDisplay::Role::Assistant: return juce::Colour(0xff1a2030);
    }
    return juce::Colour(0xff1a2030);
}

/** Compute the wrapped text height for the given string at the given pixel width. */
static int measureTextHeight(const juce::String& text, int textWidth, float fontSize)
{
    if (textWidth <= 0) return kMinRowH;

    auto font = juce::Font(juce::FontOptions(fontSize));
    const float lineHeight = std::ceil(font.getHeight() + 2.0f);
    if (lineHeight < 1.0f) return kMinRowH;

    int totalLines = 0;
    juce::StringArray hardLines;
    hardLines.addTokens(text, "\n", "");
    for (const auto& line : hardLines)
    {
        const float lineW = font.getStringWidthFloat(line.isEmpty() ? " " : line);
        totalLines += juce::jmax(1, static_cast<int>(std::ceil(lineW / static_cast<float>(textWidth))));
    }
    return juce::jmax(kMinRowH, static_cast<int>(static_cast<float>(totalLines) * lineHeight));
}

// ── Canvas ───────────────────────────────────────────────────────────────────
ChatDisplay::Canvas::Canvas()
{
    setInterceptsMouseClicks(false, false);
}

void ChatDisplay::Canvas::layout(int viewportWidth, int viewportHeight)
{
    const int textW = juce::jmax(1, viewportWidth - 2 * kPadX - kLabelW - kLabelGap);

    int totalH = kPadY;
    for (const auto& msg : messages)
        totalH += measureTextHeight(msg.text, textW, kFontSize) + kGap;
    totalH += kPadY;

    setSize(viewportWidth, juce::jmax(1, juce::jmax(viewportHeight, totalH)));
}

void ChatDisplay::Canvas::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff171a1f));

    if (messages.empty())
        return;

    const int w     = getWidth();
    const int textW = juce::jmax(1, w - 2 * kPadX - kLabelW - kLabelGap);

    auto labelFont = juce::Font(juce::FontOptions(kFontSize, juce::Font::bold));
    auto textFont  = juce::Font(juce::FontOptions(kFontSize));
    const float lineH = std::ceil(textFont.getHeight() + 2.0f);

    int y = kPadY;
    for (const auto& msg : messages)
    {
        const int rowH      = measureTextHeight(msg.text, textW, kFontSize);
        const int maxLines  = juce::jmax(1, static_cast<int>(std::ceil(static_cast<float>(rowH) / lineH)));
        const int bubblePad = 5;

        // Background bubble
        const auto bubbleCol = roleBubbleColour(msg.role);
        if (bubbleCol.getAlpha() > 0)
        {
            g.setColour(bubbleCol);
            g.fillRoundedRectangle(
                juce::Rectangle<int>(kPadX - bubblePad, y - bubblePad / 2,
                                     w - 2 * kPadX + 2 * bubblePad, rowH + bubblePad).toFloat(),
                4.0f);
        }

        // Role label (bold, coloured, top-aligned)
        g.setFont(labelFont);
        g.setColour(roleLabelColour(msg.role));
        g.drawText(roleLabel(msg.role),
                   kPadX, y, kLabelW, static_cast<int>(lineH),
                   juce::Justification::centredLeft, false);

        // Message text (word-wrapped)
        g.setFont(textFont);
        g.setColour(juce::Colour(0xffe6e9ef));
        g.drawFittedText(msg.text,
                         kPadX + kLabelW + kLabelGap, y,
                         textW, rowH,
                         juce::Justification::topLeft,
                         maxLines);

        y += rowH + kGap;
    }
}

// ── ChatDisplay ──────────────────────────────────────────────────────────────
ChatDisplay::ChatDisplay()
{
    setWantsKeyboardFocus(true);
    viewport_.setWantsKeyboardFocus(false);
    canvas_.setWantsKeyboardFocus(false);
    viewport_.setViewedComponent(&canvas_, false);
    viewport_.setScrollBarsShown(true, false);
    viewport_.setScrollBarThickness(7);
    addAndMakeVisible(viewport_);

    canvas_.messages.push_back({Role::System, "Assistant ready."});
}

void ChatDisplay::addMessage(Role role, juce::String text)
{
    text = text.trim();
    if (text.isEmpty())
        return;

    canvas_.messages.push_back({role, std::move(text)});

    constexpr size_t kMaxMessages = 128;
    if (canvas_.messages.size() > kMaxMessages)
        canvas_.messages.erase(
            canvas_.messages.begin(),
            canvas_.messages.begin() +
                static_cast<std::ptrdiff_t>(canvas_.messages.size() - kMaxMessages));

    pushAndScroll();
}

void ChatDisplay::updateLastMessage(juce::String text)
{
    if (canvas_.messages.empty())
        return;
    canvas_.messages.back().text = text.trim();
    pushAndScroll();
}

void ChatDisplay::clearMessages()
{
    canvas_.messages.clear();
    pushAndScroll();
}

void ChatDisplay::resized()
{
    viewport_.setBounds(getLocalBounds());
    pushAndScroll();
}

bool ChatDisplay::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::upKey)
    {
        scrollBy(-36);
        return true;
    }

    if (key == juce::KeyPress::downKey)
    {
        scrollBy(36);
        return true;
    }

    if (key == juce::KeyPress::pageUpKey)
    {
        scrollBy(-juce::jmax(36, viewport_.getHeight() - 36));
        return true;
    }

    if (key == juce::KeyPress::pageDownKey)
    {
        scrollBy(juce::jmax(36, viewport_.getHeight() - 36));
        return true;
    }

    if (key == juce::KeyPress::homeKey)
    {
        scrollTo(0);
        return true;
    }

    if (key == juce::KeyPress::endKey)
    {
        scrollTo(getMaxScrollY());
        return true;
    }

    return false;
}

void ChatDisplay::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    juce::ignoreUnused(event);

    const float wheelDelta = std::abs(wheel.deltaY) > std::abs(wheel.deltaX)
        ? wheel.deltaY
        : wheel.deltaX;
    if (std::abs(wheelDelta) < 0.0001f)
        return;

    const int delta = static_cast<int>(std::round(-wheelDelta * 480.0f));
    if (delta == 0)
        return;

    scrollTo(viewport_.getViewPositionY() + delta);
}

void ChatDisplay::pushAndScroll()
{
    const int vw = viewport_.getMaximumVisibleWidth();
    if (vw <= 0) return;
    canvas_.layout(vw, viewport_.getHeight());
    canvas_.repaint();
    scrollTo(getMaxScrollY());
}

void ChatDisplay::scrollBy(int deltaY)
{
    scrollTo(viewport_.getViewPositionY() + deltaY);
}

void ChatDisplay::scrollTo(int y)
{
    viewport_.setViewPosition(0, juce::jlimit(0, getMaxScrollY(), y));
}

int ChatDisplay::getMaxScrollY() const
{
    return juce::jmax(0, canvas_.getHeight() - viewport_.getHeight());
}

} // namespace more_phi
