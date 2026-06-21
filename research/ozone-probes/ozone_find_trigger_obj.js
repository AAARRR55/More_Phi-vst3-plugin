'use strict';
/* Read-only heap scan: find the LIVE object whose vtable contains the trigger
 * thunk 0xD572F0 (and/or body 0xD58A20), so we know the exact live `this` to
 * invoke headlessly. No calls, no mutation. */
var THUNK = 0xD572F0, BODY = 0xD58A20;
var mod = null, base = null, size = null;

function inMod(p) { try { return ! p.isNull() && p.compare(base) >= 0 && p.compare(base.add(size)) < 0; } catch (e) { return false; } }

function leBytes(addr) {
  var u = new Uint8Array(8);
  var v = addr;
  for (var i = 0; i < 8; i++) { u[i] = v.toUInt32() & 0xff; v = v.shr(8); }
  var s = '';
  for (i = 0; i < 8; i++) s += ('0' + u[i].toString(16)).slice(-2);
  return s;
}

function scanRdataFor(qwordPtr) {
  // find locations in .rdata whose stored qword == qwordPtr
  var pat = leBytes(qwordPtr);
  var hits = [];
  Process.enumerateRanges('r--').forEach(function (r) {
    if (! inMod(r.base)) return;
    try {
      Memory.scanSync(r.base, r.size, pat).forEach(function (m) { hits.push(m.address); });
    } catch (e) {}
  });
  return hits;
}

function scanHeapFor(vtablePtr) {
  var pat = leBytes(vtablePtr);
  var hits = [];
  Process.enumerateRanges({ protection: 'rw-', coalesce: true }).forEach(function (r) {
    try {
      Memory.scanSync(r.base, r.size, pat).forEach(function (m) { hits.push(m.address); });
    } catch (e) {}
  });
  return hits;
}

function vtableHas(vtPtr, targetVa) {
  try {
    for (var i = 0; i < 80; i++) {
      if (vtPtr.add(i * 8).readPointer().equals(targetVa)) return i;
    }
  } catch (e) {}
  return -1;
}

function run() {
  var thunkVa = base.add(THUNK);
  var bodyVa = base.add(BODY);
  var result = { thunk_rva: '0x' + THUNK.toString(16), found: [] };

  // 1. where does the thunk live in .rdata? (its vtable slot)
  var slotHits = scanRdataFor(thunkVa);
  result.thunk_rdata_slots = slotHits.map(function (a) { return '0x' + a.sub(base).toString(16); });
  send({ kind: 'progress', msg: 'thunk .rdata slots: ' + result.thunk_rdata_slots.join(' ') });

  // 2. for each slot, try vtable bases V = slot - k*8 (k=0..63); heap-scan for objects [B]==V
  slotHits.forEach(function (slot) {
    for (var k = 0; k < 64; k++) {
      var V = slot.sub(k * 8);
      if (! inMod(V)) continue;
      // verify this V is a vtable containing the thunk at slot k
      if (vtableHas(V, thunkVa) !== k) continue;
      var objs = scanHeapFor(V);
      objs.forEach(function (B) {
        result.found.push({
          object_ptr: String(B),
          vtable_ptr: String(V),
          vtable_rva: '0x' + V.sub(base).toString(16),
          thunk_slot: k
        });
      });
      if (objs.length) send({ kind: 'progress', msg: 'V=0x' + V.sub(base).toString(16) + ' k=' + k + ' -> ' + objs.length + ' object(s)' });
    }
  });

  send({ kind: 'done', result: result });
}

(function () {
  function go() { setTimeout(run, 5000); } // let the editor construct object B first
  mod = Process.findModuleByName('iZOzonePro.dll');
  if (mod) { base = mod.base; size = mod.size; go(); }
  else var w = setInterval(function () {
    mod = Process.findModuleByName('iZOzonePro.dll');
    if (mod) { base = mod.base; size = mod.size; clearInterval(w); go(); }
  }, 100);
})();
