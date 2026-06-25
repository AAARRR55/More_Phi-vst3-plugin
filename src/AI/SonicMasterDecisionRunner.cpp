/*
 * More-Phi — AI/SonicMasterDecisionRunner.cpp
 *
 * SEAM IMPLEMENTATION. The session/inference path is active when the build
 * defines MORE_PHI_HAS_ONNX=1 (ONNX Runtime linked via MORE_PHI_ENABLE_ONNX);
 * otherwise loadModel() abstains and the runner is unavailable.
 */
#include "AI/SonicMasterDecisionRunner.h"

#include <algorithm>
#include <numeric>

#ifndef MORE_PHI_HAS_ONNX
#define MORE_PHI_HAS_ONNX 0
#endif

#if MORE_PHI_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace more_phi {

#if MORE_PHI_HAS_ONNX
struct SonicMasterSessionHandle
{
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
    std::unique_ptr<Ort::AllocatorWithDefaultOptions> allocator;
    std::string inputName;
    std::string outputName;
    Ort::MemoryInfo memoryInfo { Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault) };
    std::vector<float> inputBuffer;  // [2 * kSonicMasterSegmentFrames], reused
    std::vector<int64_t> inputShape  { 1, 2, static_cast<int64_t>(kSonicMasterSegmentFrames) };
    std::vector<int64_t> outputShape { 1, static_cast<int64_t>(kSonicMasterDecisionWidth) };
};
#else
struct SonicMasterSessionHandle {};
#endif

SonicMasterDecisionRunner::SonicMasterDecisionRunner() noexcept = default;
SonicMasterDecisionRunner::~SonicMasterDecisionRunner() = default;

bool SonicMasterDecisionRunner::loadModel(std::string_view modelPath)
{
    unloadModel();

    if (modelPath.empty()) return false;

#if !MORE_PHI_HAS_ONNX
    return false;
#else
    try
    {
        session_ = std::make_unique<SonicMasterSessionHandle>();
        session_->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "sonicmaster");
        session_->allocator = std::make_unique<Ort::AllocatorWithDefaultOptions>();

        Ort::SessionOptions opts;
        // Single intra-op thread: inference runs at ~0.3 Hz on the analysis
        // thread; a thread pool would add spin-up latency for no gain.
        opts.SetIntraOpNumThreads(1);
        opts.SetGraphOptimizationLevel(ORT_ENABLE_ALL);

        const std::string pathStr { modelPath };
        session_->session = std::make_unique<Ort::Session>(
#ifdef _WIN32
            *session_->env, std::wstring(pathStr.begin(), pathStr.end()).c_str(), opts
#else
            *session_->env, pathStr.c_str(), opts
#endif
        );

        // ── Validate I/O contract ───────────────────────────────────────────
        if (session_->session->GetInputCount() != 1 || session_->session->GetOutputCount() != 1)
        { unloadModel(); return false; }

        // ORT 1.22.1: GetInputTypeInfo/GetOutputTypeInfo return TypeInfo by
        // value; GetTensorTypeAndShapeInfo() is '.' (not '->'). Same pattern
        // as OnnxNeuralMasteringRunner (commit e9bea9c).
        auto inputInfo = session_->session->GetInputTypeInfo(0);
        auto outputInfo = session_->session->GetOutputTypeInfo(0);
        auto inTensor = inputInfo.GetTensorTypeAndShapeInfo();
        auto outTensor = outputInfo.GetTensorTypeAndShapeInfo();
        if (inTensor.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            outTensor.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        { unloadModel(); return false; }

        // Query dimensions via GetDimensionsCount/GetDimensions rather than the
        // convenience GetShape() wrapper. Against ORT 1.22.1's cxx headers the
        // returned-by-value GetShape() path segfaults on this exported graph
        // (a symbolic 'batch' dim); the lower-level GetDimensions call on the
        // same handle is stable. Same data, no fault. GetElementType() above is
        // unaffected because it does not touch the dimension storage.
        const auto inDimCount = inTensor.GetDimensionsCount();
        const auto outDimCount = outTensor.GetDimensionsCount();
        std::vector<int64_t> inDims(inDimCount, -1);
        inTensor.GetDimensions(inDims.data(), inDimCount);
        std::vector<int64_t> outDims(outDimCount, -1);
        outTensor.GetDimensions(outDims.data(), outDimCount);

        const auto totalIn = std::accumulate(inDims.begin(), inDims.end(),
                                             int64_t { 1 },
                                             [](int64_t a, int64_t b) { return a * (b > 0 ? b : 1); });
        const auto totalOut = std::accumulate(outDims.begin(), outDims.end(),
                                              int64_t { 1 },
                                              [](int64_t a, int64_t b) { return a * (b > 0 ? b : 1); });
        if (totalIn != static_cast<int64_t>(2 * kSonicMasterSegmentFrames) ||
            totalOut != static_cast<int64_t>(kSonicMasterDecisionWidth))
        { unloadModel(); return false; }

        auto inNameAlloc = session_->session->GetInputNameAllocated(0, *session_->allocator);
        auto outNameAlloc = session_->session->GetOutputNameAllocated(0, *session_->allocator);
        session_->inputName = inNameAlloc.get();
        session_->outputName = outNameAlloc.get();
        session_->inputBuffer.assign(2 * kSonicMasterSegmentFrames, 0.0f);

        available_ = true;
        return true;
    }
    catch (const Ort::Exception&)
    {
        unloadModel();
        return false;
    }
    catch (...)
    {
        unloadModel();
        return false;
    }
#endif
}

void SonicMasterDecisionRunner::unloadModel() noexcept
{
    session_.reset();
    available_ = false;
}

bool SonicMasterDecisionRunner::isAvailable() const noexcept { return available_; }

bool SonicMasterDecisionRunner::runDecision(const float* stereoInterleaved,
                                            float* outDecision,
                                            std::size_t outCapacity) noexcept
{
#if !MORE_PHI_HAS_ONNX
    (void) stereoInterleaved; (void) outDecision; (void) outCapacity;
    return false;
#else
    if (!available_ || session_ == nullptr || stereoInterleaved == nullptr
        || outDecision == nullptr || outCapacity < kSonicMasterDecisionWidth)
        return false;

    try
    {
        std::copy_n(stereoInterleaved, 2 * kSonicMasterSegmentFrames, session_->inputBuffer.data());

        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            session_->memoryInfo,
            session_->inputBuffer.data(),
            session_->inputBuffer.size(),
            session_->inputShape.data(),
            session_->inputShape.size());

        const char* inputNames[] = { session_->inputName.c_str() };
        const char* outputNames[] = { session_->outputName.c_str() };

        auto outputs = session_->session->Run(
            Ort::RunOptions { nullptr }, inputNames, &inputTensor, 1, outputNames, 1);

        const float* outData = outputs[0].GetTensorData<float>();
        std::copy_n(outData, kSonicMasterDecisionWidth, outDecision);
        return true;
    }
    catch (...)
    {
        return false;
    }
#endif
}

} // namespace more_phi
