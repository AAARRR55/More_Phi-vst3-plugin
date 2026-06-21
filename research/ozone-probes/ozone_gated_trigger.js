'use strict';
/* GATED binary-interception trigger for the Master Assistant analysis-start.
 * Fires the dispatch thunk 0xD572F0 (and, in a second probe, the body 0xD58A20
 * directly) using LIVE context captured from the poller.
 *
 * OUR PROCESS ONLY (OzoneHeadlessHost). NEVER touches applier 0xEAD930.
 *
 * Arg math (verified from disasm):
 *   thunk 0xD572F0: add rcx,8 ; call body 0xD58A20
 *   body 0xD58A20:  mov rdi,rcx ; mov rbx,[rcx] -> rcx must be the controller
 *                   whose [rcx] is the vtable.
 *   => thunk input rcx = controller - 8 (thunk +8 -> controller).
 *   => direct body call rcx = controller.
 *   rdx = live in/out context from poller. r8 = fresh writable buffer.
 *
 * Fire is dispatched from a setInterval (off the poller's stack) to avoid
 * re-entering controller code while the poller holds it; we fire once. */
var THUNK=0xD572F0, BODY=0xD58A20, PIPELINE=0xE9FC30, POLLER=0xEAD3E0, APPLIER=0xEAD930;
var base=null, size=null;
var counters={ poller:0, body:0, pipeline:0 };
var live={ this:null, rdx:null };
var armed=false, fired=false, fireMode='thunk';

function inMod(p){ try{return !p.isNull()&&p.compare(base)>=0&&p.compare(base.add(size))<0;}catch(e){return false;} }

function setup(){
  Interceptor.attach(base.add(POLLER),{ onEnter:function(a){
    counters.poller++;
    if(live.this===null && !a[0].isNull()){
      live.this=a[0];
      live.rdx=a[1];
      send({kind:'captured', this:String(a[0]), rdx:String(a[1]),
            vtable_rva: inMod(a[0].readPointer()) ? '0x'+a[0].readPointer().sub(base).toString(16) : 'NA'});
    }
  }});
  Interceptor.attach(base.add(BODY),{ onEnter:function(){ counters.body++; } });
  Interceptor.attach(base.add(PIPELINE),{ onEnter:function(){ counters.pipeline++; } });
  send({kind:'hooks_set'});
}

function fireOnce(mode){
  if(fired || live.this===null) return;
  fired=true;
  var before={ body:counters.body, pipeline:counters.pipeline };
  var rdxUse = (live.rdx && !live.rdx.isNull()) ? live.rdx : Memory.alloc(0x200);
  var r8buf = Memory.alloc(0x2000); Memory.protect(r8buf,0x2000,'rw-');
  var rcx, entry;
  if(mode==='thunk'){ rcx = live.this.sub(8); entry = base.add(THUNK); }
  else              { rcx = live.this;       entry = base.add(BODY); }
  var fn = new NativeFunction(entry, 'pointer', ['pointer','pointer','pointer']);
  send({kind:'about_to_fire', mode:mode, thread:Process.getCurrentThreadId(),
        rcx:String(rcx), controller:String(live.this), rdx:String(rdxUse), r8:String(r8buf)});
  recv('__flush', function(){}).wait();

  // Run the (possibly faulting/blocking) call on a NEW native thread so the
  // JS thread stays alive to report counters + survival while it executes.
  var done={rv:null, err:null, finished:false};
  var fireThread = new Thread(function(){
    try{ done.rv = fn(rcx, rdxUse, r8buf); }
    catch(e){ done.err = String(e); }
    finally{ done.finished = true; }
  });

  // While the call runs (or until it finishes/crashes), emit live counter beats.
  var beats=0;
  var beat = setInterval(function(){
    beats++;
    send({kind:'beat', mode:mode, beat:beats,
          body:counters.body-before.body, pipeline:counters.pipeline-before.pipeline,
          call_finished: done.finished, call_err: done.err});
    if(done.finished || beats>40){
      clearInterval(beat);
      try{ fireThread.join(); }catch(e){}
      send({kind:'fired_immediate', mode:mode,
            retval: done.rv===null?null:String(done.rv), error:done.err,
            delta_immediate:{ body: counters.body-before.body, pipeline: counters.pipeline-before.pipeline }});
    }
  }, 500);
}

// The fire loop: once armed + captured, fire from the JS thread (off poller stack).
function fireLoop(){
  if(armed && !fired && live.this){
    try{ fireOnce(fireMode); }catch(e){ send({kind:'fire_top_error', err:String(e)}); }
    return;
  }
  setTimeout(fireLoop, 50);
}

(function(){
  var m=Process.findModuleByName('iZOzonePro.dll');
  if(m){ base=m.base; size=m.size; send({kind:'module', base:String(base), size:size}); setup(); }
  else { var w=setInterval(function(){ var mm=Process.findModuleByName('iZOzonePro.dll'); if(mm){base=mm.base;size=mm.size;clearInterval(w);send({kind:'module',base:String(base),size:size});setup();} },100); }
})();

rpc.exports={
  arm: function(mode){ fireMode = (mode==='body') ? 'body' : 'thunk'; armed=true; setTimeout(fireLoop, 50); return { armed:true, mode:fireMode, this_captured: live.this!==null }; },
  status: function(){ return { counters:counters, live_this: String(live.this), armed:armed, fired:fired }; },
  // driver calls this to ack the flush so recv().wait() unblocks
  flush: function(){ try{ send('__flush', {}); }catch(e){} return true; }
};
