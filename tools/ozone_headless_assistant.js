/*
 * ozone_headless_assistant.js
 *
 * Frida script that resolves iZotope Ozone's internal Master Assistant trigger
 * inside a MORE-PHI-OWNED host process (MorePhiMcpServer / HostedOzonePluginBackend)
 * and exposes an RPC to invoke it under an explicit, multi-part safety gate.
 *
 * RED LINES (docs/OZONE_IPC_RESEARCH_METHODOLOGY.md):
 *   - This script attaches ONLY to a More-Phi-owned harness process. It must
 *     never be loaded into FL64.exe, iZotope.exe, or any PACE/iLok helper.
 *   - It does NOT touch PACE/iLok, licensing, entitlement, anti-tamper, or
 *     integrity-check logic. It does NOT call the applier (0xEAD930).
 *   - The internal call is NOT executed by default. DRY-RUN mode (the default)
 *     resolves the vtable slot, logs the address + a disassembly snapshot, and
 *     returns WITHOUT calling. The call only happens when the operator sets the
 *     in-script ENABLE_PRIVATE_CALL flag AND the driver has already verified the
 *     two-part environment gate before posting the RPC.
 *
 * TRIGGER SURFACE (from offline static recon of the on-disk iZOzonePro.dll):
 *   - Module:    iZOzonePro.dll  (image base 0x180000000)
 *   - Body RVA:  0xD58A20  (the controller method body)
 *   - Vtable
 *     thunk RVA: 0xD572F0  (push rbx; sub rsp,0x20; add rcx,8; mov rbx,rdx;
 *                            call 0xD58A20; mov rax,rbx; ret)  <- GUI-Play dispatch
 *   - Signature: MSVC x64 thiscall, 3 args.
 *       RCX = controller base sub-object (this+8 after the thunk's this-adjust)
 *       RDX = in/out context (preserved across the 4848-byte body)
 *       R8  = output/options struct (consumed via `mov rdi,[r13]`)
 *       -> int thiscall RunAnalysis(this, inOutCtx, optsOrOut)
 *   - IMPORTANT: per the verify verdict, 0xD58A20 is a genuine Master-Assistant
 *     vtable method but its precise runtime semantics are NOT confirmed by
 *     dynamic correlation (forbidden here). It may be parameter-metadata
 *     enumeration rather than analysis-start. Treat the call as a RESEARCH
 *     PROBE, not a verified "start analysis" action. Observe param deltas.
 *
 * GATE (in addition to whatever the driver enforces):
 *   ENABLE_PRIVATE_CALL must be set to true in this file by the operator before
 *   invokeAssistant() will do anything other than resolve + log. The default is
 *   false, so loading this script and calling resolveOnly() is always safe.
 */

'use strict';

// ============================================================================
// CONFIGURATION  (operator edits these in-place before any live call)
// ============================================================================

var MODULE_NAME       = 'iZOzonePro.dll';
var TRIGGER_BODY_RVA  = 0xD58A20;   // the method body (instrumentation target)
var VTABLE_THUNK_RVA  = 0xD572F0;   // the vtable slot the GUI Play button dispatches through
var POLLER_RVA        = 0xEAD3E0;   // read-only observation anchor (NOT called)
var APPLIER_RVA       = 0xEAD930;   // FORBIDDEN: never call, never instrument

// THE MASTER SAFETY FLAG. Default false => dry-run only. The operator must
// explicitly flip this to true in-file AND the driver must have already passed
// its environment gate before invokeAssistant() will dispatch the NativeFunction.
var ENABLE_PRIVATE_CALL = false;

// How many bytes of the vtable thunk / body to snapshot for the operator log.
var DISASM_SNAPSHOT_BYTES = 64;

// ============================================================================
// STATE
// ============================================================================

var gModule     = null;     // resolved Module object for iZOzonePro.dll
var gThunkAddr  = null;     // NativePointer at base + VTABLE_THUNK_RVA
var gBodyAddr   = null;     // NativePointer at base + TRIGGER_BODY_RVA
var gPollerAddr = null;     // NativePointer at base + POLLER_RVA (observe only)
var gReady      = false;    // set true once the module is mapped and resolved

// ============================================================================
// HELPERS
// ============================================================================

function log(obj) {
  try {
    if (typeof obj === 'string') {
      send({ kind: 'log', msg: obj });
    } else {
      send(Object.assign({ kind: 'log' }, obj));
    }
  } catch (e) {
    // send() is best-effort during teardown
  }
}

