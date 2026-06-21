#!/usr/bin/env python3
"""Isolated trigger v2: feed audio through Ozone FIRST (so SmoothAudioDataStream
+ analysis objects are wired), then call the trigger thunk. Retries the trigger
that faulted in v1, now with the audio-stream precondition satisfied."""
import os,sys,time,json,subprocess,frida

MCP=r"G:\More_Phi-vst3-plugin\build\Release\MorePhiMcpServer.exe"
OZONE=r"C:\Program Files\Common Files\VST3\iZotope\Ozone Pro.vst3"
AUDIO=r"G:\More_Phi-vst3-plugin\scripts\neural-mastering\control\data\ssl_eval_staging\input_00.wav"
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
# 1) capture before
def snap(i):
    r=send({"jsonrpc":"2.0","method":"tools/call","params":{"name":"ozone_get_parameters","arguments":{"query":"","include_values":True}},"id":i})
    ps=r.get("result",{}).get("structuredContent",{}).get("parameters",[])
    return {x["index"]:x.get("value") for x in ps if "value" in x}
before=snap(2)
print(f"BEFORE: {len(before)} params",flush=True)

# 2) FEED AUDIO through Ozone to wire the internal streams
print("feeding audio through Ozone (renderInputAudio)...",flush=True)
r=send({"jsonrpc":"2.0","method":"tools/call","params":{"name":"ozone_run_master_assistant","arguments":{"input_audio_path":AUDIO,"analysis_seconds":20}},"id":3})
sc=r.get("result",{}).get("structuredContent",{})
print("render result:",json.dumps(sc)[:200],flush=True)

# 3) NOW resolve controller + call trigger
s=frida.attach(p.pid)
out=[];done=[]
def msg(m,d):
    pl=m.get("payload")
    if pl is not None: out.append(pl)
    if m.get("type")=="error": out.append("JSERR "+str(m.get("description")))
    if pl=="DONE": done.append(1)
script=s.create_script(r'''
var ozone=Process.findModuleByName('iZOzonePro.dll');
var thunkVa=ozone.base.add(0xd572f0);
var slot=Memory.scanSync(ozone.base.add(0x0213c000),0x00a8c400,thunkVa.toMatchPattern())[0].address;
var tl=ozone.base.add(0x1000),th=ozone.base.add(0x1000+0x01e79e00);
var st=slot; for(var i=0;i<256;i++){var pp=st.sub(8).readPointer(); if(pp.compare(tl)<0||pp.compare(th)>=0)break; st=st.sub(8);}
var rng=Process.enumerateRanges({protection:'rw-',coalesce:true}); var ctrls=[];
for(var i=0;i<rng.length;i++){var r=rng[i]; if(r.size>256*1024*1024)continue; try{var res=Memory.scanSync(r.base,r.size,st.toMatchPattern()); for(var j=0;j<res.length;j++)ctrls.push(res[j].address);}catch(e){}}
send('controllers='+ctrls.length);
var thunk=new NativeFunction(thunkVa,'pointer',['pointer']);
for(var k=0;k<ctrls.length;k++){
    send('TRY ctrl='+ctrls[k]);
    try{ var ret=thunk(ctrls[k]); send('  OK ret='+ret); }
    catch(e){ send('  THREW '+e); }
}
send('DONE');
''')
script.on("message",msg); script.load()
t0=time.time()
while not done and time.time()-t0<90: time.sleep(0.2)
time.sleep(4); s.detach()

after=snap(4)
print(f"AFTER: {len(after)} params",flush=True)
diff=[(i,before[i],after.get(i)) for i in before if after.get(i)!=before[i]]
print(f"\n===== PARAM DIFF: {len(diff)} changed =====",flush=True)
for i,b,a in diff[:40]: print(f"  param[{i}]: {b} -> {a}")
print("\n===== trigger log =====")
for o in out: print(o)
p.stdin.close(); p.terminate()
try: p.wait(timeout=5)
except: p.kill()
print("\nchild terminated",flush=True)
