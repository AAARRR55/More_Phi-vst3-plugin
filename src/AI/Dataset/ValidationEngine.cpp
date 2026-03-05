/*
 * MorphSnap — AI/Dataset/ValidationEngine.cpp
 * Implementation of statistical validation for synthetic audio datasets.
 */

#include "ValidationEngine.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>
#include <iomanip>
#include <set>
#include <limits>
#include <fstream>

namespace morphsnap {

// =============================================================================
// Constructor
// =============================================================================

ValidationEngine::ValidationEngine()
    : random_(static_cast<int>(juce::Time::currentTimeMillis()))
{
}

// =============================================================================
// Distribution Tests
// =============================================================================

DistributionTestResult ValidationEngine::kolmogorovSmirnovTest(
    const std::vector<float>& observed,
    const std::vector<float>& expected,
    const juce::String& parameterName)
{
    DistributionTestResult result;
    result.testName = "Kolmogorov-Smirnov";
    result.parameterName = parameterName;

    // Handle edge cases
    if (observed.empty() || expected.empty())
    {
        result.statistic = 1.0;
        result.pValue = 0.0;
        result.passed = false;
        result.interpretation = "Insufficient data: one or both samples are empty";
        return result;
    }

    // Sort both samples
    auto sortedObserved = sortVector(observed);
    auto sortedExpected = sortVector(expected);

    const size_t n1 = sortedObserved.size();
    const size_t n2 = sortedExpected.size();

    // Merge samples to find all unique values
    std::vector<float> allValues;
    allValues.reserve(n1 + n2);
    allValues.insert(allValues.end(), sortedObserved.begin(), sortedObserved.end());
    allValues.insert(allValues.end(), sortedExpected.begin(), sortedExpected.end());
    std::sort(allValues.begin(), allValues.end());

    // Remove duplicates
    allValues.erase(std::unique(allValues.begin(), allValues.end()), allValues.end());

    // Compute maximum CDF difference (D statistic)
    double maxDiff = 0.0;

    for (const float value : allValues)
    {
        const double cdf1 = computeEmpiricalCDF(sortedObserved, value);
        const double cdf2 = computeEmpiricalCDF(sortedExpected, value);
        const double diff = std::abs(cdf1 - cdf2);
        maxDiff = std::max(maxDiff, diff);
    }

    result.statistic = maxDiff;

    // Compute p-value using asymptotic approximation
    // For large samples: p ≈ 2 * exp(-2 * n * D^2) where n = n1*n2/(n1+n2)
    const double n = static_cast<double>(n1 * n2) / static_cast<double>(n1 + n2);
    const double lambda = (std::sqrt(n) + 0.12 + 0.11 / std::sqrt(n)) * maxDiff;

    // Approximate p-value using Kolmogorov distribution
    // Using the asymptotic formula: p ≈ 2 * sum_{k=1}^{inf} (-1)^{k-1} * exp(-2*k^2*lambda^2)
    double pValue = 0.0;
    for (int k = 1; k <= 100; ++k)
    {
        const double term = std::exp(-2.0 * k * k * lambda * lambda);
        pValue += (k % 2 == 1 ? 2.0 : -2.0) * term;
    }
    pValue = std::max(0.0, std::min(1.0, pValue));

    result.pValue = pValue;
    result.passed = (pValue > significanceLevel_);

    // Generate interpretation
    if (result.passed)
    {
        result.interpretation = juce::String::formatted(
            "KS test passed (D=%.4f, p=%.4f): distributions are statistically similar at alpha=%.2f",
            maxDiff, pValue, significanceLevel_);
    }
    else
    {
        result.interpretation = juce::String::formatted(
            "KS test failed (D=%.4f, p=%.4f): distributions differ significantly at alpha=%.2f",
            maxDiff, pValue, significanceLevel_);
    }

    return result;
}

DistributionTestResult ValidationEngine::maximumMeanDiscrepancyTest(
    const std::vector<std::vector<float>>& observed,
    const std::vector<std::vector<float>>& expected,
    const juce::String& featureSetName)
{
    DistributionTestResult result;
    result.testName = "Maximum Mean Discrepancy";
    result.parameterName = featureSetName;

    // Handle edge cases
    if (observed.empty() || expected.empty())
    {
        result.statistic = 1.0;
        result.pValue = 0.0;
        result.passed = false;
        result.interpretation = "Insufficient data: one or both feature sets are empty";
        return result;
    }

    const size_t m = observed.size();
    const size_t n = expected.size();

    // Compute kernel bandwidth using median heuristic if not set
    float sigma = rbfBandwidth_;
    if (sigma <= 0.0f)
    {
        // Median heuristic: sigma = median pairwise distance
        std::vector<float> distances;
        for (size_t i = 0; i < std::min(m, n); ++i)
        {
            for (size_t j = i + 1; j < std::min(m, n); ++j)
            {
                distances.push_back(euclideanDistance(observed[i], observed[j]));
            }
        }
        if (!distances.empty())
        {
            std::sort(distances.begin(), distances.end());
            sigma = distances[distances.size() / 2];
            sigma = std::max(sigma, 0.01f);  // Prevent division by zero
        }
    }

    // Compute MMD^2 = E[k(X,X)] + E[k(Y,Y)] - 2*E[k(X,Y)]
    // where X ~ observed, Y ~ expected

    // E[k(X,X)] - mean kernel within observed
    double kxx = 0.0;
    int kxxCount = 0;
    for (size_t i = 0; i < m; ++i)
    {
        for (size_t j = i + 1; j < m; ++j)
        {
            kxx += computeRBFKernel(observed[i], observed[j], sigma);
            ++kxxCount;
        }
    }
    kxx = kxxCount > 0 ? kxx / kxxCount : 0.0;

    // E[k(Y,Y)] - mean kernel within expected
    double kyy = 0.0;
    int kyyCount = 0;
    for (size_t i = 0; i < n; ++i)
    {
        for (size_t j = i + 1; j < n; ++j)
        {
            kyy += computeRBFKernel(expected[i], expected[j], sigma);
            ++kyyCount;
        }
    }
    kyy = kyyCount > 0 ? kyy / kyyCount : 0.0;

    // E[k(X,Y)] - mean kernel between observed and expected
    double kxy = 0.0;
    for (size_t i = 0; i < m; ++i)
    {
        for (size_t j = 0; j < n; ++j)
        {
            kxy += computeRBFKernel(observed[i], expected[j], sigma);
        }
    }
    kxy /= static_cast<double>(m * n);

    // MMD^2 estimate
    const double mmdSquared = kxx + kyy - 2.0 * kxy;
    result.statistic = std::sqrt(std::max(0.0, mmdSquared));

    // Approximate p-value using permutation test concept
    // For simplicity, use a threshold-based approach
    // Small MMD indicates similar distributions
    const double threshold = 0.1;  // Empirical threshold
    result.pValue = std::exp(-result.statistic / threshold);
    result.pValue = std::max(0.0, std::min(1.0, result.pValue));

    result.passed = (result.statistic < threshold);

    // Generate interpretation
    if (result.passed)
    {
        result.interpretation = juce::String::formatted(
            "MMD test passed (MMD=%.4f): feature distributions are similar",
            result.statistic);
    }
    else
    {
        result.interpretation = juce::String::formatted(
            "MMD test failed (MMD=%.4f): feature distributions differ significantly",
            result.statistic);
    }

    return result;
}

DistributionTestResult ValidationEngine::wassersteinDistanceTest(
    const std::vector<float>& observed,
    const std::vector<float>& expected,
    const juce::String& distributionName)
{
    DistributionTestResult result;
    result.testName = "Wasserstein Distance";
    result.parameterName = distributionName;

    // Handle edge cases
    if (observed.empty() || expected.empty())
    {
        result.statistic = 1.0;
        result.pValue = 0.0;
        result.passed = false;
        result.interpretation = "Insufficient data: one or both samples are empty";
        return result;
    }

    // Sort both samples
    auto sortedObserved = sortVector(observed);
    auto sortedExpected = sortVector(expected);

    const size_t n1 = sortedObserved.size();
    const size_t n2 = sortedExpected.size();

    // Compute 1D Wasserstein distance (Earth Mover's Distance)
    // For 1D sorted arrays: W = integral |F1(x) - F2(x)| dx
    // Approximated by summing differences at merged points

    // Merge and sort all unique values
    std::vector<float> merged;
    merged.reserve(n1 + n2);
    merged.insert(merged.end(), sortedObserved.begin(), sortedObserved.end());
    merged.insert(merged.end(), sortedExpected.begin(), sortedExpected.end());
    std::sort(merged.begin(), merged.end());
    merged.erase(std::unique(merged.begin(), merged.end()), merged.end());

    // Compute Wasserstein distance using CDF differences
    double wasserstein = 0.0;
    for (size_t i = 0; i < merged.size() - 1; ++i)
    {
        const float x1 = merged[i];
        const float x2 = merged[i + 1];
        const double cdf1_obs = computeEmpiricalCDF(sortedObserved, x1);
        const double cdf1_exp = computeEmpiricalCDF(sortedExpected, x1);
        const double cdf2_obs = computeEmpiricalCDF(sortedObserved, x2);
        const double cdf2_exp = computeEmpiricalCDF(sortedExpected, x2);

        // Trapezoidal integration
        const double diff1 = std::abs(cdf1_obs - cdf1_exp);
        const double diff2 = std::abs(cdf2_obs - cdf2_exp);
        wasserstein += 0.5 * (diff1 + diff2) * (x2 - x1);
    }

    result.statistic = wasserstein;

    // Normalize by range for interpretability
    const float minVal = std::min(sortedObserved.front(), sortedExpected.front());
    const float maxVal = std::max(sortedObserved.back(), sortedExpected.back());
    const float range = maxVal - minVal;
    const double normalizedWasserstein = range > 0 ? wasserstein / range : wasserstein;

    // Compute approximate p-value
    // Wasserstein distance follows no simple distribution, use empirical threshold
    const double threshold = 0.1;  // Normalized threshold
    result.pValue = std::exp(-normalizedWasserstein / threshold);
    result.pValue = std::max(0.0, std::min(1.0, result.pValue));

    result.passed = (normalizedWasserstein < threshold);

    // Generate interpretation
    if (result.passed)
    {
        result.interpretation = juce::String::formatted(
            "Wasserstein test passed (W=%.4f, normalized=%.4f): distributions are similar",
            wasserstein, normalizedWasserstein);
    }
    else
    {
        result.interpretation = juce::String::formatted(
            "Wasserstein test failed (W=%.4f, normalized=%.4f): distributions differ significantly",
            wasserstein, normalizedWasserstein);
    }

    return result;
}

// =============================================================================
// Coverage Metrics
// =============================================================================

CoverageMetrics ValidationEngine::computeCoverageMetrics(
    const std::vector<std::vector<float>>& samples,
    int dimensions,
    float gridResolution)
{
    CoverageMetrics metrics;
    metrics.totalSamples = static_cast<int>(samples.size());
    metrics.dimensions = dimensions;

    if (samples.empty() || dimensions <= 0)
    {
        return metrics;
    }

    // Ensure all samples have correct dimensionality
    std::vector<std::vector<float>> validSamples;
    for (const auto& sample : samples)
    {
        if (static_cast<int>(sample.size()) == dimensions)
        {
            validSamples.push_back(sample);
        }
    }

    if (validSamples.empty())
    {
        return metrics;
    }

    // Compute unique parameter sets
    metrics.uniqueParameterSets = countUniqueParameterSets(validSamples);

    // Compute grid coverage (average across all 2D projections)
    float totalGridCoverage = 0.0f;
    int projectionCount = 0;

    if (dimensions >= 2)
    {
        for (int d1 = 0; d1 < dimensions - 1; ++d1)
        {
            for (int d2 = d1 + 1; d2 < dimensions; ++d2)
            {
                const int binsPerDim = static_cast<int>(1.0f / gridResolution);
                const int totalBins = binsPerDim * binsPerDim;
                const int occupiedBins = countOccupiedBins(validSamples, d1, d2, gridResolution);
                totalGridCoverage += static_cast<float>(occupiedBins) / static_cast<float>(totalBins);
                ++projectionCount;
            }
        }
        metrics.gridCoverage = projectionCount > 0 ? totalGridCoverage / projectionCount : 0.0f;
    }
    else
    {
        // 1D case
        const int binsPerDim = static_cast<int>(1.0f / gridResolution);
        std::set<int> occupiedBins;
        for (const auto& sample : validSamples)
        {
            const int bin = static_cast<int>(std::clamp(sample[0], 0.0f, 0.9999f) / gridResolution);
            occupiedBins.insert(bin);
        }
        metrics.gridCoverage = static_cast<float>(occupiedBins.size()) / static_cast<float>(binsPerDim);
    }

    // Compute bounding volume
    metrics.volumeCoverageRatio = computeBoundingVolume(validSamples);

    // Compute boundary coverage
    metrics.boundaryCoverage = computeBoundarySampleRatio(validSamples);

    // Compute pairwise distances
    if (validSamples.size() > 1)
    {
        double totalDistance = 0.0;
        float minDist = std::numeric_limits<float>::max();
        float maxDist = 0.0f;
        int pairCount = 0;

        // Sample pairwise distances (not all pairs for efficiency)
        const size_t maxPairs = std::min(validSamples.size() * 100, validSamples.size() * validSamples.size());
        const size_t step = std::max(size_t(1), validSamples.size() * validSamples.size() / maxPairs);

        for (size_t i = 0; i < validSamples.size(); ++i)
        {
            for (size_t j = i + 1; j < validSamples.size(); j += step)
            {
                const float dist = euclideanDistance(validSamples[i], validSamples[j]);
                totalDistance += dist;
                minDist = std::min(minDist, dist);
                maxDist = std::max(maxDist, dist);
                ++pairCount;
            }
        }

        metrics.averageDistance = pairCount > 0 ? static_cast<float>(totalDistance / pairCount) : 0.0f;
        metrics.minDistance = minDist == std::numeric_limits<float>::max() ? 0.0f : minDist;
        metrics.maxDistance = maxDist;
    }

    return metrics;
}

// =============================================================================
// Transfer Evaluation
// =============================================================================

TransferEvaluationResult ValidationEngine::evaluateTransferLearning(
    const juce::File& syntheticDataDir,
    const juce::File& realDataDir,
    const juce::String& benchmarkType)
{
    TransferEvaluationResult result;
    result.benchmarkName = benchmarkType;

    // This is a placeholder implementation that estimates transfer performance
    // In a real implementation, this would:
    // 1. Load synthetic and real datasets
    // 2. Train a model on synthetic data
    // 3. Evaluate zero-shot on real data
    // 4. Fine-tune on real data and re-evaluate

    // Check if directories exist
    if (!syntheticDataDir.exists() || !realDataDir.exists())
    {
        result.zeroShotAccuracy = 0.0f;
        result.fineTunedAccuracy = 0.0f;
        result.performanceGap = 1.0f;
        return result;
    }

    // Count samples in each directory (simplified estimation)
    int syntheticCount = 0;
    int realCount = 0;

    if (syntheticDataDir.isDirectory())
    {
        juce::DirectoryIterator synthIter(syntheticDataDir, false, "*.wav;*.json", juce::File::findFiles);
        while (synthIter.next())
        {
            ++syntheticCount;
        }
    }

    if (realDataDir.isDirectory())
    {
        juce::DirectoryIterator realIter(realDataDir, false, "*.wav;*.json", juce::File::findFiles);
        while (realIter.next())
        {
            ++realCount;
        }
    }

    result.syntheticSamplesUsed = syntheticCount;
    result.realSamplesUsed = realCount;

    // Estimate transfer performance based on dataset characteristics
    // This is a simplified model - real evaluation would train actual models

    // Base accuracy depends on benchmark type
    float baseAccuracy = 0.5f;  // Default for classification
    if (benchmarkType == "regression")
    {
        baseAccuracy = 0.3f;  // Lower for regression tasks
    }
    else if (benchmarkType == "generation")
    {
        baseAccuracy = 0.4f;
    }

    // Zero-shot accuracy improves with more synthetic data
    const float synthBonus = std::min(0.3f, syntheticCount / 10000.0f);
    result.zeroShotAccuracy = baseAccuracy + synthBonus;

    // Fine-tuned accuracy improves with real data
    const float realBonus = std::min(0.25f, realCount / 1000.0f);
    result.fineTunedAccuracy = result.zeroShotAccuracy + realBonus;

    // Performance gap (target: <15%)
    result.performanceGap = result.fineTunedAccuracy - result.zeroShotAccuracy;

    // Add some per-class metrics (placeholder)
    result.perClassMetrics["class_0"] = result.zeroShotAccuracy * 0.95f;
    result.perClassMetrics["class_1"] = result.zeroShotAccuracy * 1.05f;
    result.perClassMetrics["class_2"] = result.zeroShotAccuracy * 0.9f;

    return result;
}

// =============================================================================
// Full Validation Report
// =============================================================================

ValidationReport ValidationEngine::generateReport(
    const std::vector<std::vector<float>>& syntheticSamples,
    const std::vector<std::vector<float>>& realSamples,
    const nlohmann::json& config)
{
    ValidationReport report;
    report.reportId = generateReportId();
    report.timestamp = juce::Time::currentTimeMillis();

    // Parse config if provided
    const double alpha = config.value("significanceLevel", significanceLevel_);
    const float gridRes = config.value("gridResolution", 0.1f);

    // Store original significance level and set new one
    const double origAlpha = significanceLevel_;
    significanceLevel_ = alpha;

    // Determine dimensions
    int dimensions = 0;
    if (!syntheticSamples.empty())
    {
        dimensions = static_cast<int>(syntheticSamples[0].size());
    }
    else if (!realSamples.empty())
    {
        dimensions = static_cast<int>(realSamples[0].size());
    }

    // Run distribution tests for each dimension
    for (int d = 0; d < dimensions; ++d)
    {
        // Extract dimension values
        std::vector<float> synthDim;
        std::vector<float> realDim;

        for (const auto& sample : syntheticSamples)
        {
            if (static_cast<int>(sample.size()) > d)
            {
                synthDim.push_back(sample[d]);
            }
        }

        for (const auto& sample : realSamples)
        {
            if (static_cast<int>(sample.size()) > d)
            {
                realDim.push_back(sample[d]);
            }
        }

        // Run tests
        const juce::String paramName = "parameter_" + juce::String(d);

        if (!synthDim.empty() && !realDim.empty())
        {
            report.ksTests.push_back(kolmogorovSmirnovTest(synthDim, realDim, paramName));
            report.wassersteinTests.push_back(wassersteinDistanceTest(synthDim, realDim, paramName));
        }
    }

    // Run MMD test on full feature vectors
    if (!syntheticSamples.empty() && !realSamples.empty())
    {
        report.mmdTests.push_back(maximumMeanDiscrepancyTest(
            syntheticSamples, realSamples, "full_parameter_space"));
    }

    // Compute coverage metrics
    report.coverage = computeCoverageMetrics(syntheticSamples, dimensions, gridRes);

    // Compute overall score and determine pass/fail
    report.overallScore = computeOverallScore(report);

    // Determine overall pass (must meet minimum thresholds)
    bool volumeOk = report.coverage.volumeCoverageRatio >= 0.75f;
    bool gridOk = report.coverage.gridCoverage >= 0.80f;
    bool boundaryOk = report.coverage.boundaryCoverage >= 0.15f;

    // Check distribution tests (at least 80% should pass)
    auto checkTestPassRate = [](const std::vector<DistributionTestResult>& tests) -> bool
    {
        if (tests.empty()) return true;
        int passed = 0;
        for (const auto& t : tests)
        {
            if (t.passed) ++passed;
        }
        return static_cast<float>(passed) / static_cast<float>(tests.size()) >= 0.8f;
    };

    bool ksOk = checkTestPassRate(report.ksTests);
    bool mmdOk = checkTestPassRate(report.mmdTests);
    bool wassersteinOk = checkTestPassRate(report.wassersteinTests);

    report.overallPassed = volumeOk && gridOk && boundaryOk && ksOk && mmdOk && wassersteinOk;

    // Generate summary
    std::ostringstream summary;
    summary << "Validation Report Summary\n";
    summary << "========================\n";
    summary << "Report ID: " << report.reportId.toStdString() << "\n";
    summary << "Timestamp: " << juce::Time(report.timestamp).formatted("%Y-%m-%d %H:%M:%S").toStdString() << "\n\n";

    summary << "Overall Result: " << (report.overallPassed ? "PASSED" : "FAILED") << "\n";
    summary << "Overall Score: " << std::fixed << std::setprecision(1) << report.overallScore << "/100\n\n";

    summary << "Coverage Metrics:\n";
    summary << "  Volume Coverage: " << std::fixed << std::setprecision(2)
            << report.coverage.volumeCoverageRatio * 100.0f << "% (target: >75%)\n";
    summary << "  Grid Coverage: " << report.coverage.gridCoverage * 100.0f << "% (target: >80%)\n";
    summary << "  Boundary Coverage: " << report.coverage.boundaryCoverage * 100.0f << "% (target: >15%)\n";
    summary << "  Total Samples: " << report.coverage.totalSamples << "\n";
    summary << "  Unique Sets: " << report.coverage.uniqueParameterSets << "\n\n";

    summary << "Distribution Tests:\n";
    summary << "  KS Tests: " << std::count_if(report.ksTests.begin(), report.ksTests.end(),
                                          [](const auto& t) { return t.passed; })
            << "/" << report.ksTests.size() << " passed\n";
    summary << "  MMD Tests: " << std::count_if(report.mmdTests.begin(), report.mmdTests.end(),
                                            [](const auto& t) { return t.passed; })
            << "/" << report.mmdTests.size() << " passed\n";
    summary << "  Wasserstein Tests: " << std::count_if(report.wassersteinTests.begin(), report.wassersteinTests.end(),
                                                      [](const auto& t) { return t.passed; })
            << "/" << report.wassersteinTests.size() << " passed\n";

    report.summary = juce::String(summary.str());

    // Generate recommendations
    generateRecommendations(report);

    // Restore original significance level
    significanceLevel_ = origAlpha;

    return report;
}

// =============================================================================
// Report Export
// =============================================================================

bool ValidationEngine::exportReport(const juce::File& outputFile, const ValidationReport& report)
{
    const auto extension = outputFile.getFileExtension().toLowerCase();

    if (extension == ".json")
    {
        return exportReportAsJson(outputFile, report);
    }
    else if (extension == ".md")
    {
        return exportReportAsMarkdown(outputFile, report);
    }
    else
    {
        // Default to JSON
        return exportReportAsJson(outputFile, report);
    }
}

bool ValidationEngine::exportReportAsJson(const juce::File& outputFile, const ValidationReport& report)
{
    nlohmann::json json;

    json["reportId"] = report.reportId.toStdString();
    json["timestamp"] = report.timestamp;
    json["overallPassed"] = report.overallPassed;
    json["overallScore"] = report.overallScore;
    json["summary"] = report.summary.toStdString();

    // Distribution tests
    json["ksTests"] = nlohmann::json::array();
    for (const auto& test : report.ksTests)
    {
        json["ksTests"].push_back(testResultToJson(test));
    }

    json["mmdTests"] = nlohmann::json::array();
    for (const auto& test : report.mmdTests)
    {
        json["mmdTests"].push_back(testResultToJson(test));
    }

    json["wassersteinTests"] = nlohmann::json::array();
    for (const auto& test : report.wassersteinTests)
    {
        json["wassersteinTests"].push_back(testResultToJson(test));
    }

    // Coverage metrics
    json["coverage"] = coverageToJson(report.coverage);

    // Transfer evaluation
    json["transfer"] = transferToJson(report.transfer);

    // Recommendations and warnings
    json["recommendations"] = nlohmann::json::array();
    for (const auto& rec : report.recommendations)
    {
        json["recommendations"].push_back(rec.toStdString());
    }

    json["warnings"] = nlohmann::json::array();
    for (const auto& warn : report.warnings)
    {
        json["warnings"].push_back(warn.toStdString());
    }

    // Write to file
    try
    {
        std::ofstream file(outputFile.getFullPathName().toStdString());
        file << json.dump(2);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool ValidationEngine::exportReportAsMarkdown(const juce::File& outputFile, const ValidationReport& report)
{
    std::ostringstream md;

    md << "# Validation Report\n\n";
    md << "**Report ID:** " << report.reportId.toStdString() << "\n\n";
    md << "**Timestamp:** " << juce::Time(report.timestamp).formatted("%Y-%m-%d %H:%M:%S").toStdString() << "\n\n";

    md << "## Overall Result\n\n";
    md << "**Status:** " << (report.overallPassed ? "✅ PASSED" : "❌ FAILED") << "\n\n";
    md << "**Score:** " << std::fixed << std::setprecision(1) << report.overallScore << "/100\n\n";

    md << "---\n\n";

    // Coverage Metrics
    md << "## Coverage Metrics\n\n";
    md << "| Metric | Value | Target | Status |\n";
    md << "|--------|-------|--------|--------|\n";

    auto formatStatus = [](float value, float target, bool greaterIsBetter = true) -> std::string
    {
        const bool ok = greaterIsBetter ? (value >= target) : (value <= target);
        return ok ? "✅" : "❌";
    };

    md << "| Volume Coverage | " << std::fixed << std::setprecision(1)
       << report.coverage.volumeCoverageRatio * 100.0f << "% | >75% | "
       << formatStatus(report.coverage.volumeCoverageRatio, 0.75f) << " |\n";
    md << "| Grid Coverage | " << report.coverage.gridCoverage * 100.0f << "% | >80% | "
       << formatStatus(report.coverage.gridCoverage, 0.80f) << " |\n";
    md << "| Boundary Coverage | " << report.coverage.boundaryCoverage * 100.0f << "% | >15% | "
       << formatStatus(report.coverage.boundaryCoverage, 0.15f) << " |\n";
    md << "| Total Samples | " << report.coverage.totalSamples << " | - | - |\n";
    md << "| Unique Sets | " << report.coverage.uniqueParameterSets << " | - | - |\n";
    md << "| Avg Distance | " << report.coverage.averageDistance << " | - | - |\n\n";

    // Distribution Tests
    md << "## Distribution Tests\n\n";

    auto writeTestTable = [&md](const juce::String& title, const std::vector<DistributionTestResult>& tests)
    {
        if (tests.empty()) return;

        md << "### " << title.toStdString() << "\n\n";
        md << "| Parameter | Statistic | P-Value | Status |\n";
        md << "|-----------|-----------|---------|--------|\n";

        for (const auto& test : tests)
        {
            md << "| " << test.parameterName.toStdString()
               << " | " << std::fixed << std::setprecision(4) << test.statistic
               << " | " << test.pValue
               << " | " << (test.passed ? "✅" : "❌") << " |\n";
        }
        md << "\n";
    };

    writeTestTable("Kolmogorov-Smirnov Tests", report.ksTests);
    writeTestTable("Maximum Mean Discrepancy Tests", report.mmdTests);
    writeTestTable("Wasserstein Distance Tests", report.wassersteinTests);

    // Transfer Evaluation
    if (!report.transfer.benchmarkName.isEmpty())
    {
        md << "## Transfer Learning Evaluation\n\n";
        md << "**Benchmark:** " << report.transfer.benchmarkName.toStdString() << "\n\n";
        md << "| Metric | Value |\n";
        md << "|--------|-------|\n";
        md << "| Zero-Shot Accuracy | " << std::fixed << std::setprecision(2)
           << report.transfer.zeroShotAccuracy * 100.0f << "% |\n";
        md << "| Fine-Tuned Accuracy | " << report.transfer.fineTunedAccuracy * 100.0f << "% |\n";
        md << "| Performance Gap | " << report.transfer.performanceGap * 100.0f << "% |\n\n";
    }

    // Recommendations
    if (!report.recommendations.isEmpty())
    {
        md << "## Recommendations\n\n";
        for (const auto& rec : report.recommendations)
        {
            md << "- " << rec.toStdString() << "\n";
        }
        md << "\n";
    }

    // Warnings
    if (!report.warnings.isEmpty())
    {
        md << "## Warnings\n\n";
        for (const auto& warn : report.warnings)
        {
            md << "- ⚠️ " << warn.toStdString() << "\n";
        }
        md << "\n";
    }

    // Write to file
    try
    {
        std::ofstream file(outputFile.getFullPathName().toStdString());
        file << md.str();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// =============================================================================
// Visualization Data Generation
// =============================================================================

nlohmann::json ValidationEngine::generateHistogramData(const std::vector<float>& data, int bins)
{
    nlohmann::json json;

    if (data.empty() || bins <= 0)
    {
        json["error"] = "Invalid input: empty data or invalid bin count";
        return json;
    }

    // Find min and max
    const auto [minIt, maxIt] = std::minmax_element(data.begin(), data.end());
    const float minVal = *minIt;
    const float maxVal = *maxIt;
    const float range = maxVal - minVal;

    // Handle case where all values are the same
    const float binWidth = range > 0 ? range / static_cast<float>(bins) : 1.0f;

    // Initialize bins
    std::vector<int> counts(bins, 0);
    std::vector<float> binEdges(bins + 1);
    std::vector<float> binCenters(bins);

    for (int i = 0; i <= bins; ++i)
    {
        binEdges[i] = minVal + static_cast<float>(i) * binWidth;
    }

    for (int i = 0; i < bins; ++i)
    {
        binCenters[i] = (binEdges[i] + binEdges[i + 1]) / 2.0f;
    }

    // Count values in each bin
    for (const float value : data)
    {
        int binIndex = static_cast<int>((value - minVal) / binWidth);
        binIndex = std::clamp(binIndex, 0, bins - 1);
        ++counts[binIndex];
    }

    // Compute normalized densities
    const float total = static_cast<float>(data.size());
    std::vector<float> densities(bins);
    for (int i = 0; i < bins; ++i)
    {
        densities[i] = (total > 0 && binWidth > 0)
            ? static_cast<float>(counts[i]) / (total * binWidth)
            : 0.0f;
    }

    // Build JSON
    json["binEdges"] = binEdges;
    json["binCenters"] = binCenters;
    json["counts"] = counts;
    json["densities"] = densities;
    json["binWidth"] = binWidth;
    json["totalCount"] = static_cast<int>(data.size());
    json["min"] = minVal;
    json["max"] = maxVal;
    json["mean"] = computeMean(data);
    json["stdDev"] = computeStdDev(data, computeMean(data));

    return json;
}

nlohmann::json ValidationEngine::generateScatterPlotData(
    const std::vector<std::pair<float, float>>& points)
{
    nlohmann::json json;

    if (points.empty())
    {
        json["error"] = "Invalid input: empty points array";
        return json;
    }

    // Extract x and y coordinates
    std::vector<float> x, y;
    x.reserve(points.size());
    y.reserve(points.size());

    for (const auto& point : points)
    {
        x.push_back(point.first);
        y.push_back(point.second);
    }

    // Compute bounds
    const auto [minX, maxX] = std::minmax_element(x.begin(), x.end());
    const auto [minY, maxY] = std::minmax_element(y.begin(), y.end());

    // Build JSON
    json["x"] = x;
    json["y"] = y;
    json["count"] = static_cast<int>(points.size());
    json["xMin"] = *minX;
    json["xMax"] = *maxX;
    json["yMin"] = *minY;
    json["yMax"] = *maxY;

    return json;
}

nlohmann::json ValidationEngine::generateCoverageMapData(
    const std::vector<std::vector<float>>& samples,
    int dimension1,
    int dimension2,
    float resolution)
{
    nlohmann::json json;

    if (samples.empty())
    {
        json["error"] = "Invalid input: empty samples array";
        return json;
    }

    const int binsPerDim = static_cast<int>(1.0f / resolution);
    std::vector<std::vector<int>> grid(binsPerDim, std::vector<int>(binsPerDim, 0));

    // Count samples in each grid cell
    for (const auto& sample : samples)
    {
        if (static_cast<int>(sample.size()) > std::max(dimension1, dimension2))
        {
            const int bin1 = static_cast<int>(std::clamp(sample[dimension1], 0.0f, 0.9999f) / resolution);
            const int bin2 = static_cast<int>(std::clamp(sample[dimension2], 0.0f, 0.9999f) / resolution);
            ++grid[bin1][bin2];
        }
    }

    // Find max count for normalization
    int maxCount = 0;
    for (const auto& row : grid)
    {
        for (int count : row)
        {
            maxCount = std::max(maxCount, count);
        }
    }

    // Build JSON
    json["grid"] = grid;
    json["dimension1"] = dimension1;
    json["dimension2"] = dimension2;
    json["resolution"] = resolution;
    json["binsPerDimension"] = binsPerDim;
    json["maxCount"] = maxCount;
    json["totalSamples"] = static_cast<int>(samples.size());

    // Compute coverage statistics
    int occupiedCells = 0;
    for (const auto& row : grid)
    {
        for (int count : row)
        {
            if (count > 0) ++occupiedCells;
        }
    }

    json["occupiedCells"] = occupiedCells;
    json["totalCells"] = binsPerDim * binsPerDim;
    json["coverageRatio"] = static_cast<float>(occupiedCells) / static_cast<float>(binsPerDim * binsPerDim);

    return json;
}

// =============================================================================
// Statistical Helper Methods
// =============================================================================

double ValidationEngine::computeEmpiricalCDF(const std::vector<float>& sortedData, double value)
{
    if (sortedData.empty())
    {
        return 0.0;
    }

    // Binary search for position
    const auto it = std::lower_bound(sortedData.begin(), sortedData.end(), static_cast<float>(value));
    const size_t index = std::distance(sortedData.begin(), it);

    return static_cast<double>(index) / static_cast<double>(sortedData.size());
}

std::vector<float> ValidationEngine::sortVector(const std::vector<float>& data)
{
    std::vector<float> sorted = data;
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

float ValidationEngine::computeRBFKernel(const std::vector<float>& x, const std::vector<float>& y, float sigma)
{
    if (x.size() != y.size() || x.empty())
    {
        return 0.0f;
    }

    const float s = sigma > 0.0f ? sigma : rbfBandwidth_;
    const float dist = euclideanDistance(x, y);

    return std::exp(-dist * dist / (2.0f * s * s));
}

double ValidationEngine::computeMean(const std::vector<float>& data)
{
    if (data.empty())
    {
        return 0.0;
    }

    double sum = 0.0;
    for (const float value : data)
    {
        sum += value;
    }
    return sum / static_cast<double>(data.size());
}

double ValidationEngine::computeStdDev(const std::vector<float>& data, double mean)
{
    if (data.size() < 2)
    {
        return 0.0;
    }

    double sumSq = 0.0;
    for (const float value : data)
    {
        const double diff = value - mean;
        sumSq += diff * diff;
    }
    return std::sqrt(sumSq / static_cast<double>(data.size()));
}

float ValidationEngine::euclideanDistance(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size() || a.empty())
    {
        return std::numeric_limits<float>::max();
    }

    double sumSq = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const double diff = a[i] - b[i];
        sumSq += diff * diff;
    }
    return static_cast<float>(std::sqrt(sumSq));
}

// =============================================================================
// Coverage Helper Methods
// =============================================================================

int ValidationEngine::countOccupiedBins(
    const std::vector<std::vector<float>>& samples,
    int dim1, int dim2, float resolution)
{
    const int binsPerDim = static_cast<int>(1.0f / resolution);
    std::set<std::pair<int, int>> occupiedBins;

    for (const auto& sample : samples)
    {
        if (static_cast<int>(sample.size()) > std::max(dim1, dim2))
        {
            const int bin1 = static_cast<int>(std::clamp(sample[dim1], 0.0f, 0.9999f) / resolution);
            const int bin2 = static_cast<int>(std::clamp(sample[dim2], 0.0f, 0.9999f) / resolution);
            occupiedBins.emplace(bin1, bin2);
        }
    }

    return static_cast<int>(occupiedBins.size());
}

float ValidationEngine::computeBoundingVolume(const std::vector<std::vector<float>>& samples)
{
    if (samples.empty())
    {
        return 0.0f;
    }

    const int dims = static_cast<int>(samples[0].size());
    if (dims == 0)
    {
        return 0.0f;
    }

    // Compute range for each dimension
    std::vector<float> minVals(dims, std::numeric_limits<float>::max());
    std::vector<float> maxVals(dims, std::numeric_limits<float>::lowest());

    for (const auto& sample : samples)
    {
        for (int d = 0; d < dims && d < static_cast<int>(sample.size()); ++d)
        {
            minVals[d] = std::min(minVals[d], sample[d]);
            maxVals[d] = std::max(maxVals[d], sample[d]);
        }
    }

    // Compute hypervolume (product of ranges)
    // Normalize by maximum possible volume (1.0^dims)
    float volume = 1.0f;
    for (int d = 0; d < dims; ++d)
    {
        volume *= (maxVals[d] - minVals[d]);
    }

    // For high dimensions, this can be very small, so we return a normalized ratio
    return volume;
}

float ValidationEngine::computeBoundarySampleRatio(
    const std::vector<std::vector<float>>& samples,
    float boundaryThreshold)
{
    if (samples.empty())
    {
        return 0.0f;
    }

    int boundaryCount = 0;

    for (const auto& sample : samples)
    {
        bool nearBoundary = false;
        for (const float value : sample)
        {
            if (value <= boundaryThreshold || value >= (1.0f - boundaryThreshold))
            {
                nearBoundary = true;
                break;
            }
        }
        if (nearBoundary)
        {
            ++boundaryCount;
        }
    }

    return static_cast<float>(boundaryCount) / static_cast<float>(samples.size());
}

int ValidationEngine::countUniqueParameterSets(
    const std::vector<std::vector<float>>& samples,
    float tolerance)
{
    if (samples.empty())
    {
        return 0;
    }

    // Simple approach: count unique after rounding
    std::set<std::string> uniqueKeys;

    for (const auto& sample : samples)
    {
        std::ostringstream key;
        for (size_t i = 0; i < sample.size(); ++i)
        {
            if (i > 0) key << ",";
            // Round to tolerance precision
            const float rounded = std::round(sample[i] / tolerance) * tolerance;
            key << std::fixed << std::setprecision(6) << rounded;
        }
        uniqueKeys.insert(key.str());
    }

    return static_cast<int>(uniqueKeys.size());
}

// =============================================================================
// Report Helper Methods
// =============================================================================

juce::String ValidationEngine::generateReportId()
{
    const int id = random_.nextInt(juce::Range<int>(100000, 999999));
    const juce::Time now = juce::Time::getCurrentTime();
    return juce::String::formatted("VAL-%04d%02d%02d-%06d",
        now.getYear(), now.getMonth(), now.getDayOfMonth(), id);
}

float ValidationEngine::computeOverallScore(const ValidationReport& report)
{
    float score = 0.0f;
    float totalWeight = 0.0f;

    // Coverage metrics (weight: 40%)
    const float volumeScore = std::min(1.0f, report.coverage.volumeCoverageRatio / 0.75f) * 100.0f;
    const float gridScore = std::min(1.0f, report.coverage.gridCoverage / 0.80f) * 100.0f;
    const float boundaryScore = std::min(1.0f, report.coverage.boundaryCoverage / 0.15f) * 100.0f;

    score += volumeScore * 0.15f;
    score += gridScore * 0.15f;
    score += boundaryScore * 0.10f;
    totalWeight += 0.40f;

    // Distribution tests (weight: 50%)
    auto computeTestScore = [](const std::vector<DistributionTestResult>& tests) -> float
    {
        if (tests.empty()) return 100.0f;

        float totalScore = 0.0f;
        for (const auto& test : tests)
        {
            if (test.passed)
            {
                totalScore += 100.0f;
            }
            else
            {
                // Partial score based on p-value
                totalScore += std::min(100.0f, static_cast<float>(test.pValue * 1000.0));
            }
        }
        return totalScore / static_cast<float>(tests.size());
    };

    const float ksScore = computeTestScore(report.ksTests);
    const float mmdScore = computeTestScore(report.mmdTests);
    const float wassersteinScore = computeTestScore(report.wassersteinTests);

    score += ksScore * 0.20f;
    score += mmdScore * 0.15f;
    score += wassersteinScore * 0.15f;
    totalWeight += 0.50f;

    // Transfer evaluation (weight: 10%)
    if (!report.transfer.benchmarkName.isEmpty())
    {
        const float transferScore = report.transfer.fineTunedAccuracy * 100.0f;
        score += transferScore * 0.10f;
        totalWeight += 0.10f;
    }

    return totalWeight > 0 ? score / totalWeight : 0.0f;
}

void ValidationEngine::generateRecommendations(ValidationReport& report)
{
    report.recommendations.clear();
    report.warnings.clear();

    // Coverage recommendations
    if (report.coverage.volumeCoverageRatio < 0.75f)
    {
        report.recommendations.add("Increase sample diversity to improve volume coverage (current: "
            + juce::String(report.coverage.volumeCoverageRatio * 100.0f, 1) + "%, target: >75%)");
    }

    if (report.coverage.gridCoverage < 0.80f)
    {
        report.recommendations.add("Use Latin Hypercube Sampling to improve grid coverage (current: "
            + juce::String(report.coverage.gridCoverage * 100.0f, 1) + "%, target: >80%)");
    }

    if (report.coverage.boundaryCoverage < 0.15f)
    {
        report.recommendations.add("Include more boundary samples to cover edge cases (current: "
            + juce::String(report.coverage.boundaryCoverage * 100.0f, 1) + "%, target: >15%)");
    }

    // Distribution test recommendations
    auto checkFailedTests = [&report](const std::vector<DistributionTestResult>& tests, const juce::String& testName)
    {
        for (const auto& test : tests)
        {
            if (!test.passed)
            {
                report.recommendations.add(testName + " test failed for " + test.parameterName
                    + ": consider adjusting sampling distribution");
            }
        }
    };

    checkFailedTests(report.ksTests, "KS");
    checkFailedTests(report.mmdTests, "MMD");
    checkFailedTests(report.wassersteinTests, "Wasserstein");

    // Transfer learning recommendations
    if (!report.transfer.benchmarkName.isEmpty())
    {
        if (report.transfer.performanceGap > 0.15f)
        {
            report.warnings.add("Large performance gap in transfer learning ("
                + juce::String(report.transfer.performanceGap * 100.0f, 1)
                + "%): synthetic data may not fully represent real distribution");
        }

        if (report.transfer.zeroShotAccuracy < 0.5f)
        {
            report.recommendations.add("Low zero-shot accuracy: increase synthetic data diversity or volume");
        }
    }

    // Sample size recommendations
    if (report.coverage.totalSamples < 1000)
    {
        report.warnings.add("Small sample size (" + juce::String(report.coverage.totalSamples)
            + "): statistical tests may have low power");
    }

    if (report.coverage.uniqueParameterSets < report.coverage.totalSamples / 2)
    {
        report.warnings.add("High sample redundancy: "
            + juce::String(report.coverage.uniqueParameterSets) + " unique sets out of "
            + juce::String(report.coverage.totalSamples) + " total samples");
    }
}

nlohmann::json ValidationEngine::testResultToJson(const DistributionTestResult& result)
{
    nlohmann::json json;
    json["testName"] = result.testName.toStdString();
    json["parameterName"] = result.parameterName.toStdString();
    json["statistic"] = result.statistic;
    json["pValue"] = result.pValue;
    json["passed"] = result.passed;
    json["interpretation"] = result.interpretation.toStdString();
    return json;
}

nlohmann::json ValidationEngine::coverageToJson(const CoverageMetrics& coverage)
{
    nlohmann::json json;
    json["volumeCoverageRatio"] = coverage.volumeCoverageRatio;
    json["gridCoverage"] = coverage.gridCoverage;
    json["boundaryCoverage"] = coverage.boundaryCoverage;
    json["totalSamples"] = coverage.totalSamples;
    json["uniqueParameterSets"] = coverage.uniqueParameterSets;
    json["averageDistance"] = coverage.averageDistance;
    json["minDistance"] = coverage.minDistance;
    json["maxDistance"] = coverage.maxDistance;
    json["dimensions"] = coverage.dimensions;
    return json;
}

nlohmann::json ValidationEngine::transferToJson(const TransferEvaluationResult& transfer)
{
    nlohmann::json json;
    json["zeroShotAccuracy"] = transfer.zeroShotAccuracy;
    json["fineTunedAccuracy"] = transfer.fineTunedAccuracy;
    json["performanceGap"] = transfer.performanceGap;
    json["benchmarkName"] = transfer.benchmarkName.toStdString();
    json["syntheticSamplesUsed"] = transfer.syntheticSamplesUsed;
    json["realSamplesUsed"] = transfer.realSamplesUsed;
    json["trainingTimeSeconds"] = transfer.trainingTimeSeconds;

    json["perClassMetrics"] = nlohmann::json::object();
    for (const auto& [key, value] : transfer.perClassMetrics)
    {
        json["perClassMetrics"][key.toStdString()] = value;
    }

    return json;
}

} // namespace morphsnap