function hexBytes(ptr, n) {
  try {
    var bytes = ptr.readByteArray(n);
    if (!bytes) return '';
    var u = new Uint8Array(bytes);
    var out = [];
    for (var i = 0; i < u.length; i++) {
      out.push(('0' + u[i].toString(16)).slice(-2));
    }
    return out.join(' ');
  } catch (e) {
    return '<read-failed: ' + e + '>';
  }
}

function findModuleRetry(name, attempts, intervalMs) {
  for (var i = 0; i < attempts; i++) {
    var m = Process.findModuleByName(name);
    if (m) return m;
    Thread.sleep(intervalMs / 1000.0);
  }
  return null;
}

// Resolve the trigger. Returns a resolution record or throws. Never calls the
// trigger. Safe to invoke at any time.
function resolveTrigger() {
  if (!gReady) {
    gModule = findModuleRetry(MODULE_NAME, 100, 100); // up to ~10s for load
    if (!gModule) {
      throw new Error('Module ' + MODULE_NAME + ' is not loaded in this process. ' +
                      'Confirm OZONE_VST3_PATH points at the Ozone Pro VST3 and the ' +
                      'host has finished loading it.');
    }
    gThunkAddr  = gModule.base.add(VTABLE_THUNK_RVA);
    gBodyAddr   = gModule.base.add(TRIGGER_BODY_RVA);
    gPollerAddr = gModule.base.add(POLLER_RVA);
    gReady      = true;
  }

  // Bounds sanity: the resolved addresses must land inside the mapped module.
  var modBase = gModule.base;
  var modEnd  = modBase.add(gModule.size);
  if (gThunkAddr.compare(modBase) < 0 || gThunkAddr.compare(modEnd) >= 0) {
    throw new Error('vtable thunk RVA resolves outside module range; aborting.');
  }
  if (gBodyAddr.compare(modBase) < 0 || gBodyAddr.compare(modEnd) >= 0) {
    throw new Error('trigger body RVA resolves outside module range; aborting.');
  }

  return {
    module:       gModule.name,
    path:         gModule.path,
    base:         String(gModule.base),
    size:         gModule.size,
    thunk_addr:   String(gThunkAddr),
    thunk_rva:    '0x' + VTABLE_THUNK_RVA.toString(16),
    body_addr:    String(gBodyAddr),
    body_rva:     '0x' + TRIGGER_BODY_RVA.toString(16),
    poller_addr:  String(gPollerAddr),
    poller_rva:   '0x' + POLLER_RVA.toString(16),
    applier_rva:  '0x' + APPLIER_RVA.toString(16) + ' (FORBIDDEN: never call)',
    thunk_bytes:  hexBytes(gThunkAddr, DISASM_SNAPSHOT_BYTES),
    body_bytes:   hexBytes(gBodyAddr, DISASM_SNAPSHOT_BYTES)
  };
}

// Validate that a candidate `this` pointer (the controller object base) looks
// like a live, fully-constructed object: it must be readable, its first qword
// (the vtable pointer) must point INSIDE iZOzonePro.dll's mapped range, and the
// [+0x90] flag byte the body reads must be readable. This is the precondition
// guard from the synthesis verdict -- a fake/garbage `this` faults on the very
// first `mov rbx,[rcx]` then again on `cmp byte [rbx+0x90]`.
function validateControllerThis(thisPtr) {
  if (!gReady) resolveTrigger();

  var p = ptr(thisPtr);
  if (p.isNull()) {
    throw new Error('controller this is NULL.');
  }

  var vtable;
  try {
    vtable = p.readPointer();           // [rcx] -> first sub-object's vtable
  } catch (e) {
    throw new Error('cannot read [this] (vtable ptr) at ' + p + ': ' + e);
  }

  var modBase = gModule.base;
  var modEnd  = modBase.add(gModule.size);
  if (vtable.compare(modBase) < 0 || vtable.compare(modEnd) >= 0) {
    throw new Error('controller vtable ' + vtable + ' is OUTSIDE ' + MODULE_NAME +
                    ' range [' + modBase + ',' + modEnd + '). A raw or fake `this` ' +
                    'will fault inside the trigger. Aborting.');
  }

  // The body does: rbx = [rcx]; ... cmp byte [rbx+0x90], r14b; and writes
  // [rbx+0x98]/[+0xA0]/[+0xA8]. Probe a couple of those offsets for readability
  // so a partially-constructed object fails loudly here instead of inside the call.
  var probeOffsets = [0x90, 0x98, 0xA0, 0xA8];
  for (var i = 0; i < probeOffsets.length; i++) {
    try {
      p.add(probeOffsets[i]).readU8();
    } catch (e) {
      throw new Error('controller this ' + p + ' is not readable at +0x' +
                      probeOffsets[i].toString(16) + ' -- object is not fully ' +
                      'constructed. Aborting before dispatch.');
    }
  }

  return {
    controller_this: String(p),
    vtable_ptr:      String(vtable),
    vtable_in_module: true
  };
}

