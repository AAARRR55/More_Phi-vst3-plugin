/*
 * MorphSnap — AI/Dataset/PluginChainEngine.h
 * Sequential plugin chain engine for dataset generation.
 * Supports multiple plugin types: EQ, Dynamics, Mastering chains, etc.
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <memory>

namespace morphsnap {

/**
 * Types of plugin chains for different processing scenarios.
 */
enum class ChainType {
    EQOnly,          // Single EQ plugin
    DynamicsOnly,    // Single dynamics plugin (comp/limiter)
    Mastering,       // EQ -> Comp -> Limiter
    Mixing,          // Multiple mixing plugins
    Creative,        // Creative effects chain
    Custom           // User-defined chain
};

/**
 * Configuration for a single plugin slot in the chain.
 */
struct PluginSlot {
    juce::PluginDescription description;
    std::vector<float> parameters;  // Initial normalized parameter values
    bool bypass = false;
    int settleTimeMs = 50;          // Time to wait after parameter changes

    /** JSON serialization */
    static PluginSlot fromJson(const nlohmann::json& j);
    nlohmann::json toJson() const;
};

/**
 * Full chain configuration.
 */
struct ChainConfig {
    ChainType type = ChainType::Custom;
    juce::Array<PluginSlot> plugins;
    double sampleRate = 48000.0;
    int blockSize = 512;
    int numChannels = 2;

    /** JSON serialization */
    static ChainConfig fromJson(const nlohmann::json& j);
    nlohmann::json toJson() const;

    /** Factory: Create a standard mastering chain configuration */
    static ChainConfig createMasteringChain();

    /** Factory: Create an EQ-only chain configuration */
    static ChainConfig createEQChain();

    /** Factory: Create a dynamics-only chain configuration */
    static ChainConfig createDynamicsChain();
};

/**
 * Parameter mapping for global parameter index to plugin-local index.
 */
struct ParameterMapping {
    int pluginIndex;           // Index in the plugin chain
    int parameterIndex;        // Local parameter index within the plugin
    juce::String parameterName;
    juce::String parameterId;
    float normalizedValue = 0.0f;
    float rawValue = 0.0f;
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float defaultValue = 0.0f;
    juce::String label;        // Unit label (dB, Hz, %, etc.)
    bool isDiscrete = false;
    bool isBoolean = false;
};

/**
 * Engine for managing sequential plugin chains.
 * Extends the single-plugin hosting concept to support multiple plugins in series.
 */
class PluginChainEngine
{
public:
    PluginChainEngine();
    ~PluginChainEngine();

    // Prevent copying
    PluginChainEngine(const PluginChainEngine&) = delete;
    PluginChainEngine& operator=(const PluginChainEngine&) = delete;

    // ── Chain Management ─────────────────────────────────────────────────────

    /**
     * Load a plugin chain from configuration.
     * @param config The chain configuration specifying plugins and settings.
     * @return true if all plugins loaded successfully.
     */
    bool loadChain(const ChainConfig& config);

    /**
     * Unload the current chain and release all plugin instances.
     */
    void unloadChain();

    /**
     * Check if a chain is currently loaded.
     */
    bool isChainLoaded() const { return !plugins_.isEmpty(); }

    /**
     * Get the current chain configuration.
     */
    const ChainConfig& getCurrentConfig() const { return currentConfig_; }

    // ── Audio Processing ─────────────────────────────────────────────────────

    /**
     * Prepare the chain for audio processing.
     * Must be called before processBlock().
     */
    void prepare(double sampleRate, int blockSize, int numChannels);

