// src/Plugin/OnnxRuntimeDelayLoadHook.cpp
//
// AUDIT (2026-06-26): forces the bundled onnxruntime.dll (v1.22.1, ORT C-API
// version 22) to load from the PLUGIN'S OWN directory, defeating the Windows
// DLL search-order shadow that otherwise resolves onnxruntime.dll against
// C:\Windows\System32 (Microsoft's old v1.17.1 "Windows AI Runtime",
// os-germanium build — API [1,17] only).
//
// SYMPTOM: FL Studio reports "There was a problem opening the plugin MorePhi
// for an unknown reason." ORT's stderr prints:
//     "The requested API version [22] is not available, only API versions
//      [1, 17] are supported in this build. Current ORT Version is: 1.17.1"
// and GetPluginFactory() returns a processor whose first ORT call fails → FL
// marks the load as failed.
//
// ROOT CAUSE: CMakeLists links `onnxruntime.lib` (the import lib), so the
// plugin carries a DIRECT import of onnxruntime.dll (Ordinal 1 = OrtGetApi).
// The VST3 SDK host loads the module with plain LoadLibraryW(path) — NO
// altered search path, NO SetDllDirectory, NO SetDefaultDllDirectories. The
// Windows loader therefore applies the default search order, in which
// %SystemRoot%\System32 is searched BEFORE the directory the loaded module
// lives in. C:\Windows\System32\onnxruntime.dll (v1.17.1) is found first and
// shadows the v1.22.1 DLL sitting right beside MorePhi.vst3. A .local
// redirection file is NOT honored by VST3 hosts (they don't call LoadLibrary
// with the LOAD_LIBRARY_* flags that enable dot-local), so it doesn't fix this.
//
// FIX: convert the ORT import to DELAY-LOAD and install the delayimp notify
// hook (link /DELAYLOAD:onnxruntime.dll + link delayimp.lib). The hook fires
// on first ORT call, resolves the DLL path from THIS MODULE's location via
// GetModuleFileName + PathFindFileName, and loads it with
// LOAD_WITH_ALTERED_SEARCH_PATH (0x8) so its OWN deps (also bundled beside the
// plugin — onnxruntime_providers_shared.dll) resolve from the same directory
// rather than System32. This is the same pattern iZotope/Native Instruments
// use to bundle runtime dependencies with VST plugins on Windows.
//
// GATING: this TU only compiles on Windows AND when the build delay-loads ORT
// (CMakeLists sets MOREPHI_DELAYLOAD_ORT). On Linux/macOS, onnxruntime is
// resolved via RPATH and no hook is needed.
#if defined(_WIN32) && defined(MOREPHI_DELAYLOAD_ORT)

// delayimp.h declares __pfnDliNotifyHook2 as a `const PfnDliHook`. To assign
// our own hook at link time it must be writable — this macro drops the const
// (the standard, documented way to provide a custom delay-load notify hook).
#define DELAYIMP_INSECURE_WRITABLE_HOOKS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <delayimp.h>      // DelayLoadInfo, __pfnDliNotifyHook2, dliNotePreLoadLibrary
#include <string>

namespace more_phi {
namespace onnx_delayload {

namespace {

// Returns the directory containing THIS module (the .vst3 binary), with a
// trailing backslash. Falls back to the host process directory on the
// (impossible) failure of GetModuleFileName.
std::wstring moduleDirectory()
{
    wchar_t path[MAX_PATH] = {};
    // NULL handle = querying the EXE; for the DLL we want hinst of this TU,
    // which is what GetModuleHandleEx gives us from a function pointer.
    HMODULE hSelf = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&moduleDirectory),
        &hSelf);
    DWORD len = GetModuleFileNameW(hSelf, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return std::wstring{};

    // Strip the filename, keep the directory (with trailing separator).
    std::wstring s{path, len};
    const auto lastSlash = s.find_last_of(L"\\/");
    if (lastSlash == std::wstring::npos)
        return std::wstring{};
    return s.substr(0, lastSlash + 1);
}

// Full path to the bundled onnxruntime.dll beside this module, or empty if the
// directory could not be resolved (in which case the hook lets the default
// loader proceed — same behavior as before this fix).
std::wstring bundledOnnxPath()
{
    const auto dir = moduleDirectory();
    if (dir.empty())
        return {};
    return dir + L"onnxruntime.dll";
}

} // namespace

} // namespace onnx_delayload
} // namespace more_phi

