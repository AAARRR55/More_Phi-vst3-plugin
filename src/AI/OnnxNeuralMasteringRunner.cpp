/*
 * More-Phi — AI/OnnxNeuralMasteringRunner.cpp
 *
 * SEAM IMPLEMENTATION (see header). The ONNX-free I/O transforms are fully
 * implemented and unit-tested. The session/inference path is active when the
 * build defines MORE_PHI_HAS_ONNX=1 (i.e. ONNX Runtime is linked via the
 * MORE_PHI_ENABLE_ONNX CMake option); otherwise loadModel() abstains and the
 * runner is behaviourally identical to the Null runner.
 */
#include "AI/OnnxNeuralMasteringRunner.h"
#include "AI/NeuralMasteringModelMetadata.h"
#include "Core/NeuralMasteringSafetyPolicy.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <numeric>   // std::accumulate (I/O shape validation in the ONNX path)

#ifndef MORE_PHI_HAS_ONNX
#define MORE_PHI_HAS_ONNX 0
#endif

    bool inferenceSucceeded = false;

#if MORE_PHI_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace more_phi {

// ── Pimpl: the only place ONNX types may appear ──────────────────────────────
struct OnnxSessionHandle
{
    char reservedModelId[kNeuralMasteringModelIdCapacity] {};
    char reservedChecksum[kNeuralMasteringChecksumCapacity] {};

#if MORE_PHI_HAS_ONNX
    // Owning env + session. Created once in loadModel(), reused for every
    // proposePlan() call — zero per-inference allocation. The unique_ptrs keep
    // destruction off any hot path and guarantee cleanup on unload.
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
    std::unique_ptr<Ort::AllocatorWithDefaultOptions> allocator;
    std::string inputName;
    std::string outputName;
    Ort::MemoryInfo memoryInfo { Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault) };
#endif
};

namespace {

// ── Helpers shared by the I/O transforms ─────────────────────────────────────

float finiteOrZero(float value) noexcept
{
    return std::isfinite(value) ? value : 0.0f;
}

template <std::size_t N>
void writeArray(float*& cursor, const std::array<float, N>& values, float (&scratch)[N]) noexcept
{
    for (std::size_t i = 0; i < N; ++i)
    {
        scratch[i] = finiteOrZero(values[i]);
        *cursor++ = scratch[i];
    }
}

template <std::size_t N>
void readArray(const float*& cursor, std::array<float, N>& out) noexcept
{
    for (std::size_t i = 0; i < N; ++i)
        out[i] = *cursor++;
}

template <std::size_t N>
bool sanitizeArray(std::array<float, N>& values) noexcept
{
    bool changed = false;
    for (auto& v : values)
    {
        if (!std::isfinite(v))
        {
            v = 0.0f;
            changed = true;
        }
    }
    return changed;
}

template <std::size_t N>
bool allFiniteArray(const std::array<float, N>& values) noexcept
{
    for (const auto value : values)
        if (!std::isfinite(value))
            return false;
    return true;
}

bool hasPlausibleFeatureFrame(const NeuralMasteringFeatureFrame& frame) noexcept
{
    if (frame.schemaVersion != kNeuralMasteringFeatureSchemaVersion)
        return false;

    if (!std::isfinite(frame.sampleRate) || frame.sampleRate < 8000.0 || frame.sampleRate > 192000.0)
        return false;

    if (frame.channelCount != 1 && frame.channelCount != 2)
        return false;

    if (frame.blockSize < 16 || frame.blockSize > 8192)
        return false;

    const float scalars[] {
        frame.integratedLUFS,
        frame.shortTermLUFS,
        frame.momentaryLUFS,
        frame.loudnessRange,
        frame.truePeakDbTp,
        frame.crestFactorDb,
        frame.spectralTilt,
        frame.monoFoldDownDeltaDb,
        frame.transientDensity,
        frame.harmonicRisk,
        frame.sourceQualityScore,
    };
    for (const auto value : scalars)
        if (!std::isfinite(value))
            return false;

    if (frame.integratedLUFS < -90.0f || frame.integratedLUFS > 12.0f)
        return false;
    if (frame.truePeakDbTp < -120.0f || frame.truePeakDbTp > 24.0f)
        return false;
    if (frame.loudnessRange < 0.0f || frame.loudnessRange > 80.0f)
        return false;
    if (frame.sourceQualityScore < -0.01f || frame.sourceQualityScore > 1.01f)
        return false;

    return allFiniteArray(frame.spectralBands)
        && allFiniteArray(frame.stereoCorrelation)
        && allFiniteArray(frame.midSideRatio);
}

float maxAbsDelta(const float* values, std::size_t count) noexcept
{
    if (values == nullptr || count < kOnnxOutputDeltaCount)
        return 0.0f;

    float maxAbs = 0.0f;
    for (std::size_t i = 0; i < kOnnxOutputDeltaCount; ++i)
        maxAbs = std::max(maxAbs, std::abs(values[i]));
    return maxAbs;
}

bool allFiniteDeltas(const float* values, std::size_t count) noexcept
{
    if (values == nullptr || count < kOnnxOutputDeltaCount)
        return false;

    for (std::size_t i = 0; i < kOnnxOutputDeltaCount; ++i)
        if (!std::isfinite(values[i]))
            return false;
    return true;
}

// Sanitize deltas into [-1, 1]. Returns true if any value changed.
template <std::size_t N>
bool clampDeltaArray(std::array<float, N>& values) noexcept
{
    bool changed = false;
    for (auto& v : values)
    {
        const float finite = std::isfinite(v) ? v : 0.0f;
        if (finite != v)
            changed = true;
        const float clamped = std::clamp(finite, -1.0f, 1.0f);
        if (clamped != finite)
            changed = true;
        v = clamped;
    }
    return changed;
}

// Default editable mask: non-high-risk controls only. Mirrors the controls
// the DeterministicBaseline runner is willing to move, so switching the
// controller's runner between the two does not silently expand authority.
MasteringControlMask defaultEditableMask() noexcept
{
    MasteringControlMask mask;
    mask.eq       = true;
    mask.dynamics = true;
    mask.stereo   = true;
    mask.loudness = true;
    // harmonic + limiter are high-risk (see NeuralMasteringSafetyPolicy::defaultConfig).
    return mask;
}

} // namespace

