/*
 * More-Phi — Host/SafePluginCreate.h
 *
 * SEH-guarded wrapper around AudioPluginFormatManager::createPluginInstance.
 *
 * Why a separate header + TU:
 *   createPluginInstance returns std::unique_ptr by value. MSVC forbids using
 *   __try/__except in a function that holds objects requiring unwinding
 *   (error C2712), so the SEH guard cannot live next to a unique_ptr. The
 *   matching .cpp is compiled with /EHa, under which a C++ catch(...) ALSO
 *   catches structured (hardware) exceptions. That lets us wrap the whole
 *   creation call — including the unique_ptr result — in a single try/catch
 *   that survives an access violation from a buggy third-party VST3 factory
 *   (e.g. Spectrasonics, some iZotope builds), converting a host crash into
 *   a graceful load failure.
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>

namespace more_phi {

/**
 * Calls createPluginInstance under a hardware-exception guard.
 * On any exception (C++ or SEH), returns nullptr and fills errorMessage.
 * Implemented in SafePluginCreate.cpp (compiled with /EHa on MSVC).
 */
std::unique_ptr<juce::AudioPluginInstance> safeCreatePluginInstance(
    juce::AudioPluginFormatManager& formatManager,
    const juce::PluginDescription& desc,
    double sampleRate, int blockSize,
    juce::String& errorMessage);

} // namespace more_phi