    /**
     * Process audio through the entire plugin chain.
     * Audio is processed sequentially through each plugin.
     */
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);

    /**
     * Release resources held by all plugins.
     */
    void releaseResources();

    // ── Parameter Management ─────────────────────────────────────────────────

    /**
     * Get the total number of parameters across all plugins in the chain.
     */
    int getTotalParameterCount() const;

    /**
     * Get all parameter mappings for the entire chain.
     */
    std::vector<ParameterMapping> getAllParameters() const;

    /**
     * Set a parameter by global index (normalized 0-1).
     * @param globalIndex The global parameter index across all plugins.
     * @param value Normalized value in range [0, 1].
     */
    void setParameterNormalized(int globalIndex, float value);

    /**
     * Get a parameter value by global index (normalized 0-1).
     * @param globalIndex The global parameter index across all plugins.
     * @return Normalized value in range [0, 1].
     */
    float getParameterNormalized(int globalIndex) const;

    /**
     * Apply a complete set of normalized parameters to all plugins.
     * @param normalizedValues Vector of normalized values (must match getTotalParameterCount()).
     */
    void applyParameterSet(const std::vector<float>& normalizedValues);

    /**
     * Capture the current parameter state from all plugins.
     * @return Vector of normalized parameter values.
     */
    std::vector<float> captureParameterSet() const;

    /**
     * Set parameters for a specific plugin by chain index.
     * @param pluginIndex Index of the plugin in the chain.
     * @param normalizedValues Vector of normalized values for that plugin.
     */
    void setPluginParameters(int pluginIndex, const std::vector<float>& normalizedValues);

    /**
     * Get parameters for a specific plugin by chain index.
     * @param pluginIndex Index of the plugin in the chain.
     * @return Vector of normalized parameter values for that plugin.
     */
    std::vector<float> getPluginParameters(int pluginIndex) const;

    // ── Plugin Settle Time ───────────────────────────────────────────────────

    /**
     * Set the default settle time in milliseconds.
     * This is the time waited after parameter changes for plugins to stabilize.
     */
    void setSettleTimeMs(int ms) { defaultSettleTimeMs_ = ms; }

    /**
     * Get the default settle time in milliseconds.
     */
    int getSettleTimeMs() const { return defaultSettleTimeMs_; }

    /**
     * Wait for plugins to settle after parameter changes.
     * Processes silence through the chain for the settle duration.
     */
    void waitForSettle();

    /**
     * Wait for a specific settle time.
     * @param settleTimeMs Time to wait in milliseconds.
     */
    void waitForSettle(int settleTimeMs);

    // ── Factory Methods ──────────────────────────────────────────────────────

    /**
     * Create a standard mastering chain configuration.
     * Typically: EQ -> Compressor -> Limiter
     */
    static ChainConfig createMasteringChain();

    /**
     * Create an EQ-only chain configuration.
     */
    static ChainConfig createEQChain();

    /**
     * Create a dynamics-only chain configuration.
     */
    static ChainConfig createDynamicsChain();

    // ── Plugin Access ────────────────────────────────────────────────────────

    /**
     * Get a plugin instance by chain index.
     * @param index Index of the plugin in the chain.
     * @return Pointer to the plugin instance, or nullptr if index is invalid.
     */
    juce::AudioPluginInstance* getPlugin(int index);

    /**
     * Get a const plugin instance by chain index.
     */
    const juce::AudioPluginInstance* getPlugin(int index) const;

    /**
     * Get the number of plugins in the chain.
     */
    int getPluginCount() const { return plugins_.size(); }

    /**
     * Get the format manager for plugin loading.
     */
    juce::AudioPluginFormatManager& getFormatManager() { return formatManager_; }

    /**
     * Get the known plugins list.
     */
    juce::KnownPluginList& getKnownPlugins() { return knownPlugins_; }

    // ── Bypass Control ───────────────────────────────────────────────────────

    /**
     * Set bypass state for a specific plugin.
     * @param pluginIndex Index of the plugin in the chain.
     * @param bypass True to bypass, false to enable.
     */
    void setPluginBypass(int pluginIndex, bool bypass);

    /**
     * Get bypass state for a specific plugin.
     */
    bool getPluginBypass(int pluginIndex) const;

    /**
     * Set bypass state for all plugins.
     */
    void setAllBypass(bool bypass);

private:
    /**
     * Build the parameter index map for global parameter access.
     * Called after chain loading to map global indices to (plugin, local) pairs.
     */
    void buildParameterIndexMap();

    /**
     * Load a single plugin and add it to the chain.
     * @param slot The plugin slot configuration.
     * @return true if the plugin loaded successfully.
     */
    bool loadSinglePlugin(const PluginSlot& slot);

    /**
     * Process silence through the chain for a specified duration.
     * Used for settle time and latency compensation.
     */
    void processSilence(int numSamples);

    juce::AudioPluginFormatManager formatManager_;
    juce::KnownPluginList knownPlugins_;
    juce::OwnedArray<juce::AudioPluginInstance> plugins_;
    ChainConfig currentConfig_;

    int defaultSettleTimeMs_ = 50;
    double currentSampleRate_ = 48000.0;
    int currentBlockSize_ = 512;
    int currentNumChannels_ = 2;
    bool isPrepared_ = false;

    // Maps global parameter index to (pluginIndex, localParamIndex)
    std::vector<std::pair<int, int>> parameterIndexMap_;

    // Bypass states for each plugin
    juce::Array<bool> bypassStates_;

    // Settle time per plugin
    juce::Array<int> pluginSettleTimes_;
};

} // namespace morphsnap
