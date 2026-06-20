"""Feature/label codec for the More-Phi neural mastering control regressor.

Single source of truth for the train/serve tensor layout. This MUST match the
C++ I/O contract exactly:

    - serializeFeatureFrame()   src/AI/OnnxNeuralMasteringRunner.cpp
    - buildPlanCandidate()      src/AI/OnnxNeuralMasteringRunner.cpp
    - kNeuralMastering*Count    src/Core/NeuralMasteringTypes.h

If you change an offset here, the runtime ONNX input head and the C++
OnnxNeuralMasteringRunner::proposePlan() will silently disagree and the model
becomes useless. The parity test (tests/test_codec_parity.py) asserts the
layout against the C++ constants; keep it green.

Layout (feature schema v1 / plan schema v1):

    Input tensor  [63] = 11 scalars + 32 spectral + 8 stereoCorr + 8 midSide + 4 meta
    Output tensor [72] = 32 eq + 8 dynamics + 8 stereo + 8 harmonic + 8 limiter + 8 loudness

All control outputs are per-control DELTAS in [-1, 1]; the safety policy
(NeuralMasteringSafetyPolicy) projects previous + delta and clamps per
maxDeltaPerPlan. The model emits tanh so the range is structural.
"""

from __future__ import annotations

from dataclasses import dataclass

# ── Counts — mirrored from src/Core/NeuralMasteringTypes.h ────────────────────
EQ_COUNT = 32
DYNAMICS_COUNT = 8
STEREO_COUNT = 8
HARMONIC_COUNT = 8
LIMITER_COUNT = 8
LOUDNESS_COUNT = 8
SPECTRAL_BAND_COUNT = 32
STEREO_BAND_COUNT = 8

SCALAR_FEATURE_COUNT = 11
INPUT_FEATURE_COUNT = (
    SCALAR_FEATURE_COUNT
    + SPECTRAL_BAND_COUNT
    + STEREO_BAND_COUNT
    + STEREO_BAND_COUNT
    + 4  # sampleRate, channelCount, blockSize, frameIndex (as float)
)
OUTPUT_DELTA_COUNT = (
    EQ_COUNT + DYNAMICS_COUNT + STEREO_COUNT + HARMONIC_COUNT + LIMITER_COUNT + LOUDNESS_COUNT
)

# Control-group output slice offsets — matches C++ buildPlanCandidate read order.
EQ_SLICE = (0, EQ_COUNT)
DYNAMICS_SLICE = (EQ_SLICE[1], EQ_SLICE[1] + DYNAMICS_COUNT)
STEREO_OUT_SLICE = (DYNAMICS_SLICE[1], DYNAMICS_SLICE[1] + STEREO_COUNT)
HARMONIC_SLICE = (STEREO_OUT_SLICE[1], STEREO_OUT_SLICE[1] + HARMONIC_COUNT)
LIMITER_SLICE = (HARMONIC_SLICE[1], HARMONIC_SLICE[1] + LIMITER_COUNT)
LOUDNESS_SLICE = (LIMITER_SLICE[1], LIMITER_SLICE[1] + LOUDNESS_COUNT)
assert LOUDNESS_SLICE[1] == OUTPUT_DELTA_COUNT


@dataclass(frozen=True)
class FeatureFrame:
    """Python mirror of more_phi::NeuralMasteringFeatureFrame.

    Only the fields the model input head consumes are listed. Scalars left
    unset default to 0.0; band arrays default to zero-filled.
    """

    integrated_lufs: float = 0.0
    short_term_lufs: float = 0.0
    momentary_lufs: float = 0.0
    loudness_range: float = 0.0
    true_peak_dbtp: float = 0.0
    crest_factor_db: float = 0.0
    spectral_tilt: float = 0.0
    mono_fold_down_delta_db: float = 0.0
    transient_density: float = 0.0
    harmonic_risk: float = 0.0
    source_quality_score: float = 1.0
    spectral_bands: tuple[float, ...] = ()
    stereo_correlation: tuple[float, ...] = ()
    mid_side_ratio: tuple[float, ...] = ()
    sample_rate: float = 48000.0
    channel_count: int = 2
    block_size: int = 512
    frame_index: int = 0


def _finite_or_zero(value: float) -> float:
    import math

    return value if math.isfinite(value) else 0.0


