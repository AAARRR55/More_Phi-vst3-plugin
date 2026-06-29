#!/usr/bin/env python3
"""Isolated trigger probe: host Ozone in MorePhiMcpServer, attach Frida to that
SAME child process, resolve the Master Assistant controller instance, and report
whether the trigger object exists in the isolated (no-DAW) process.

All in one script so nothing relies on cross-call process persistence.
No trigger CALL yet — this is the resolution+validation step (read-only on the
hosted Ozone). A crash here only kills this throwaway MCP child.
"""
import os, sys, time, json, subprocess, frida

MCP_EXE = r"G:\More_Phi-vst3-plugin\build\Release\MorePhiMcpServer.exe"
OZONE = r"C:\Program Files\Common Files\VST3\iZotope\Ozone Pro.vst3"

env = dict(os.environ)
env["OZONE_VST3_PATH"] = OZONE
env["OZONE_DISCOVERY_VERBOSE"] = "1"

p = subprocess.Popen([MCP_EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT, env=env, text=True, bufsize=1)
print(f"MCP child PID={p.pid}", flush=True)

def send(obj):
    p.stdin.write(json.dumps(obj) + "\n"); p.stdin.flush()
    return p.stdout.readline().strip()

init = send({"jsonrpc":"2.0","method":"initialize","params":{"bearer_token":"test"},"id":1})
pr = send({"jsonrpc":"2.0","method":"tools/call","params":{"name":"ozone_get_parameters",
        "arguments":{"query":"master","include_values":False}},"id":2})
try:
    pj = json.loads(pr)
    sc = pj.get("result",{}).get("structuredContent",{})
    print(f"Ozone loaded={sc.get('plugin',{}).get('loaded')} params={sc.get('count')}", flush=True)
except Exception as e:
    print("parse issue:", pr[:120], flush=True)

# Now attach Frida to THIS child (same process Ozone lives in) and resolve controller.
session = frida.attach(p.pid)
out=[]
def on_msg(m,d):
    if m.get("type")=="send": out.append(m["payload"])
    elif m.get("type")=="error": out.append("JSERR "+str(m.get("description")))
script = session.create_script(r"""
var ozone = Process.findModuleByName('iZOzonePro.dll');
if(!ozone){ send('OZONE_NOT_LOADED'); send('DONE'); }
else {
    send('ozone base='+ozone.base+' size='+ozone.size);
    var thunkVa = ozone.base.add(0xd572f0);
    var rdBase = ozone.base.add(0x0213c000); var rdSize = 0x00a8c400;
    var slots = Memory.scanSync(rdBase, rdSize, thunkVa.toMatchPattern());
    send('vtable_slots='+slots.length);
    if(slots.length){
        var slot=slots[0].address;
        var textLo=ozone.base.add(0x1000), textHi=ozone.base.add(0x1000+0x01e79e00);
        var start=slot; for(var i=0;i<256;i++){var pp=start.sub(8).readPointer(); if(pp.compare(textLo)<0||pp.compare(textHi)>=0)break; start=start.sub(8);}
        send('vtable_start='+start);
        var ranges=Process.enumerateRanges({protection:'rw-',coalesce:true});
        var inst=[];
        for(var i=0;i<ranges.length;i++){var r=ranges[i]; if(r.size>256*1024*1024)continue; try{var res=Memory.scanSync(r.base,r.size,start.toMatchPattern()); for(var j=0;j<res.length;j++)inst.push(res[j].address.toString());}catch(e){}}
        send('controller_instances='+inst.length);
        for(var k=0;k<Math.min(inst.length,8);k++) send('  CTRL '+inst[k]);
        // also check the [+8] sub-object vtable: does any object's vptr point to a vtable whose [2]=thunk? (already covered by start scan)
    }
    send('DONE');
}
""")
done=[]
def msg2(m,d):
    pl=m.get("payload")
    out.append(pl)
    if pl=="DONE": done.append(1)
    if m.get("type")=="error": out.append("JSERR "+str(m.get("description"))); done.append(1)
script.on("message", msg2)
script.load()
t0=time.time()
while not done and time.time()-t0<120: time.sleep(0.3)
session.detach()
for o in out: print(o)

p.stdin.close(); p.terminate()
try: p.wait(timeout=5)
except: p.kill()
print("child terminated", flush=True)
