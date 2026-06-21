# SonicMaster Inference Server

Local Python bridge that hosts the `masteringbrainv2` checkpoint and serves
mastering decisions to the More-Phi plugin over HTTP. This is the path that
works **today** while the checkpoint's faithful ONNX export remains an open
problem (the model uses fused-kernel transformer/MHA ops + an FFT-based
spectral injection that torch's ONNX exporter cannot trace).

## Why this exists

The plugin's realtime path is built around `ISonicMasterInferenceSource`. Two
implementations exist:

1. **`SonicMasterRunnerInferenceSource`** — runs an ONNX model in-process.
   *Not usable yet* because the checkpoint won't export faithfully to ONNX.
2. **`SonicMasterHttpInferenceSource`** — POSTs a 6 s stereo window to this
   server and parses the 44-float decision back. **Default; works today.**

The plugin defaults to the HTTP source. If the server isn't reachable the
"Neural Master" toggle reads "unavailable (no model)" and the feature is a
no-op. Start this server to enable it.

## Prerequisites

- Python 3.11+
- The unpacked `sonicmaster-v3-decision-engine` package (carries the checkpoint
  and the `master_audio` loader)
- Python deps: `torch`, `lightning`, `torchaudio`, `pyloudnorm`, `soundfile`,
  `jsonschema`, `fastapi`, `uvicorn`, `numpy`

```bash
pip install torch lightning torchaudio pyloudnorm soundfile jsonschema fastapi uvicorn numpy
```

## Run

```bash
python tools/inference_server/server.py \
    --package "C:/Users/HP/Downloads/sonicmaster-v3-decision-engine-20260530T121536Z" \
    --host 127.0.0.1 --port 8765
```

The plugin expects the default `127.0.0.1:8765`. The server logs the model
architecture + decision width once it loads, then listens. Leave it running in
the background while you use the plugin.

## API

| Method | Path | Body / Query | Returns |
|---|---|---|---|
| GET | `/health` | — | `{"status":"ok"}` |
| GET | `/status` | — | `{"loaded":bool,"architecture":str,"sampleRate":44100,"decisionWidth":44,"segmentFrames":262138,"lastError":str\|null}` |
| POST | `/infer?target_lufs=<float>` | raw little-endian float32 interleaved stereo, `2 × 262138 × 4` bytes | `{"decision":[44 floats],"inference_ms":float}` |

The `/infer` body is raw float32 (not JSON) so a ~2 MB payload stays cheap. The
response is JSON so the plugin can parse it with its existing JSON helpers.

## Quick sanity check

```bash
# Server up?
curl http://127.0.0.1:8765/health

# POST a synthetic 6 s signal (Python one-liner) and see the decision:
python -c "
import numpy as np, urllib.request, json
n=262138; t=np.arange(n,dtype=np.float32)/44100
mono=0.25*np.sin(2*np.pi*55*t)
body=np.stack([mono,mono]).astype(np.float32).tobytes()
req=urllib.request.Request('http://127.0.0.1:8765/infer?target_lufs=-14',data=body,method='POST')
print(json.loads(urllib.request.urlopen(req,timeout=60).read())['decision'][:11])
"
```

A sane response has `decision[8]` (target LUFS) near −14 and `decision[9]`
(true-peak ceiling) near −1.

## How the plugin uses it

The analysis engine (`src/AI/SonicMasterAnalysisEngine.cpp`) wakes every 3 s,
captures the latest 6 s of audio, calls this source's `infer()` (which POSTs
here), decodes the 44-float decision via `decodeSonicMasterDecision`, validates
it through the safety policy, and ramps the result into the built-in DSP chain.
The ~300 ms CPU inference is well within the 3 s cycle. See
`docs/superpowers/specs/2026-06-21-sonicmaster-vst3-realtime-integration-design.md`.

## Notes

- This is a research-grade, preview path. The checkpoint failed several of its
  own release gates; treat the plugin's "Neural Master" toggle as an assistant.
- Security: bind to `127.0.0.1` only. Do not expose this server on a public
  interface — the endpoints are unauthenticated and accept large bodies.
