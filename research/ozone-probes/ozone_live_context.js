'use strict';
/* Observe-only live-context capture for the binary-interception path.
 * Hooks the POLLER 0xEAD3E0 (and a couple of harmless read-only anchors) on the
 * live Ozone controller inside OUR OWN OzoneHeadlessHost process.
 * Captures, for the first few poller ticks:
 *   - rcx (the live controller 'this')
 *   - rdx, r8 (live call context)
 *   - the controller's vtable RVA, and the object's first qwords
 *   - a cheap read-only stack backtrace (return addrs that land in the module)
 * NEVER touches the applier 0xEAD930. Pure reads. No NativeFunction calls. */
var POLLER=0xEAD3E0, GETTER=0xD50740, HELPER_A=0xEAD2E0, HELPER_B=0xEAD360;
var PIPELINE=0xE9FC30, THUNK=0xD572F0, BODY=0xD58A20;
var base=null, size=null, n=0, pipelineHits=0, bodyHits=0;
function inMod(p){ try{return !p.isNull()&&p.compare(base)>=0&&p.compare(base.add(size))<0;}catch(e){return false;} }
function rva(p){ try{return inMod(p)?'0x'+p.sub(base).toString(16):String(p);}catch(e){return String(p);} }
function rawByte(p,off){ try{ return '0x'+p.add(off).readU8().toString(16); }catch(e){ return 'NA'; } }
function describeThis(p){
  if(p.isNull()||!p) return {null:true};
  var r={ptr:String(p), inMod:inMod(p)};
  try{
    var v=p.readPointer();
    r.vtable=String(v); r.vtable_inMod=inMod(v); r.vtable_rva=rva(v);
    r.qw=[];
    for(var i=0;i<8;i++){ try{ r.qw.push('0x'+p.add(i*8).readU64().toString(16)); }catch(e){ r.qw.push('NA'); } }
    r.b0=rawByte(p,0); r.b0x90=rawByte(p,0x90); r.b0x98=rawByte(p,0x98); r.b0xa0=rawByte(p,0xa0);
  }catch(e){ r.err=String(e); }
  return r;
}
function captureBacktrace(ctx){
  var bt=[];
  try{
    var sp=ctx.sp;
    for(var i=0;i<0x300 && bt.length<10;i++){
      var v=sp.add(i*8).readPointer();
      if(inMod(v)) bt.push(rva(v));
    }
  }catch(e){}
  return bt;
}
function setup(){
  Interceptor.attach(base.add(POLLER),{ onEnter:function(a){
    n++;
    if(n<=3){
      send({kind:'poller_hit', tick:n, thread:Process.getCurrentThreadId(),
            rcx:describeThis(a[0]),
            rdx:{ptr:String(a[1]), inMod:inMod(a[1])},
            r8 :{ptr:String(a[2]), inMod:inMod(a[2])},
            bt:captureBacktrace(this.context)});
    }
  }});
  Interceptor.attach(base.add(PIPELINE),{ onEnter:function(){ pipelineHits++; } });
  Interceptor.attach(base.add(BODY),{ onEnter:function(){ bodyHits++; } });
  send({kind:'hooks_set', poller:String(base.add(POLLER))});
}
(function(){
  var m=Process.findModuleByName('iZOzonePro.dll');
  if(m){ base=m.base; size=m.size; send({kind:'module', base:String(base), size:size}); setup(); }
  else { var w=setInterval(function(){ var mm=Process.findModuleByName('iZOzonePro.dll'); if(mm){base=mm.base;size=mm.size;clearInterval(w);send({kind:'module',base:String(base),size:size});setup();} },100); }
})();
setTimeout(function(){ send({kind:'summary', poller_ticks:n, pipeline_hits:pipelineHits, body_hits:bodyHits}); }, 12000);
