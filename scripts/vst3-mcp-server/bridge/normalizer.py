"""
Value ↔ normalized [0,1] conversions for mastering parameters.

The C++ VST3 host works in normalized [0,1] space. This module converts human
values (dB, Hz, Q, LUFS) to normalized values and back for display.
"""

from __future__ import annotations

import math
from dataclasses import dataclass


@dataclass(frozen=True)
class ParameterRange:
    min_value: float
    max_value: float
    default_value: float = 0.0
    unit: str = ""
    log_scale: bool = False

    def clamp(self, value: float) -> float:
        return min(self.max_value, max(self.min_value, value))

    def to_normalized(self, value: float) -> float:
        value = self.clamp(value)
        if self.log_scale:
            if value <= 0 or self.min_value <= 0 or self.max_value <= 0:
                # Fall back to linear if log domain is invalid.
                return (value - self.min_value) / (self.max_value - self.min_value)
            return (math.log(value / self.min_value)) / math.log(
                self.max_value / self.min_value
            )
        return (value - self.min_value) / (self.max_value - self.min_value)

    def from_normalized(self, normalized: float) -> float:
        normalized = min(1.0, max(0.0, normalized))
        if self.log_scale:
            if self.min_value <= 0 or self.max_value <= 0:
                return self.min_value + normalized * (self.max_value - self.min_value)
            return self.min_value * (self.max_value / self.min_value) ** normalized
        return self.min_value + normalized * (self.max_value - self.min_value)


# ---------------------------------------------------------------------------
# Mastering parameter ranges used by the built-in fallback registry.
# ---------------------------------------------------------------------------
EQ_GAIN_RANGE = ParameterRange(min_value=-24.0, max_value=24.0, default_value=0.0, unit="dB")
EQ_FREQ_RANGE = ParameterRange(
    min_value=20.0, max_value=20000.0, default_value=1000.0, unit="Hz", log_scale=True
)
EQ_Q_RANGE = ParameterRange(min_value=0.1, max_value=10.0, default_value=1.0, unit="")
COMP_RATIO_RANGE = ParameterRange(min_value=1.0, max_value=20.0, default_value=2.0, unit=":1")
COMP_THRESHOLD_RANGE = ParameterRange(min_value=-60.0, max_value=0.0, default_value=-18.0, unit="dB")
COMP_ATTACK_RANGE = ParameterRange(min_value=0.1, max_value=100.0, default_value=10.0, unit="ms")
COMP_RELEASE_RANGE = ParameterRange(min_value=10.0, max_value=1000.0, default_value=100.0, unit="ms")
LIMITER_CEILING_RANGE = ParameterRange(min_value=-12.0, max_value=0.0, default_value=-1.0, unit="dBTP")
LUFS_TARGET_RANGE = ParameterRange(min_value=-23.0, max_value=-6.0, default_value=-14.0, unit="LUFS")
OUTPUT_GAIN_RANGE = ParameterRange(min_value=-24.0, max_value=24.0, default_value=0.0, unit="dB")
SATURATION_DRIVE_RANGE = ParameterRange(min_value=0.0, max_value=100.0, default_value=0.0, unit="%")
STEREO_WIDTH_RANGE = ParameterRange(min_value=0.0, max_value=200.0, default_value=100.0, unit="%")


def normalize_gain_db(gain_db: float) -> float:
    return EQ_GAIN_RANGE.to_normalized(gain_db)


def denormalize_gain_db(normalized: float) -> float:
    return EQ_GAIN_RANGE.from_normalized(normalized)


def normalize_frequency_hz(hz: float) -> float:
    return EQ_FREQ_RANGE.to_normalized(hz)


def denormalize_frequency_hz(normalized: float) -> float:
    return EQ_FREQ_RANGE.from_normalized(normalized)


def normalize_q(q: float) -> float:
    return EQ_Q_RANGE.to_normalized(q)


def denormalize_q(normalized: float) -> float:
    return EQ_Q_RANGE.from_normalized(normalized)


def normalize_compressor_ratio(ratio: float) -> float:
    return COMP_RATIO_RANGE.to_normalized(ratio)


def normalize_compressor_threshold(threshold_db: float) -> float:
    return COMP_THRESHOLD_RANGE.to_normalized(threshold_db)


def normalize_limiter_ceiling_db(ceiling_db: float) -> float:
    return LIMITER_CEILING_RANGE.to_normalized(ceiling_db)


def denormalize_limiter_ceiling_db(normalized: float) -> float:
    return LIMITER_CEILING_RANGE.from_normalized(normalized)


def normalize_lufs_target(lufs: float) -> float:
    return LUFS_TARGET_RANGE.to_normalized(lufs)


def denormalize_lufs_target(normalized: float) -> float:
    return LUFS_TARGET_RANGE.from_normalized(normalized)


def normalize_output_gain_db(gain_db: float) -> float:
    return OUTPUT_GAIN_RANGE.to_normalized(gain_db)


def denormalize_output_gain_db(normalized: float) -> float:
    return OUTPUT_GAIN_RANGE.from_normalized(normalized)


def normalize_saturation_drive(drive: float) -> float:
    return SATURATION_DRIVE_RANGE.to_normalized(drive)


def denormalize_saturation_drive(normalized: float) -> float:
    return SATURATION_DRIVE_RANGE.from_normalized(normalized)


def normalize_stereo_width(percent: float) -> float:
    return STEREO_WIDTH_RANGE.to_normalized(percent)


def denormalize_stereo_width(normalized: float) -> float:
    return STEREO_WIDTH_RANGE.from_normalized(normalized)


def format_db(value: float) -> str:
    return f"{value:+.2f} dB"


def format_hz(value: float) -> str:
    if value >= 1000.0:
        return f"{value / 1000.0:.2f} kHz"
    return f"{value:.0f} Hz"


def format_q(value: float) -> str:
    return f"Q={value:.2f}"
