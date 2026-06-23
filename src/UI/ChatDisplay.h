/*
 * More-Phi - UI/ChatDisplay.h
 * Scrollable assistant transcript with proper multi-line layout.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

namespace more_phi {

class ChatDisplay final : public juce::Component
{
public:
    enum class Role { System, User, Assistant };

    struct Message
    {
        Role         role = Role::Assistant;
        juce::String text;
    };

    ChatDisplay();

    void addMessage(Role role, juce::String text);
    /** Replace the text of the last message. No-op if no messages exist. */
    void updateLastMessage(juce::String text);
    void clearMessages();

    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;

#if MORE_PHI_TEST_MODE
    int getScrollYForTests() const { return viewport_.getViewPositionY(); }
    int getCanvasHeightForTests() const { return canvas_.getHeight(); }
    int getViewportHeightForTests() const { return viewport_.getHeight(); }
    int getMaxScrollYForTests() const { return getMaxScrollY(); }
#endif

private:
    // ── Scrollable canvas ────────────────────────────────────────────────────
    // ponytail: each message is a read-only TextEditor so the user can select
    // and copy arbitrary spans. The role label + bubble are still painted; the
    // selectable text is the TextEditor's, not drawn.
    class MessageEditor final : public juce::TextEditor
    {
    public:
        MessageEditor();
        void colourChanged() override;
    };

    class Canvas final : public juce::Component
    {
    public:
        Canvas();
        std::vector<Message> messages;
        void paint(juce::Graphics& g) override;
        /** Resize canvas height to fit all messages at the given viewport width. */
        void layout(int viewportWidth, int viewportHeight);
        /** Rebuild the read-only TextEditors to match `messages`. */
        void rebuildEditors();

        // ponytail: one editor per message, kept in lockstep with `messages`.
        juce::OwnedArray<MessageEditor> editors;
    };

    juce::Viewport viewport_;
    Canvas         canvas_;

    /** Relayout canvas and scroll to the newest message. */
    void pushAndScroll();
    void scrollBy(int deltaY);
    void scrollTo(int y);
    int getMaxScrollY() const;
    /** Full transcript as "Role: text" lines, for copy-all-to-clipboard. */
    juce::String getAllTranscriptText() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChatDisplay)
};

} // namespace more_phi