// ── delayimp notify hook ────────────────────────────────────────────────────
//
// delayimp.h declares `extern const PfnDliHook __pfnDliNotifyHook2;` — a
// function POINTER we must ASSIGN, not a function we redefine. (We made it
// writable via DELAYIMP_INSECURE_WRITABLE_HOOKS above.) The hook fires on each
// delayimp notification; we intercept dliNotePreLoadLibrary (notify=1) for
// onnxruntime.dll ONLY and return a pre-loaded HMODULE so delayimp uses OUR
// handle instead of its own LoadLibrary call (which would hit System32's
// 1.17.1). Returning nullptr for everything else lets delayimp's default
// proceed. See the in-function comment for why dliStartProcessing is the wrong
// notification to handle.
static FARPROC WINAPI onnxDelayLoadNotify(unsigned dliNotify, PDelayLoadInfo pdli)
{
    // Only intercept onnxruntime.dll. Other delay-loaded libs get the default.
    const char* dllName = (pdli && pdli->szDll) ? pdli->szDll : nullptr;
    if (dllName == nullptr || _stricmp(dllName, "onnxruntime.dll") != 0)
        return nullptr;

    // dliNotePreLoadLibrary (notify=1): delayimp is about to call LoadLibrary.
    // This is the CORRECT notification to override with our own HMODULE. (Do
    // NOT handle dliStartProcessing — returning non-null there bypasses
    // GetProcAddress entirely, which breaks Ordinal-1 resolution → error 1114.)
    if (dliNotify != dliNotePreLoadLibrary)
        return nullptr;

    const auto path = more_phi::onnx_delayload::bundledOnnxPath();
    if (path.empty())
        return nullptr; // couldn't resolve module dir — fall back to default

    // LOAD_WITH_ALTERED_SEARCH_PATH (0x8): the loader uses `path`'s directory
    // as the first search dir for the DLL's own dependencies. This ensures
    // onnxruntime_providers_shared.dll (also bundled beside the plugin) is
    // resolved correctly rather than shadowed by System32's copy.
    HMODULE h = LoadLibraryExW(path.c_str(), nullptr,
                               LOAD_WITH_ALTERED_SEARCH_PATH);
    if (h == nullptr)
        return nullptr; // let delayimp surface the error normally

    // Return the HMODULE as a FARPROC — delayimp stores it in
    // DelayLoadInfo.hmodCur and proceeds to GetProcAddress(Ordinal 1) on its
    // own, which is what we want.
    return reinterpret_cast<FARPROC>(h);
}

// Assign our hook to the global pointer delayimp consults. Static-init order:
// this runs before main/entry, well before any ORT call, so the hook is in
// place when the first OrtGetApi thunk fires. With DELAYIMP_INSECURE_WRITABLE_HOOKS
// defined, delayimp.h declares `extern "C" PfnDliHook __pfnDliNotifyHook2;`
// (non-const), so the definition here must match — no const.
//
// RETENTION: MSVC's /OPT:REF strips this TU unless the symbol is explicitly
// referenced. CMakeLists passes /INCLUDE:__pfnDliNotifyHook2 to the linker to
// force-retain it (same technique as the model-data anchor). The pragma below
// is belt-and-suspenders for any consumer that forgets the /INCLUDE flag.
#pragma comment(linker, "/INCLUDE:__pfnDliNotifyHook2")
extern "C" PfnDliHook __pfnDliNotifyHook2 = onnxDelayLoadNotify;

#endif // _WIN32 && MOREPHI_DELAYLOAD_ORT
