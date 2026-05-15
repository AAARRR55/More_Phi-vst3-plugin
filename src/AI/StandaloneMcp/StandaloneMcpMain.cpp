#include "StandaloneMcpServer.h"

#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>

int main()
{
    try
    {
        juce::ScopedJuceInitialiser_GUI juceInitialiser;
        more_phi::standalone_mcp::StandaloneMcpServer server;
        server.run(std::cin, std::cout);
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[MorePhiMcpServer] Fatal: " << e.what() << "\n";
        return 1;
    }
}
