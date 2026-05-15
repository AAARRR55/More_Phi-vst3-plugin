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

// RBJ high-shelf: gain dBgain at shelf frequency fc, Q = 1/sqrt(2)
LUFSMeter::BiquadCoeffs
LUFSMeter::makeHighShelf(float fc, float dBgain, double sr) noexcept
{
    const float A   = std::pow(10.0f, dBgain / 40.0f);  // sqrt(linear_gain)
    const float w0  = 2.0f * 3.14159265f * fc / static_cast<float>(sr);
    const float cos_w0 = std::cos(w0);
    const float sin_w0 = std::sin(w0);
    const float alpha  = sin_w0 / 2.0f * std::sqrt((A + 1.0f / A)
                         * (1.0f / 0.7071f - 1.0f) + 2.0f);  // S=1 form

    const float a0 =       (A + 1.0f) - (A - 1.0f) * cos_w0 + 2.0f * std::sqrt(A) * alpha;
    const float b0 = A * ( (A + 1.0f) + (A - 1.0f) * cos_w0 + 2.0f * std::sqrt(A) * alpha);
    const float b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cos_w0);
    const float b2 = A * ( (A + 1.0f) + (A - 1.0f) * cos_w0 - 2.0f * std::sqrt(A) * alpha);
    const float a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cos_w0);
    const float a2 = (A + 1.0f) - (A - 1.0f) * cos_w0 - 2.0f * std::sqrt(A) * alpha;

    return { b0/a0, b1/a0, b2/a0, a1/a0, a2/a0 };
}

// 2nd-order Butterworth high-pass at fc with quality factor Q
LUFSMeter::BiquadCoeffs
LUFSMeter::makeHighPass2(float fc, float Q, double sr) noexcept
{
    const float K    = std::tan(3.14159265f * fc / static_cast<float>(sr));
    const float norm = 1.0f / (1.0f + K / Q + K * K);

    return {  norm,           // b0
             -2.0f * norm,    // b1
              norm,           // b2
              2.0f * (K * K - 1.0f) * norm,          // a1
             (1.0f - K / Q + K * K) * norm };         // a2
}

void LUFSMeter::computeKWeightingCoeffs() noexcept
{
    // ITU-R BS.1770-4 K-weighting chain:
    //   Stage 1: high-shelf  +4 dB at 1681.974 Hz (pre-filter)
    //   Stage 2: high-pass   38.135 Hz, Q = 0.5003 (removes sub content)
    pre_ = makeHighShelf(1681.974f, +4.0f,  sampleRate_);
    hp_  = makeHighPass2(38.135f,   0.5003f, sampleRate_);
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void LUFSMeter::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_       = sampleRate;
    blockSizeSamples_ = static_cast<int>(sampleRate * 0.1);  // 100 ms
    computeKWeightingCoeffs();
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
    blockLUFS_.fill(-std::numeric_limits<float>::infinity());

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

    // Store block loudness in circular history
    const float blockL = meanSquareToLUFS(ms);
    blockLUFS_[static_cast<size_t>(historyHead_)] = blockL;
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
    int validBlocks = 0;
    for (int i = 0; i < count; ++i)
    {
        const int idx = (historyHead_ - 1 - i + kHistoryBlocks) % kHistoryBlocks;
        const float lufs = blockLUFS_[static_cast<size_t>(idx)];
        if (lufs > kAbsoluteGateLUFS)
        {
            // Convert LUFS back to mean-square for proper averaging
            sumMs += std::pow(10.0f, (lufs + 0.691f) / 10.0f);
            ++validBlocks;
        }
    }
    if (validBlocks == 0) return -std::numeric_limits<float>::infinity();
    return meanSquareToLUFS(sumMs / static_cast<float>(validBlocks));
}

void LUFSMeter::updateLongTermMetrics() noexcept
{
    // Momentary: last 4 blocks (400 ms)
    momentary_.store(windowedMeanLUFS(4), std::memory_order_relaxed);

    // Short-term: last 30 blocks (3 s)
    shortTerm_.store(windowedMeanLUFS(30), std::memory_order_relaxed);

    // Integrated: BS.1770-4 gated mean
    // Pass 1: absolute gate -70 LUFS — get ungated mean
    if (historyCount_ < 2)
    {
        integrated_.store(-std::numeric_limits<float>::infinity(), std::memory_order_relaxed);
        lra_.store(0.0f, std::memory_order_relaxed);
        return;
    }

    // Collect gated blocks into a small local array (on stack, no heap)
    // At most kHistoryBlocks = 600 entries — safe on stack (600 * 4 = 2.4 KB)
    float gated[kHistoryBlocks]{};
    int   gatedCount = 0;

    float sumMs = 0.0f;
    for (int i = 0; i < historyCount_; ++i)
    {
        const float l = blockLUFS_[static_cast<size_t>(i)];
        if (l > kAbsoluteGateLUFS)
        {
            sumMs += std::pow(10.0f, (l + 0.691f) / 10.0f);
            gated[gatedCount++] = l;
        }
    }

    if (gatedCount == 0)
    {
        integrated_.store(-std::numeric_limits<float>::infinity(), std::memory_order_relaxed);
        lra_.store(0.0f, std::memory_order_relaxed);
        return;
    }

    // Pass 1 ungated mean
    const float ungatedLUFS = meanSquareToLUFS(sumMs / static_cast<float>(gatedCount));
    // Pass 2: relative gate = ungatedLUFS + relativeGateOffset
    const float relGate = ungatedLUFS + kRelativeGateOffset;

    float sumMs2  = 0.0f;
    int   count2  = 0;
    float lraGated[kHistoryBlocks]{};
    int   lraCount = 0;

    for (int i = 0; i < gatedCount; ++i)
    {
        if (gated[i] > relGate)
        {
            sumMs2 += std::pow(10.0f, (gated[i] + 0.691f) / 10.0f);
            ++count2;
            lraGated[lraCount++] = gated[i];
        }
    }

    if (count2 == 0)
    {
        integrated_.store(ungatedLUFS, std::memory_order_relaxed);
        lra_.store(0.0f, std::memory_order_relaxed);
        return;
    }

    integrated_.store(meanSquareToLUFS(sumMs2 / static_cast<float>(count2)),
                      std::memory_order_relaxed);

    // LRA: sort lraGated, take 95th - 10th percentile (EBU TECH 3342)
    if (lraCount >= 2)
    {
        // Simple insertion sort (lraCount ≤ 600, acceptable)
        for (int i = 1; i < lraCount; ++i)
        {
            const float key = lraGated[i];
            int j = i - 1;
            while (j >= 0 && lraGated[j] > key)
            {
                lraGated[j + 1] = lraGated[j];
                --j;
            }
            lraGated[j + 1] = key;
        }
        const int idx10 = std::max(0, static_cast<int>(lraCount * 0.10f));
        const int idx95 = std::min(lraCount - 1, static_cast<int>(lraCount * 0.95f));
        lra_.store(lraGated[idx95] - lraGated[idx10], std::memory_order_relaxed);
    }
    else
    {
        lra_.store(0.0f, std::memory_order_relaxed);
    }
}

} // namespace more_phi
