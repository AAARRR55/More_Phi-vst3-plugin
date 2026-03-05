/*
 * MorphSnap — AI/Dataset/ValidationEngine.h
 * Statistical validation for synthetic audio dataset generation.
 * Provides distribution matching tests, coverage metrics, and transfer evaluation.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <map>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <cstdint>

namespace morphsnap {

/** Result of a single statistical distribution test */
struct DistributionTestResult
{
    juce::String testName;           ///< Name of the test (KS, MMD, Wasserstein)
    juce::String parameterName;      ///< Parameter or feature set being tested
    double statistic = 0.0;          ///< Test statistic value
    double pValue = 0.0;             ///< P-value for hypothesis test
    bool passed = false;             ///< True if test passes at significance level 0.05
    juce::String interpretation;     ///< Human-readable interpretation

    /** Default constructor */
    DistributionTestResult() = default;

    /** Constructor with all fields */
    DistributionTestResult(juce::String test, juce::String param,
                          double stat, double p, bool pass, juce::String interp)
        : testName(std::move(test)), parameterName(std::move(param)),
          statistic(stat), pValue(p), passed(pass), interpretation(std::move(interp)) {}
};

/** Coverage metrics for the parameter space */
struct CoverageMetrics
{
    float volumeCoverageRatio = 0.0f;    ///< Ratio of occupied volume to total (Target: >0.75)
    float gridCoverage = 0.0f;           ///< Fraction of grid cells with samples (Target: >0.80)
    float boundaryCoverage = 0.0f;       ///< Fraction of samples near boundaries (Target: >15%)
    int totalSamples = 0;                ///< Total number of samples
    int uniqueParameterSets = 0;         ///< Number of unique parameter combinations
    float averageDistance = 0.0f;        ///< Average pairwise distance between samples
    float minDistance = 0.0f;            ///< Minimum pairwise distance
    float maxDistance = 0.0f;            ///< Maximum pairwise distance
    int dimensions = 0;                  ///< Number of dimensions in parameter space

    /** Default constructor */
    CoverageMetrics() = default;
};

/** Result of synthetic-to-real transfer evaluation */
struct TransferEvaluationResult
{
    float zeroShotAccuracy = 0.0f;       ///< Accuracy without fine-tuning
    float fineTunedAccuracy = 0.0f;      ///< Accuracy after fine-tuning on real data
    float performanceGap = 0.0f;         ///< Gap between synthetic-only and real (Target: <15%)
    juce::String benchmarkName;          ///< Name of the benchmark task
    std::map<juce::String, float> perClassMetrics;  ///< Per-class accuracy metrics
    int syntheticSamplesUsed = 0;        ///< Number of synthetic samples used
    int realSamplesUsed = 0;             ///< Number of real samples used
    float trainingTimeSeconds = 0.0f;    ///< Time spent training

    /** Default constructor */
    TransferEvaluationResult() = default;
};

/** Complete validation report for a synthetic dataset */
struct ValidationReport
{
    juce::String reportId;               ///< Unique identifier for this report
    int64_t timestamp = 0;               ///< Unix timestamp of report generation

    // Distribution tests
    std::vector<DistributionTestResult> ksTests;        ///< Kolmogorov-Smirnov test results
    std::vector<DistributionTestResult> mmdTests;       ///< Maximum Mean Discrepancy test results
    std::vector<DistributionTestResult> wassersteinTests; ///< Wasserstein distance test results

    // Coverage metrics
    CoverageMetrics coverage;

    // Transfer evaluation
    TransferEvaluationResult transfer;

    // Summary
    bool overallPassed = false;          ///< True if all critical tests pass
    float overallScore = 0.0f;           ///< Weighted score (0-100)
    juce::String summary;                ///< Human-readable summary
    juce::StringArray recommendations;   ///< List of recommendations for improvement
    juce::StringArray warnings;          ///< List of warnings or issues

    /** Default constructor */
    ValidationReport() = default;
};

/**
 * Validation engine for synthetic audio datasets.
 * Provides statistical tests for distribution matching, coverage analysis,
 * and synthetic-to-real transfer evaluation.
 */
class ValidationEngine
{
public:
    ValidationEngine();
    ~ValidationEngine() = default;

    // =========================================================================
    // Distribution Tests
    // =========================================================================

    /**
     * Perform Kolmogorov-Smirnov test for distribution comparison.
     * Tests whether two samples come from the same distribution.
     *
     * @param observed Observed/synthetic data samples
     * @param expected Expected/real data samples
     * @param parameterName Name of the parameter being tested
     * @return Test result with statistic, p-value, and interpretation
     */
    DistributionTestResult kolmogorovSmirnovTest(
        const std::vector<float>& observed,
        const std::vector<float>& expected,
        const juce::String& parameterName);

