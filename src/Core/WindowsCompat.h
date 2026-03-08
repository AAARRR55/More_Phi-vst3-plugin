/*
 * MorphSnap - Core/WindowsCompat.h
 * Windows compatibility macros and workarounds for JUCE conflicts.
 */
#pragma once

#if JUCE_WINDOWS

// Prevent Windows macros from interfering with JUCE
#ifdef WIN32_LEAN_AND_MEAN
#undef WIN32_LEAN_AND_MEAN
#endif
#define WIN32_LEAN_AND_MEAN

#ifdef NOMINMAX
#undef NOMINMAX
#endif
#define NOMINMAX

// Common Windows macro conflicts
#ifdef small
#undef small
#endif

#ifdef max
#undef max
#endif

#ifdef min
#undef min
#endif

#ifdef GetMessage
#undef GetMessage
#endif

#ifdef CreateWindow
#undef CreateWindow
#endif

#ifdef DeleteFile
#undef DeleteFile
#endif

#ifdef CopyFile
#undef CopyFile
#endif

#ifdef MoveFile
#undef MoveFile
#endif

#ifdef LoadImage
#undef LoadImage
#endif

#ifdef PlaySound
#undef PlaySound
#endif

#endif // JUCE_WINDOWS