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
#include <string_view>

namespace more_phi {

struct SonicMasterSessionHandle; // pimpl — defined in the .cpp

// Stereo sample count the model ingests (~6 s @ 44.1 kHz). Matches
// MasteringDecisionNet.segment_samples for this checkpoint.
inline constexpr std::size_t kSonicMasterSegmentFrames = 262138;

// ONNX session wrapper for the waveform->decision contract.
class SonicMasterDecisionRunner
{
public:
    SonicMasterDecisionRunner() noexcept;
    ~SonicMasterDecisionRunner();

    SonicMasterDecisionRunner(const SonicMasterDecisionRunner&) = delete;
    SonicMasterDecisionRunner& operator=(const SonicMasterDecisionRunner&) = delete;

    // Message thread only. Loads + shape-validates an ONNX model whose contract
    // matches masteringbrain_v2_decision (input product == 2 * segment frames,
    // output product == decision width). Returns false (and leaves the runner
    // unavailable) when ORT is not linked, the file is missing, the contract
    // JSON is unreadable, or the I/O shapes do not match. `contractPath` points
    // at the masteringbrain_v2_contract.json emitted by the export script.
    bool loadModel(std::string_view modelPath, std::string_view contractPath);

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

private:
    std::unique_ptr<SonicMasterSessionHandle> session_;
    bool available_ = false;
};

} // namespace more_phi
