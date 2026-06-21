'use strict';
/* Gated headless trigger, thread-correct: the invoke fires from WITHIN the
 * poller's onEnter (the same thread that drives the controller), avoiding the
 * cross-thread reentrancy crash of calling from Frida's JS thread.
 * Locates the trigger-owning object, captures applier context, and on the next
 * poller tick after enable_invoke, calls the trigger thunk 0xD572F0 once.
 * Default DRY-RUN; our process only. */
var THUNK=0xD572F0, BODY=0xD58A20, PIPELINE=0xE9FC30, APPLIER=0xEAD930, POLLER=0xEAD3E0;
var base=null, size=null;
var counters={ body:0, pipeline:0, applier:0, poller:0 };
var ctx={ rdx:null, r8:null };
var objects=[];
var ready=false, wanted=false, invoked=false, bodySeen=false;

function inMod(p){ try{return !p.isNull()&&p.compare(base)>=0&&p.compare(base.add(size))<0;}catch(e){return false;} }
function leBytes(a){ var u=new Uint8Array(8); var v=a; for(var i=0;i<8;i++){u[i]=v.toUInt32()&0xff; v=v.shr(8);} var s=''; for(i=0;i<8;i++) s+=('0'+u[i].toString(16)).slice(-2); return s; }
function vtHas(vt,t){ try{for(var i=0;i<80;i++){if(vt.add(i*8).readPointer().equals(t))return i;}}catch(e){} return -1; }
function disasmAt(addrPtr,n){ var out=[]; try{ var p=addrPtr; for(var i=0;i<n;i++){ var ins=Instruction.parse(p); var loc=inMod(ins.address)?('0x'+ins.address.sub(base).toString(16)):String(ins.address); out.push(loc+': '+ins.mnemonic+' '+(ins.opStr||'')); p=ins.next; } }catch(e){ out.push('disasm_err: '+e); } return out; }
function whichMod(p){ try{ var m=Process.findModuleByAddress(ptr(p)); return m?m.name:null; }catch(e){ return null; } }

function findObjects(){
  var tv=base.add(THUNK); var pat=leBytes(tv); var slot=null;
  Process.enumerateRanges('r--').forEach(function(r){ if(slot||!inMod(r.base))return; try{Memory.scanSync(r.base,r.size,pat).forEach(function(m){if(!slot)slot=m.address;});}catch(e){} });
  if(!slot) return;
  for(var k=0;k<64;k++){ var V=slot.sub(k*8); if(!inMod(V)||vtHas(V,tv)!==k) continue;
    var op=leBytes(V);
    Process.enumerateRanges({protection:'rw-',coalesce:true}).forEach(function(r){ try{Memory.scanSync(r.base,r.size,op).forEach(function(m){objects.push({ptr:m.address,vtable:V,slot:k});});}catch(e){} });
  }
}

function doInvoke(){
  invoked=true;
  var thunkFn=new NativeFunction(base.add(THUNK),'int',['pointer','pointer','pointer']);
  // Construct fallback context: body reads [r8]->rdi (output ptr) and uses [rdx].
  var outBuf=Memory.alloc(0x4000);
  var r8fb=Memory.alloc(0x40); r8fb.writePointer(outBuf);
  var rdxfb=Memory.alloc(0x40); rdxfb.writePointer(outBuf);
  var rdxUse=(ctx.rdx&&!ctx.rdx.isNull())?ctx.rdx:rdxfb;
  var r8Use=(ctx.r8&&!ctx.r8.isNull())?ctx.r8:r8fb;
  send({kind:'invoke_ctx', rdx:String(rdxUse), r8:String(r8Use), synthetic:(r8Use===r8fb)});
  for(var i=0;i<objects.length;i++){
    var o=objects[i];
    var before={b:counters.body,p:counters.pipeline,a:counters.applier};
    var rv=null,err=null;
    try{ rv=thunkFn(o.ptr, rdxUse, r8Use); }
    catch(e){ err=String(e); }
    send({kind:'invoke', object:String(o.ptr), slot:o.slot, vtable_rva:'0x'+o.vtable.sub(base).toString(16),
          retval:rv===null?null:String(rv), error:err,
          delta:{body:counters.body-before.b, pipeline:counters.pipeline-before.p, applier:counters.applier-before.a}});
    if(err) break;
  }
  send({kind:'counters_after', counters:counters});
}

