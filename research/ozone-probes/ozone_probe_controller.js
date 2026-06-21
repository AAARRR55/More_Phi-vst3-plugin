'use strict';
/*
 * Read-only probe of the LIVE Master Assistant controller object captured from
 * the idle poller (OzoneHeadlessHost). Finds which object owns the trigger
 * (thunk 0xD572F0 / body 0xD58A20) as a vtable slot -> the exact live `this` to
 * invoke headlessly. NO calls, NO mutation.
 */
var TRIGGER_THUNK = 0xD572F0;
var TRIGGER_BODY = 0xD58A20;
var mod = null, base = null, size = null;

function inMod(p) { try { return p.compare(base) >= 0 && p.compare(base.add(size)) < 0; } catch (e) { return false; } }
function rvaOf(p) { try { return inMod(p) ? '0x' + p.sub(base).toString(16) : null; } catch (e) { return null; } }

function readVTableEntries(vtPtr, n) {
  var out = [];
  try {
    for (var i = 0; i < n; i++)
      out.push(rvaOf(vtPtr.add(i * 8).readPointer()));
  } catch (e) {}
  return out;
}

function hasTrigger(entries) {
  return entries.indexOf('0x' + TRIGGER_THUNK.toString(16)) >= 0
      || entries.indexOf('0x' + TRIGGER_BODY.toString(16)) >= 0;
}

function setup() {
  send({ kind: 'mod', base: String(base) });
  Interceptor.attach(base.add(0xEAD3E0), {
    onEnter: function (a) {
      var ths = a[0];
      var report = { this_ptr: String(ths), sub_objects: [] };

      // Object A: its own vtable.
      try {
        var vtA = ths.readPointer();
        var entA = readVTableEntries(vtA, 64);
        report.objA = { vtable_rva: rvaOf(vtA), has_trigger: hasTrigger(entA),
                        vtable_entries: entA,
                        trigger_slots: entA.reduce(function (acc, r, i) { if (r === '0x' + TRIGGER_THUNK.toString(16) || r === '0x' + TRIGGER_BODY.toString(16)) acc.push(i); return acc; }, []) };
      } catch (e) { report.objA = { err: String(e) }; }

      // Scan object A fields for sub-objects with in-module vtables; dump each vtable.
      try {
        for (var off = 0; off < 0x400; off += 8) {
          var fp;
          try { fp = ths.add(off).readPointer(); } catch (e) { continue; }
          if (fp.isNull() || ! inMod(fp)) {
            // could still be a heap pointer; test by reading [fp] as a vtable
          }
          var subVt;
          try { subVt = fp.readPointer(); } catch (e) { continue; }
          var subVtRva = rvaOf(subVt);
          if (! subVtRva) continue;
          var ent = readVTableEntries(subVt, 64);
          report.sub_objects.push({
            field_off: '0x' + off.toString(16),
            sub_ptr: String(fp),
            sub_vtable_rva: subVtRva,
            has_trigger: hasTrigger(ent),
            trigger_slots: ent.reduce(function (acc, r, i) { if (r === '0x' + TRIGGER_THUNK.toString(16) || r === '0x' + TRIGGER_BODY.toString(16)) acc.push(i); return acc; }, [])
          });
        }
      } catch (e) { report.scan_err = String(e); }

      send({ kind: 'controller', report: report });
      // one capture is enough
    }
  });
  send({ kind: 'ready' });
}

(function () {
  mod = Process.findModuleByName('iZOzonePro.dll');
  if (mod) { base = mod.base; size = mod.size; setup(); }
  else {
    var w = setInterval(function () {
      mod = Process.findModuleByName('iZOzonePro.dll');
      if (mod) { base = mod.base; size = mod.size; clearInterval(w); setup(); }
    }, 100);
  }
})();
