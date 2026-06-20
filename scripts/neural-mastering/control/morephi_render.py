# =============================================================================
# tools/headless_mastering_render/morephi_render.py
#
# Python ctypes binding for the T2 headless mastering render harness.
# dlopen()s libmore_phi_headless_render.{so,dll,dylib} and exposes a
# render_candidate() function returning (rendered_pcm, meters_dict).
#
# Usage:
#   from morephi_render import HeadlessRenderer
#   r = HeadlessRenderer("./libmore_phi_headless_render.so", sample_rate=48000)
#   rendered, meters = r.render_candidate(pcm_interleaved, delta72)
# =============================================================================
from __future__ import annotations

import ctypes
import os
import sys
from dataclasses import dataclass
from typing import Tuple

import numpy as np

# Meter layout returned by render() (matches headless_render.cpp out_ pointers).
METER_KEYS = ("lufs_integrated", "true_peak_dbtp", "limiter_gain_reduction_db")


@dataclass
class RenderMeters:
    lufs_integrated: float           # post-chain integrated LUFS (<= -200 if <3s audio)
    true_peak_dbtp: float            # post-chain true-peak dBTP
    limiter_gain_reduction_db: float # limiter GR (dB)


def _load_library(lib_path: str) -> ctypes.CDLL:
    if not os.path.exists(lib_path):
        raise FileNotFoundError(f"headless_render library not found: {lib_path}")
    # CDLL on Linux/macOS, WinDLL on Windows (stdcall vs cdecl doesn't apply to
    # extern "C" but WinDLL avoids the x64 __cdecl/__stdcall ambiguity).
    if sys.platform.startswith("win"):
        return ctypes.WinDLL(lib_path)
    return ctypes.CDLL(lib_path)


