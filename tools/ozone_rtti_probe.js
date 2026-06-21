'use strict';
/* Read-only: read MSVC RTTI class names for the live assistant controller object
 * (from the idle poller) and its sub-objects, to identify which object/method is
 * the analysis trigger. No calls, no mutation. */
var TRIGGER_THUNK = 0xD572F0, TRIGGER_BODY = 0xD58A20;
var mod = null, base = null, size = null;

function inMod(p) { try { return p.compare(base) >= 0 && p.compare(base.add(size)) < 0; } catch (e) { return false; } }
function rvaOf(p) { try { return inMod(p) ? '0x' + p.sub(base).toString(16) : null; } catch (e) { return null; } }

function readVTableEntries(vt, n) {
  var out = [];
  try { for (var i = 0; i < n; i++) out.push(rvaOf(vt.add(i * 8).readPointer())); } catch (e) {}
  return out;
}
function hasTrigger(e) { return e.indexOf('0x' + TRIGGER_THUNK.toString(16)) >= 0 || e.indexOf('0x' + TRIGGER_BODY.toString(16)) >= 0; }

function rttiName(objPtr) {
  try {
    var vt = objPtr.readPointer();
    var colPtr = vt.sub(8).readPointer(); // COL pointer at vtable-8
    if (! inMod(colPtr)) return null;
    var sig = colPtr.readU32();
    var pTD;
    if (sig === 1) {            // x64 image-relative
      var tdRva = colPtr.add(12).readU32();
      pTD = base.add(tdRva);
    } else {                    // legacy pointer form
      pTD = colPtr.add(12).readPointer();
    }
    var name = pTD.add(16).readUtf8String(160);
    return name || null;
  } catch (e) { return null; }
}

function setup() {
  send({ kind: 'mod', base: String(base) });
  Interceptor.attach(base.add(0xEAD3E0), {
    onEnter: function (a) {
      var ths = a[0];
      var report = {
        this_ptr: String(ths),
        objA: { vtable_rva: rvaOf(ths.readPointer()), class: rttiName(ths),
                has_trigger: hasTrigger(readVTableEntries(ths.readPointer(), 64)) },
        sub_objects: []
      };
      try {
        for (var off = 0; off < 0x400; off += 8) {
          var fp;
          try { fp = ths.add(off).readPointer(); } catch (e) { continue; }
          var subVt;
          try { subVt = fp.readPointer(); } catch (e) { continue; }
          if (! rvaOf(subVt)) continue;
          var ent = readVTableEntries(subVt, 64);
          report.sub_objects.push({
            field_off: '0x' + off.toString(16), sub_ptr: String(fp),
            sub_vtable_rva: rvaOf(subVt), class: rttiName(fp),
            has_trigger: hasTrigger(ent)
          });
        }
      } catch (e) { report.scan_err = String(e); }
      send({ kind: 'controller', report: report });
    }
  });
  send({ kind: 'ready' });
}

(function () {
  mod = Process.findModuleByName('iZOzonePro.dll');
  if (mod) { base = mod.base; size = mod.size; setup(); }
  else var w = setInterval(function () {
    mod = Process.findModuleByName('iZOzonePro.dll');
    if (mod) { base = mod.base; size = mod.size; clearInterval(w); setup(); }
  }, 100);
})();