// ── Pure I/O transforms ──────────────────────────────────────────────────────

void serializeFeatureFrame(const NeuralMasteringFeatureFrame& frame,
                           float* outTensor,
                           std::size_t capacity) noexcept
{
    if (outTensor == nullptr || capacity < kOnnxInputFeatureCount)
        return;

    float* cursor = outTensor;

    // [0..10] scalar features
    cursor[0]  = finiteOrZero(frame.integratedLUFS);
    cursor[1]  = finiteOrZero(frame.shortTermLUFS);
    cursor[2]  = finiteOrZero(frame.momentaryLUFS);
    cursor[3]  = finiteOrZero(frame.loudnessRange);
    cursor[4]  = finiteOrZero(frame.truePeakDbTp);
    cursor[5]  = finiteOrZero(frame.crestFactorDb);
    cursor[6]  = finiteOrZero(frame.spectralTilt);
    cursor[7]  = finiteOrZero(frame.monoFoldDownDeltaDb);
    cursor[8]  = finiteOrZero(frame.transientDensity);
    cursor[9]  = finiteOrZero(frame.harmonicRisk);
    cursor[10] = finiteOrZero(frame.sourceQualityScore);
    cursor += kOnnxScalarFeatureCount;

    // [11..42] spectral bands
    float scratchSpec[kNeuralMasteringSpectralBandCount] {};
    writeArray(cursor, frame.spectralBands, scratchSpec);

    // [43..50] stereo correlation
    float scratchStereo[kNeuralMasteringStereoBandCount] {};
    writeArray(cursor, frame.stereoCorrelation, scratchStereo);

    // [51..58] mid/side ratio
    writeArray(cursor, frame.midSideRatio, scratchStereo);

    // [59..62] meta (as float; the model treats these as conditioning)
    cursor[0] = finiteOrZero(static_cast<float>(frame.sampleRate));
    cursor[1] = finiteOrZero(static_cast<float>(frame.channelCount));
    cursor[2] = finiteOrZero(static_cast<float>(frame.blockSize));
    // frameIndex is uint64; feed the low 32 bits as float to avoid wrap surprises.
    cursor[3] = finiteOrZero(static_cast<float>(static_cast<std::uint32_t>(frame.frameIndex)));
}