class HeadlessRenderer:
    """ctypes wrapper around the headless mastering render harness.

    Parameters
    ----------
    lib_path : str
        Path to libmore_phi_headless_render.{so,dll,dylib}.
    sample_rate : float
        Target sample rate (Hz). Pinned for CMA-ES reproducibility.
    block_size : int
        processBlock chunk size; must match the maxBlockSize the .so was
        prepared with (default 512). Never feed render() a larger chunk.
    normalizer_mode : int
        0 = disable LoudnessNormalizer (gain=1.0; loudness delta is setpoint
            only; RECOMMENDED for EQ/dynamics/stereo search).
        1 = pump updateCorrectionGain() at ~10 Hz (search the loudness axis).
    """

    _FLOAT_P = ctypes.POINTER(ctypes.c_float)

    def __init__(
        self,
        lib_path: str,
        sample_rate: float = 48000.0,
        block_size: int = 512,
        normalizer_mode: int = 0,
    ):
        self._lib = _load_library(lib_path)
        self._configure_signatures()

        rc = self._lib.morephi_headless_init(
            ctypes.c_double(float(sample_rate)),
            ctypes.c_int(int(block_size)),
            ctypes.c_int(int(normalizer_mode)),
        )
        if rc != 0:
            raise RuntimeError(f"morephi_headless_init failed (rc={rc})")

        self._sample_rate = float(sample_rate)
        self._block_size = int(block_size)
        self._n_channels = 2
        self._delta_count = 72
        self._chain_latency = self.chain_latency()

    # -- signature wiring ----------------------------------------------------

    def _configure_signatures(self) -> None:
        L = self._lib
        P = self._FLOAT_P

        L.morephi_headless_init.argtypes = [ctypes.c_double, ctypes.c_int, ctypes.c_int]
        L.morephi_headless_init.restype = ctypes.c_int

        L.morephi_headless_shutdown.argtypes = []
        L.morephi_headless_shutdown.restype = None

        L.morephi_headless_chain_latency.argtypes = []
        L.morephi_headless_chain_latency.restype = ctypes.c_int

        L.render.argtypes = [
            P,               # unmastered_pcm (interleaved stereo [n*2])
            ctypes.c_int,    # n_samples (frames)
            ctypes.c_double, # sample_rate
            P,               # delta72 [72]
            P,               # out_rendered [n*2]
            ctypes.POINTER(ctypes.c_float),  # out_lufs
            ctypes.POINTER(ctypes.c_float),  # out_dbtp
            ctypes.POINTER(ctypes.c_float),  # out_limiter_gr
        ]
        L.render.restype = ctypes.c_int

    # -- public API ----------------------------------------------------------

    def chain_latency(self) -> int:
        """Mastering-chain latency (limiter lookahead + exciter OS) in samples."""
        return int(self._lib.morephi_headless_chain_latency())

    @property
    def sample_rate(self) -> float:
        return self._sample_rate

    @property
    def delta_count(self) -> int:
        return self._delta_count

    def render_candidate(
        self,
        pcm_interleaved: np.ndarray,
        delta72: np.ndarray,
    ) -> Tuple[np.ndarray, RenderMeters]:
        """Render one candidate 72-delta vector through the mastering chain.

        Parameters
        ----------
        pcm_interleaved : np.ndarray, shape [n_samples*2], dtype float32
            Interleaved stereo input (L,R,L,R,...). Use np.ascontiguousarray to
            avoid silently passing a wrong pointer for a non-contiguous slice.
        delta72 : np.ndarray, shape [72], dtype float32
            Mastering deltas in order eq(32)+dynamics(8)+stereo(8)+harmonic(8)
            +limiter(8)+loudness(8). Clamped to [-1,1] inside the .so. 43 of 72
            slots are wired; 29 are dead (fix dead slots at 0 to save search dim).

        Returns
        -------
        rendered : np.ndarray, shape [n_samples, 2], dtype float32
            Rendered stereo PCM (planar: column 0 = L, column 1 = R).
        meters : RenderMeters
            Post-chain integrated LUFS, true-peak dBTP, limiter GR (dB).
        """
        if pcm_interleaved.ndim != 1 or pcm_interleaved.shape[0] % 2 != 0:
            raise ValueError(
                f"pcm_interleaved must be 1-D interleaved stereo [n*2]; got shape {pcm_interleaved.shape}"
            )
        if delta72.shape != (72,):
            raise ValueError(f"delta72 must have shape (72,); got {delta72.shape}")

        # CRITICAL: ctypes reads the raw float* — must be C-contiguous float32.
        pcm = np.ascontiguousarray(pcm_interleaved, dtype=np.float32)
        delta = np.ascontiguousarray(delta72, dtype=np.float32)
        n_samples = pcm.shape[0] // 2

        out = np.zeros(n_samples * 2, dtype=np.float32)
        lufs = ctypes.c_float(-999.0)
        dbtp = ctypes.c_float(-999.0)
        gr = ctypes.c_float(0.0)

        rc = self._lib.render(
            pcm.ctypes.data_as(self._FLOAT_P),
            ctypes.c_int(n_samples),
            ctypes.c_double(self._sample_rate),
            delta.ctypes.data_as(self._FLOAT_P),
            out.ctypes.data_as(self._FLOAT_P),
            ctypes.byref(lufs),
            ctypes.byref(dbtp),
            ctypes.byref(gr),
        )
        if rc != 0:
            raise RuntimeError(
                f"render failed rc={rc} "
                "(2=not init; 3=safety-policy reject; 4=applyValidatedPlan no-op; 5=exception)"
            )

        meters = RenderMeters(
            lufs_integrated=float(lufs.value),
            true_peak_dbtp=float(dbtp.value),
            limiter_gain_reduction_db=float(gr.value),
        )
        return out.reshape(n_samples, 2), meters

    def close(self) -> None:
        """Release the engine + JUCE MessageManager."""
        try:
            self._lib.morephi_headless_shutdown()
        except Exception:
            pass

    def __del__(self):
        self.close()


# -- module-level convenience -------------------------------------------------

def _self_test() -> None:
    """Quick smoke test: render a 3s sine sweep at the zero delta vector."""
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("--lib", required=True, help="path to libmore_phi_headless_render.so")
    p.add_argument("--sr", type=float, default=48000.0)
    args = p.parse_args()

    sr = args.sr
    duration_s = 3.0
    n = int(sr * duration_s)
    t = np.arange(n, dtype=np.float32) / sr
    # Interleaved stereo: 220 Hz sine, slightly different per channel.
    left = 0.3 * np.sin(2 * np.pi * 220.0 * t).astype(np.float32)
    right = 0.25 * np.sin(2 * np.pi * 220.0 * t).astype(np.float32)
    pcm = np.empty(n * 2, dtype=np.float32)
    pcm[0::2] = left
    pcm[1::2] = right

    delta = np.zeros(72, dtype=np.float32)
    # Put a small EQ bump on band 8 (gain axis only).
    delta[8] = 0.2

    r = HeadlessRenderer(args.lib, sample_rate=sr)
    rendered, meters = r.render_candidate(pcm, delta)
    print(f"chain_latency = {r.chain_latency()} samples")
    print(f"rendered shape = {rendered.shape}, peak = {np.abs(rendered).max():.4f}")
    print(f"meters: {meters}")


if __name__ == "__main__":
    _self_test()
