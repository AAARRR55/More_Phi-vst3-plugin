/*
 * More-Phi — AI/SonicMasterDecisionRunner.h
 *
 * Thin ONNX Runtime session wrapper for the masteringbrainv2 waveform→decision
 * contract. Thread-unsafe by design: the analysis engine guarantees
 * single-threaded use (one session, one analysis thread, joined before the
 * destructor runs — see SonicMasterAnalysisEngine).
 *
 * Mirrors the pimpl + MORE_PHI_HAS_ONNX gating pattern of OnnxNeuralMasteringRunner:
 *   - When MORE_PHI_ENABLE_ONNX is OFF, loadModel() returns false and
 *     isAvailable() reports false, so the analysis engine abstains cleanly.
 *   - When ON, loadModel() creates a session and validates the I/O shapes
 *     against the exported contract.json, refusing a mismatched checkpoint.
 *
 * I/O SCHEMA:
 *   Input  tensor : [batch, 2, kSonicMasterSegmentFrames] float (stereo waveform)
 *   Output tensor : [batch, kSonicMasterDecisionWidth]    float (decision vector)
 */
#pragma once

#include "AI/SonicMasterDecisionDecoder.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <atomic>
#include <array>
#include <cstdint>

namespace more_phi {

struct SonicMasterSessionHandle; // pimpl — defined in the .cpp

// Stereo sample count the model ingests (~5.94 s @ 44.1 kHz). Matches
// MasteringDecisionNet.segment_samples for this checkpoint.
// AUDIT-7: this window is shorter than the ITU-R BS.1770 LUFS-M 400 ms gate by
// ~15x and far shorter than Integrated LUFS (whole-program, minutes). Decisions
// derived from it are at best LUFS-S/short-term, never Integrated. Treat any
// "integrated loudness" label downstream as mislabeled short-term.
inline constexpr std::size_t kSonicMasterSegmentFrames = 262138;

// AUDIT-FIX (A2): the preprocessing the C++ path applies to every inference.
// Pinned in the model's sibling .contract.json and validated at loadModel time
// so a model retrained with different preprocessing is rejected at startup
// rather than producing silently-wrong decisions out-of-distribution.
inline constexpr float kSonicMasterPeakTargetLinear = 0.89125094f;  // 10^(-1/20) ≈ -1 dBFS
inline constexpr double kSonicMasterModelSampleRate = 44100.0;
inline constexpr const char* kSonicMasterInputLayout = "deinterleaved_chw";
inline constexpr const char* kSonicMasterNormalization = "peak_to_-1dBFS";
inline constexpr const char* kSonicMasterDtype = "float32";
inline constexpr int kSonicMasterContractSchema = 1;

// Parsed view of the sibling .contract.json. All fields are validated against
// the runtime constants above; mismatch fails the model load.
struct SonicMasterModelContract
{
    int schema = 0;
    std::string inputLayout;
    std::string normalization;
    std::string dtype;
    double sampleRate = 0.0;
    std::size_t segmentFrames = 0;
    float peakTargetLinear = 0.0f;
    // Optional: SHA-256 of the pre-inference tensor for a fixed 1 kHz reference
    // sine, computed by the export pipeline. When non-empty, the runtime can
    // recompute and compare for byte-exact preprocessing parity. MVP (A2)
    // validates the scalar fields only; the hash is the upgrade path.
    std::string preprocessFingerprint;

    // Returns true iff every field matches the runtime contract constants.
    bool validate() const noexcept
    {
        return schema == kSonicMasterContractSchema
            && inputLayout == kSonicMasterInputLayout
            && normalization == kSonicMasterNormalization
            && dtype == kSonicMasterDtype
            && std::abs(sampleRate - kSonicMasterModelSampleRate) < 0.5
            && segmentFrames == kSonicMasterSegmentFrames
            && std::abs(peakTargetLinear - kSonicMasterPeakTargetLinear) < 1e-4f;
    }
};

// Parse a .contract.json file. Returns true on success; false (and leaves the
// out struct default-initialized) on any parse/schema error. Message thread only.
bool parseSonicMasterContract(const std::string& contractJsonPath, SonicMasterModelContract& out);

// ONNX session wrapper for the waveform->decision contract.
class SonicMasterDecisionRunner
{
public:
    SonicMasterDecisionRunner() noexcept;
    ~SonicMasterDecisionRunner();

