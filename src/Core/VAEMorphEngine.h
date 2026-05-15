/*
 * More-Phi — Core/VAEMorphEngine.h
 *
 * Variational Autoencoder morph engine.
 *
 * IMPORTANT: This runs on the MESSAGE THREAD only, not the audio thread.
 * It provides latent space coordinates that the MorphProcessor can use
 * for navigation, but inference is too expensive for real-time.
 *
 * For V2 MVP, this is a stub that establishes the API.
 * Real ONNX Runtime integration will be added post-MVP.
 *
 * Architecture:
 *   The VAE has an encoder network (parameter snapshot → latent vector)
 *   and a decoder network (latent vector → parameter snapshot). The latent
 *   space is typically low-dimensional (8–32 dims) compared to the full
 *   parameter count (up to 2048).
 *
 *   buildLatentMap() uses PCA (or t-SNE post-MVP) to project the latent
 *   space onto the 2D morph pad, enabling gestural navigation of a
 *   continuously interpolated latent space.
 *
 * Thread safety:
 *   All public methods must be called from the message thread.
 *   No audio-thread access. The VAEMorphEngine does not hold any
 *   audio-thread locks.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include <cstddef>

namespace more_phi {

/**
 * VAEMorphEngine
 *
 * Message-thread-only VAE engine for latent space navigation.
 *
 * V2 MVP stub: All inference methods return empty vectors or reasonable
 * defaults. interpolateLatent() is fully implemented as a linear blend.
 * loadModel() / unloadModel() are stubs that toggle modelLoaded_.
 */
class VAEMorphEngine
{
public:
    enum class BackendMode
    {
        Stub = 0,      // Safe non-inference mode (default)
        OnnxRuntime    // Reserved for future integration
    };

    VAEMorphEngine();
    ~VAEMorphEngine();

    VAEMorphEngine(const VAEMorphEngine&)            = delete;
    VAEMorphEngine& operator=(const VAEMorphEngine&) = delete;

    // ─── Model lifecycle (message thread) ────────────────────────────────

    /**
     * Load an ONNX model file (message thread).
     *
     * V2 MVP stub: validates that the file exists and sets modelLoaded_.
     * Post-MVP: creates an ONNX Runtime InferenceSession and verifies
     * input/output tensor shapes match the expected VAE topology.
     *
     * @param onnxFile  Path to a .onnx model file
     * @return          true if the model was loaded successfully
     */
    bool loadModel(const juce::File& onnxFile);

    /**
     * Unload the current model and free all runtime resources.
     * Safe to call when no model is loaded.
     */
    void unloadModel();

    /**
     * Whether a model is currently loaded and ready for inference.
     */
    [[nodiscard]] bool isModelLoaded() const noexcept { return modelLoaded_; }
    [[nodiscard]] BackendMode getBackendMode() const noexcept { return backendMode_; }
    [[nodiscard]] juce::String getBackendStatus() const;

    // ─── Inference (message thread, may be slow) ─────────────────────────

    /**
     * Encode a parameter snapshot into latent space (message thread).
     *
     * V2 MVP stub: returns an empty vector.
     * Post-MVP: runs the encoder sub-graph of the ONNX model.
     *
     * @param parameterSnapshot  Normalised parameter vector ([0,1] per param)
     * @return                   Latent vector of size getLatentDimensions(),
     *                           or empty vector if no model is loaded.
     */
    std::vector<float> encode(const std::vector<float>& parameterSnapshot);

    /**
     * Decode a latent vector back to parameter space (message thread).
     *
     * V2 MVP stub: returns an empty vector.
     * Post-MVP: runs the decoder sub-graph of the ONNX model.
     *
     * @param latentVector  Latent coordinates of size getLatentDimensions()
     * @return              Normalised parameter vector, or empty if no model.
     */
    std::vector<float> decode(const std::vector<float>& latentVector);

    /**
     * Interpolate in latent space between two parameter snapshots.
     *
     * This is FULLY IMPLEMENTED in the MVP: it encodes both snapshots,
     * linearly interpolates the latent vectors, and decodes the result.
     * When no model is loaded, it falls back to direct linear interpolation
     * in parameter space.
     *
     * @param latentA  Latent vector for snapshot A (from encode())
     * @param latentB  Latent vector for snapshot B (from encode())
     * @param alpha    Blend factor: 0 = pure A, 1 = pure B
     * @return         Interpolated latent vector
     */
    std::vector<float> interpolateLatent(const std::vector<float>& latentA,
                                          const std::vector<float>& latentB,
                                          float alpha);

    /**
     * Return the dimensionality of the latent space.
     * Returns 0 if no model is loaded.
     */
    [[nodiscard]] int getLatentDimensions() const noexcept { return latentDims_; }

    // ─── Latent map (2D pad → latent space navigation) ───────────────────

    /**
     * 2D pad position paired with its corresponding latent vector.
     * Built by buildLatentMap() using PCA projection.
     */
    struct LatentMapping
    {
        float padX;                 ///< X position in [0,1] on the morph pad
        float padY;                 ///< Y position in [0,1] on the morph pad
        std::vector<float> latent;  ///< Full latent vector at this pad position
    };

    /**
     * Build a 2D map of the latent space from a set of encoded snapshots.
     *
     * V2 MVP stub: stores snapshots as evenly-spaced points on the pad
     * (no real PCA). Post-MVP: runs PCA/t-SNE on the encoded latent
     * vectors to compute meaningful 2D projections.
     *
     * @param snapshots  Raw parameter snapshots to encode and project
     */
    void buildLatentMap(const std::vector<std::vector<float>>& snapshots);

    /**
     * Map a 2D morph pad position to a latent vector for continuous
     * navigation of the latent space.
     *
     * V2 MVP stub: returns an empty vector when no map is built, or the
     * nearest stored latent vector by Euclidean distance in 2D pad space.
     *
     * Post-MVP: bilinear interpolation between the 4 nearest map points.
     *
     * @param x  Pad X position in [0,1]
     * @param y  Pad Y position in [0,1]
     * @return   Latent vector at this pad position
     */
    std::vector<float> padPositionToLatent(float x, float y) const;

private:
    bool modelLoaded_ = false;
    int  latentDims_  = 0;
    BackendMode backendMode_ = BackendMode::Stub;
    juce::String backendStatus_ = "stub backend active (no model loaded)";

    // Stored latent map (built by buildLatentMap)
    std::vector<LatentMapping> latentMap_;

    // TODO (post-MVP): Ort::Env onnxEnv_;
    //                  Ort::Session encoderSession_;
    //                  Ort::Session decoderSession_;
    //                  std::vector<const char*> encoderInputNames_;
    //                  std::vector<const char*> decoderOutputNames_;
};

} // namespace more_phi
