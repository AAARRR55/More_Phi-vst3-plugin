/* More-Phi — UI/AIStatusPanel.cpp */
#include "AIStatusPanel.h"
#include "Plugin/PluginProcessor.h"

namespace more_phi {

AIStatusPanel::AIStatusPanel(MorePhiProcessor& p) : proc_(p)
{
    addAndMakeVisible(statusLabel_);
    addAndMakeVisible(portLabel_);
    addAndMakeVisible(clientsLabel_);
    addAndMakeVisible(toggleBtn_);
    addAndMakeVisible(copyTokenBtn_);

    statusLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff888888));
    portLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff888888));
    clientsLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff888888));

    toggleBtn_.addListener(this);
    toggleBtn_.setTooltip(
        "Start or stop the built-in MCP server. When running, AI tools and external "
        "applications can connect on the displayed port to control More-Phi.");
    copyTokenBtn_.addListener(this);
    copyTokenBtn_.setTooltip(
        "Copy the MCP authentication token to the clipboard. "
        "Paste this into your AI client to authorize the connection.");

    startTimerHz(2);  // Update status 2x/sec
}

void AIStatusPanel::resized()
{
    auto b = getLocalBounds().reduced(6, 2);
    statusLabel_.setBounds(b.removeFromLeft(72));
    b.removeFromLeft(6);
    portLabel_.setBounds(b.removeFromLeft(72));
    b.removeFromLeft(6);
    clientsLabel_.setBounds(b.removeFromLeft(72));
    b.removeFromLeft(8);
    copyTokenBtn_.setBounds(b.removeFromRight(80));
    b.removeFromRight(6);
    toggleBtn_.setBounds(b.removeFromRight(80));
}

void AIStatusPanel::paint(juce::Graphics& g)
{
    g.setColour(juce::Colour(0xff0d0d10));
    g.fillRect(getLocalBounds());
    g.setColour(juce::Colour(0xff0f3460));
    g.drawLine(0, 0, static_cast<float>(getWidth()), 0, 1.0f);
}

void AIStatusPanel::timerCallback()
{
    auto& mcp = proc_.getMCPServer();
    const bool running = mcp.isRunning();

    // H5: track pending transitions with timeout
    const juce::int64 now = juce::Time::currentTimeMillis();
    constexpr juce::int64 kTransitionTimeoutMs = 5000;

    if (pendingStart_)
    {
        if (running)
        {
            pendingStart_ = false;
            toggleBtn_.setEnabled(true);
        }
        else if (now - transitionStartMs_ > kTransitionTimeoutMs)
        {
            // Start timed out — surface error and reset
            pendingStart_ = false;
            toggleBtn_.setEnabled(true);
            statusLabel_.setText("[!] Start Failed",
                                 juce::dontSendNotification);
            statusLabel_.setColour(juce::Label::textColourId,
                                   juce::Colour(0xffe94560));  // coral-red
            return;
        }
    }

    if (pendingStop_)
    {
        if (!running)
        {
            pendingStop_ = false;
            toggleBtn_.setEnabled(true);
        }
        else if (now - transitionStartMs_ > kTransitionTimeoutMs)
        {
            pendingStop_ = false;
            toggleBtn_.setEnabled(true);
        }
    }

    // Show transition state with ASCII glyph prefix (colorblind-safe shape + colour)
    if (pendingStart_)
    {
        statusLabel_.setText(juce::CharPointer_UTF8("[~] Starting\u2026"),
                             juce::dontSendNotification);
        statusLabel_.setColour(juce::Label::textColourId,
                               juce::Colour(0xfff9e596));  // amber
        return;
    }
    if (pendingStop_)
    {
        statusLabel_.setText(juce::CharPointer_UTF8("[~] Stopping\u2026"),
                             juce::dontSendNotification);
        statusLabel_.setColour(juce::Label::textColourId,
                               juce::Colour(0xfff9e596));
        return;
    }

    // L6: ASCII glyph prefix gives shape redundancy beyond colour (colorblind-safe).
    statusLabel_.setText(running ? "[*] AI Ready" : "[ ] AI Offline",
                         juce::dontSendNotification);
    statusLabel_.setColour(juce::Label::textColourId,
                           running ? juce::Colour(0xff34d399)  // green
                                   : juce::Colour(0xff888888));

    portLabel_.setText(running ? "Port " + juce::String(mcp.getPort()) : "",
                       juce::dontSendNotification);
    const int clients = mcp.getConnectedClients();
    clientsLabel_.setText(running ? (clients > 0 ? juce::String(clients) + " client" + (clients != 1 ? "s" : "") : "no clients") : "",
                           juce::dontSendNotification);

    toggleBtn_.setButtonText(running ? "Stop MCP" : "Start MCP");
}

void AIStatusPanel::buttonClicked(juce::Button* b)
{
    auto& mcp = proc_.getMCPServer();

    if (b == &toggleBtn_)
    {
        if (pendingStart_ || pendingStop_)
            return;  // debounce during transition (H5)

        if (mcp.isRunning())
        {
            pendingStop_ = true;
            transitionStartMs_ = juce::Time::currentTimeMillis();
            toggleBtn_.setEnabled(false);
            mcp.stopServer();
        }
        else
        {
            pendingStart_ = true;
            transitionStartMs_ = juce::Time::currentTimeMillis();
            toggleBtn_.setEnabled(false);
            mcp.startServer(30001);
        }
    }
    else if (b == &copyTokenBtn_)
    {
        juce::SystemClipboard::copyTextToClipboard(mcp.getAuthToken());
    }
}

} // namespace more_phi
