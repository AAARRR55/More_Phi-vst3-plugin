/*
 * More-Phi — Core/AdaptiveEQ.cpp
 */
#include "AdaptiveEQ.h"
#include <algorithm>
#include <cmath>

namespace more_phi {

static constexpr float kPi = 3.14159265358979f;

AdaptiveEQ::AdaptiveEQ()
{
    // Mark all bands dirty so coefficients are computed on first prepare()
    for (auto& p : params_)
        p.dirty.store(true, std::memory_order_relaxed);
}

void AdaptiveEQ::setBand(int band, const BandParams& p) noexcept
{
    if (band < 0 || band >= kNumBands) return;
    auto& ap = params_[band];
    ap.freqHz.store(p.freqHz, std::memory_order_relaxed);
    ap.gainDB.store(std::clamp(p.gainDB, -kMaxGainDB, kMaxGainDB), std::memory_order_relaxed);
    ap.Q.store(std::max(0.1f, p.Q), std::memory_order_relaxed);
    ap.type.store(static_cast<int>(p.type), std::memory_order_relaxed);
    ap.enabled.store(p.enabled, std::memory_order_relaxed);
    ap.dirty.store(true, std::memory_order_release);
}

void AdaptiveEQ::setBandGain(int band, float gainDB) noexcept
{
    if (band < 0 || band >= kNumBands) return;
    params_[band].gainDB.store(std::clamp(gainDB, -kMaxGainDB, kMaxGainDB), std::memory_order_relaxed);
    params_[band].dirty.store(true, std::memory_order_release);
}

AdaptiveEQ::BandParams AdaptiveEQ::getBand(int band) const noexcept
{
    if (band < 0 || band >= kNumBands) return {};
    const auto& ap = params_[band];
    BandParams p;
    p.freqHz  = ap.freqHz.load(std::memory_order_relaxed);
    p.gainDB  = ap.gainDB.load(std::memory_order_relaxed);
    p.Q       = ap.Q.load(std::memory_order_relaxed);
    p.type    = static_cast<BandType>(ap.type.load(std::memory_order_relaxed));
    p.enabled = ap.enabled.load(std::memory_order_relaxed);
    return p;
}

void AdaptiveEQ::prepare(double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate;
    for (int b = 0; b < kNumBands; ++b)
        recomputeCoeffs(b);
    reset();
}

void AdaptiveEQ::reset() noexcept
{
    for (auto& bandStates : states_)
        for (auto& s : bandStates)
            s = {0.f, 0.f};
}

AdaptiveEQ::BiquadCoeffs AdaptiveEQ::makePeak(float f, float g, float Q, double sr) noexcept
{
    const float A  = std::pow(10.f, g / 40.f);
    const float w0 = 2.f * kPi * f / static_cast<float>(sr);
    const float cw = std::cos(w0);
    const float sw = std::sin(w0);
    const float alpha = sw / (2.f * Q);

    BiquadCoeffs c;
    const float inv = 1.f / (1.f + alpha / A);
    c.b0 = (1.f + alpha * A) * inv;
    c.b1 = -2.f * cw * inv;
    c.b2 = (1.f - alpha * A) * inv;
    c.a1 = -2.f * cw * inv;
    c.a2 = (1.f - alpha / A) * inv;
    return c;
}

AdaptiveEQ::BiquadCoeffs AdaptiveEQ::makeLowShelf(float f, float g, float Q, double sr) noexcept
{
    const float A  = std::pow(10.f, g / 40.f);
    const float w0 = 2.f * kPi * f / static_cast<float>(sr);
    const float cw = std::cos(w0);
    const float sw = std::sin(w0);
    const float alpha = sw / (2.f * Q);
    const float sqA  = std::sqrt(A);

    BiquadCoeffs c;
    const float a0 = (A + 1.f) + (A - 1.f) * cw + 2.f * sqA * alpha;
    const float inv = 1.f / a0;
    c.b0 = A * ((A + 1.f) - (A - 1.f) * cw + 2.f * sqA * alpha) * inv;
    c.b1 = 2.f * A * ((A - 1.f) - (A + 1.f) * cw) * inv;
    c.b2 = A * ((A + 1.f) - (A - 1.f) * cw - 2.f * sqA * alpha) * inv;
    c.a1 = -2.f * ((A - 1.f) + (A + 1.f) * cw) * inv;
    c.a2 = ((A + 1.f) + (A - 1.f) * cw - 2.f * sqA * alpha) * inv;
    return c;
}

AdaptiveEQ::BiquadCoeffs AdaptiveEQ::makeHighShelf(float f, float g, float Q, double sr) noexcept
{
    const float A  = std::pow(10.f, g / 40.f);
    const float w0 = 2.f * kPi * f / static_cast<float>(sr);
    const float cw = std::cos(w0);
    const float sw = std::sin(w0);
    const float alpha = sw / (2.f * Q);
    const float sqA  = std::sqrt(A);

    BiquadCoeffs c;
    const float a0 = (A + 1.f) - (A - 1.f) * cw + 2.f * sqA * alpha;
    const float inv = 1.f / a0;
    c.b0 = A * ((A + 1.f) + (A - 1.f) * cw + 2.f * sqA * alpha) * inv;
    c.b1 = -2.f * A * ((A - 1.f) + (A + 1.f) * cw) * inv;
    c.b2 = A * ((A + 1.f) + (A - 1.f) * cw - 2.f * sqA * alpha) * inv;
    c.a1 = 2.f * ((A - 1.f) - (A + 1.f) * cw) * inv;
    c.a2 = ((A + 1.f) - (A - 1.f) * cw - 2.f * sqA * alpha) * inv;
    return c;
}

AdaptiveEQ::BiquadCoeffs AdaptiveEQ::makeLowPass(float f, float Q, double sr) noexcept
{
    const float w0 = 2.f * kPi * f / static_cast<float>(sr);
    const float cw = std::cos(w0);
    const float sw = std::sin(w0);
    const float alpha = sw / (2.f * Q);

    BiquadCoeffs c;
    const float inv = 1.f / (1.f + alpha);
    c.b0 = (1.f - cw) * 0.5f * inv;
    c.b1 = (1.f - cw) * inv;
    c.b2 = (1.f - cw) * 0.5f * inv;
    c.a1 = -2.f * cw * inv;
    c.a2 = (1.f - alpha) * inv;
    return c;
}

AdaptiveEQ::BiquadCoeffs AdaptiveEQ::makeHighPass(float f, float Q, double sr) noexcept
{
    const float w0 = 2.f * kPi * f / static_cast<float>(sr);
    const float cw = std::cos(w0);
    const float sw = std::sin(w0);
    const float alpha = sw / (2.f * Q);

    BiquadCoeffs c;
    const float inv = 1.f / (1.f + alpha);
    c.b0 =  (1.f + cw) * 0.5f * inv;
    c.b1 = -(1.f + cw) * inv;
    c.b2 =  (1.f + cw) * 0.5f * inv;
    c.a1 = -2.f * cw * inv;
    c.a2 = (1.f - alpha) * inv;
    return c;
}

void AdaptiveEQ::recomputeCoeffs(int band) noexcept
{
    auto& ap        = params_[band];
    const float f   = ap.freqHz.load(std::memory_order_relaxed);
    const float g   = ap.gainDB.load(std::memory_order_relaxed);
    const float Q   = ap.Q.load(std::memory_order_relaxed);
    const auto  typ = static_cast<BandType>(ap.type.load(std::memory_order_relaxed));

    switch (typ) {
        case BandType::Peak:       coeffs_[band] = makePeak(f, g, Q, sampleRate_); break;
        case BandType::LowShelf:   coeffs_[band] = makeLowShelf(f, g, Q, sampleRate_); break;
        case BandType::HighShelf:  coeffs_[band] = makeHighShelf(f, g, Q, sampleRate_); break;
        case BandType::LowPass:    coeffs_[band] = makeLowPass(f, Q, sampleRate_); break;
        case BandType::HighPass:   coeffs_[band] = makeHighPass(f, Q, sampleRate_); break;
    }
    ap.dirty.store(false, std::memory_order_release);
}

void AdaptiveEQ::processBlock(juce::AudioBuffer<float>& buf) noexcept
{
    if (!enabled_.load(std::memory_order_relaxed)) return;

    const int ns  = buf.getNumSamples();
    const int nch = std::min(buf.getNumChannels(), kMaxChannels);

    for (int b = 0; b < kNumBands; ++b)
    {
        auto& ap = params_[b];
        if (!ap.enabled.load(std::memory_order_relaxed)) continue;

        // Recompute coefficients if dirty (atomic acquire)
        if (ap.dirty.load(std::memory_order_acquire))
            recomputeCoeffs(b);

        const BiquadCoeffs& c = coeffs_[b];

        // Skip unity bands (saves CPU for unused bands)
        if (std::abs(c.b0 - 1.f) < 1e-6f && std::abs(c.b1) < 1e-6f &&
            std::abs(c.b2) < 1e-6f && std::abs(c.a1) < 1e-6f && std::abs(c.a2) < 1e-6f)
            continue;

        for (int ch = 0; ch < nch; ++ch)
        {
            float* data = buf.getWritePointer(ch);
            BiquadState& st = states_[b][ch];

            for (int i = 0; i < ns; ++i)
            {
                const float x = data[i];
                const float y = c.b0 * x + st.z1;
                st.z1 = c.b1 * x - c.a1 * y + st.z2;
                st.z2 = c.b2 * x - c.a2 * y;
                data[i] = y;
            }
        }
    }
}

} // namespace more_phi
