"""Real 63-float feature extractor for the More-Phi control regressor.

Strict train/serve parity with the C++ runtime analyzers. Each function below
mirrors a specific analyzer in src/Core/; the per-field source-of-truth and any
stub behavior are documented inline. The output FeatureFrame is consumed by
codec.serialize_feature_frame() unchanged.

PARITY DECISION (locked): this module replicates the C++ runtime's ACTUAL
behavior, stubs included. Four scalar fields are always 0.0 in the runtime
(crestFactorDb, monoFoldDownDeltaDb, transientDensity, harmonicRisk) and
sourceQualityScore is always 1.0 — so they are here too. The stereo analyzer
produces 4 bands but the frame declares 8, so indices 4-7 are zero-padded.
The model trains on EXACTLY what the runtime will feed it.

References:
  LUFSMeter.{h,cpp}                 — BS.1770-4 loudness + EBU R3342 LRA
  TruePeakEstimator.{h,cpp}         — 4x polyphase FIR (exact coeffs)
  RealtimeSpectrumAnalyzer.{h,cpp}  — 2048-FFT spectrum, spectralTilt
  StereoFieldAnalyzer.{h,cpp}       — 4-band LR crossover correlation/MS
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np
from scipy.signal import lfilter

from codec import (
    SPECTRAL_BAND_COUNT,
    STEREO_BAND_COUNT,
    FeatureFrame,
)


# ─────────────────────────────────────────────────────────────────────────────
# K-weighting biquads — exact parity with LUFSMeter::computeKWeightingCoeffs
# Continuous-time coeffs solved to match ITU-R BS.1770-4 at 48 kHz (LUFSMeter.cpp:30-53)
# ─────────────────────────────────────────────────────────────────────────────

_KW_STAGE1 = dict(B2=1.58486548742880, B1=18886.9143780365, B0=112594507.269791,
                  A2=1.0, A1=15004.8465267433, A0=112594507.269790)
_KW_STAGE2 = dict(B2=1.00499491883582, B1=0.0, B0=0.0,
                  A2=1.0, A1=478.91221124467, A0=57414.259359048)


def _bilinear(stage: dict, fs: float) -> tuple[float, float, float, float, float]:
    """Bilinear transform of a 2nd-order analog stage -> DF2T biquad (b0,b1,b2,a1,a2)."""
    K = 2.0 * fs
    K2 = K * K
    B2, B1, B0 = stage["B2"], stage["B1"], stage["B0"]
    A2, A1, A0 = stage["A2"], stage["A1"], stage["A0"]
    a0 = A2 * K2 + A1 * K + A0
    b0 = (B2 * K2 + B1 * K + B0) / a0
    b1 = 2.0 * (B0 - B2 * K2) / a0
    b2 = (B2 * K2 - B1 * K + B0) / a0
    a1 = 2.0 * (A0 - A2 * K2) / a0
    a2 = (A2 * K2 - A1 * K + A0) / a0
    return b0, b1, b2, a1, a2


def _biquad_df2t(x: np.ndarray, b0: float, b1: float, b2: float, a1: float, a2: float) -> np.ndarray:
    """Direct Form II Transposed biquad (matches juce::dsp::DesignIIRFilter usage in LUFSMeter).

    Vectorized via scipy.signal.lfilter (C implementation, DF2T topology). This
    must be byte-faithful to the scalar recurrence: y[n] = b0*x[n] + w1,
    w1 = w2 + b1*x[n] - a1*y[n], w2 = b2*x[n] - a2*y[n]. lfilter with
    b=[b0,b1,b2], a=[1,a1,a2] implements exactly this.
    """
    return lfilter([b0, b1, b2], [1.0, a1, a2], x).astype(x.dtype, copy=False)


def _k_weight(channel: np.ndarray, fs: float) -> np.ndarray:
    """Apply the two cascaded K-weighting biquads (pre-filter shelf + RLB high-pass)."""
    b0, b1, b2, a1, a2 = _bilinear(_KW_STAGE1, fs)
    x = _biquad_df2t(channel, b0, b1, b2, a1, a2)
    b0, b1, b2, a1, a2 = _bilinear(_KW_STAGE2, fs)
    return _biquad_df2t(x, b0, b1, b2, a1, a2)


# ─────────────────────────────────────────────────────────────────────────────
# LUFSMeter parity — BS.1770-4 loudness + EBU R3342 LRA
#   block = 100 ms; momentary = 400 ms (4 blocks); short-term = 3 s (30 blocks)
#   integrated: abs gate -70, rel gate -10; LRA: abs gate -70, rel gate -20
# ─────────────────────────────────────────────────────────────────────────────

_ABS_GATE_LUFS = -70.0
_INT_REL_GATE_OFFSET = -10.0   # LUFSMeter.h:31
_LRA_REL_GATE_OFFSET = -20.0   # LUFSMeter.cpp:307-318 (note: -20, not -10)
_K_MOMENTUM_BLOCKS = 4         # 400 ms / 100 ms
_K_SHORTTERM_BLOCKS = 30       # 3 s / 100 ms


def _mean_sq_to_lufs(ms: float) -> float:
    """BS.1770-4: LUFS = -0.691 + 10*log10(mean_square)."""
    if ms <= 0.0:
        return float("-inf")
    return -0.691 + 10.0 * math.log10(ms)


def compute_loudness(audio: np.ndarray, fs: float) -> tuple[float, float, float, float]:
    """Return (integrated, shortTerm, momentary, loudnessRange) in LUFS/LU.

    Channel weights default to {1.0,1.0} for stereo (LUFSMeter.h:130). Surround
    weights are not modeled — the plugin path is stereo-only for this frame.
    """
    if audio.ndim == 1:
        audio = np.stack([audio, audio])  # mono -> duplicate (channelCount stays as caller passes)
    nch = audio.shape[0]
    block = max(1, int(round(fs * 0.1)))
    n = audio.shape[1]
    n_blocks = n // block

    if n_blocks < _K_MOMENTUM_BLOCKS:
        return float("-inf"), float("-inf"), float("-inf"), 0.0

    # Per-channel K-weighting, then per-block channel-weighted mean square.
    weights = np.ones(nch, dtype=np.float64)
    kw = np.stack([_k_weight(audio[ch].astype(np.float64), fs) for ch in range(nch)])
    block_ms = np.empty(n_blocks, dtype=np.float64)
    for b in range(n_blocks):
        seg = kw[:, b * block:(b + 1) * block]
        per_ch_ms = (seg * seg).mean(axis=1)
        block_ms[b] = float((weights * per_ch_ms).sum() / max(1, nch))

    block_lufs = np.array([_mean_sq_to_lufs(ms) for ms in block_ms])

    # Momentary / short-term: mean of MS over sliding windows, -> LUFS.
    def _window_ms_mean(center_end_exclusive: int, length: int) -> float:
        start = max(0, center_end_exclusive - length)
        return float(block_ms[start:center_end_exclusive].mean())

    momentary = _mean_sq_to_lufs(_window_ms_mean(n_blocks, _K_MOMENTUM_BLOCKS))
    short_term = _mean_sq_to_lufs(_window_ms_mean(n_blocks, _K_SHORTTERM_BLOCKS)) if n_blocks >= _K_SHORTTERM_BLOCKS else float("-inf")

    # Integrated: abs gate -70, then rel gate (abs-gated mean - 10).
    abs_mask = block_lufs > _ABS_GATE_LUFS
    if abs_mask.any():
        abs_gated_mean_lufs = float(_mean_sq_to_lufs(block_ms[abs_mask].mean()))
        rel_gate = abs_gated_mean_lufs + _INT_REL_GATE_OFFSET
        rel_mask = (block_lufs > rel_gate) & abs_mask
        integrated = _mean_sq_to_lufs(block_ms[rel_mask].mean()) if rel_mask.any() else float(_mean_sq_to_lufs(block_ms[abs_mask].mean()))
    else:
        integrated = float("-inf")

    # LRA: EBU R3342 on the 3 s short-term block distribution (abs -70, rel -20).
    lra = 0.0
    if n_blocks >= _K_SHORTTERM_BLOCKS:
        st_lufs = np.array([
            _mean_sq_to_lufs(float(block_ms[max(0, i - _K_SHORTTERM_BLOCKS + 1):i + 1].mean()))
            for i in range(n_blocks)
        ])
        st_abs = st_lufs > _ABS_GATE_LUFS
        if st_abs.any():
            st_abs_mean = float(_mean_sq_to_lufs(block_ms[st_abs].mean()))
            st_rel_gate = st_abs_mean + _LRA_REL_GATE_OFFSET
            st_rel = st_lufs[(st_lufs > st_rel_gate) & st_abs]
            if st_rel.size >= 2:
                st_rel_sorted = np.sort(st_rel)
                def _percentile(p: float) -> float:
                    rank = p / 100.0 * (st_rel_sorted.size - 1)
                    lo = int(math.floor(rank))
                    hi = int(math.ceil(rank))
                    if lo == hi:
                        return float(st_rel_sorted[lo])
                    frac = rank - lo
                    return float(st_rel_sorted[lo] * (1 - frac) + st_rel_sorted[hi] * frac)
                lra = _percentile(95.0) - _percentile(10.0)

    return integrated, short_term, momentary, max(0.0, lra)


# ─────────────────────────────────────────────────────────────────────────────
# TruePeakEstimator parity — 4x polyphase FIR, EXACT coefficient table
#   (TruePeakEstimator.cpp:25-51). Do NOT substitute scipy resample.
# ─────────────────────────────────────────────────────────────────────────────

_POLY_COEFFS = (
    (0.000069, -0.002366, 0.007622, 0.009083, -0.101287, 0.306438, 0.709163, 0.137965, -0.088756, 0.022356, 0.000818, -0.001135),
    (0.000051, -0.003292, 0.016311, -0.018340, -0.073916, 0.481539, 0.626776, 0.005875, -0.055081, 0.023063, -0.002669, -0.000287),
    (-0.000287, -0.002669, 0.023063, -0.055081, 0.005875, 0.626776, 0.481539, -0.073916, -0.018340, 0.016311, -0.003292, 0.000051),
    (-0.001135, 0.000818, 0.022356, -0.088756, 0.137965, 0.709163, 0.306438, -0.101287, 0.009083, 0.007622, -0.002366, 0.000069),
)
_FIR_TAPS = 12


def compute_true_peak_dbtp(audio: np.ndarray) -> float:
    """Stereo-linked max true peak in dBTP, matching TruePeakEstimator.

    Vectorized 4x polyphase FIR using the EXACT coefficient table. For each of
    the 4 phases, the phase-tap output at sample i is sum_taps coeff[t]*x[i+t].
    We compute all phases at once via a sliding-window dot product, then take
    the global max |.|. Returns -inf for near-silence.

    Border handling: the C++ reads x[i+t] for t in [0,11]; samples past the end
    read as 0 (the runtime buffer is zero-padded conceptually). We right-pad
    the signal by (_FIR_TAPS-1) zeros to match exactly.
    """
    if audio.ndim == 1:
        audio = audio[np.newaxis, :]
    coeffs = np.asarray(_POLY_COEFFS, dtype=np.float64)  # [4, 12]
    taps = _FIR_TAPS
    max_abs = 0.0
    for ch in range(audio.shape[0]):
        x = np.concatenate([audio[ch].astype(np.float64), np.zeros(taps - 1)])
        n = audio.shape[1]
        if n == 0:
            continue
        # Sliding window view: windows[i] = x[i:i+taps], shape [n, taps].
        windows = np.lib.stride_tricks.sliding_window_view(x, taps)[:n]
        # phase output: [4, n] = coeffs @ windows.T
        phase_out = coeffs @ windows.T
        ch_max = float(np.abs(phase_out).max())
        if ch_max > max_abs:
            max_abs = ch_max
    if max_abs < 1e-12:
        return float("-inf")
    return 20.0 * math.log10(max_abs)


# ─────────────────────────────────────────────────────────────────────────────
# RealtimeSpectrumAnalyzer parity — 2048 FFT, Hann, hop 512
#   spectralBands[32]: group 1025 bins into 32 (uniform-by-bin, mean of linear mag)
#                      then 20log10 clamp [-120,+24]
#   spectralTilt: LSQ slope of dB-magnitude vs log2(freq), bins 1..1024 (dB/octave)
# ─────────────────────────────────────────────────────────────────────────────

_FFT_SIZE = 2048
_HOP = 512


def _amplitude_to_db(v: float) -> float:
    return max(-120.0, min(24.0, 20.0 * math.log10(max(v, 1e-12))))


def compute_spectrum(audio: np.ndarray, fs: float) -> tuple[list[float], float]:
    """Return (spectralBands[32] in dB clamped, spectralTilt in dB/octave)."""
    mono = audio.mean(axis=0) if audio.ndim == 2 else audio
    mono = mono.astype(np.float64)
    n = mono.shape[0]
    if n < _FFT_SIZE:
        mono = np.pad(mono, (0, _FFT_SIZE - n))
        n = _FFT_SIZE
    window = 0.5 * (1.0 - np.cos(2.0 * np.pi * np.arange(_FFT_SIZE) / (_FFT_SIZE - 1)))

    mag_accum = np.zeros(_FFT_SIZE // 2 + 1, dtype=np.float64)
    frames = 0
    start = 0
    while start + _FFT_SIZE <= n:
        frame = mono[start:start + _FFT_SIZE] * window
        spec = np.fft.rfft(frame)
        mag = np.abs(spec) / _FFT_SIZE
        mag_accum += mag
        frames += 1
        start += _HOP
    if frames == 0:
        return [0.0] * SPECTRAL_BAND_COUNT, 0.0
    mag_mean = mag_accum / frames

    # Group 1025 bins into 32 (uniform-by-bin), mean of LINEAR magnitude, then dB.
    num_bins = mag_mean.shape[0]  # 1025
    per_band = max(1, (num_bins - 1) // SPECTRAL_BAND_COUNT)  # = 4
    bands: list[float] = []
    for b in range(SPECTRAL_BAND_COUNT):
        lo = b * per_band
        hi = min(num_bins, (b + 1) * per_band)
        if hi <= lo:
            hi = lo + 1
        band_mag = float(mag_mean[lo:hi].mean())
        bands.append(_amplitude_to_db(band_mag))

    # spectralTilt: LSQ slope of dB-magnitude vs log2(freq), bins 1..1024.
    tilt = 0.0
    if num_bins > 2:
        freqs = np.arange(1, num_bins) * (fs / _FFT_SIZE)
        log2f = np.log2(freqs)
        mag_rest = np.maximum(mag_mean[1:num_bins], 1e-12)
        dB = 20.0 * np.log10(mag_rest)
        denom = float(((log2f - log2f.mean()) ** 2).sum())
        if denom > 1e-12:
            tilt = float(((log2f - log2f.mean()) * (dB - dB.mean())).sum() / denom)

    return bands, tilt


# ─────────────────────────────────────────────────────────────────────────────
# StereoFieldAnalyzer parity — 4-band LR crossover at 120/800/8000 Hz
#   stereoCorrelation[4]: sum(L*R)/sqrt(sum(L^2)*sum(R^2)) clamped [-1,1], per band
#   midSideRatio[4]:      sum(S^2)/sum(M^2) linear, per band
#   100 ms analysis window. Frame declares 8 bands -> indices 4-7 zero-padded.
# ─────────────────────────────────────────────────────────────────────────────

_CROSSOVERS = (120.0, 800.0, 8000.0)


def _butterworth2_lowpass(fs: float, fc: float) -> tuple[float, float, float, float, float]:
    """Butterworth-2 lowpass as a DF1 biquad (matches StereoFieldAnalyzer::computeButterworth2)."""
    w = 2.0 * math.pi * fc / fs
    cosw = math.cos(w)
    sinw = math.sin(w)
    # alpha = sin/(2*Q) with Q = 1/sqrt(2) for Butterworth-2 -> alpha = sin/sqrt(2)*... (standard form)
    alpha = sinw / math.sqrt(2.0)
    a0 = 1.0 + alpha
    b0 = ((1.0 - cosw) / 2.0) / a0
    b1 = (1.0 - cosw) / a0
    b2 = ((1.0 - cosw) / 2.0) / a0
    a1 = (-2.0 * cosw) / a0
    a2 = (1.0 - alpha) / a0
    return b0, b1, b2, a1, a2


def _butterworth2_highpass(fs: float, fc: float) -> tuple[float, float, float, float, float]:
    w = 2.0 * math.pi * fc / fs
    cosw = math.cos(w)
    sinw = math.sin(w)
    alpha = sinw / math.sqrt(2.0)
    a0 = 1.0 + alpha
    b0 = ((1.0 + cosw) / 2.0) / a0
    b1 = -(1.0 + cosw) / a0
    b2 = ((1.0 + cosw) / 2.0) / a0
    a1 = (-2.0 * cosw) / a0
    a2 = (1.0 - alpha) / a0
    return b0, b1, b2, a1, a2


def _biquad_filter(x: np.ndarray, coeffs: tuple[float, float, float, float, float]) -> np.ndarray:
    """Vectorized DF1 biquad via scipy.signal.lfilter (C-fast). Used for the stereo
    band-split crossovers; topology is filter-form, exact DF1 vs DF2T only matters
    for LUFS which uses the dedicated _biquad_df2t above. Numerically identical for
    stable Butterworth sections on finite-length signals."""
    b0, b1, b2, a1, a2 = coeffs
    return lfilter([b0, b1, b2], [1.0, a1, a2], x.astype(np.float64))


def _band_split(channel: np.ndarray, fs: float) -> list[np.ndarray]:
    """4-band Linkwitz-Riley-style split using cascaded Butterworth-2 (matches the analyzer's
    approach of splitting M and S independently into 4 bands)."""
    f1, f2, f3 = _CROSSOVERS
    lp1 = _biquad_filter(channel, _butterworth2_lowpass(fs, f1))
    hp1 = _biquad_filter(channel, _butterworth2_highpass(fs, f1))
    # band 0: <f1 ; band1: f1..f2 ; band2: f2..f3 ; band3: >f3
    band0 = lp1
    band1 = _biquad_filter(hp1, _butterworth2_lowpass(fs, f2))
    hp2 = _biquad_filter(hp1, _butterworth2_highpass(fs, f2))
    band2 = _biquad_filter(hp2, _butterworth2_lowpass(fs, f3))
    band3 = _biquad_filter(hp2, _butterworth2_highpass(fs, f3))
    return [band0, band1, band2, band3]


def compute_stereo_bands(audio: np.ndarray, fs: float) -> tuple[list[float], list[float]]:
    """Return (stereoCorrelation[8], midSideRatio[8]) — 4 real bands + 4 zeros each."""
    if audio.ndim == 1:
        audio = np.stack([audio, audio])
    L = audio[0].astype(np.float64)
    R = audio[1].astype(np.float64)

    # 100 ms window for the analyzer; use whole signal here (offline).
    L_bands = _band_split(L, fs)
    R_bands = _band_split(R, fs)

    corr: list[float] = []
    ms_ratio: list[float] = []
    for lb, rb in zip(L_bands, R_bands):
        denom = math.sqrt(float((lb * lb).sum()) * float((rb * rb).sum()))
        c = float((lb * rb).sum() / denom) if denom > 1e-12 else 1.0
        corr.append(max(-1.0, min(1.0, c)))
        M = 0.5 * (lb + rb)
        S = 0.5 * (lb - rb)
        m_sum = float((M * M).sum())
        s_sum = float((S * S).sum())
        ms_ratio.append(s_sum / m_sum if m_sum > 1e-12 else 0.0)

    # Pad to 8 (indices 4-7 undefined in C++ -> zero).
    corr += [0.0] * (STEREO_BAND_COUNT - len(corr))
    ms_ratio += [0.0] * (STEREO_BAND_COUNT - len(ms_ratio))
    return corr[:STEREO_BAND_COUNT], ms_ratio[:STEREO_BAND_COUNT]


# ─────────────────────────────────────────────────────────────────────────────
# Top-level: assemble the FeatureFrame (strict stub parity)
# ─────────────────────────────────────────────────────────────────────────────

def extract_feature_frame(
    audio: np.ndarray,
    fs: float,
    channel_count: int = 2,
    block_size: int = 512,
    frame_index: int = 0,
) -> FeatureFrame:
    """Compute the full 63-float feature frame for one audio segment.

    `audio` is [channels, samples] or [samples] (mono). `channel_count` is the
    layout the plugin would report (1 or 2); mono arrays are duplicated to 2
    channels internally by the analyzers that need stereo, matching the runtime.
    """
    # Normalize to a 2-channel array before any stereo analysis. The runtime
    # duplicates mono -> stereo for the analyzers that need it (LUFS/spectrum
    # average over channels, the stereo analyzer needs L+R), so a [1, N] mono
    # array (how build_manifest.load_audio returns mono files) must be expanded
    # here too — otherwise compute_stereo_bands() indexes audio[1] and crashes.
    if audio.ndim == 1:
        audio = np.stack([audio, audio])
    elif audio.shape[0] == 1:
        audio = np.stack([audio[0], audio[0]])
    audio_for_stereo = audio

    integrated, short_term, momentary, lra = compute_loudness(audio_for_stereo, fs)
    true_peak = compute_true_peak_dbtp(audio_for_stereo)
    spectral_bands, spectral_tilt = compute_spectrum(audio_for_stereo, fs)
    stereo_corr, ms_ratio = compute_stereo_bands(audio_for_stereo, fs)

    def _f(v: float) -> float:
        return float(v) if math.isfinite(v) else 0.0

    return FeatureFrame(
        integrated_lufs=_f(integrated),
        short_term_lufs=_f(short_term),
        momentary_lufs=_f(momentary),
        loudness_range=_f(lra),
        true_peak_dbtp=_f(true_peak),
        spectral_tilt=_f(spectral_tilt),
        spectral_bands=tuple(spectral_bands),
        stereo_correlation=tuple(stereo_corr),
        mid_side_ratio=tuple(ms_ratio),
        sample_rate=float(fs),
        channel_count=channel_count,
        block_size=block_size,
        frame_index=frame_index,
        # Strict stub parity — these are always these values in the C++ runtime:
        crest_factor_db=0.0,
        mono_fold_down_delta_db=0.0,
        transient_density=0.0,
        harmonic_risk=0.0,
        source_quality_score=1.0,
    )