function setup(){
  // Binary-interception: catch the EXACT fault when the trigger body faults,
  // so we know which pointer it derefs and which module faults (PACE trip vs body).
  Process.setExceptionHandler(function(d){
    try{
      var rip=d.context?d.context.rip:null;
      var fa=(d.memory&&d.memory.address)?d.memory.address:null;
      var rec={kind:'fault', type:String(d.type), rip:String(rip),
        rip_mod: rip?whichMod(rip):null,
        rip_rva: (rip&&inMod(rip))?('0x'+ptr(rip).sub(base).toString(16)):null,
        fault_addr:String(fa), fault_mod: fa?whichMod(fa):null,
        op:(d.memory&&d.memory.operation)?d.memory.operation:null,
        rcx: d.context?String(d.context.rcx):null,
        rdx: d.context?String(d.context.rdx):null,
        r8:  d.context?String(d.context.r8):null,
        rsp: d.context?String(d.context.rsp):null};
      if(rip){ try{rec.disasm=disasmAt(ptr(rip),6);}catch(e){rec.disasm=['disasm_err '+e];} }
      send(rec);
    }catch(e){ send({kind:'fault_handler_err',err:String(e)}); }
    return false; // capture + let it die (state corrupted mid-call)
  });
  Interceptor.attach(base.add(POLLER),{ onEnter:function(a){
    counters.poller++;
    // capture context from the poller too (same controller)
    if(!ctx.rdx && !a[1].isNull()){ ctx.rdx=a[1]; ctx.r8=a[2]; }
    if(wanted && ready && !invoked && objects.length){
      send({kind:'invoking_from_poller', thread: Process.getCurrentThreadId(), objects:objects.length});
      try{ doInvoke(); }catch(e){ send({kind:'invoke_top_error', err:String(e)}); }
    }
  }});
  Interceptor.attach(base.add(APPLIER),{ onEnter:function(a){ counters.applier++; if(!ctx.rdx&&!a[1].isNull()){ctx.rdx=a[1];ctx.r8=a[2];} }});
  Interceptor.attach(base.add(BODY),{ onEnter:function(a){
    counters.body++;
    if(!bodySeen){ bodySeen=true; // one-shot: what is the body actually called with?
      var dr={}; ['0x0','0x8','0x10','0x18','0x20','0x28','0x30','0x38'].forEach(function(o){
        try{dr['rcx'+o]='0x'+a[0].add(o).readPointer().toString();}catch(e){dr['rcx'+o]='NA';} });
      try{dr['rdx_deref']='0x'+a[1].readPointer().toString();}catch(e){dr['rdx_deref']='NA';}
      try{dr['r8_deref']='0x'+a[2].readPointer().toString();}catch(e){dr['r8_deref']='NA';}
      send({kind:'body_entry', rcx:String(a[0]), rdx:String(a[1]), r8:String(a[2]), deref:dr});
    }
  } });
  Interceptor.attach(base.add(PIPELINE),{ onEnter:function(){ counters.pipeline++; } });
  send({kind:'status', msg:'hooks set (thread-correct invoke via poller).'});
  setTimeout(function(){ try{ findObjects(); ready=true;
    send({kind:'objects', count:objects.length, ctx:{rdx:String(ctx.rdx),r8:String(ctx.r8)}, counters_before:counters});
    send({kind:'static_disasm', thunk:disasmAt(base.add(THUNK),8), body_head:disasmAt(base.add(BODY),18)});
    if(!wanted) send({kind:'dry_run', msg:'not enabled; will NOT call.'});
  }catch(e){ send({kind:'setup_err',err:String(e)}); } }, 5000);
}

(function(){
  base=(Process.findModuleByName('iZOzonePro.dll')||{}).base;
  if(base){ var m=Process.findModuleByName('iZOzonePro.dll'); size=m.size; setup(); }
  else var w=setInterval(function(){ var m=Process.findModuleByName('iZOzonePro.dll'); if(m){base=m.base;size=m.size;clearInterval(w);setup();} },100);
})();

rpc.exports={ enableInvoke:function(){ wanted=true; return {wanted:true, ready:ready, objects:objects.length}; } };
