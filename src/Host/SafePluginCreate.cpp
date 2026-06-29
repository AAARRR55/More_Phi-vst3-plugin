/*
 * More-Phi — Host/SafePluginCreate.cpp
 *
 * IMPLEMENTATION NOTE: This translation unit is compiled with /EHa on MSVC
 * (see CMakeLists.txt). Under /EHa, a C++ catch(...) block ALSO catches
 * structured (hardware) exceptions such as access violations. That is exactly
 * what we need around createPluginInstance, which calls into a third-party
 * VST3 COM factory that may fault. Without this, /EHsc means catch(...) does
 * NOT catch SEH, so a buggy factory crashes the host (FL Studio) the moment
 * the user picks that plugin from the dropdown.
 *
 * We cannot use __try/__except here directly because createPluginInstance
 * returns a std::unique_ptr (C2712: cannot use __try in functions requiring
 * object unwinding). /EHa sidesteps that entirely.
 */
#include "SafePluginCreate.h"

namespace more_phi {

std::unique_ptr<juce::AudioPluginInstance> safeCreatePluginInstance(
    juce::AudioPluginFormatManager& formatManager,
    const juce::PluginDescription& desc,
    double sampleRate, int blockSize,
    juce::String& errorMessage)
{
    try
    {
        return formatManager.createPluginInstance(desc, sampleRate, blockSize, errorMessage);
    }
    catch (const std::exception& e)
    {
        errorMessage = "Exception during plugin creation: ";
        errorMessage << e.what();
        return nullptr;
    }
    catch (...)
    {
        // Under /EHa this catches hardware (SEH) exceptions too.
        errorMessage = "Hardware/unknown exception during plugin creation";
        return nullptr;
    }
}

} // namespace more_phi
