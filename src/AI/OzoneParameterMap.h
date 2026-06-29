/*
 * More-Phi — AI/OzoneParameterMap.h
 *
 * Static parameter index table for iZotope Ozone 11 Advanced.
 *
 * !! HOW TO POPULATE !!
 * Run:  python scripts/audit_ozone_params.py --out-map ozone11_param_map.json
 * Then update kOzone11 in OzoneParameterMap.cpp with the real parameter indices.
 * All indices below default to -1 (disabled) until the audit is done.
 *
 * Normalization conventions (all values delivered as VST3 normalized [0..1]):
 *   - Gain dB:       normalizeGain(dB, minDB, maxDB)
 *   - Frequency Hz:  normalizeFreq(hz) using log scale
 *   - Threshold dBFS: normalizeThreshold(dBFS) linear over [-60, 0]
 *   - Width [0..2]:  normalizeWidth(w) → w * 0.5f
 *   - LUFS target:   normalizeLUFS(lufs) over typical mastering range [-24, -6]
 */
#pragma once

#include <juce_core/juce_core.h>
#include <array>

namespace more_phi {

class IParameterBridge;

/** Per-band EQ parameter index bundle for one Ozone EQ band. */
struct OzoneEQBandMap
{
    int freqIdx    = -1;   ///< Frequency parameter index
    int gainIdx    = -1;   ///< Gain (dB) parameter index
    int qIdx       = -1;   ///< Q / Bandwidth parameter index
    int typeIdx    = -1;   ///< Filter type (shelf/peak/etc.) parameter index
    int enabledIdx = -1;   ///< Band enable/bypass parameter index
};

/** Dynamics module parameter indices (single-band / global envelope). */
struct OzoneDynamicsMap
{
    int thresholdIdx = -1;   ///< Threshold (dBFS)
    int ratioIdx     = -1;   ///< Compression ratio
    int attackIdx    = -1;   ///< Attack time (ms)
    int releaseIdx   = -1;   ///< Release time (ms)
};

/** AUDIT-FIX (L4-1, 2026-06-29): per-band compressor parameter indices.
 *  Ozone 11's Dynamics module exposes per-band compression (3 bands matching
 *  the neural model's kNeuralMasteringCompBandCount). Each band has its own
 *  threshold, ratio, attack, release, makeup gain, and knee. When these are
 *  discovered from the hosted plugin, the OzonePlanApplicator can route the
 *  model's 3-band × 6-param compression output instead of collapsing to a
 *  single scalar. Indices default to -1 (not mapped). */
struct OzoneCompBandMap
{
    int thresholdIdx = -1;   ///< Per-band threshold (dBFS)
    int ratioIdx     = -1;   ///< Per-band compression ratio
    int attackIdx    = -1;   ///< Per-band attack time (ms)
    int releaseIdx   = -1;   ///< Per-band release time (ms)
    int makeupIdx    = -1;   ///< Per-band makeup gain (dB)
    int kneeIdx      = -1;   ///< Per-band soft-knee width (dB)
};

/** Imager per-band stereo width indices (sub / low / mid / high). */
struct OzoneImagerMap
{
    std::array<int, 4> widthIdx = { -1, -1, -1, -1 };
};

/** Maximizer output level and true-peak ceiling indices. */
struct OzoneMaximizerMap
{
    int outputLevelIdx = -1;   ///< Target output level (dBFS or LUFS depending on mode)
    int ceilingIdx     = -1;   ///< True Peak ceiling (dBTP)
};

/**
 * Complete Ozone 11 parameter map.
 *
 * Instantiate via OzoneParameterMap::buildForOzone11().
 * All index members default to -1 meaning "not mapped / skip".
 * OzonePlanApplicator skips any parameter whose index is -1.
 */
class OzoneParameterMap
{
public:
    static constexpr int kEQBands = 8;

    std::array<OzoneEQBandMap,  kEQBands> eq       {};
    OzoneDynamicsMap                       dynamics {};
    // AUDIT-FIX (L4-1, 2026-06-29): per-band compressor map (3 bands).
    // Populated by buildFromHostedPlugin when "Comp Band N" parameter names are
    // found. When all per-band indices remain -1, applyDynamics() falls back to
    // the scalar compressionNeed → global envelope mapping (current behavior).
    static constexpr int kCompBands = 3;
    std::array<OzoneCompBandMap, kCompBands> compBands {};
    int compEnableIdx = -1;  ///< Per-band compressor enable/bypass toggle
    OzoneImagerMap                         imager   {};
    OzoneMaximizerMap                      maximizer{};

    // ── Factory ───────────────────────────────────────────────────────────────

    /**
     * Construct the map populated with Ozone 11 Advanced parameter indices.
     * Indices must be filled from the audit output (ozone11_param_map.json).
     * Until the audit is run, all indices remain -1 and no parameters are sent.
     */
    static OzoneParameterMap buildForOzone11();

