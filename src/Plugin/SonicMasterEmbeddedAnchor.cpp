// src/Plugin/SonicMasterEmbeddedAnchor.cpp
//
// AUDIT (2026-06-26): link anchor for the embedded SonicMaster model.
//
// Problem: JUCE's BinaryData stores the model + contract as `static const
// unsigned char[]` arrays (BinaryData1.cpp) referenced only through the
// getNamedResource lookup table (BinaryData.cpp). The MSVC linker treats the
// data arrays as unreferenced and strips them when MorePhiSonicMasterModel.lib
// is linked into the .vst3 — so the plugin shipped at 8.4 MB with the resource
// NAME present but ZERO model bytes, and BinaryData::getNamedResource returned
// nullptr at runtime, silently falling back to HTTP even in an ONNX-enabled
// build. (WHOLEARCHIVE was tried but collides with the separate MorePhiFonts
// BinaryData namespace on `originalFilenames`. This anchor is the surgical fix.)
//
// Fix: take the address of each named-resource pointer in a TU that the plugin
// links. The compiler must emit a relocation against the data symbols, so the
// linker keeps them. The pointers are Volatile-evaluated at startup into a
// volatile sink so the optimizer can't elide the reference. No runtime cost:
// this runs once at static init and the sink is never read.
#include "BinaryData.h"

namespace more_phi {
namespace sonicmaster_embedded_anchor {

// Volatile sink — the optimizer cannot prove this is dead, so the reloc stays.
[[maybe_unused]] volatile const void* g_modelDataAnchor = nullptr;
[[maybe_unused]] volatile const void* g_contractDataAnchor = nullptr;

namespace {
struct AnchorInit
{
    AnchorInit()
    {
        // Taking the address of the extern char* forces the linker to resolve
        // the data array backing each named resource. The Size constants are
        // also touched so they aren't stripped (they live in the same TU as the
        // extern decls and confirm the resource is non-empty).
        g_modelDataAnchor = static_cast<const void*>(::BinaryData::masteringbrain_v2_decision_onnx);
        g_contractDataAnchor = static_cast<const void*>(::BinaryData::masteringbrain_v2_decision_contract_json);
        (void) ::BinaryData::masteringbrain_v2_decision_onnxSize;
        (void) ::BinaryData::masteringbrain_v2_decision_contract_jsonSize;
    }
} g_anchorInit;
} // namespace

} // namespace sonicmaster_embedded_anchor
} // namespace more_phi