NeuralMasteringPlanCandidate
buildPlanCandidate(const float* deltaTensor,
                   std::size_t count,
                   const NeuralMasteringFeatureFrame& frame,
                   float confidence,
                   NeuralMasteringEvidenceLevel evidence,
                   MasteringControlMask editableMask) noexcept
{
    NeuralMasteringPlanCandidate candidate {};
    candidate.schemaVersion   = kNeuralMasteringPlanSchemaVersion;
    candidate.runtimeMode     = NeuralMasteringRuntimeMode::Background;
    candidate.producedAtFrame = frame.frameIndex;
    // Plans are valid for ~30 s at 48 kHz; the safety policy re-checks age.
    candidate.expiresAfterFrame = frame.frameIndex + 96000;
    candidate.confidence      = std::isfinite(confidence) ? std::clamp(confidence, 0.0f, 1.0f) : 0.0f;
    candidate.evidenceLevel   = evidence;
    candidate.editableMask    = editableMask;

    if (deltaTensor != nullptr && count >= kOnnxOutputDeltaCount)
    {
        const float* cursor = deltaTensor;
        readArray(cursor, candidate.deltas.eq);
        readArray(cursor, candidate.deltas.dynamics);
        readArray(cursor, candidate.deltas.stereo);
        readArray(cursor, candidate.deltas.harmonic);
        readArray(cursor, candidate.deltas.limiter);
        readArray(cursor, candidate.deltas.loudness);
    }

    // Convention (matches DeterministicBaseline): targets == deltas. The
    // safety policy projects (previous + delta) and treats deltas as the
    // authoritative control vector; setting targets equal keeps the candidate
    // self-consistent for review/display.
    candidate.targets = candidate.deltas;

    // Mask out deltas the runner is not authorised to move so a model that
    // emits movement on, e.g., harmonic while the mask forbids it, has those
    // values zeroed before validation. (The safety policy would reject the
    // high-risk mask anyway; this keeps the candidate honest for telemetry.)
    if (!editableMask.eq)       candidate.deltas.eq.fill(0.0f);
    if (!editableMask.dynamics) candidate.deltas.dynamics.fill(0.0f);
    if (!editableMask.stereo)   candidate.deltas.stereo.fill(0.0f);
    if (!editableMask.harmonic) candidate.deltas.harmonic.fill(0.0f);
    if (!editableMask.limiter)  candidate.deltas.limiter.fill(0.0f);
    if (!editableMask.loudness) candidate.deltas.loudness.fill(0.0f);
    candidate.targets = candidate.deltas;

    sanitizePlanCandidate(candidate);
    return candidate;
}

bool sanitizePlanCandidate(NeuralMasteringPlanCandidate& candidate) noexcept
{
    bool changed = false;

    if (!std::isfinite(candidate.confidence))
    {
        candidate.confidence = 0.0f;
        changed = true;
    }
    else if (candidate.confidence < 0.0f || candidate.confidence > 1.0f)
    {
        candidate.confidence = std::clamp(candidate.confidence, 0.0f, 1.0f);
        changed = true;
    }

    changed |= sanitizeArray(candidate.targets.eq);
    changed |= sanitizeArray(candidate.targets.dynamics);
    changed |= sanitizeArray(candidate.targets.stereo);
    changed |= sanitizeArray(candidate.targets.harmonic);
    changed |= sanitizeArray(candidate.targets.limiter);
    changed |= sanitizeArray(candidate.targets.loudness);

    changed |= clampDeltaArray(candidate.deltas.eq);
    changed |= clampDeltaArray(candidate.deltas.dynamics);
    changed |= clampDeltaArray(candidate.deltas.stereo);
    changed |= clampDeltaArray(candidate.deltas.harmonic);
    changed |= clampDeltaArray(candidate.deltas.limiter);
    changed |= clampDeltaArray(candidate.deltas.loudness);

    return changed;
}

