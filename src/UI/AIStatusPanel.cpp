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
    copyTokenBtn_.addListener(this);

    startTimerHz(2);  // Update status 2x/sec
}

void AIStatusPanel::resized()
{
    auto b = getLocalBounds().reduced(6, 2);
    statusLabel_.setBounds(b.removeFromLeft(60));
    b.removeFromLeft(8);
    portLabel_.setBounds(b.removeFromLeft(90));
    b.removeFromLeft(8);
    clientsLabel_.setBounds(b.removeFromLeft(80));
    b.removeFromLeft(8);
    copyTokenBtn_.setBounds(b.removeFromRight(80));
    b.removeFromRight(6);
    toggleBtn_.setBounds(b.removeFromRight(80));
}

void AIStatusPanel::paint(juce::Graphics& g)
{
    g.setColour(juce::Colour(0xff16213e));
    g.fillRect(getLocalBounds());
    g.setColour(juce::Colour(0xff0f3460));
    g.drawLine(0, 0, static_cast<float>(getWidth()), 0, 1.0f);
}

void AIStatusPanel::timerCallback()
{
    auto& mcp = proc_.getMCPServer();
    bool running = mcp.isRunning();

    statusLabel_.setText(running ? "MCP: ON" : "MCP: OFF", juce::dontSendNotification);
    statusLabel_.setColour(juce::Label::textColourId,
                           running ? juce::Colour(0xff4ade80)  // green
                                   : juce::Colour(0xff888888));

    portLabel_.setText(running ? "Port: " + juce::String(mcp.getPort()) : "",
                       juce::dontSendNotification);
    clientsLabel_.setText(running ? "Clients: " + juce::String(mcp.getConnectedClients()) : "",
                           juce::dontSendNotification);

    toggleBtn_.setButtonText(running ? "Stop MCP" : "Start MCP");
}

void AIStatusPanel::buttonClicked(juce::Button* b)
{
    auto& mcp = proc_.getMCPServer();

    if (b == &toggleBtn_)
    {
        if (mcp.isRunning())
            mcp.stopServer();
        else
            mcp.startServer(30001);
    }
    else if (b == &copyTokenBtn_)
    {
        juce::SystemClipboard::copyTextToClipboard(mcp.getAuthToken());
    }
}

} // namespace more_phi
