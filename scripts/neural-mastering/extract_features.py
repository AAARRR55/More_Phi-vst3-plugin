#!/usr/bin/env python3
"""Create schema-shaped offline neural mastering feature fixtures."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def make_frame(sample_rate: float, channels: int, block_size: int, frame_index: int) -> dict[str, object]:
    status = "success" if channels in (1, 2) and sample_rate > 0 and block_size > 0 else "unsupported"
    return {
        "status": status,
        "frame": {
            "schemaVersion": 1,
            "sampleRate": sample_rate,
            "channelCount": channels,
            "blockSize": block_size,
            "frameIndex": frame_index,
            "layoutClass": "mono" if channels == 1 else "stereo" if channels == 2 else "unsupported",
            "loudness": {
                "integrated": -14.0,
                "shortTerm": -14.0,
                "momentary": -14.0,
                "range": 0.0,
            },
            "truePeakDbTp": -1.0,
            "spectralBands": [0.0] * 32,
            "stereoCorrelation": [0.0] * 8,
            "sourceQualityFlags": [],
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sample-rate", type=float, default=48000.0)
    parser.add_argument("--channels", type=int, default=2)
    parser.add_argument("--block-size", type=int, default=512)
    parser.add_argument("--frame-index", type=int, default=0)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    payload = make_frame(args.sample_rate, args.channels, args.block_size, args.frame_index)
    args.output.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0 if payload["status"] == "success" else 2


if __name__ == "__main__":
    raise SystemExit(main())
