/*
 * More-Phi — Core/ABCompareEngine.cpp
 */
#include "ABCompareEngine.h"
#include <juce_core/juce_core.h>

namespace more_phi {

void ABCompareEngine::captureCheckpoint()
{
    if (captureFunc_)
        captureFunc_(kReservedSlot);
    hasPending_.store(true, std::memory_order_relaxed);
}

void ABCompareEngine::startComparison()
{
    comparing_.store(true, std::memory_order_relaxed);
    startTimer(kAnalysisMs);
}

void ABCompareEngine::commitCandidate()
{
    stopTimer();
    comparing_.store(false, std::memory_order_relaxed);
    hasPending_.store(false, std::memory_order_relaxed);
    juce::Logger::writeToLog("ABCompare: candidate committed.");
}

void ABCompareEngine::rollbackCandidate()
{
    stopTimer();
    if (restoreFunc_ && hasPending_.load(std::memory_order_relaxed))
        restoreFunc_(kReservedSlot);
    comparing_.store(false, std::memory_order_relaxed);
    hasPending_.store(false, std::memory_order_relaxed);
    juce::Logger::writeToLog("ABCompare: rolled back to checkpoint.");
}

ABCompareEngine::Metrics ABCompareEngine::readCurrentMetrics() const noexcept
{
    Metrics m;
    if (meter_)
    {
        m.lufsIntegrated = meter_->getIntegrated();
        m.lra            = meter_->getLRA();
    }
    m.spectralScore = 0.f;  // populated by SpectralBalanceAnalyser when integrated
    return m;
}

void ABCompareEngine::timerCallback()
{
    compareAndDecide();
    stopTimer();
}

void ABCompareEngine::compareAndDecide()
{
    if (!hasPending_.load(std::memory_order_relaxed)) return;

    const Metrics candidate = readCurrentMetrics();
    int worseCount = 0;

    // Compare LUFS: candidate should be closer to target (0 dB deviation = better)
    // We compare absolute deviation — a candidate that is too loud or too quiet is worse
    const float lufsBaselineDev   = std::abs(baseline_.lufsIntegrated);
    const float lfusCandidateDev  = std::abs(candidate.lufsIntegrated);
    if (lfusCandidateDev > lufsBaselineDev + 0.5f) ++worseCount;

    // Compare LRA: prefer candidate with LRA closer to genre target (simplified: prefer higher)
    if (candidate.lra < baseline_.lra - 1.0f) ++worseCount;

    // Compare spectral score
    if (candidate.spectralScore < baseline_.spectralScore - 0.5f) ++worseCount;

    if (worseCount >= 2)
    {
        juce::Logger::writeToLog("ABCompare: candidate worse on " +
                                  juce::String(worseCount) + " metrics — rolling back.");
        rollbackCandidate();
    }
    else
    {
        juce::Logger::writeToLog("ABCompare: candidate accepted.");
        commitCandidate();
    }

    comparing_.store(false, std::memory_order_relaxed);
}

} // namespace more_phi
