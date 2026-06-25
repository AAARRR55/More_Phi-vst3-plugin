/*
 * More-Phi — AI/SonicMasterHttpInferenceSource.h
 *
 * ISonicMasterInferenceSource implementation that drives the local Python
 * inference server (tools/inference_server/server.py) over HTTP on localhost.
 *
 * Role: the FALLBACK inference path. The plugin's primary path is the in-process
 * ONNX runner (SonicMasterDecisionRunner), which loads the exported
 * masteringbrain_v2_decision.onnx graph produced by
 * tools/export_onnx/export_patched.py (manual Multi-Head Attention + STFT-based
 * spectral injection; parity verified). This HTTP source is used when the ONNX
 * runner is unavailable at runtime (no model staged, ORT not linked into this
 * build, or a load-time validation failure): it lets the plugin fall back to
 * running the checkpoint directly via PyTorch — the path it was originally
 * validated with — without linking PyTorch into the VST3. A small localhost
 * server hosts the model, and this source POSTs a 6s stereo window and parses
 * the 44-float decision back. The PluginProcessor selects ONNX-first,
 * HTTP-fallback at construction.
 *
 * Protocol (see tools/inference_server/server.py):
 *   GET  /health                          -> {"status":"ok"}
 *   GET  /status                          -> {"loaded":bool, ...}
 *   POST /infer?target_lufs=<float>       body = raw little-endian float32
 *                                         interleaved stereo (2*N*4 bytes)
 *                                         -> {"decision":[44 floats],
 *                                             "inference_ms":float}
 *
 * Threading: infer() runs on the analysis thread (no realtime budget). All
 * JUCE URL/WebInputStream calls are blocking; the analysis engine tolerates
 * a 200-500 ms round trip well within its 3 s cycle.
 */
#pragma once

#include "AI/SonicMasterAnalysisEngine.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace more_phi {

// ── Pure, testable serialization helpers (no network, no JUCE I/O) ──────────

// Build the raw little-endian float32 interleaved-stereo request body from two
// mono float arrays of `n` frames. Writes 2*n floats into outInterleaved.
// Caller must size outInterleaved to >= 2*n. Pure, noexcept.
void buildInferRequestBody(const float* left,
                           const float* right,
                           std::size_t n,
                           float* outInterleaved) noexcept;

// Parse a /infer JSON response and extract the decision vector. Returns true
// and fills outDecision (up to outCapacity floats, clamped to 44) on success.
// Tolerant: missing/malformed "decision" array -> false. Pure (parses the
// string, no I/O). Used by both the live source and the unit test.
bool parseInferResponse(std::string_view jsonBody,
                        float* outDecision,
                        std::size_t outCapacity) noexcept;

// ── The HTTP-backed inference source ─────────────────────────────────────────

class SonicMasterHttpInferenceSource final : public ISonicMasterInferenceSource
{
public:
    SonicMasterHttpInferenceSource() = default;

    // Configure the server endpoint. Defaults to http://127.0.0.1:8765. The
    // target LUFS is sent per-request by the caller (we expose a setter for the
    // default applied when the caller doesn't override).
    void setEndpoint(std::string baseUrl, int port = 8765);
    void setTargetLufs(float lufs) noexcept { targetLufs_.store(lufs, std::memory_order_relaxed); }

    // Pure cache read — safe to call from the message thread (never blocks).
    [[nodiscard]] bool isAvailable() const noexcept override;

    // Probes /health and updates the cache. BLOCKING — call from a background
    // thread only, never the message thread. Throttled to once per ~5 s.
    void refreshProbe() noexcept;

    // Runs one inference. Blocking (analysis thread only). Returns false on
    // any network/parse error; the analysis engine then skips the cycle.
    bool infer(const float* stereoInterleaved, float* outDecision,
               std::size_t outCapacity) noexcept override;

    // Force the next isAvailable() to re-probe (call after the user starts the
    // server so the toggle enables immediately).
    void invalidateAvailability() const noexcept;

private:
    std::string baseUrl_ { "http://127.0.0.1" };
    int  port_  = 8765;
    std::atomic<float> targetLufs_ { -14.0f };   // AUDIT-1: written by analysis thread, read in infer()
    mutable std::atomic<int64_t> lastProbeMs_   { 0 };     // cache window (ms)
    mutable std::atomic<bool>    cachedAvailable_{ false };
    mutable std::atomic<bool>    probeFresh_     { false };
    // AUDIT-FIX (A8): guards against stacking concurrent infer() POSTs when a
    // detached worker is still wedged on a hanging server. Set on entry, cleared
    // on observed completion (or by refreshProbe during a health re-check).
    std::atomic<bool> inFlight_ { false };

    // For test access to the pure helpers without exposing internals publicly.
    friend struct SonicMasterHttpTestAccess;
};

} // namespace more_phi
