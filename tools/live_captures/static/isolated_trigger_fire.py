#!/usr/bin/env python3
"""Isolated trigger FIRE: host Ozone headless, resolve the controller, call the
trigger thunk 0xd572f0 with rcx=controller, capture before/after Ozone params.

No DAW is involved; a crash only kills this throwaway MCP child.
Proof of success: a non-empty before/after hosted-parameter diff.
"""
import os, sys, time, json, subprocess, frida

MCP_EXE = r"G:\More_Phi-vst3-plugin\build\Release\MorePhiMcpServer.exe"
OZONE = r"C:\Program Files\Common Files\VST3\iZotope\Ozone Pro.vst3"
env = dict(os.environ)
env["OZONE_VST3_PATH"] = OZONE

p = subprocess.Popen([MCP_EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT, env=env, text=True, bufsize=1)
print(f"MCP child PID={p.pid}", flush=True)
drained = []
def send(obj):
    p.stdin.write(json.dumps(obj) + "\n"); p.stdin.flush()
    # skip non-JSON discovery log lines, return first real JSON line
    while True:
        line = p.stdout.readline().strip()
        if not line:
            continue
        try:
            return json.loads(line)
        except json.JSONDecodeError:
            drained.append(line)  # discovery log junk
            continue

send({"jsonrpc":"2.0","method":"initialize","params":{"bearer_token":"test"},"id":1})

def snapshot_params(label):
    r = send({"jsonrpc":"2.0","method":"tools/call","params":{
        "name":"ozone_get_parameters","arguments":{"query":"","include_values":True}},"id":label})
    sc = r.get("result",{}).get("structuredContent",{})
    params = sc.get("parameters",[])
    return {pp["index"]: pp.get("value") for pp in params if "value" in pp}

before = snapshot_params(2)
print(f"BEFORE: {len(before)} params captured", flush=True)

# Attach Frida to this child, resolve controller, call trigger, watch phase.
session = frida.attach(p.pid)
out=[]; done=[]
def msg(m,d):
    pl=m.get("payload")
    if pl is not None: out.append(pl)
    if m.get("type")=="error": out.append("JSERR "+str(m.get("description")))
    if pl=="DONE": done.append(1)
script = session.create_script(r"""
var ozone = Process.findModuleByName('iZOzonePro.dll');
var thunkVa = ozone.base.add(0xd572f0);
var rdBase = ozone.base.add(0x0213c000), rdSize=0x00a8c400;
var slot = Memory.scanSync(rdBase, rdSize, thunkVa.toMatchPattern())[0].address;
var textLo=ozone.base.add(0x1000), textHi=ozone.base.add(0x1000+0x01e79e00);
var start=slot; for(var i=0;i<256;i++){var pp=start.sub(8).readPointer(); if(pp.compare(textLo)<0||pp.compare(textHi)>=0)break; start=start.sub(8);}
var ranges=Process.enumerateRanges({protection:'rw-',coalesce:true});
var ctrl=null;
for(var i=0;i<ranges.length && !ctrl;i++){var r=ranges[i]; if(r.size>256*1024*1024)continue; try{var res=Memory.scanSync(r.base,r.size,start.toMatchPattern()); if(res.length) ctrl=res[0].address;}catch(e){}}
send('controller='+ctrl);
if(!ctrl){ send('NO_CONTROLLER'); send('DONE'); }
else {
    // Microsoft x64: call the thunk with rcx=ctrl. The thunk does add rcx,8 then calls 0xd58a20.
    // We invoke on a fresh Frida thread (no DAW audio thread dependency).
    var thunk = new NativeFunction(thunkVa, 'pointer', ['pointer']);
    send('calling trigger thunk...');
    try {
        var ret = thunk(ctrl);
        send('TRIGGER_RETURNED ret='+ret);
    } catch(e) {
        send('TRIGGER_THREW '+e);
    }
    // give the analysis a moment
    send('DONE');
}
""")
script.on("message", msg)
script.load()
t0=time.time()
while not done and time.time()-t0<60: time.sleep(0.2)

# wait a bit for any async analysis to touch params
time.sleep(5)
session.detach()

after = snapshot_params(3)
print(f"AFTER: {len(after)} params captured", flush=True)

# diff
changed = [(i, before[i], after[i]) for i in before if i in after and before[i] != after[i]]
print(f"\n===== PARAMETER DIFF: {len(changed)} changed =====")
for i,b,a in changed[:40]:
    print(f"  param[{i}]: {b} -> {a}")
print("\n===== phase/trigger messages =====")
for o in out: print(o)

p.stdin.close(); p.terminate()
try: p.wait(timeout=5)
except: p.kill()
print("\nchild terminated", flush=True)
