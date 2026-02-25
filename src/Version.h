/*
 * MorphSnap — Version.h
 * Version tracking and changelog for MorphSnap VST plugin.
 */
#pragma once

#include <string>

namespace morphsnap {

// Current version - matching SnappySnap video evidence (3.3.0)
constexpr int VERSION_MAJOR = 3;
constexpr int VERSION_MINOR = 3;
constexpr int VERSION_PATCH = 0;

constexpr const char* VERSION_STRING = "3.3.0";
constexpr const char* VERSION_CODENAME = "Synthesizer Edition";

// Build info
constexpr const char* BUILD_DATE = __DATE__;
constexpr const char* BUILD_TIME = __TIME__;

// Feature flags for this version
struct VersionFeatures
{
    // Core features
    static constexpr bool SNAPSHOT_MORPHING = true;
    static constexpr bool PHYSICS_MODES = true;
    static constexpr bool GENETIC_BREEDING = true;
    
    // AI Integration (v2.0+)
    static constexpr bool MCP_INTEGRATION = true;
    static constexpr bool AI_TEACHER_MODE = true;
    
    // Learn Mode (v3.0+)
    static constexpr bool LEARN_MODE = true;
    static constexpr bool PARAMETER_CLASSIFICATION = true;
    static constexpr bool TOKEN_OPTIMIZATION = true;
    
    // Synthesizer Support (v3.3+)
    static constexpr bool DISCRETE_PARAMETER_HANDLING = true;
    static constexpr bool MORPH_COMPATIBILITY_ANALYSIS = true;
    static constexpr bool SMART_INTERPOLATION = true;
};

// Version history and changelog
struct ChangelogEntry
{
    const char* version;
    const char* date;
    const char* changes;
};

inline const ChangelogEntry CHANGELOG[] = {
    {
        "3.3.0",
        "February 2026",
        "Synthesizer Edition\n"
        "- NEW: Learn Mode with parameter importance tracking\n"
        "- NEW: Discrete parameter detection and handling\n"
        "- NEW: Morph compatibility analysis for smooth transitions\n"
        "- NEW: Token optimization and cost tracking\n"
        "- NEW: AI Teacher Mode with parameter explanations\n"
        "- NEW: Intermediate snapshot suggestions\n"
        "- IMPROVED: Synthesizer support with discrete param handling\n"
        "- IMPROVED: Parameter reduction (Serum: 2623 → 39 params)\n"
        "- IMPROVED: Multi-instance coordination\n"
    },
    {
        "3.2.0",
        "January 2026",
        "Performance Update\n"
        "- NEW: SIMD-optimized interpolation (AVX2/SSE)\n"
        "- NEW: Lock-free command queues\n"
        "- IMPROVED: CPU usage reduced by 40%\n"
        "- IMPROVED: Reduced audio thread blocking\n"
    },
    {
        "3.1.0",
        "December 2025",
        "MCP Enhancement\n"
        "- NEW: Native MCP (Model Context Protocol) support\n"
        "- NEW: Claude Desktop integration\n"
        "- NEW: AI parameter control via JSON-RPC\n"
        "- NEW: Batch parameter updates\n"
    },
    {
        "3.0.0",
        "November 2025",
        "AI Edition\n"
        "- NEW: AI Bridge for LLM integration\n"
        "- NEW: Conversational parameter control\n"
        "- NEW: Intelligent preset suggestions\n"
        "- IMPROVED: Enhanced preset breeding\n"
    },
    {
        "2.1.0",
        "October 2025",
        "Physics Update\n"
        "- NEW: Elastic physics mode (spring-damper)\n"
        "- NEW: Drift mode with Perlin noise\n"
        "- NEW: Multiple physics presets\n"
        "- IMPROVED: Smoother morphing curves\n"
    },
    {
        "2.0.0",
        "September 2025",
        "Major Release\n"
        "- NEW: 12-slot snapshot system\n"
        "- NEW: XY pad morphing (Snap Void)\n"
        "- NEW: 1D fader morphing\n"
        "- NEW: Genetic breeding algorithm\n"
        "- NEW: MIDI control support\n"
    },
    {
        "1.0.0",
        "August 2025",
        "Initial Release\n"
        "- Initial VST3/AU plugin host\n"
        "- 2-snapshot basic morphing\n"
        "- VST3/AU plugin loading\n"
    }
};

inline constexpr int CHANGELOG_COUNT = sizeof(CHANGELOG) / sizeof(CHANGELOG[0]);

// Version comparison
inline bool isVersionAtLeast(int major, int minor, int patch = 0)
{
    if (VERSION_MAJOR != major)
        return VERSION_MAJOR > major;
    if (VERSION_MINOR != minor)
        return VERSION_MINOR > minor;
    return VERSION_PATCH >= patch;
}

// Get version string with build info
inline std::string getFullVersionString()
{
    return std::string(VERSION_STRING) + " (" + VERSION_CODENAME + ") - Built " + BUILD_DATE;
}

// Edition detection
enum class Edition
{
    LE,         // Free/Light Edition
    Full,       // Full paid version
    Trial       // Time-limited trial
};

inline Edition getEdition()
{
    // This would be determined by license check in real implementation
    return Edition::Full;
}

inline const char* getEditionString()
{
    switch (getEdition())
    {
        case Edition::LE: return "LE";
        case Edition::Full: return "Full";
        case Edition::Trial: return "Trial";
    }
    return "Unknown";
}

} // namespace morphsnap