NeuralMasteringProposalDisposition
evaluateNeuralMasteringProposal(const float* deltaTensor,
                                std::size_t count,
                                const NeuralMasteringFeatureFrame& frame) noexcept
{
    NeuralMasteringProposalDisposition disposition {};

    if (!hasPlausibleFeatureFrame(frame) || !allFiniteDeltas(deltaTensor, count))
    {
        disposition.confidence = 0.0f;
        disposition.abstain = true;
        disposition.reviewOnly = false;
        disposition.requestedFallbackMode = NeuralMasteringFallbackMode::TransparentBypass;
        return disposition;
    }

    const auto maxAbs = maxAbsDelta(deltaTensor, count);
    if (maxAbs <= 0.01f)
    {
        // A plausible frame plus a near-zero neural proposal is a useful
        // decision: "do nothing". Do not abstain, because abstention would hand
        // the frame to a fallback correction heuristic that may move controls.
        disposition.confidence = 0.96f;
        disposition.abstain = false;
        disposition.reviewOnly = false;
        disposition.requestedFallbackMode = NeuralMasteringFallbackMode::None;
        return disposition;
    }

    float confidence = 0.88f;
    if (maxAbs > 0.35f)
        confidence -= std::min(0.18f, (maxAbs - 0.35f) * 0.35f);
    if (maxAbs > 0.70f)
        confidence -= std::min(0.20f, (maxAbs - 0.70f) * 0.80f);

    if (frame.sourceQualityScore < 0.5f)
        confidence -= (0.5f - frame.sourceQualityScore) * 0.20f;
    if (frame.channelCount == 1)
        confidence -= 0.05f;

    disposition.confidence = std::clamp(confidence, 0.0f, 1.0f);
    disposition.abstain = false;
    disposition.reviewOnly = disposition.confidence < NeuralMasteringSafetyPolicy::defaultConfig().minConfidence
                          || maxAbs > 0.85f;
    disposition.requestedFallbackMode = disposition.reviewOnly
        ? NeuralMasteringFallbackMode::ReviewOnly
        : NeuralMasteringFallbackMode::None;
    return disposition;
}

// ── OnnxNeuralMasteringRunner ────────────────────────────────────────────────

OnnxNeuralMasteringRunner::OnnxNeuralMasteringRunner() noexcept
    : editableMask_(defaultEditableMask())
{
    // Pre-size the reusable buffers up front so even the seam build allocates
    // exactly once at construction (never inside proposePlan()).
    inputBuffer_.resize(kOnnxInputFeatureCount, 0.0f);
    outputBuffer_.resize(kOnnxOutputDeltaCount, 0.0f);
}

OnnxNeuralMasteringRunner::~OnnxNeuralMasteringRunner() = default;

