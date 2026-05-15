/*
 * More-Phi — Host/PluginScanner.h
 * Background thread for scanning VST3/AU plugin folders.
 */
#pragma once

#include "PluginHostManager.h"
#include <juce_core/juce_core.h>

namespace more_phi {

class PluginScanner : private juce::Thread
{
public:
    explicit PluginScanner(PluginHostManager& host)
        : juce::Thread("MorePhi-Scanner"), host_(host) {}

    void startScan()  { startThread(); }
    void stopScan()   { stopThread(5000); }

    bool isScanning() const { return isThreadRunning(); }

private:
    void run() override { host_.scanPluginFolders(); }
    PluginHostManager& host_;
};

} // namespace more_phi
