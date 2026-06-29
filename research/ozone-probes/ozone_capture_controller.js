'use strict';
/*
 * Observe-only capture of live Master Assistant controller pointers from the
 * idle poller and siblings, INSIDE OUR OWN host process (MorePhiMcpServer).
 *
 * NO NativeFunction calls, NO argument mutation, NO return spoofing. Pure
 * Interceptor.attach onEnter reads. Red-line compliant (own process, observe).
 *
 * Goal: the poller 0xEAD3E0 fires during idle; capturing its RCX (and the
 * helpers'/hub's RCX) yields the live controller object pointer needed to call
 * the trigger thunk 0xD572F0 headlessly.
 */

var TARGETS = {
  poller_state:   0xEAD3E0,
  state_helper_a: 0xEAD2E0,
  state_helper_b: 0xEAD360,
  hub:            0xEABDB0,
  thunk:          0xD572F0,
  body:           0xD58A20,
  getter:         0xD50740,
  pipeline_root:  0xE9FC30,
  applier:        0xEAD930,
};

var mod = null;
var base = null, size = null;
var counts = {};
Object.keys(TARGETS).forEach(function (k) { counts[k] = 0; });
var hooked = false;

function inMod(a) {
  try {
    var p = ptr(a);
    return p.compare(base) >= 0 && p.compare(base.add(size)) < 0;
  } catch (e) { return false; }
}

function describePtr(p) {
  // For a candidate controller 'this': read vtable + the offsets the trigger touches.
  var r = { ptr: String(p), in_mod: inMod(p) };
  try {
    var vt = p.readPointer();
    r.vtable = String(vt);
    r.vtable_in_mod = inMod(vt);
    if (r.vtable_in_mod) r.vtable_rva = '0x' + vt.sub(base).toString(16);
  } catch (e) { r.vtable_err = String(e); }
  ['0x90', '0x98', '0xa0', '0xa8', '0x0c'].forEach(function (off) {
    try { r['deref' + off] = '0x' + p.add(off).readU8().toString(16); } catch (e) { r['deref' + off] = 'NA'; }
  });
  return r;
}

function hookAll() {
  if (hooked) return;
  Object.keys(TARGETS).forEach(function (label) {
    var addr = base.add(TARGETS[label]);
    try {
      Interceptor.attach(addr, {
        onEnter: function (args) {
          counts[label]++;
          if (counts[label] <= 4) {
            var rec = { label: label, n: counts[label],
                        rcx: describePtr(args[0]),
                        rdx: String(args[1]), r8: String(args[2]), r9: String(args[3]) };
            send({ kind: 'hit', rec: rec });
          }
        }
      });
      send({ kind: 'hook_ok', label: label, addr: String(addr) });
    } catch (e) {
      send({ kind: 'hook_fail', label: label, err: String(e) });
    }
  });
  hooked = true;
  send({ kind: 'all_hooked' });
}

// Wait for iZOzonePro.dll to be mapped, then hook.
function tryFind() {
  if (mod) return true;
  mod = Process.findModuleByName('iZOzonePro.dll');
  if (mod) {
    base = mod.base; size = mod.size;
    send({ kind: 'module_found', base: String(base), size: size, path: mod.path });
    hookAll();
    return true;
  }
  return false;
}

if (!tryFind()) {
  var waiter = setInterval(function () {
    if (tryFind()) clearInterval(waiter);
  }, 100);
}

setInterval(function () { send({ kind: 'tally', counts: counts }); }, 2000);

rpc.exports = {
  tally: function () { return counts; }
};
