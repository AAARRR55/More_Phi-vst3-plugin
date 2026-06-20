#!/usr/bin/env python3
"""Contract + parity tests for the mastering control regressor artifact.

Run after `train.py --export-onnx`. Asserts every guarantee the C++
OnnxNeuralMasteringRunner seam relies on:

    - ONNX loads cleanly in onnxruntime
    - Input shape is [N, 63], output shape is [N, 72]
    - Every output is finite and in [-1, 1] (tanh head is structural)
    - Inference is deterministic (same input -> same output)
    - The Python codec layout matches what the C++ serializeFeatureFrame /
      buildPlanCandidate expect (parity with the seam constants)

Requires: onnxruntime, numpy. (Not required to just train/export.)

Usage:
    python tests/test_contract.py control_regressor.onnx
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from codec import (  # noqa: E402
    INPUT_FEATURE_COUNT,
    OUTPUT_DELTA_COUNT,
    SCALAR_FEATURE_COUNT,
    SPECTRAL_BAND_COUNT,
    STEREO_BAND_COUNT,
    FeatureFrame,
    control_deltas_to_vector,
    serialize_feature_frame,
    vector_to_control_deltas,
)


def _load_session(onnx_path: Path):
    import onnxruntime as ort

    return ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])


def _run(session, feature_vec) -> np.ndarray:
    arr = np.asarray(feature_vec, dtype=np.float32).reshape(1, INPUT_FEATURE_COUNT)
    (out,) = session.run(None, {"input": arr})
    return np.asarray(out, dtype=np.float32).reshape(OUTPUT_DELTA_COUNT)


def test_contract(onnx_path: Path) -> None:
    session = _load_session(onnx_path)

    # Shape contract.
    in_meta = session.get_inputs()[0]
    assert in_meta.name == "input", f"input name {in_meta.name!r} != 'input'"
    assert in_meta.shape in ([1, INPUT_FEATURE_COUNT], ["batch", INPUT_FEATURE_COUNT], [INPUT_FEATURE_COUNT]), (
        f"input shape {in_meta.shape} incompatible with [{1}, {INPUT_FEATURE_COUNT}]"
    )
    out_meta = session.get_outputs()[0]
    assert out_meta.name == "output", f"output name {out_meta.name!r} != 'output'"

    # Construct a representative frame and encode it with the parity codec.
    frame = FeatureFrame(
        integrated_lufs=-14.0,
        short_term_lufs=-12.0,
        momentary_lufs=-10.0,
        loudness_range=7.0,
        true_peak_dbtp=-1.0,
        crest_factor_db=12.0,
        spectral_tilt=1.5,
        mono_fold_down_delta_db=0.3,
        transient_density=0.45,
        harmonic_risk=0.1,
        source_quality_score=0.92,
        spectral_bands=tuple(0.01 * i for i in range(SPECTRAL_BAND_COUNT)),
        stereo_correlation=tuple(0.8 - 0.05 * i for i in range(STEREO_BAND_COUNT)),
        mid_side_ratio=tuple(0.5 + 0.02 * i for i in range(STEREO_BAND_COUNT)),
        sample_rate=48000.0,
        channel_count=2,
        block_size=512,
        frame_index=12345,
    )
    feature_vec = serialize_feature_frame(frame)
    assert len(feature_vec) == INPUT_FEATURE_COUNT

    deltas = _run(session, feature_vec)

    # Finiteness + range (tanh head guarantees this structurally).
    assert np.all(np.isfinite(deltas)), "non-finite delta in model output"
    assert deltas.min() >= -1.0 - 1e-6 and deltas.max() <= 1.0 + 1e-6, (
        f"delta out of [-1,1]: min={deltas.min()} max={deltas.max()}"
    )

    # Determinism — same input, same output (the C++ runner reuses buffers and
    # the safety policy treats the result as a stable plan).
    again = _run(session, feature_vec)
    assert np.array_equal(deltas, again), "inference not deterministic"

    # Codec parity: round-trip through control_deltas_to_vector / vector_to_control_deltas.
    cd = vector_to_control_deltas(deltas)
    roundtrip = control_deltas_to_vector(cd)
    assert len(roundtrip) == OUTPUT_DELTA_COUNT
    assert np.allclose(roundtrip, deltas, atol=1e-6)

    # Layout sanity: scalar block sits at the front of the input tensor.
    assert feature_vec[0] == frame.integrated_lufs
    assert feature_vec[10] == frame.source_quality_score
    assert feature_vec[SCALAR_FEATURE_COUNT] == frame.spectral_bands[0]
    assert feature_vec[SCALAR_FEATURE_COUNT + SPECTRAL_BAND_COUNT - 1] == frame.spectral_bands[-1]

    print(
        f"PASS  {onnx_path}\n"
        f"  input {INPUT_FEATURE_COUNT} -> output {OUTPUT_DELTA_COUNT}\n"
        f"  delta range [{deltas.min():.4f}, {deltas.max():.4f}]\n"
        f"  deterministic: yes\n"
        f"  codec parity: yes"
    )


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_contract.py <model.onnx>", file=sys.stderr)
        return 2
    onnx_path = Path(sys.argv[1])
    if not onnx_path.exists():
        print(f"missing: {onnx_path}", file=sys.stderr)
        return 2
    test_contract(onnx_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
