/*
 * More-Phi — Core/LUFSMeter.cpp
 *
 * ITU-R BS.1770-4 integrated loudness meter implementation.
 */
#include "LUFSMeter.h"
#include <cstring>
#include <numeric>

namespace more_phi {

// ── Biquad helpers ────────────────────────────────────────────────────────────

float LUFSMeter::processBiquad(float x, BiquadState& st, const BiquadCoeffs& c) noexcept
{
    // Direct Form II Transposed
    const float y = c.b0 * x + st.z1;
    st.z1 = c.b1 * x - c.a1 * y + st.z2;
    st.z2 = c.b2 * x - c.a2 * y;
    return y;
}

void LUFSMeter::computeKWeightingCoeffs() noexcept
{
    const double fs = sampleRate_;
    const double K = 2.0 * fs;
    const double K2 = K * K;

    // Continuous-time coefficients for Stage 1 (pre-filter, high shelving):
    // Solved to match ITU-R BS.1770-4 coefficients exactly at 48 kHz
    const double B2 = 1.58486548742880;
    const double B1 = 18886.9143780365;
    const double B0 = 112594507.269791;
    const double A2 = 1.0;
    const double A1 = 15004.8465267433;
    const double A0 = 112594507.269790;

    const double a0_pre = A2 * K2 + A1 * K + A0;
    pre_.b0 = static_cast<float>((B2 * K2 + B1 * K + B0) / a0_pre);
    pre_.b1 = static_cast<float>(2.0 * (B0 - B2 * K2) / a0_pre);
    pre_.b2 = static_cast<float>((B2 * K2 - B1 * K + B0) / a0_pre);
    pre_.a1 = static_cast<float>(2.0 * (A0 - A2 * K2) / a0_pre);
    pre_.a2 = static_cast<float>((A2 * K2 - A1 * K + A0) / a0_pre);

    // Continuous-time coefficients for Stage 2 (RLB high pass):
    // Solved to match ITU-R BS.1770-4 coefficients exactly at 48 kHz
    const double B2_hp = 1.00499491883582;
    const double B1_hp = 0.0;
    const double B0_hp = 0.0;
    const double A2_hp = 1.0;
    const double A1_hp = 478.91221124467;
    const double A0_hp = 57414.259359048;

    const double a0_hp = A2_hp * K2 + A1_hp * K + A0_hp;
    hp_.b0 = static_cast<float>((B2_hp * K2 + B1_hp * K + B0_hp) / a0_hp);
    hp_.b1 = static_cast<float>(2.0 * (B0_hp - B2_hp * K2) / a0_hp);
    hp_.b2 = static_cast<float>((B2_hp * K2 - B1_hp * K + B0_hp) / a0_hp);
    hp_.a1 = static_cast<float>(2.0 * (A0_hp - A2_hp * K2) / a0_hp);
    hp_.a2 = static_cast<float>((A2_hp * K2 - A1_hp * K + A0_hp) / a0_hp);
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void LUFSMeter::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_       = sampleRate;
    blockSizeSamples_ = static_cast<int>(sampleRate * 0.1);  // 100 ms
    computeKWeightingCoeffs();
    gated_.reserve(kHistoryBlocks);
    lraGated_.reserve(kHistoryBlocks);
    reset();
}

void LUFSMeter::reset() noexcept
{
    for (int ch = 0; ch < kMaxChannels; ++ch)
    {
        preState_[ch] = {};
        hpState_[ch]  = {};
        blockSumSq_[ch] = 0.0f;
    }
    blockAccum_  = 0;
    historyHead_ = 0;
    historyCount_ = 0;
    blockMS_.fill(0.0f);

    momentary_.store(-std::numeric_limits<float>::infinity(), std::memory_order_relaxed);
    shortTerm_.store(-std::numeric_limits<float>::infinity(), std::memory_order_relaxed);
    integrated_.store(-std::numeric_limits<float>::infinity(), std::memory_order_relaxed);
    lra_.store(0.0f, std::memory_order_relaxed);
}

// ── Audio thread ──────────────────────────────────────────────────────────────

void LUFSMeter::processBlock(const float* const* channels,
                              int numChannels,
                              int numSamples) noexcept
{
    const int nch = std::min(numChannels, kMaxChannels);

    for (int i = 0; i < numSamples; ++i)
    {
        for (int ch = 0; ch < nch; ++ch)
        {
            // Apply K-weighting (two cascaded biquads)
            float x = channels[ch][i];
            x = processBiquad(x, preState_[ch], pre_);
            x = processBiquad(x, hpState_[ch],  hp_);

            // Accumulate mean square
            blockSumSq_[ch] += x * x;
        }
        ++blockAccum_;

        // Commit block when 100ms of samples have accumulated
        if (blockAccum_ >= blockSizeSamples_)
            commitBlock(nch);
    }
}

void LUFSMeter::commitBlock(int numChannels) noexcept
{
    // ITU-R BS.1770-4 channel-weighted mean square
    // Weights: L=1.0, R=1.0, C=1.0, Ls=1.41, Rs=1.41 (stereo: L+R only)
    float ms = 0.0f;
    const float blockSamples = static_cast<float>(blockSizeSamples_);
    for (int ch = 0; ch < numChannels; ++ch)
        ms += blockSumSq_[ch] / blockSamples;
    // For stereo, no extra channel weighting (both G=1.0)

    // Reset accumulator
    for (int ch = 0; ch < kMaxChannels; ++ch)
        blockSumSq_[ch] = 0.0f;
    blockAccum_ = 0;

    // Store block mean-square in circular history
    blockMS_[static_cast<size_t>(historyHead_)] = ms;
    historyHead_ = (historyHead_ + 1) % kHistoryBlocks;
    if (historyCount_ < kHistoryBlocks)
        ++historyCount_;

    // Update all long-term metrics
    updateLongTermMetrics();
}

float LUFSMeter::windowedMeanLUFS(int numBlocks) const noexcept
{
    const int count = std::min(numBlocks, historyCount_);
    if (count <= 0) return -std::numeric_limits<float>::infinity();

    float sumMs = 0.0f;
    for (int i = 0; i < count; ++i)
    {
        const int idx = (historyHead_ - 1 - i + kHistoryBlocks) % kHistoryBlocks;
        sumMs += blockMS_[static_cast<size_t>(idx)];
    }
    return meanSquareToLUFS(sumMs / static_cast<float>(count));
}

void LUFSMeter::updateLongTermMetrics() noexcept
{
    // Momentary: last 4 blocks (400 ms)
    momentary_.store(windowedMeanLUFS(4), std::memory_order_relaxed);

    // Short-term: last 30 blocks (3 s)
    shortTerm_.store(windowedMeanLUFS(30), std::memory_order_relaxed);

    // Integrated: BS.1770-4 gated mean
    if (historyCount_ < 4) // Need at least 400ms (4 blocks) to form a Momentary block
    {
        integrated_.store(-std::numeric_limits<float>::infinity(), std::memory_order_relaxed);
        lra_.store(0.0f, std::memory_order_relaxed);
        return;
    }

    gated_.clear();

    // 1. Generate 400ms blocks (Momentary blocks) and apply absolute gate
    float sumMsAbs = 0.0f;
    int absCount = 0;

    const int numMomBlocks = historyCount_ - 3;
    for (int j = 0; j < numMomBlocks; ++j)
    {
        // Compute mean-square of the 400ms block ending at index j+3
        float momMs = 0.0f;
        for (int i = 0; i < 4; ++i)
        {
            const int idx = (historyHead_ - historyCount_ + j + i + kHistoryBlocks) % kHistoryBlocks;
            momMs += blockMS_[static_cast<size_t>(idx)];
        }
        momMs /= 4.0f;

        const float momLUFS = meanSquareToLUFS(momMs);
        if (momLUFS > kAbsoluteGateLUFS)
        {
            sumMsAbs += momMs;
            gated_.push_back(momMs);
            absCount++;
        }
    }

    if (absCount == 0)
    {
        integrated_.store(-std::numeric_limits<float>::infinity(), std::memory_order_relaxed);
        lra_.store(0.0f, std::memory_order_relaxed);
        return;
    }

    // 2. Calculate relative gate threshold
    const float absGatedLUFS = meanSquareToLUFS(sumMsAbs / static_cast<float>(absCount));
    const float relGateThreshold = absGatedLUFS + kRelativeGateOffset; // kRelativeGateOffset is -10.0f

    // 3. Apply relative gate and calculate Integrated Loudness
    float sumMsRel = 0.0f;
    int relCount = 0;

    for (float momMs : gated_)
    {
        if (meanSquareToLUFS(momMs) > relGateThreshold)
        {
            sumMsRel += momMs;
            relCount++;
        }
    }

    if (relCount == 0)
    {
        integrated_.store(absGatedLUFS, std::memory_order_relaxed);
    }
    else
    {
        integrated_.store(meanSquareToLUFS(sumMsRel / static_cast<float>(relCount)), std::memory_order_relaxed);
    }

    // ── Loudness Range (LRA) ──────────────────────────────────────────────────
    // LRA is based on Short-Term (3-second) loudness blocks (30 overlapping 100ms blocks)
    if (historyCount_ < 30)
    {
        lra_.store(0.0f, std::memory_order_relaxed);
        return;
    }

    lraGated_.clear();

    // 1. Generate 3-second blocks and apply absolute gate
    float sumMsLraAbs = 0.0f;
    int lraAbsCount = 0;

    const int numStBlocks = historyCount_ - 29;
    for (int j = 0; j < numStBlocks; ++j)
    {
        float stMs = 0.0f;
        for (int i = 0; i < 30; ++i)
        {
            const int idx = (historyHead_ - historyCount_ + j + i + kHistoryBlocks) % kHistoryBlocks;
            stMs += blockMS_[static_cast<size_t>(idx)];
        }
        stMs /= 30.0f;

        const float stLUFS = meanSquareToLUFS(stMs);
        if (stLUFS > kAbsoluteGateLUFS)
        {
            sumMsLraAbs += stMs;
            lraGated_.push_back(stLUFS); // store LUFS for percentile sorting
            lraAbsCount++;
        }
    }

    if (lraAbsCount < 2)
    {
        lra_.store(0.0f, std::memory_order_relaxed);
        return;
    }

    // 2. Calculate relative gate threshold for LRA (EBU Tech 3342: -20 LU relative gate)
    const float lraAbsGatedLUFS = meanSquareToLUFS(sumMsLraAbs / static_cast<float>(lraAbsCount));
    const float lraRelGateThreshold = lraAbsGatedLUFS - 20.0f;

    // 3. Apply relative gate to Short-Term blocks
    int lraRelCount = 0;
    for (int i = 0; i < lraAbsCount; ++i)
    {
        if (lraGated_[static_cast<size_t>(i)] > lraRelGateThreshold)
        {
            lraGated_[static_cast<size_t>(lraRelCount++)] = lraGated_[static_cast<size_t>(i)];
        }
    }

    if (lraRelCount >= 2)
    {
        const int idx10 = std::max(0, static_cast<int>(lraRelCount * 0.10f));
        const int idx95 = std::min(lraRelCount - 1, static_cast<int>(lraRelCount * 0.95f));

        std::nth_element(lraGated_.begin(), lraGated_.begin() + idx10, lraGated_.begin() + lraRelCount);
        const float val10 = lraGated_[static_cast<size_t>(idx10)];

        std::nth_element(lraGated_.begin(), lraGated_.begin() + idx95, lraGated_.begin() + lraRelCount);
        const float val95 = lraGated_[static_cast<size_t>(idx95)];

        lra_.store(val95 - val10, std::memory_order_relaxed);
    }
    else
    {
        lra_.store(0.0f, std::memory_order_relaxed);
    }
}

} // namespace more_phi
