/*
 * More-Phi — Core/ParameterDistributionStore.h
 *
 * Header-only posterior distribution store over mastering parameters.
 *
 * After each GeneticOptimizer run, the surviving population is used to
 * estimate a mean and standard deviation per parameter.  This posterior
 * is exposed to the UI via AIAssistant::getSuggestionWithConfidence() to
 * display "mean ± 1σ" confidence intervals next to each parameter.
 *
 * Also used by the heuristic fallback in NeuralCompressor to bias
 * initial compression parameters toward the genre-specific distribution.
 *
 * Thread safety:
 *   updateFromPopulation() — message thread only (GeneticEngine runs there).
 *   get() / sample() — any thread (atomic reads of pre-computed values).
 */
#pragma once

#include <atomic>
#include <vector>
#include <cmath>
#include <algorithm>
#include <functional>

namespace more_phi {

struct ParamDistribution
{
    float mean   = 0.0f;
    float stddev = 0.0f;
};

class ParameterDistributionStore
{
public:
    ParameterDistributionStore() = default;

    /** Resize to hold distributions for numParams parameters. Call from message thread. */
    void resize(int numParams)
    {
        means_.resize(static_cast<size_t>(numParams), 0.f);
        stddevs_.resize(static_cast<size_t>(numParams), 0.f);
        numParams_ = numParams;
    }

    /**
     * Update distributions from a population of parameter vectors.
     * Each element of population is a vector of floats (one per parameter).
     * Call from message thread after GeneticEngine completes a generation.
     */
    void updateFromPopulation(const std::vector<std::vector<float>>& population)
    {
        if (population.empty()) return;
        const int n = static_cast<int>(population[0].size());
        if (n != numParams_) resize(n);

        for (int p = 0; p < n; ++p)
        {
            float sum = 0.f, sum2 = 0.f;
            const int m = static_cast<int>(population.size());
            for (const auto& ind : population)
            {
                if (p >= static_cast<int>(ind.size())) continue;
                sum  += ind[p];
                sum2 += ind[p] * ind[p];
            }
            const float mean  = sum / static_cast<float>(m);
            const float var   = sum2 / static_cast<float>(m) - mean * mean;
            means_[p]   = mean;
            stddevs_[p] = std::sqrt(std::max(0.f, var));
        }
    }

    /** Get distribution for a parameter. Any thread. */
    [[nodiscard]] ParamDistribution get(int paramIndex) const noexcept
    {
        if (paramIndex < 0 || paramIndex >= numParams_) return {};
        return { means_[paramIndex], stddevs_[paramIndex] };
    }

    /**
     * Sample from the distribution using the inverse CDF approximation.
     * u01 is a uniform random value in [0, 1].
     * Returns mean + stddev * quantile(u01) using rational approx of probit.
     */
    [[nodiscard]] float sample(int paramIndex, float u01) const noexcept
    {
        if (paramIndex < 0 || paramIndex >= numParams_) return 0.f;
        const float z = probit(std::clamp(u01, 0.001f, 0.999f));
        return means_[paramIndex] + stddevs_[paramIndex] * z;
    }

    [[nodiscard]] int numParams() const noexcept { return numParams_; }

private:
    // Peter Acklam's rational approx of the probit function (max error < 1.15e-9)
    static float probit(float p) noexcept
    {
        static const float a[] = { -3.969683028665376e+01f,  2.209460984245205e+02f,
                                   -2.759285104469687e+02f,  1.383577518672690e+02f,
                                   -3.066479806614716e+01f,  2.506628277459239e+00f };
        static const float b[] = { -5.447609879822406e+01f,  1.615858368580409e+02f,
                                   -1.556989798598866e+02f,  6.680131188771972e+01f,
                                   -1.328068155288572e+01f };
        static const float c[] = { -7.784894002430293e-03f, -3.223964580411365e-01f,
                                   -2.400758277161838e+00f, -2.549732539343734e+00f,
                                    4.374664141464968e+00f,  2.938163982698783e+00f };
        static const float d[] = {  7.784695709041462e-03f,  3.224671290700398e-01f,
                                    2.445134137142996e+00f,  3.754408661907416e+00f };

        const float pLow  = 0.02425f;
        const float pHigh = 1.f - pLow;

        if (p < pLow)
        {
            const float q = std::sqrt(-2.f * std::log(p));
            return (((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) /
                   ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1.f);
        }
        else if (p <= pHigh)
        {
            const float q = p - 0.5f;
            const float r = q * q;
            return (((((a[0]*r+a[1])*r+a[2])*r+a[3])*r+a[4])*r+a[5])*q /
                   (((((b[0]*r+b[1])*r+b[2])*r+b[3])*r+b[4])*r+1.f);
        }
        else
        {
            const float q = std::sqrt(-2.f * std::log(1.f - p));
            return -(((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) /
                    ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1.f);
        }
    }

    std::vector<float> means_;
    std::vector<float> stddevs_;
    int numParams_ = 0;
};

} // namespace more_phi
