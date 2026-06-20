#!/usr/bin/env python3
"""T2 multi-objective loss — how close a candidate 72-delta render is to the target.

Every term is measured by RENDERING the candidate through the headless harness
(morephi_render.HeadlessRenderer) then running features.py analyzers on the
rendered output, so the teacher measures the result exactly the way the runtime
does (zero train/serve skew).

L = w_loud*L_loud + w_spec*L_spec + w_crest*L_crest + w_stereo*L_stereo
  + w_tp*L_tp + w_artifact*L_artifact + w_eqsmooth*L_eqsmooth + w_prior*L_prior

  L_loud     ((integrated_lufs(render) - T_lufs)/6)^2
  L_spec     mean_b ((spec_dB(render)[b] - T_spec[b])/12)^2
  L_crest    max(0,(T_crest - crest_db(render))/6)^2        # one-sided (penalize crushed)
  L_stereo   mean_b ((corr_b(render) - T_corr)/0.3)^2 + 10*max(0, min(corr)+0.2)  # mono-safety
  L_tp       max(0, true_peak_dbtp(render) - ceiling)^2 * 5
  L_artifact high-band energy proxy (harshness/alias guardrail)
  L_eqsmooth mean |eq[i+1]-eq[i]| / 0.24                    # musical-curve prior
  L_prior    mean ((delta - x_T1)^2)                         # restraint anchor
"""
from __future__ import annotations

import numpy as np

from features import extract_feature_frame
from target import TargetProfile

WEIGHTS = dict(loud=2.0, spec=1.0, crest=1.0, stereo=1.0, tp=5.0,
               artifact=8.0, eqsmooth=0.5, prior=0.4)


def _to_interleaved(seg) -> np.ndarray:
    a = np.asarray(seg, dtype=np.float32)
    if a.ndim == 1:
        return np.repeat(a, 2).astype(np.float32)
    if a.ndim == 2:
        if a.shape[0] > a.shape[1]:      # [n, ch]
            L, R = a[:, 0], a[:, 1]
        else:                            # [ch, n]
            L, R = a[0], a[1]
        out = np.empty(L.shape[0] * 2, dtype=np.float32)
        out[0::2] = L
        out[1::2] = R
        return out
    raise ValueError(f"bad segment shape {a.shape}")


def _artifact_proxy(rendered: np.ndarray, sr: float) -> float:
    """Harshness/alias guardrail: penalize excessive high-band energy (a proxy for
    the harshness the exciter/limiter can introduce). Phase 2: real THD/PEAQ."""
    mono = rendered.mean(axis=1) if rendered.ndim == 2 else rendered
    n = 1 << int(np.floor(np.log2(max(1024, len(mono)))))
    spec = np.abs(np.fft.rfft(mono[:n]))
    hb = float(np.sum(spec[len(spec) // 4:] ** 2))   # top 3/4 of bands (> ~sr/8)
    tot = float(np.sum(spec ** 2)) + 1e-12
    return float(np.tanh(8.0 * (hb / tot - 0.15)))    # >15% high-band -> rising penalty


def loss(delta72, seg, target: TargetProfile, host, x_t1=None, weights=None) -> tuple[float, dict]:
    w = weights or WEIGHTS
    pcm = _to_interleaved(seg)
    rendered, meters = host.render_candidate(pcm, np.asarray(delta72, dtype=np.float32))

    # features.py expects [channels, samples]; rendered is [n,2] planar -> transpose.
    frame = extract_feature_frame(np.ascontiguousarray(rendered.T), host.sample_rate,
                                  channel_count=2, block_size=512, frame_index=0)

    lufs = meters.lufs_integrated if meters.lufs_integrated > -200.0 else frame.integrated_lufs
    tp = meters.true_peak_dbtp if meters.true_peak_dbtp > -60.0 else frame.true_peak_dbtp

    L = {}
    L["loud"] = ((lufs - target.lufs) / 6.0) ** 2
    sb, ts = list(frame.spectral_bands), target.spec_db
    k = min(len(sb), len(ts))
    L["spec"] = float(np.mean([((sb[b] - ts[b]) / 12.0) ** 2 for b in range(k)])) if k else 0.0
    peak = float(np.max(np.abs(rendered)) + 1e-9)
    rms = float(np.sqrt(np.mean(rendered ** 2)) + 1e-9)
    crest = 20.0 * np.log10(peak / rms)
    L["crest"] = max(0.0, (target.crest_db - crest) / 6.0) ** 2
    corr, tc = list(frame.stereo_correlation), target.corr
    m = min(len(corr), len(tc), 4)
    L["stereo"] = float(np.mean([((corr[b] - tc[b]) / 0.3) ** 2 for b in range(m)])) if m else 0.0
    if m:
        L["stereo"] += 10.0 * max(0.0, min(corr[:m]) + 0.2)   # mono-incompatibility
    L["tp"] = (max(0.0, tp - target.ceiling_dbtp)) ** 2 * 5.0
    L["artifact"] = _artifact_proxy(rendered, host.sample_rate)
    eq = np.asarray(delta72[:32], dtype=np.float32)
    L["eqsmooth"] = float(np.mean(np.abs(np.diff(eq))) / 0.24) if len(eq) > 1 else 0.0
    if x_t1 is not None:
        L["prior"] = float(np.mean((np.asarray(delta72, dtype=np.float32)
                                    - np.asarray(x_t1, dtype=np.float32)) ** 2))
    else:
        L["prior"] = 0.0

    total = sum(w[k] * L[k] for k in WEIGHTS)
    return float(total), L