def serialize_feature_frame(frame: FeatureFrame) -> list[float]:
    """Pack a FeatureFrame into the 63-float input tensor (C++ parity).

    Mirrors OnnxNeuralMasteringRunner::serializeFeatureFrame() in src/AI.
    Non-finite scalars are coerced to 0.0; missing band entries are zero-filled
    so the tensor is always exactly INPUT_FEATURE_COUNT long and finite.
    """
    out: list[float] = [0.0] * INPUT_FEATURE_COUNT

    out[0] = _finite_or_zero(frame.integrated_lufs)
    out[1] = _finite_or_zero(frame.short_term_lufs)
    out[2] = _finite_or_zero(frame.momentary_lufs)
    out[3] = _finite_or_zero(frame.loudness_range)
    out[4] = _finite_or_zero(frame.true_peak_dbtp)
    out[5] = _finite_or_zero(frame.crest_factor_db)
    out[6] = _finite_or_zero(frame.spectral_tilt)
    out[7] = _finite_or_zero(frame.mono_fold_down_delta_db)
    out[8] = _finite_or_zero(frame.transient_density)
    out[9] = _finite_or_zero(frame.harmonic_risk)
    out[10] = _finite_or_zero(frame.source_quality_score)

    cursor = SCALAR_FEATURE_COUNT

    def _write_bands(src: tuple[float, ...], count: int) -> None:
        nonlocal cursor
        for i in range(count):
            out[cursor + i] = _finite_or_zero(src[i]) if i < len(src) else 0.0
        cursor += count

    _write_bands(frame.spectral_bands, SPECTRAL_BAND_COUNT)
    _write_bands(frame.stereo_correlation, STEREO_BAND_COUNT)
    _write_bands(frame.mid_side_ratio, STEREO_BAND_COUNT)

    out[cursor + 0] = _finite_or_zero(float(frame.sample_rate))
    out[cursor + 1] = _finite_or_zero(float(frame.channel_count))
    out[cursor + 2] = _finite_or_zero(float(frame.block_size))
    out[cursor + 3] = _finite_or_zero(float(frame.frame_index & 0xFFFFFFFF))

    return out


@dataclass(frozen=True)
class ControlDeltas:
    """Python mirror of more_phi::MasteringTargetVector used as the model label.

    Each field is a tuple of per-control deltas in [-1, 1]. Lengths are fixed
    by the constants above; the helpers below enforce them.
    """

    eq: tuple[float, ...]
    dynamics: tuple[float, ...]
    stereo: tuple[float, ...]
    harmonic: tuple[float, ...]
    limiter: tuple[float, ...]
    loudness: tuple[float, ...]


def control_deltas_to_vector(deltas: ControlDeltas) -> list[float]:
    """Flatten a ControlDeltas into the 72-float output tensor (C++ parity).

    Order matches buildPlanCandidate()'s readArray sequence in src/AI.
    """
    out: list[float] = []
    for group, count in (
        (deltas.eq, EQ_COUNT),
        (deltas.dynamics, DYNAMICS_COUNT),
        (deltas.stereo, STEREO_COUNT),
        (deltas.harmonic, HARMONIC_COUNT),
        (deltas.limiter, LIMITER_COUNT),
        (deltas.loudness, LOUDNESS_COUNT),
    ):
        if len(group) != count:
            raise ValueError(f"control group length {len(group)} != expected {count}")
        out.extend(group)
    if len(out) != OUTPUT_DELTA_COUNT:
        raise AssertionError(f"output vector length {len(out)} != {OUTPUT_DELTA_COUNT}")
    return out


def vector_to_control_deltas(vec) -> ControlDeltas:
    """Inverse of control_deltas_to_vector; accepts list/ndarray of len 72."""
    vec = list(vec)
    if len(vec) != OUTPUT_DELTA_COUNT:
        raise ValueError(f"delta vector length {len(vec)} != {OUTPUT_DELTA_COUNT}")

    def _slice(slc):
        return tuple(vec[slc[0] : slc[1]])

    return ControlDeltas(
        eq=_slice(EQ_SLICE),
        dynamics=_slice(DYNAMICS_SLICE),
        stereo=_slice(STEREO_OUT_SLICE),
        harmonic=_slice(HARMONIC_SLICE),
        limiter=_slice(LIMITER_SLICE),
        loudness=_slice(LOUDNESS_SLICE),
    )