// Resolve the actual vtable slot entry the GUI Play button dispatches through.
// The thunk at VTABLE_THUNK_RVA is itself the dispatch surface (it does the
// +8 this-adjust then tail-calls the body). We invoke the THUNK, not the raw
// body, per the verify verdict: "invoke through the controller vtable SLOT,
// not the raw RVA body".
function resolveDispatchSlot() {
  resolveTrigger();
  return {
    dispatch_addr: String(gThunkAddr),
    dispatch_kind: 'vtable_thunk',
    note: 'Invoke through this thunk (NOT the raw body ' + gBodyAddr + '). ' +
          'The thunk applies the MSVC +8 this-adjustor before tail-calling ' +
          'the body, matching how the GUI Play button reaches it.'
  };
}

// ============================================================================
// THE GATED INVOCATION
// ============================================================================

// invokeAssistant(opts)
//
// opts: {
//   controllerThis:  string|hex  REQUIRED. Live controller object base pointer,
//                                obtained from the hosted instance's EditController
//                                state (NOT synthesized). Validated by
//                                validateControllerThis() before any dispatch.
//   inOutCtx:        string|hex  REQUIRED. RDX. Valid in/out context. In dry-run
//                                we only log it; in live mode we pass it through.
//                                The operator must source a real one.
//   optsOrOut:       string|hex  REQUIRED. R8. Writable output/options struct.
//   enable:          bool        MUST be true to dispatch. Even if
//                                ENABLE_PRIVATE_CALL is true, this per-call flag
//                                must also be true, providing defense in depth.
// }
//
// RETURN (dry-run):  { called: false, resolution: {...}, controller: {...}, slot: {...} }
// RETURN (live):     { called: true, retval: '0x...', note: '...' }
// THROWS:            on any gate failure, bad `this`, or out-of-range slot.
function invokeAssistant(opts) {
  opts = opts || {};

  // ---- GATE 1: in-script master flag ----
  if (!ENABLE_PRIVATE_CALL) {
    var dry = {
      called: false,
      reason: 'DRY-RUN: ENABLE_PRIVATE_CALL is false in ozone_headless_assistant.js. ' +
              'Resolved the trigger and returning WITHOUT calling. To dispatch, the ' +
              'operator must (1) set ENABLE_PRIVATE_CALL=true in this file, (2) run the ' +
              'driver with OZONE_HEADLESS_INVOKE=1 --i-understand-the-risk, (3) pass ' +
              'opts.enable=true, and (4) provide a live controllerThis sourced from the ' +
              'hosted instance. See docs/OZONE_HEADLESS_ASSISTANT_RUNBOOK.md.'
    };
    try {
      dry.resolution = resolveTrigger();
    } catch (e) { dry.resolve_error = String(e); }
    log({ stage: 'invokeAssistant', result: 'dry_run' });
    return dry;
  }

  // ---- GATE 2: per-call enable ----
  if (opts.enable !== true) {
    throw new Error('GATE FAILED: opts.enable is not true. Even with ENABLE_PRIVATE_CALL ' +
                    'set, each invocation requires an explicit opts.enable=true.');
  }

  // ---- GATE 3: required args ----
  if (!opts.controllerThis) {
    throw new Error('GATE FAILED: opts.controllerThis is required (live controller object ' +
                    'base, sourced from the hosted EditController -- never synthesized).');
  }
  if (!opts.inOutCtx) {
    throw new Error('GATE FAILED: opts.inOutCtx (RDX) is required.');
  }
  if (!opts.optsOrOut) {
    throw new Error('GATE FAILED: opts.optsOrOut (R8) is required.');
  }

  // ---- GATE 4: re-resolve + bounds-check immediately before dispatch ----
  // (guards against a relocated / patched image between resolve and call)
  resolveTrigger();
  var ctrl = validateControllerThis(opts.controllerThis);
  var slot = resolveDispatchSlot();

  log({
    stage: 'invokeAssistant',
    result: 'gates_passed',
    dispatch_addr: slot.dispatch_addr,
    controller_this: ctrl.controller_this,
    vtable_ptr: ctrl.vtable_ptr
  });

  // ---- DISPATCH via the vtable thunk (NOT the raw body) ----
  // Signature: int thiscall thunk(this RCX, inOutCtx RDX, optsOrOut R8)
  // The thunk does `add rcx,8` internally, so we pass the controller base as-is
  // and the thunk adjusts it to the sub-object expected by the body.
  var fn = new NativeFunction(gThunkAddr, 'int', ['pointer', 'pointer', 'pointer']);
  var rv;
  try {
    rv = fn(ptr(opts.controllerThis), ptr(opts.inOutCtx), ptr(opts.optsOrOut));
  } catch (e) {
    log({ stage: 'invokeAssistant', result: 'dispatch_threw', error: String(e) });
    throw new Error('NativeFunction dispatch threw: ' + e +
                    '. If this is an access violation, controllerThis/inOutCtx/optsOrOut ' +
                    'are not valid live objects (see preconditions). Do NOT retry with ' +
                    'a synthesized this.');
  }

  log({ stage: 'invokeAssistant', result: 'dispatch_ok', retval: String(rv) });
  return {
    called: true,
    retval: String(rv),
    dispatch_addr: slot.dispatch_addr,
    controller_this: ctrl.controller_this,
    note: 'Trigger dispatched through vtable thunk. The verify verdict notes the ' +
          'precise runtime semantics of 0xD58A20 are NOT confirmed by dynamic ' +
          'correlation (forbidden here) -- it may be parameter-metadata enumeration ' +
          'rather than analysis-start. Inspect Ozone param deltas (driver diffs them).'
  };
}