    /**
     * Perform Maximum Mean Discrepancy (MMD) test for multivariate distributions.
     * Uses RBF kernel to compare distributions in feature space.
     *
     * @param observed Observed/synthetic feature vectors
     * @param expected Expected/real feature vectors
     * @param featureSetName Name of the feature set being tested
     * @return Test result with MMD statistic and interpretation
     */
    DistributionTestResult maximumMeanDiscrepancyTest(
        const std::vector<std::vector<float>>& observed,
        const std::vector<std::vector<float>>& expected,
        const juce::String& featureSetName);

    /**
     * Compute Wasserstein (Earth Mover's) distance between distributions.
     * Measures the "work" needed to transform one distribution into another.
     *
     * @param observed Observed/synthetic data samples
     * @param expected Expected/real data samples
     * @param distributionName Name of the distribution being tested
     * @return Test result with Wasserstein distance and interpretation
     */
    DistributionTestResult wassersteinDistanceTest(
        const std::vector<float>& observed,
        const std::vector<float>& expected,
        const juce::String& distributionName);

    // =========================================================================
    // Coverage Metrics
    // =========================================================================

    /**
     * Compute coverage metrics for a synthetic dataset.
     * Measures how well samples cover the parameter space.
     *
     * @param samples Vector of parameter samples
     * @param dimensions Number of dimensions (parameters)
     * @param gridResolution Resolution for grid coverage calculation (default 0.1)
     * @return Coverage metrics including volume, grid, and boundary coverage
     */
    CoverageMetrics computeCoverageMetrics(
        const std::vector<std::vector<float>>& samples,
        int dimensions,
        float gridResolution = 0.1f);

    // =========================================================================
    // Transfer Evaluation
    // =========================================================================

    /**
     * Evaluate synthetic-to-real transfer learning performance.
     * This is a placeholder that simulates transfer evaluation metrics.
     *
     * @param syntheticDataDir Directory containing synthetic data
     * @param realDataDir Directory containing real data
     * @param benchmarkType Type of benchmark (e.g., "classification", "regression")
     * @return Transfer evaluation result with zero-shot and fine-tuned metrics
     */
    TransferEvaluationResult evaluateTransferLearning(
        const juce::File& syntheticDataDir,
        const juce::File& realDataDir,
        const juce::String& benchmarkType);

    // =========================================================================
    // Full Validation Report
    // =========================================================================

    /**
     * Generate a complete validation report comparing synthetic to real data.
     *
     * @param syntheticSamples Synthetic parameter samples
     * @param realSamples Real parameter samples for comparison
     * @param config Optional configuration JSON
     * @return Complete validation report with all tests and metrics
     */
    ValidationReport generateReport(
        const std::vector<std::vector<float>>& syntheticSamples,
        const std::vector<std::vector<float>>& realSamples,
        const nlohmann::json& config = {});

    // =========================================================================
    // Report Export
    // =========================================================================

    /**
     * Export validation report to a file (auto-detects format from extension).
     *
     * @param outputFile Output file path
     * @param report Validation report to export
     * @return true if export succeeded
     */
    bool exportReport(const juce::File& outputFile, const ValidationReport& report);

    /**
     * Export validation report as JSON.
     *
     * @param outputFile Output file path
     * @param report Validation report to export
     * @return true if export succeeded
     */
    bool exportReportAsJson(const juce::File& outputFile, const ValidationReport& report);

    /**
     * Export validation report as Markdown.
     *
     * @param outputFile Output file path
     * @param report Validation report to export
     * @return true if export succeeded
     */
    bool exportReportAsMarkdown(const juce::File& outputFile, const ValidationReport& report);

    // =========================================================================
    // Visualization Data Generation
    // =========================================================================

    /**
     * Generate histogram data for visualization.
     *
     * @param data Input data samples
     * @param bins Number of histogram bins (default 20)
     * @return JSON object with bin edges, counts, and normalized densities
     */
    nlohmann::json generateHistogramData(const std::vector<float>& data, int bins = 20);

    /**
     * Generate scatter plot data for 2D visualization.
     *
     * @param points Vector of (x, y) coordinate pairs
     * @return JSON object with point coordinates and metadata
     */
    nlohmann::json generateScatterPlotData(const std::vector<std::pair<float, float>>& points);