    SonicMasterDecisionRunner(const SonicMasterDecisionRunner&) = delete;
    SonicMasterDecisionRunner& operator=(const SonicMasterDecisionRunner&) = delete;

    // Message thread only. Loads + shape-validates an ONNX model, and (when
    // contractPath is non-empty) validates the sibling .contract.json against
    // the runtime preprocessing constants — a model retrained with different
    // normalization/layout/dtype/sample-rate is rejected here at startup.
    // Returns false when ORT is not linked, the file is missing, the I/O shapes
    // do not match the expected segment_frames / decision_width constants, or
    // the contract validation fails. AUDIT-FIX (A2).
    bool loadModel(std::string_view modelPath, std::string_view contractPath = {});

    void unloadModel() noexcept;

    [[nodiscard]] bool isAvailable() const noexcept;

    // Analysis thread only. Runs one inference. `stereoInterleaved` must hold
    // at least 2 * kSonicMasterSegmentFrames floats (L0,R0,L1,R1,...). Writes
    // kSonicMasterDecisionWidth floats into outDecision. Returns false on any
    // error (caller skips the cycle). noexcept at the boundary — ORT errors are
    // caught internally and converted to a false return.
    bool runDecision(const float* stereoInterleaved,
                     float* outDecision,
                     std::size_t outCapacity) noexcept;

    // AUDIT (C2, 2026-06-25): wall-clock duration of the last session->Run() in
    // milliseconds, measured around the ONNX inference call. 0.0 before the first
    // successful run. Read from any thread (relaxed atomic) for diagnostics /
    // profiling — the analysis cycle budget is 3 s (kSonicMasterAnalysisInterval),
    // so a sustained value approaching that budget would indicate the model can no
    // longer keep up at the configured cadence. Previously there was NO latency
    // instrumentation in the C++ ONNX path (the Python fallback server returned an
    // inference_ms field that the C++ client discarded).
    [[nodiscard]] float getLastInferenceMs() const noexcept
    {
        return lastInferenceMs_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float getMaxInferenceMs() const noexcept
    {
        return maxInferenceMs_.load(std::memory_order_relaxed);
    }
    void resetInferenceTiming() noexcept
    {
        lastInferenceMs_.store(0.0f, std::memory_order_relaxed);
        maxInferenceMs_.store(0.0f, std::memory_order_relaxed);
    }

    // DIAG (2026-06-26): the last exception message from session->Run() (empty
    // if the last run succeeded). ORT failures are intermittent in production
    // and the catch(...) in runDecision swallowed the reason entirely, leaving
    // only a false return. This surfaces the real ORT error string so the MCP
    // failure response can report it instead of "model unavailable." Capped at
    // 256 chars (no heap — a fixed buffer) so the audio/analysis threads never
    // allocate. Read from any thread (relaxed); advisory.
    [[nodiscard]] std::string getLastRunError() const noexcept
    {
        return std::string { lastRunError_.data(), errorLen_.load(std::memory_order_relaxed) };
    }
    [[nodiscard]] std::uint64_t getRunCount() const noexcept { return runCount_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t getFailCount() const noexcept { return failCount_.load(std::memory_order_relaxed); }

private:
    std::unique_ptr<SonicMasterSessionHandle> session_;
    bool available_ = false;

    std::atomic<float> lastInferenceMs_{ 0.0f };
    std::atomic<float> maxInferenceMs_{ 0.0f };

    // DIAG: last ORT exception message (truncated). Fixed buffer so writes from
    // the analysis thread never allocate.
    static constexpr std::size_t kErrorBufLen = 256;
    std::array<char, kErrorBufLen> lastRunError_ {};
    std::atomic<std::size_t> errorLen_ { 0 };
    std::atomic<std::uint64_t> runCount_  { 0 };
    std::atomic<std::uint64_t> failCount_ { 0 };

    // DIAG helper: copies msg into lastRunError_ (truncated, null-terminated).
    // noexcept + fixed buffer — safe from the analysis thread.
    void recordError(const char* msg) noexcept;
};

} // namespace more_phi
