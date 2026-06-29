"""SonicMaster inference server — local PyTorch bridge for the More-Phi plugin.

The plugin's realtime path can't (yet) load this checkpoint as ONNX (the model
uses fused-kernel ops torch's exporter can't trace). This server runs the
checkpoint directly via PyTorch — the exact inference path it was validated
with — and exposes a tiny HTTP API the plugin's SonicMasterHttpInferenceSource
calls from its analysis thread.

Run:

    python tools/inference_server/server.py \\
        --package "C:/Users/HP/Downloads/sonicmaster-v3-decision-engine-20260530T121536Z"

Endpoints (127.0.0.1:8765 by default):

    GET  /health              -> {"status":"ok"}
    GET  /status              -> model + server metadata
    POST /infer?target_lufs=  -> body = raw little-endian float32 interleaved
                                 stereo (2*262138*4 bytes); returns
                                 {"decision":[44 floats],"inference_ms":...}

The body is raw float32 (not JSON) to keep a ~2 MB payload cheap. The response
is JSON so the plugin can parse it with its existing JSON helpers.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import sys
import time
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any

import numpy as np
import torch
from fastapi import FastAPI, HTTPException, Query, Request
from fastapi.responses import JSONResponse
import uvicorn

SEGMENT_FRAMES = 262138
DECISION_WIDTH = 44
DEFAULT_TARGET_LUFS = -14.0


class ModelState:
    """Holds the loaded Lightning module + config; populated at startup."""

    def __init__(self) -> None:
        self.module = None
        self.output_width = DECISION_WIDTH
        self.sample_rate = 44100
        self.architecture = "unknown"
        self.loaded = False
        self.last_error: str | None = None


STATE = ModelState()


def load_model(package_root: Path) -> None:
    ckpt = package_root / "models" / "v3" / "mastering-brain-v2-fullchain-best" / "checkpoints" / "best.ckpt"
    if not ckpt.exists():
        raise FileNotFoundError(f"checkpoint not found: {ckpt}")
    sys.path.insert(0, str(package_root / "training" / "neural-mastering" / "bin" / "training"))
    from master_audio import load_module_from_checkpoint  # type: ignore

    module, hparams = load_module_from_checkpoint(ckpt, torch.device("cpu"))
    module.eval()
    STATE.module = module
    STATE.output_width = int(module.model_config.mastering_decision_head.output_width)
    STATE.sample_rate = int(module.model_config.sample_rate)
    STATE.architecture = str(hparams.get("modelConfig", {}).get("architectureName", "unknown"))
    STATE.loaded = True


def infer(waveform_np: np.ndarray, target_lufs: float) -> np.ndarray:
    """Run one decision inference. waveform_np: float32 [2, SEGMENT_FRAMES]."""
    if not STATE.loaded or STATE.module is None:
        raise RuntimeError("model not loaded")
    wav = torch.from_numpy(waveform_np.astype(np.float32)).unsqueeze(0)  # [1,2,N]
    tl = torch.tensor([float(target_lufs)], dtype=torch.float32)
    with torch.no_grad():
        # MasteringDecisionNet caches the decision on network._last_mastering_decisions;
        # the Lightning module's forward returns the waveform. Mirror the official
        # run_decision_inference._collect_decision_predictions extraction.
        ret = STATE.module(wav, target_lufs_db=tl)
        if ret is not None and ret.ndim == 2 and int(ret.shape[-1]) == STATE.output_width:
            decision = ret.detach().cpu().numpy().reshape(-1)
        else:
            cached = getattr(STATE.module.network, "_last_mastering_decisions", None)
            if cached is None:
                raise RuntimeError("model produced no decision tensor")
            decision = cached.detach().cpu().numpy().reshape(-1)
    if decision.size < STATE.output_width:
        raise RuntimeError(f"decision width {decision.size} < {STATE.output_width}")
    return decision[:STATE.output_width]


def decode_interleaved_stereo(samples: np.ndarray) -> np.ndarray:
    """Convert [L0,R0,L1,R1,...] float samples to contiguous [2, frames]."""
    samples = np.asarray(samples, dtype=np.float32)
    expected_samples = 2 * SEGMENT_FRAMES
    if samples.size < expected_samples:
        raise ValueError(f"not enough samples: {samples.size}, need {expected_samples}")

    frames_lr = samples[:expected_samples].reshape(SEGMENT_FRAMES, 2)
    return np.ascontiguousarray(frames_lr.T)


@asynccontextmanager
async def lifespan(app: FastAPI):
    yield
    # nothing to tear down beyond Python GC


app = FastAPI(title="SonicMaster Inference Server", version="1.0.0", lifespan=lifespan)


@app.get("/health")
async def health() -> dict[str, Any]:
    return {"status": "ok"}


@app.get("/status")
async def status() -> dict[str, Any]:
    return {
        "loaded": STATE.loaded,
        "architecture": STATE.architecture,
        "sampleRate": STATE.sample_rate,
        "decisionWidth": STATE.output_width,
        "segmentFrames": SEGMENT_FRAMES,
        "lastError": STATE.last_error,
    }


@app.post("/infer")
async def infer_endpoint(request: Request,
                         target_lufs: float = Query(default=DEFAULT_TARGET_LUFS)) -> JSONResponse:
    if not STATE.loaded:
        raise HTTPException(status_code=503, detail="model not loaded")
    try:
        raw = await request.body()
        expected = 2 * SEGMENT_FRAMES * 4  # float32, interleaved stereo
        if len(raw) < expected:
            raise HTTPException(
                status_code=400,
                detail=f"body too short: {len(raw)} bytes, need {expected}")
        samples = np.frombuffer(raw[:expected], dtype="<f4").astype(np.float32)
        # Interleaved L,R,L,R -> channels-first [2, SEGMENT_FRAMES].
        waveform = decode_interleaved_stereo(samples)
        t0 = time.perf_counter()
        decision = infer(waveform, target_lufs)
        elapsed_ms = (time.perf_counter() - t0) * 1000.0
        return JSONResponse({
            "decision": [float(x) for x in decision],
            "inference_ms": elapsed_ms,
        })
    except HTTPException:
        raise
    except Exception as exc:
        STATE.last_error = str(exc)
        raise HTTPException(status_code=500, detail=str(exc))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--package", type=Path, required=True,
                    help="sonicmaster-v3-decision-engine package root")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    args = ap.parse_args()

    print(f"[server] loading model from {args.package} ...", flush=True)
    try:
        load_model(args.package)
    except Exception as exc:
        print(f"[server] FATAL: could not load model: {exc}", flush=True)
        return 1
    print(f"[server] model loaded: {STATE.architecture}, "
          f"decisionWidth={STATE.output_width}, SR={STATE.sample_rate}", flush=True)
    print(f"[server] listening on http://{args.host}:{args.port}", flush=True)
    uvicorn.run(app, host=args.host, port=args.port, log_level="warning")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