    /**
     * Generate coverage map data for 2D projection of parameter space.
     *
     * @param samples Multi-dimensional samples
     * @param dimension1 First dimension to project
     * @param dimension2 Second dimension to project
     * @param resolution Grid resolution for coverage map (default 0.1)
     * @return JSON object with grid data for heatmap visualization
     */
    nlohmann::json generateCoverageMapData(
        const std::vector<std::vector<float>>& samples,
        int dimension1,
        int dimension2,
        float resolution = 0.1f);

    // =========================================================================
    // Configuration
    // =========================================================================

    /** Set significance level for statistical tests (default 0.05) */
    void setSignificanceLevel(double alpha) { significanceLevel_ = alpha; }

    /** Get current significance level */
    double getSignificanceLevel() const noexcept { return significanceLevel_; }

    /** Set RBF kernel bandwidth for MMD computation */
    void setRbfBandwidth(float sigma) { rbfBandwidth_ = sigma; }

    /** Get current RBF bandwidth */
    float getRbfBandwidth() const noexcept { return rbfBandwidth_; }

private:
    // =========================================================================
    // Statistical Helper Methods
    // =========================================================================

    /**
     * Compute empirical CDF value at a given point.
     *
     * @param sortedData Data sorted in ascending order
     * @param value Point at which to evaluate CDF
     * @return CDF value in [0, 1]
     */
    double computeEmpiricalCDF(const std::vector<float>& sortedData, double value);

    /**
     * Sort a vector and return a copy.
     *
     * @param data Input data
     * @return Sorted copy of data
     */
    std::vector<float> sortVector(const std::vector<float>& data);

    /**
     * Compute RBF (Gaussian) kernel between two vectors.
     *
     * @param x First vector
     * @param y Second vector
     * @param sigma Kernel bandwidth (default uses rbfBandwidth_)
     * @return Kernel value
     */
    float computeRBFKernel(const std::vector<float>& x, const std::vector<float>& y, float sigma = -1.0f);

    /**
     * Compute mean of a vector.
     */
    double computeMean(const std::vector<float>& data);

    /**
     * Compute standard deviation of a vector.
     */
    double computeStdDev(const std::vector<float>& data, double mean);

    /**
     * Compute Euclidean distance between two vectors.
     */
    float euclideanDistance(const std::vector<float>& a, const std::vector<float>& b);

    // =========================================================================
    // Coverage Helper Methods
    // =========================================================================

    /**
     * Count occupied bins in a 2D grid projection.
     *
     * @param samples Multi-dimensional samples
     * @param dim1 First dimension index
     * @param dim2 Second dimension index
     * @param resolution Grid resolution
     * @return Number of occupied bins
     */
    int countOccupiedBins(const std::vector<std::vector<float>>& samples,
                          int dim1, int dim2, float resolution);

    /**
     * Compute bounding volume of samples (hypervolume approximation).
     *
     * @param samples Multi-dimensional samples
     * @return Estimated bounding volume
     */
    float computeBoundingVolume(const std::vector<std::vector<float>>& samples);

    /**
     * Compute ratio of samples near parameter space boundaries.
     *
     * @param samples Multi-dimensional samples
     * @param boundaryThreshold Distance threshold for boundary (default 0.1)
     * @return Ratio of boundary samples
     */
    float computeBoundarySampleRatio(const std::vector<std::vector<float>>& samples,
                                     float boundaryThreshold = 0.1f);

    /**
     * Count unique parameter sets (within tolerance).
     *
     * @param samples Multi-dimensional samples
     * @param tolerance Tolerance for uniqueness (default 1e-6)
     * @return Number of unique parameter sets
     */
    int countUniqueParameterSets(const std::vector<std::vector<float>>& samples,
                                 float tolerance = 1e-6f);

    // =========================================================================
    // Report Helper Methods
    // =========================================================================

    /**
     * Generate unique report ID.
     */
    juce::String generateReportId();

    /**
     * Compute overall score from individual test results.
     */
    float computeOverallScore(const ValidationReport& report);

    /**
     * Generate recommendations based on test results.
     */
    void generateRecommendations(ValidationReport& report);

    /**
     * Convert DistributionTestResult to JSON.
     */
    nlohmann::json testResultToJson(const DistributionTestResult& result);

    /**
     * Convert CoverageMetrics to JSON.
     */
    nlohmann::json coverageToJson(const CoverageMetrics& coverage);

    /**
     * Convert TransferEvaluationResult to JSON.
     */
    nlohmann::json transferToJson(const TransferEvaluationResult& transfer);

    // =========================================================================
    // Member Variables
    // =========================================================================

    double significanceLevel_ = 0.05;   ///< Significance level for hypothesis tests
    float rbfBandwidth_ = 1.0f;         ///< Bandwidth for RBF kernel in MMD
    juce::Random random_;               ///< Random number generator for report IDs
};

} // namespace morphsnap
