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
#include <fstream>
#include <sstream>
#include <string>
#include <cmath>
#include <chrono>
#include <nlohmann/json.hpp>

#ifndef MORE_PHI_HAS_ONNX
#define MORE_PHI_HAS_ONNX 0
#endif

#if MORE_PHI_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#ifdef _WIN32
#include <windows.h>  // MultiByteToWideChar (L1-8)
#endif
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

bool SonicMasterDecisionRunner::loadModel(std::string_view modelPath, std::string_view contractPath)
{
    unloadModel();

    if (modelPath.empty()) return false;

    // AUDIT-FIX (A2): validate the sibling .contract.json before touching ORT.
    // This converts "model was retrained with different preprocessing" from a
    // silent out-of-distribution degradation into a startup error. The false
    // return is surfaced via the caller's existing "model not found" log path.
    if (!contractPath.empty())
    {
        SonicMasterModelContract contract;
        if (!parseSonicMasterContract(std::string(contractPath), contract) || !contract.validate())
            return false;
    }

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

#ifdef _WIN32
        // AUDIT-FIX (L1-8, 2026-06-29): use MultiByteToWideChar for proper
        // UTF-8→UTF-16 conversion. The old std::wstring(pathStr.begin(),
        // pathStr.end()) did a code-unit-by-code-unit copy that mangled
        // non-ASCII characters (e.g. CJK paths, accented chars), causing
        // ORT to fail with an unhelpful error. The wstring must outlive the
        // Ort::Session constructor call, so it is declared before make_unique.
        std::wstring widePath;
        {
            const int len = MultiByteToWideChar(CP_UTF8, 0,
                pathStr.c_str(), static_cast<int>(pathStr.size()), nullptr, 0);
            widePath.resize(static_cast<std::size_t>(len));
            MultiByteToWideChar(CP_UTF8, 0,
                pathStr.c_str(), static_cast<int>(pathStr.size()), &widePath[0], len);
        }
        session_->session = std::make_unique<Ort::Session>(
            *session_->env, widePath.c_str(), opts);
#else
        session_->session = std::make_unique<Ort::Session>(
            *session_->env, pathStr.c_str(), opts);
#endif

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

        // AUDIT-FIX (A1): validate rank + channel dim explicitly, not just the
        // element product. A model exported as [2, N] would pass a product-only
        // check and feed wrongly-shaped data at inference time.
        const bool inputShapeOK = (inDimCount == 3)
            && (inDims[0] == 1 || inDims[0] == -1)        // batch (symbolic ok)
            && (inDims[1] == 2 || inDims[1] == -1)        // stereo channels
            && (inDims[2] == static_cast<int64_t>(kSonicMasterSegmentFrames) || inDims[2] == -1);

        const auto totalIn = std::accumulate(inDims.begin(), inDims.end(),
                                             int64_t { 1 },
                                             [](int64_t a, int64_t b) { return a * (b > 0 ? b : 1); });
        const auto totalOut = std::accumulate(outDims.begin(), outDims.end(),
                                              int64_t { 1 },
                                              [](int64_t a, int64_t b) { return a * (b > 0 ? b : 1); });
        if (!inputShapeOK ||
            totalIn != static_cast<int64_t>(2 * kSonicMasterSegmentFrames) ||
            totalOut != static_cast<int64_t>(kSonicMasterDecisionWidth))
        { unloadModel(); return false; }

        auto inNameAlloc = session_->session->GetInputNameAllocated(0, *session_->allocator);
        auto outNameAlloc = session_->session->GetOutputNameAllocated(0, *session_->allocator);
        session_->inputName = inNameAlloc.get();
        session_->outputName = outNameAlloc.get();
        session_->inputBuffer.assign(2 * kSonicMasterSegmentFrames, 0.0f);

        // AUDIT-FIX (L1-7, 2026-06-29): warmup inference to trigger ORT lazy init
        // and JIT compilation. The first session->Run() can be 2-10× slower than
        // subsequent calls due to graph optimization and memory allocation inside
        // ORT. Running a zero-input inference at load time populates those caches so
        // the first real inference on the analysis thread doesn't blow the 5-second
        // MCP timeout. Warmup failure is non-fatal — it just means the first real
        // inference may be slow.
        try
        {
            Ort::Value warmupInput = Ort::Value::CreateTensor<float>(
                session_->memoryInfo,
                session_->inputBuffer.data(),
                session_->inputBuffer.size(),
                session_->inputShape.data(),
                session_->inputShape.size());
            const char* wIn[]  = { session_->inputName.c_str() };
            const char* wOut[] = { session_->outputName.c_str() };
            session_->session->Run(Ort::RunOptions{nullptr}, wIn, &warmupInput, 1, wOut, 1);
        }
        catch (...) { /* warmup failure non-fatal */ }

        available_.store(true, std::memory_order_release);  // AUDIT-FIX (L1-1)
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
    available_.store(false, std::memory_order_release);  // AUDIT-FIX (L1-1)
}