bool OnnxNeuralMasteringRunner::loadModel(std::string_view absolutePath,
                                          std::string_view modelId,
                                          std::string_view checksum)
{
    // Always reset any prior session first so a failed reload leaves the runner
    // in a clean abstain state (never half-loaded).
    unloadModel();

    if (!session_)
        session_ = std::make_unique<OnnxSessionHandle>();

    NeuralMasteringModelMetadata meta;
    meta.setModelId(modelId);
    meta.setChecksum(checksum);
    if (absolutePath.empty() || !meta.hasModelId())
    {
        available_ = false;
        return false;
    }

    // Record id/checksum for metadata regardless of whether ORT is linked.
    std::memcpy(session_->reservedModelId, meta.modelId.data(),
                meta.modelId.size() * sizeof(char));
    std::memcpy(session_->reservedChecksum, meta.checksum.data(),
                meta.checksum.size() * sizeof(char));

#if !MORE_PHI_HAS_ONNX
    // SEAM: ORT not linked. Abstain; the controller falls back to the
    // deterministic baseline transparently.
    (void) absolutePath;
    available_ = false;
    return false;
#else
    // ── Real ONNX Runtime session creation (message thread) ──────────────────
    try
    {
        session_->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "more-phi");
        session_->allocator = std::make_unique<Ort::AllocatorWithDefaultOptions>();

        Ort::SessionOptions opts;
        // Single intra-op thread: these models are tiny and run at 1-5 Hz on the
        // message thread; a thread pool would add spin-up latency for no gain.
        opts.SetIntraOpNumThreads(1);
        opts.SetGraphOptimizationLevel(ORT_ENABLE_ALL);

        // Wide-char path on Windows (Ort::Session's char* ctor is narrow-only).
        const std::string pathStr { absolutePath };
        session_->session = std::make_unique<Ort::Session>(
#ifdef _WIN32
            *session_->env, std::wstring(pathStr.begin(), pathStr.end()).c_str(), opts
#else
            *session_->env, pathStr.c_str(), opts
#endif
        );

        // ── Validate I/O contract against the v1 schema ──────────────────────
        // Exactly one input tensor [1, 63] and one output tensor [1, 72]. Any
        // mismatch means the model was not trained for this runner — refuse it.
        if (session_->session->GetInputCount() != 1 || session_->session->GetOutputCount() != 1)
        {
            unloadModel();
            return false;
        }

        // NOTE: ORT 1.22.1's Session API is GetInputTypeInfo/GetOutputTypeInfo
        // (returning Ort::TypeInfo by value). The *Allocated suffix only exists
        // for the *Name* accessors, not TypeInfo — using it here was the
        // original compile error against the real 1.22.1 headers.
        auto inputInfo = session_->session->GetInputTypeInfo(0);
        auto outputInfo = session_->session->GetOutputTypeInfo(0);
        // GetTensorTypeAndShapeInfo() returns a ConstTensorTypeAndShapeInfo by
        // value (not a pointer), so member access is '.', not '->'.
        auto inputTensor = inputInfo.GetTensorTypeAndShapeInfo();
        auto outputTensor = outputInfo.GetTensorTypeAndShapeInfo();

        if (inputTensor.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            outputTensor.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        {
            unloadModel();
            return false;
        }

        // Query dimensions via GetDimensionsCount/GetDimensions rather than the
        // convenience GetShape() wrapper. Against ORT 1.22.1's cxx headers the
        // returned-by-value GetShape() path segfaults on exported graphs with a
        // symbolic 'batch' dim (verified on the waveform->decision runner — see
        // SonicMasterDecisionRunner::loadModel, commit a28b621); the lower-level
        // GetDimensions call on the same handle is stable. Same data, no fault.
        // GetElementType() above is unaffected because it does not touch the
        // dimension storage. This runner is currently seam-only (ORT not linked
        // into its path) but the fix is applied preemptively so a future
        // feature-frame model wiring cannot reintroduce the segfault.
        const auto inDimCount = inputTensor.GetDimensionsCount();
        const auto outDimCount = outputTensor.GetDimensionsCount();
        std::vector<int64_t> inDims(inDimCount, -1);
        inputTensor.GetDimensions(inDims.data(), inDimCount);
        std::vector<int64_t> outDims(outDimCount, -1);
        outputTensor.GetDimensions(outDims.data(), outDimCount);
        // Accept [1, N] or [N]; require the trailing product to match the schema.
        const auto totalIn = std::accumulate(inDims.begin(), inDims.end(), int64_t { 1 },
                                             [](int64_t a, int64_t b) { return a * (b > 0 ? b : 1); });
        const auto totalOut = std::accumulate(outDims.begin(), outDims.end(), int64_t { 1 },
                                              [](int64_t a, int64_t b) { return a * (b > 0 ? b : 1); });
        if (totalIn != static_cast<int64_t>(kOnnxInputFeatureCount) ||
            totalOut != static_cast<int64_t>(kOnnxOutputDeltaCount))
        {
            unloadModel();
            return false;
        }

        // Cache I/O tensor names (names vary by exporter; read from the model).
        auto inputNameAlloc = session_->session->GetInputNameAllocated(0, *session_->allocator);
        auto outputNameAlloc = session_->session->GetOutputNameAllocated(0, *session_->allocator);
        session_->inputName = inputNameAlloc.get();
        session_->outputName = outputNameAlloc.get();

        available_ = true;
        return true;
    }
    catch (const Ort::Exception&)
    {
        unloadModel();
        return false;
    }
#endif
}

void OnnxNeuralMasteringRunner::unloadModel() noexcept
{
    session_.reset();
    available_ = false;
    std::fill(inputBuffer_.begin(),  inputBuffer_.end(),  0.0f);
    std::fill(outputBuffer_.begin(), outputBuffer_.end(), 0.0f);
}

void OnnxNeuralMasteringRunner::setEditableMask(MasteringControlMask mask) noexcept
{
    editableMask_ = mask;
}

bool OnnxNeuralMasteringRunner::isAvailable() const noexcept
{
    return available_;
}

bool OnnxNeuralMasteringRunner::usesExternalInference() const noexcept
{
    // Only claim external inference when a session is genuinely live, so an
    // unloaded runner wired into a controller is indistinguishable from the
    // Null runner for the status flags.
    return available_;
}

NeuralMasteringModelMetadata OnnxNeuralMasteringRunner::metadata() const noexcept
{
    NeuralMasteringModelMetadata meta;
    meta.featureSchemaVersion = kNeuralMasteringFeatureSchemaVersion;
    meta.outputSchemaVersion  = kNeuralMasteringPlanSchemaVersion;
    meta.evidenceLevel        = NeuralMasteringEvidenceLevel::Planning;
    meta.licenseStatus        = NeuralMasteringLicenseStatus::Unresolved;
    meta.enabled              = available_;
    meta.audioCallbackInference = false; // enforced: this runner never runs on audio thread

    if (session_)
    {
        std::memcpy(meta.modelId.data(),  session_->reservedModelId,
                    meta.modelId.size()  * sizeof(char));
        std::memcpy(meta.checksum.data(), session_->reservedChecksum,
                    meta.checksum.size() * sizeof(char));
    }
    return meta;
}