// ============================================================================
// READ-ONLY OBSERVATION (always safe; no call into the trigger)
// ============================================================================

// Attach an observation hook to the POLLER (0xEAD3E0) only. This is the
// read-only anchor from prior Frida work and is explicitly NOT the trigger.
// Useful to confirm the hosted instance is alive and the poller is firing.
function observePoller(enable) {
  resolveTrigger();
  if (enable === false) {
    log({ stage: 'observePoller', action: 'no-op (observation opt-in only)' });
    return { observing: false };
  }
  // NOTE: we do NOT auto-attach; the driver controls whether observation hooks
  // are installed. This is a placeholder for an explicit observe() call.
  return {
    observing: false,
    poller_addr: String(gPollerAddr),
    note: 'Call observePollerAttach() from the driver to install a read-only counter.'
  };
}

// ============================================================================
// rpc.exports  (the driver calls these)
// ============================================================================

rpc.exports = {
  // Always safe. Resolves module + addresses, returns a snapshot. No call.
  resolve: function () {
    try {
      var r = resolveTrigger();
      log({ stage: 'resolve', ok: true, thunk: r.thunk_addr });
      return r;
    } catch (e) {
      log({ stage: 'resolve', ok: false, error: String(e) });
      throw e;
    }
  },

  // Always safe. Validates a controller `this` without calling anything.
  validateThis: function (thisPtr) {
    return validateControllerThis(thisPtr);
  },

  // The gated entry point. See invokeAssistant() above for the gate chain.
  invoke: function (opts) {
    return invokeAssistant(opts);
  },

  // Report the in-script master flag state so the driver can refuse to even
  // post invoke() unless both the driver gate AND this flag are set.
  gateState: function () {
    return {
      enable_private_call: ENABLE_PRIVATE_CALL,
      module_name: MODULE_NAME,
      body_rva: '0x' + TRIGGER_BODY_RVA.toString(16),
      thunk_rva: '0x' + VTABLE_THUNK_RVA.toString(16),
      applier_rva: '0x' + APPLIER_RVA.toString(16),
      applier_policy: 'FORBIDDEN: never call, never instrument'
    };
  },

  // Read-only health ping.
  ping: function () {
    return {
      ok: true,
      module_loaded: !!(gModule),
      ready: gReady,
      module: gModule ? gModule.name : null,
      base: gModule ? String(gModule.base) : null
    };
  }
};

// Emit a ready banner so the driver knows the script is alive. We do NOT
// resolve the module yet -- the driver may need to wait for the host to finish
// loading Ozone first, then call rpc.resolve().
log({
  stage: 'script_loaded',
  enable_private_call: ENABLE_PRIVATE_CALL,
  module_name: MODULE_NAME,
  body_rva: '0x' + TRIGGER_BODY_RVA.toString(16),
  thunk_rva: '0x' + VTABLE_THUNK_RVA.toString(16),
  note: 'DRY-RUN by default. Call rpc.resolve() once Ozone is loaded. ' +
        'invoke() will NOT dispatch unless ENABLE_PRIVATE_CALL=true in-file.'
});