bool SonicMasterDecisionRunner::isAvailable() const noexcept
{
    return available_.load(std::memory_order_acquire);  // AUDIT-FIX (L1-1)
}

bool SonicMasterDecisionRunner::runDecision(const float* stereoInterleaved,
                                            float* outDecision,
                                            std::size_t outCapacity) noexcept
{
#if !MORE_PHI_HAS_ONNX
    (void) stereoInterleaved; (void) outDecision; (void) outCapacity;
    return false;
#else
    if (!available_.load(std::memory_order_acquire) || session_ == nullptr || stereoInterleaved == nullptr
        || outDecision == nullptr || outCapacity < kSonicMasterDecisionWidth)
        return false;

    try
    {
        // AUDIT-FIX (A1): de-interleave stereo into the [1, 2, N] row-major layout
        // the tensor declares (inputShape = {1, 2, kSonicMasterSegmentFrames}).
        // The previous verbatim std::copy_n of L0,R0,L1,R1,... into a [1,2,N]
        // tensor fed the model all samples as channel 0 (L0,R0,L1,R1 in time)
        // with channel 1 reading L(N),R(N),... — i.e. wrongly-shaped data.
        // ponytail: two-pass copy, ~2x memcpy cost but negligible at 0.3 Hz
        // inference. SIMD de-interleave only if profiling ever shows it (it won't).
        float* dst = session_->inputBuffer.data();
        const std::size_t N = kSonicMasterSegmentFrames;
        for (std::size_t t = 0; t < N; ++t)
        {
            dst[t]     = stereoInterleaved[2 * t + 0];  // channel 0 = all L
            dst[N + t] = stereoInterleaved[2 * t + 1];  // channel 1 = all R
        }

        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            session_->memoryInfo,
            session_->inputBuffer.data(),
            session_->inputBuffer.size(),
            session_->inputShape.data(),
            session_->inputShape.size());

        const char* inputNames[] = { session_->inputName.c_str() };
        const char* outputNames[] = { session_->outputName.c_str() };

        // AUDIT (C2, 2026-06-25): time the inference call so the 3 s analysis-
        // cycle budget can be monitored. steady_clock (monotonic) is the right
        // clock — wall-clock could jump on NTP sync. Relaxed atomic stores are
        // fine: these are diagnostics, no dependent data.
        const auto t0 = std::chrono::steady_clock::now();
        auto outputs = session_->session->Run(
            Ort::RunOptions { nullptr }, inputNames, &inputTensor, 1, outputNames, 1);
        const auto t1 = std::chrono::steady_clock::now();
        const float elapsedMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
        lastInferenceMs_.store(elapsedMs, std::memory_order_relaxed);
        // Saturating max so a single slow first-run warmup doesn't get lost.
        float prevMax = maxInferenceMs_.load(std::memory_order_relaxed);
        while (elapsedMs > prevMax
               && !maxInferenceMs_.compare_exchange_weak(prevMax, elapsedMs,
                                                          std::memory_order_relaxed))
        {
            // prevMax refreshed by CAS failure; loop until stored or overtaken.
        }

        // AUDIT-FIX (L1-2, 2026-06-29): verify the output tensor has at least
        // kSonicMasterDecisionWidth elements before reading. Symbolic dimensions in
        // the model shape (e.g. batch = -1) pass load-time validation, but the
        // runtime output may be shorter if the model is corrupted or version-mismatched.
        const auto outInfo = outputs[0].GetTensorTypeAndShapeInfo();
        if (static_cast<std::size_t>(outInfo.GetElementCount()) < kSonicMasterDecisionWidth)
        {
            recordError("ONNX output dimension mismatch: fewer elements than expected");
            failCount_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const float* outData = outputs[0].GetTensorData<float>();
        std::copy_n(outData, kSonicMasterDecisionWidth, outDecision);

        // DIAG (2026-06-26): clear the error + bump counters on success so the
        // caller can distinguish "never failed" from "last run failed."
        errorLen_.store(0, std::memory_order_relaxed);
        runCount_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    catch (const Ort::Exception& e)
    {
        // DIAG: capture the real ORT error. Truncated to the fixed buffer so the
        // analysis thread never allocates.
        recordError(e.what());
        failCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    catch (const std::exception& e)
    {
        recordError(e.what());
        failCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    catch (...)
    {
        recordError("unknown exception during ORT session->Run()");
        failCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
#endif
}

void SonicMasterDecisionRunner::recordError(const char* msg) noexcept
{
    if (msg == nullptr) msg = "(null)";
    // AUDIT-FIX (L1-3, 2026-06-29): lock the buffer so concurrent getLastRunError()
    // readers see a consistent snapshot. recordError is called from the analysis
    // thread only; getLastRunError can be called from any thread (e.g. MCP handler).
    const std::lock_guard<std::mutex> lock(errorBufMutex_);
    std::size_t n = 0;
    for (; n < kErrorBufLen - 1 && msg[n] != '\0'; ++n)
        lastRunError_[n] = msg[n];
    lastRunError_[n] = '\0';
    errorLen_.store(n, std::memory_order_relaxed);
}

// ── AUDIT-FIX (A2): contract parsing ─────────────────────────────────────────

bool parseSonicMasterContract(const std::string& contractJsonPath, SonicMasterModelContract& out)
{
    out = SonicMasterModelContract{};

    std::ifstream ifs(contractJsonPath, std::ios::binary);
    if (!ifs.is_open())
        return false;

    std::stringstream ss;
    ss << ifs.rdbuf();
    const std::string text = ss.str();
    if (text.empty())
        return false;

    try
    {
        const auto j = nlohmann::json::parse(text);

        // Required scalar fields. Missing or wrong-type → fail-closed.
        if (!j.contains("schema") || !j["schema"].is_number_integer()) return false;
        if (!j.contains("input_layout") || !j["input_layout"].is_string()) return false;
        if (!j.contains("normalization") || !j["normalization"].is_string()) return false;
        if (!j.contains("dtype") || !j["dtype"].is_string()) return false;
        if (!j.contains("sample_rate") || !j["sample_rate"].is_number()) return false;
        if (!j.contains("segment_frames") || !j["segment_frames"].is_number_integer()) return false;
        if (!j.contains("peak_target_linear") || !j["peak_target_linear"].is_number()) return false;

        out.schema = j["schema"].get<int>();
        out.inputLayout = j["input_layout"].get<std::string>();
        out.normalization = j["normalization"].get<std::string>();
        out.dtype = j["dtype"].get<std::string>();
        out.sampleRate = j["sample_rate"].get<double>();
        out.segmentFrames = static_cast<std::size_t>(j["segment_frames"].get<long long>());
        out.peakTargetLinear = j["peak_target_linear"].get<float>();
        out.preprocessFingerprint = j.value("preprocess_fingerprint", "");

        return true;
    }
    catch (...)
    {
        return false;
    }
}

} // namespace more_phi
