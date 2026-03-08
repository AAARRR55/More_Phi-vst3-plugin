/*
 * MorphSnap - Core/WindowsCompat.h
 * Windows compatibility macros and workarounds for JUCE conflicts.
 *
 * IMPORTANT: Include this BEFORE any JUCE headers to ensure Windows macros
 * don't interfere with C++ syntax. CMakeLists.txt sets WIN32_LEAN_AND_MEAN
 * and NOMINMAX globally, but this header provides additional safety.
 */
#pragma once

#ifdef _WIN32

// Undefine Windows macros that conflict with C++ and JUCE
// These may have been defined by Windows.h included indirectly

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

// Prevent bcrypt.h conflicts with C++ keywords
#ifdef interface
#undef interface
#endif

// Additional Windows macro conflicts
#ifdef CONST
#undef CONST
#endif

#ifdef CALLBACK
#undef CALLBACK
#endif

#ifdef WINAPI
#undef WINAPI
#endif

// Ensure WIN32_LEAN_AND_MEAN and NOMINMAX are set for any late Windows.h includes
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#endif // _WIN32