NeuralMasteringPlannerResult
OnnxNeuralMasteringRunner::proposePlan(const NeuralMasteringFeatureFrame& frame) noexcept
{
    NeuralMasteringPlannerResult result;
    result.producedCandidate = false;
    result.usedModel = false;
    result.fallbackMode = NeuralMasteringFallbackMode::DeterministicBaseline;
    result.candidate.producedAtFrame = frame.frameIndex;
    result.candidate.expiresAfterFrame = frame.frameIndex;
    result.candidate.abstain = true;

    // SEAM contract: with no live session, abstain. The controller's
    // makeCandidate() then transparently falls back to the deterministic
    // baseline, so wiring this runner in changes nothing observable until a
    // model is actually loaded.
    if (!available_)
        return result;

    // ── Real inference path (executed when a session is live) ───────────────
    // All buffers are pre-allocated in the ctor / loadModel(); NO per-call
    // allocation is permitted here — proposePlan() must be allocation-free so
    // it is safe to invoke from the message-thread timer at 1-5 Hz indefinitely.
    serializeFeatureFrame(frame, inputBuffer_.data(), inputBuffer_.size());

#if MORE_PHI_HAS_ONNX
    // Inference: build a tensor view over the pre-allocated input buffer, run,
    // copy the output floats into the pre-allocated output buffer. The Ort::Value
    // wrapper itself is stack-allocated and does not own the float storage.
    try
    {
        const std::array<int64_t, 2> inShape { 1, static_cast<int64_t>(kOnnxInputFeatureCount) };
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            session_->memoryInfo,
            inputBuffer_.data(),
            inputBuffer_.size(),
            inShape.data(),
            inShape.size());

        const char* inNames[]  = { session_->inputName.c_str() };
        const char* outNames[] = { session_->outputName.c_str() };

        auto outputs = session_->session->Run(
            Ort::RunOptions { nullptr },
            inNames, &inputTensor, 1,
            outNames, 1);

        if (! outputs.empty() && outputs[0].IsTensor())
        {
            const float* outData = outputs[0].GetTensorData<float>();
            const size_t outCount = std::min(outputBuffer_.size(),
                                             outputs[0].GetTensorTypeAndShapeInfo().GetElementCount());
            std::copy(outData, outData + outCount, outputBuffer_.begin());
            inferenceSucceeded = outCount >= outputBuffer_.size();
        }
        else
        {
            std::fill(outputBuffer_.begin(), outputBuffer_.end(), 0.0f);
        }
    }
    catch (const Ort::Exception&)
    {
        // Inference failed — produce an all-zero (no-op) plan and let the
        // safety policy project it as "no change". The next frame retries.
        std::fill(outputBuffer_.begin(), outputBuffer_.end(), 0.0f);
    }
#else
    // ORT not linked: leave outputBuffer_ zeroed so the candidate is a no-op
    // plan (accepted by the safety policy, projects as "no change"). This keeps
    // the contract testable without a model runtime.
    std::fill(outputBuffer_.begin(), outputBuffer_.end(), 0.0f);
#endif

    const auto disposition = inferenceSucceeded
        ? evaluateNeuralMasteringProposal(outputBuffer_.data(), outputBuffer_.size(), frame)
        : NeuralMasteringProposalDisposition { 0.0f, true, false, NeuralMasteringFallbackMode::TransparentBypass };

    result.candidate = buildPlanCandidate(outputBuffer_.data(),
                                          outputBuffer_.size(),
                                          frame,
                                          disposition.confidence,
                                          NeuralMasteringEvidenceLevel::PrototypeMeasured,
                                          editableMask_);
    result.candidate.abstain = disposition.abstain;
    result.candidate.reviewOnly = disposition.reviewOnly;
    result.candidate.requestedFallbackMode = disposition.requestedFallbackMode;
    result.producedCandidate = true;
    result.usedModel = true;
    result.fallbackMode = disposition.requestedFallbackMode;
    return result;
}

} // namespace more_phi
