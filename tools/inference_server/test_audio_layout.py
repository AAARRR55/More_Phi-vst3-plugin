"""Regression checks for the SonicMaster HTTP server audio layout."""
from __future__ import annotations

from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

from server import SEGMENT_FRAMES, decode_interleaved_stereo  # noqa: E402


def test_decode_interleaved_stereo_preserves_distinct_channels() -> None:
    samples = np.empty(2 * SEGMENT_FRAMES, dtype=np.float32)
    left = np.arange(SEGMENT_FRAMES, dtype=np.float32)
    right = left + 1000.0
    samples[0::2] = left
    samples[1::2] = right

    decoded = decode_interleaved_stereo(samples)

    assert decoded.shape == (2, SEGMENT_FRAMES)
    assert decoded.flags.c_contiguous
    np.testing.assert_array_equal(decoded[0, :16], left[:16])
    np.testing.assert_array_equal(decoded[1, :16], right[:16])


if __name__ == "__main__":
    test_decode_interleaved_stereo_preserves_distinct_channels()
    print("audio layout regression passed")