    /**
     * Construct the map by discovering Ozone 11 parameter indices from the
     * currently hosted plugin's exposed parameter names.
     */
    static OzoneParameterMap buildFromHostedPlugin(const IParameterBridge& bridge);

    /**
     * AUDIT-FIX-5: return true if at least one parameter index is mapped (>= 0).
     * Used by OzonePlanApplicator to fail LOUD (log + signal) instead of silently
     * no-op'ing when the map is still all-stubs (the buildForOzone11() factory
     * default, before audit_ozone_parameters runs against a hosted plugin).
     */
    [[nodiscard]] bool hasAnyMapping() const noexcept;

    /** AUDIT-FIX (L4-1, 2026-06-29): returns true if at least one per-band
     *  compressor parameter is mapped (>= 0). When true, applyDynamics() uses
     *  the full 3-band × 6-param compression path instead of the scalar fallback. */
    [[nodiscard]] bool hasPerBandCompMapping() const noexcept
    {
        for (int b = 0; b < kCompBands; ++b)
            if (compBands[static_cast<std::size_t>(b)].thresholdIdx >= 0)
                return true;
        return false;
    }

    /**
     * AUDIT (W1, 2026-06-25): count of mapped parameter indices (>= 0) across
     * all modules — EQ bands (freq/gain/q/type/enabled), dynamics (4), imager
     * (4 width), maximizer (2). Exposed via the sonicmaster_decision MCP tool so
     * a caller can tell WHY a plan applied zero parameters (unmapped hosted
     * plugin) vs. the plan genuinely contained no changes. Stable across modules:
     * a "ready" map has >= 1; a fully-mapped Ozone instance approaches the slot
     * ceiling (8*5 + 4 + 4 + 2 = 50).
     */
    [[nodiscard]] int mappedSlotCount() const noexcept;

    /**
     * Return true if the given plugin display name appears to be iZotope Ozone 11.
     * Checked case-insensitively; matches "Ozone 11" or "iZotope Ozone 11".
     */
    static bool isOzone11(const juce::String& pluginName) noexcept;

    // ── Normalization helpers ─────────────────────────────────────────────────

    /** Map gain in dB to VST3 normalized [0..1].
     *  Default range [-18 dB, +18 dB] covering typical mastering EQ gain. */
    static float normalizeGain(float gainDB,
                               float minDB = -18.0f,
                               float maxDB =  18.0f) noexcept;

    /** Map frequency in Hz to VST3 normalized [0..1] using a log scale. */
    static float normalizeFreq(float hz,
                               float minHz =    20.0f,
                               float maxHz = 20000.0f) noexcept;

    /** Map threshold in dBFS to VST3 normalized [0..1].
     *  Range [-60, 0] dBFS. 0 dBFS = most aggressive threshold (full-scale),
     *  -60 dBFS = barely compresses. AUDIT-FIX-10: prior comment inverted this. */
    static float normalizeThreshold(float dBFS,
                                    float minDBFS = -60.0f,
                                    float maxDBFS =   0.0f) noexcept;

    /** Map stereo width [0..2] to VST3 normalized [0..1].
     *  0 = fully mono, 1 = unity, 2 = double width. */
    static float normalizeWidth(float width) noexcept;

    /** Map an EQ band Q (bandwidth) value to VST3 normalized [0..1].
     *
     *  Linear over [0.1, 8.0]. The range matches this codebase's own Q
     *  documentation — PluginSemanticMapper advertises Q as "0.3 to 8.0" and
     *  EQParameterTranslator clamps Q to a narrow mastering range with a 0.1
     *  floor — and Q is exposed by hosted plugins as linear in normalized
     *  VST3 space, so a log curve (previously mis-applied via normalizeFreq)
     *  distorts the value. Do NOT reuse normalizeFreq for Q. */
    static float normalizeQ(float q, float minQ = 0.1f, float maxQ = 8.0f) noexcept;

    /** Map a target LUFS value to VST3 normalized [0..1].
     *  Range [-24, -6] LUFS covering typical mastering targets. */
    static float normalizeLUFS(float lufs,
                               float minLUFS = -24.0f,
                               float maxLUFS =  -6.0f) noexcept;

    /** Map true-peak ceiling in dBTP to VST3 normalized [0..1].
     *  Range [-3, 0] dBTP. */
    static float normalizeCeiling(float dBTP,
                                  float minDBTP = -3.0f,
                                  float maxDBTP =  0.0f) noexcept;

    // ── Filter type encoding ──────────────────────────────────────────────────

    /**
     * Map a filter type string from eqPrescriptionJSON to a VST3 normalized value.
     * Ozone 11 filter type indices (normalized): peak≈0.25, lowshelf≈0.0, highshelf≈0.5
     * These values are estimated — verify with the parameter audit.
     */
    static float encodeFilterType(const juce::String& typeName) noexcept;
};

} // namespace more_phi
