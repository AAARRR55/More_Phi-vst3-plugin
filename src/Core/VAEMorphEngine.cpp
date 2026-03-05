/*
 * MorphSnap — Core/VAEMorphEngine.cpp
 *
 * Variational Autoencoder morph engine — message thread only.
 *
 * V2 MVP stub implementation. All inference methods return reasonable
 * defaults. interpolateLatent() performs linear blending in latent space,
 * which is the mathematically correct operation regardless of ONNX status.
 *
 * Post-MVP: Replace the TODO stubs with actual ONNX Runtime calls:
 *   #include <onnxruntime_cxx_api.h>
 *   Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "VAEMorphEngine");
 *   Ort::Session session(env, modelPath.toWideCharPointer(), sessionOptions);
 *
 * WARNING: This engine is currently a STUB. It provides linear interpolation
 * in latent space but does NOT perform actual neural network inference.
 * The ONNX Runtime integration is planned for a post-MVP release.
 */
#include "VAEMorphEngine.h"
#include <algorithm>
#include <cmath>
#include <cassert>

namespace morphsnap {

// ── Constructor / Destructor ─────────────────────────────────────────────────

VAEMorphEngine::VAEMorphEngine()
    : modelLoaded_(false)
    , latentDims_(0)
{
    // No resources to acquire until loadModel() is called.
}

VAEMorphEngine::~VAEMorphEngine()
{
    // Ensure ONNX session is released before destruction.
    unloadModel();
}

// ── Model Lifecycle ──────────────────────────────────────────────────────────

bool VAEMorphEngine::loadModel(const juce::File& onnxFile)
{
    // Validate file exists before attempting to load.
    if (!onnxFile.existsAsFile())
    {
        DBG("VAEMorphEngine::loadModel — file not found: " + onnxFile.getFullPathName());
        return false;
    }

    // TODO (post-MVP): Create ONNX Runtime InferenceSession:
    //
    //   Ort::SessionOptions opts;
    //   opts.SetIntraOpNumThreads(1);
    //   opts.SetGraphOptimizationLevel(ORT_ENABLE_EXTENDED);
    //
    //   encoderSession_ = std::make_unique<Ort::Session>(
    //       onnxEnv_, onnxFile.getFullPathName().toWideCharPointer(), opts);
    //   decoderSession_ = std::make_unique<Ort::Session>(
    //       onnxEnv_, onnxFile.getFullPathName().toWideCharPointer(), opts);
    //
    //   // Inspect model input/output tensors to determine latent dimensions
    //   latentDims_ = static_cast<int>(
    //       encoderSession_->GetOutputTypeInfo(0)
    //           .GetTensorTypeAndShapeInfo().GetShape().back());
    //
    // For MVP, stub out with a fixed latent dimension of 16.
    latentDims_  = 16;
    modelLoaded_ = true;

    // Runtime warning: this is a stub implementation
    assert(false && "VAEMorphEngine::loadModel — STUB: ONNX Runtime not integrated. Using linear latent interpolation only.");
    DBG("VAEMorphEngine::loadModel — WARNING: STUB IMPLEMENTATION (no ONNX runtime). Using linear latent interpolation only. latentDims="
        + juce::String(latentDims_));
    return true;
}

void VAEMorphEngine::unloadModel()
{
    if (!modelLoaded_)
        return;

    // TODO (post-MVP): Destroy ONNX Runtime sessions here:
    //   encoderSession_.reset();
    //   decoderSession_.reset();

    latentMap_.clear();
    latentDims_  = 0;
    modelLoaded_ = false;

    DBG("VAEMorphEngine::unloadModel — model unloaded");
}

// ── Inference ────────────────────────────────────────────────────────────────

std::vector<float> VAEMorphEngine::encode(const std::vector<float>& parameterSnapshot)
{
    if (!modelLoaded_ || parameterSnapshot.empty())
        return {};

    // TODO (post-MVP): Run encoder sub-graph:
    //
    //   auto memInfo = Ort::MemoryInfo::CreateCpu(
    //       OrtArenaAllocator, OrtMemTypeDefault);
    //
    //   int64_t inputShape[] = { 1, static_cast<int64_t>(parameterSnapshot.size()) };
    //   Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
    //       memInfo,
    //       const_cast<float*>(parameterSnapshot.data()),
    //       parameterSnapshot.size(),
    //       inputShape, 2);
    //
    //   auto outputs = encoderSession_->Run(
    //       Ort::RunOptions{nullptr},
    //       encoderInputNames_.data(), &inputTensor, 1,
    //       encoderOutputNames_.data(), encoderOutputNames_.size());
    //
    //   const float* latentData = outputs[0].GetTensorData<float>();
    //   return std::vector<float>(latentData, latentData + latentDims_);

    // MVP stub: return a zero latent vector of the correct dimensionality.
    return std::vector<float>(static_cast<size_t>(latentDims_), 0.0f);
}

std::vector<float> VAEMorphEngine::decode(const std::vector<float>& latentVector)
{
    if (!modelLoaded_ || latentVector.empty())
        return {};

    // TODO (post-MVP): Run decoder sub-graph (mirror of encode()).
    //   Pass latentVector as input tensor, receive parameter snapshot output.

    // MVP stub: return an empty vector (caller falls back to parameter-space morph).
    return {};
}

std::vector<float> VAEMorphEngine::interpolateLatent(
    const std::vector<float>& latentA,
    const std::vector<float>& latentB,
    float alpha)
{
    // This method is FULLY IMPLEMENTED: linear interpolation in latent space.
    // It works correctly regardless of whether the model is loaded, as long as
    // both vectors are non-empty and have the same dimensionality.

    if (latentA.empty() || latentB.empty())
        return {};

    const size_t dim = std::min(latentA.size(), latentB.size());
    const float  t   = std::max(0.0f, std::min(1.0f, alpha));
    const float  oneMinusT = 1.0f - t;

    std::vector<float> result(dim);
    for (size_t i = 0; i < dim; ++i)
        result[i] = oneMinusT * latentA[i] + t * latentB[i];

    return result;
}

// ── Latent Map ───────────────────────────────────────────────────────────────

void VAEMorphEngine::buildLatentMap(const std::vector<std::vector<float>>& snapshots)
{
    latentMap_.clear();

    if (snapshots.empty())
        return;

    // TODO (post-MVP): Run PCA or t-SNE on the encoded latent vectors to
    // compute 2D projections. For n snapshots:
    //   1. Encode each snapshot: latents[i] = encode(snapshots[i])
    //   2. Compute 2×N PCA projection matrix from latents
    //   3. Project each latent onto the first 2 principal components
    //   4. Normalise projected coordinates to [0,1] pad space
    //   5. Store as LatentMapping entries

    // MVP stub: lay out snapshots on a regular grid covering the pad.
    const int n = static_cast<int>(snapshots.size());

    // Compute a roughly square grid layout
    const int cols = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n)))));
    const int rows = std::max(1, (n + cols - 1) / cols);

    for (int i = 0; i < n; ++i)
    {
        const int col = i % cols;
        const int row = i / cols;

        LatentMapping mapping;
        mapping.padX   = (cols > 1) ? static_cast<float>(col) / static_cast<float>(cols - 1) : 0.5f;
        mapping.padY   = (rows > 1) ? static_cast<float>(row) / static_cast<float>(rows - 1) : 0.5f;
        mapping.latent = encode(snapshots[static_cast<size_t>(i)]);

        latentMap_.push_back(std::move(mapping));
    }

    DBG("VAEMorphEngine::buildLatentMap — mapped " + juce::String(n)
        + " snapshots onto " + juce::String(cols) + "x" + juce::String(rows) + " grid");
}

std::vector<float> VAEMorphEngine::padPositionToLatent(float x, float y) const
{
    if (latentMap_.empty())
        return {};

    // TODO (post-MVP): Bilinear interpolation between the 4 nearest map points
    // in 2D pad space (weighted by inverse squared distance to each anchor).

    // MVP stub: return the latent vector of the nearest stored map point
    // using Euclidean distance in 2D pad space.
    size_t nearestIdx  = 0;
    float  nearestDist = std::numeric_limits<float>::max();

    for (size_t i = 0; i < latentMap_.size(); ++i)
    {
        const float dx = latentMap_[i].padX - x;
        const float dy = latentMap_[i].padY - y;
        const float dist = dx * dx + dy * dy;  // squared distance (no sqrt needed)

        if (dist < nearestDist)
        {
            nearestDist = dist;
            nearestIdx  = i;
        }
    }

    return latentMap_[nearestIdx].latent;
}

} // namespace morphsnap
