#!/usr/bin/env python3
"""Candidate sweep: empirically try each vtable slot + pipeline root against the
isolated hosted-Ozone controller. Records which (if any) completes without an
access violation and produces a non-empty parameter diff.

Empirical complement to the prologue classification (factory/stub/accessor/
destructor/deep-arg-pipeline). If ALL fault, the trigger is not directly
callable from the controller alone — the live DAW context (or a real GUI-path
trace) is required.
"""
import os,sys,time,json,subprocess,frida

MCP=r"G:\More_Phi-vst3-plugin\build\Release\MorePhiMcpServer.exe"
OZONE=r"C:\Program Files\Common Files\VST3\iZotope\Ozone Pro.vst3"
env=dict(os.environ); env["OZONE_VST3_PATH"]=OZONE
p=subprocess.Popen([MCP],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=env,text=True,bufsize=1)
print(f"MCP child PID={p.pid}",flush=True)
def send(o):
    p.stdin.write(json.dumps(o)+"\n"); p.stdin.flush()
    while True:
        l=p.stdout.readline().strip()
        try: return json.loads(l)
        except: continue
send({"jsonrpc":"2.0","method":"initialize","params":{"bearer_token":"t"},"id":1})
def snap(i):
    r=send({"jsonrpc":"2.0","method":"tools/call","params":{"name":"ozone_get_parameters","arguments":{"query":"","include_values":True}},"id":i})
    ps=r.get("result",{}).get("structuredContent",{}).get("parameters",[])
    return {x["index"]:x.get("value") for x in ps if "value" in x}
before=snap(2)
print(f"BEFORE: {len(before)} params",flush=True)

s=frida.attach(p.pid)
out=[];done=[]
def msg(m,d):
    pl=m.get("payload")
    if pl is not None: out.append(pl)
    if m.get("type")=="error": out.append("JSERR "+str(m.get("description")))
    if pl=="DONE": done.append(1)
# For each candidate, resolve a controller, call it with rcx=ctrl (1-arg thiscall).
# vt slots are thiscall on the controller; pipeline 0xe9fc30 takes rcx too.
script=s.create_script(r'''
var ozone=Process.findModuleByName('iZOzonePro.dll');
var rdBase=ozone.base.add(0x0213c000), rdSize=0x00a8c400;
function vtableStart(){
    var slot=Memory.scanSync(rdBase,rdSize,ozone.base.add(0xd572f0).toMatchPattern())[0].address;
    var tl=ozone.base.add(0x1000),th=ozone.base.add(0x1000+0x01e79e00);
    var st=slot; for(var i=0;i<256;i++){var pp=st.sub(8).readPointer(); if(pp.compare(tl)<0||pp.compare(th)>=0)break; st=st.sub(8);}
    return st;
}
function findCtrl(vt){
    var rng=Process.enumerateRanges({protection:'rw-',coalesce:true});
    for(var i=0;i<rng.length;i++){var r=rng[i]; if(r.size>256*1024*1024)continue; try{var res=Memory.scanSync(r.base,r.size,vt.toMatchPattern()); if(res.length)return res[0].address;}catch(e){}}
    return null;
}
var vt=vtableStart();
var cands=[
    ['vt0_0xd57310', 0xd57310],
    ['vt1_0xd1b930', 0xd1b930],
    ['vt3_0xd572e0', 0xd572e0],
    ['vt4_0xd572a0', 0xd572a0],
    ['trig_0xd58a20', 0xd58a20],
    ['pipe_0xe9fc30', 0xe9fc30]
];
for(var c=0;c<cands.length;c++){
    var ctrl=findCtrl(vt);
    if(!ctrl){send('NO_CTRL'); continue;}
    var fnVa=ozone.base.add(cands[c][1]);
    var fn=new NativeFunction(fnVa,'pointer',['pointer']);
    send('CALL '+cands[c][0]+' fn='+fnVa+' ctrl='+ctrl);
    try{ var ret=fn(ctrl); send('  OK ret='+ret); }
    catch(e){ send('  FAULT '+e); }
}
send('DONE');
''')
script.on("message",msg); script.load()
t0=time.time()
while not done and time.time()-t0<120: time.sleep(0.3)
time.sleep(3); s.detach()
after=snap(3)
diff=[(i,before[i],after.get(i)) for i in before if after.get(i)!=before[i]]
print(f"\n===== PARAM DIFF: {len(diff)} changed =====",flush=True)
print("\n===== candidate results =====")
for o in out: print(o)
p.stdin.close(); p.terminate()
try: p.wait(timeout=5)
except: p.kill()
print("\nchild terminated",flush=True)
