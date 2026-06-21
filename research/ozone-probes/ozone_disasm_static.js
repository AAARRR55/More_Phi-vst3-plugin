'use strict';
/* One-shot static disasm of the known controller RVAs (no call, no mutation).
 * Goal: find the poller's conditional branch into the analysis path + any
 * "request" flag byte it reads off the controller (rcx). Pure read. */
var TARGETS = {
  poller:        0xEAD3E0,
  state_helper_a:0xEAD2E0,
  state_helper_b:0xEAD360,
  hub:           0xEABDB0,
  getter:        0xD50740,
  pipeline_root: 0xE9FC30,
  applier:       0xEAD930,
  thunk:         0xD572F0,
  body:          0xD58A20,
  caller:        0x166CA90,
  data_stream_ctor: 0xFD7F30,
};

function disasmAt(addr, n) {
  var out = [];
  try {
    var p = ptr(addr);
    for (var i = 0; i < n; i++) {
      var ins = Instruction.parse(p);
      var rva = '0x' + ins.address.sub(base).toString(16);
      // annotate calls/jmps with target RVA if in-module
      var tgt = '';
      var m = /^(call|jmp|je|jne|jz|jnz|jb|jae|jb|jbe|ja)\s+(.*)$/.exec(ins.mnemonic + ' ' + (ins.opStr || ''));
      out.push(rva + ': ' + ins.mnemonic + ' ' + (ins.opStr || ''));
      p = ins.next;
    }
  } catch (e) { out.push('err ' + e); }
  return out;
}

var base = null;
function go() {
  var m = Process.findModuleByName('iZOzonePro.dll');
  if (!m) return false;
  base = m.base;
  var r = { base: String(base) };
  Object.keys(TARGETS).forEach(function (k) { r[k] = disasmAt(base.add(TARGETS[k]), 40); });
  send({ kind: 'disasm', data: r });
  return true;
}

if (!go()) { var w = setInterval(function () { if (go()) clearInterval(w); }, 100); }
