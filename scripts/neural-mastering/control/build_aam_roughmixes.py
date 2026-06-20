#!/usr/bin/env python3
"""Build unmastered rough mixes from AAM multitrack stems.

AAM layout (verified against the downloaded archive): FLAT per-instrument stem
files named ``{songID}_{Instrument}.flac`` (e.g. ``2001_Drums.flac``,
``2001_DoubleBassArco.flac``). This groups stems by the numeric songID prefix,
sums them (RMS-normalized per stem, no bus compression/limiting/EQ = deliberately
UNMASTERED), and writes one stereo WAV per song. The output dir is then fed to
build_manifest.py, which segments + extracts the 63-feature vector + labels with
the teacher (T1 today; T2 once built).

Why sum-with-no-bus: the rough mix is unmastered material the teacher must
CORRECT -- the informative positive-label regime. (AAM's pre-made
``*-audio-mixes.zip`` are the human-mixed references, reserved for restraint/T3.)

License: AAM is CC-BY 4.0 (Zenodo 5794629) -> licenseStatus="approved",
referenceQuality="synthetic" (labels are teacher-generated, not human masters).

Example:
  python build_aam_roughmixes.py --stems-dir data/raw/aam/stems \\
      --out-dir data/raw/aam/roughmix --sample-rate 48000
  python build_manifest.py --source-dir data/raw/aam/roughmix \\
      --out-dir data/manifest_aam --corpus-name aam-roughmix --license-status approved
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

SONG_RE = re.compile(r"^(\d+)_")


def load_stem(path: Path, target_sr: int) -> np.ndarray:
    """Load a stem FLAC/WAV -> [channels, samples] float32 at target_sr."""
    audio, sr = sf.read(str(path), always_2d=True)
    audio = audio.T.astype(np.float32)  # [ch, samples]
    if sr != target_sr:
        import librosa
        audio = np.stack(
            [librosa.resample(audio[c], orig_sr=sr, target_sr=target_sr) for c in range(audio.shape[0])]
        )
    return audio


def sum_stems(stems: list[np.ndarray]) -> np.ndarray | None:
    """Sum stems into a stereo rough mix: RMS-normalize each, pad to max len, sum,
    then peak-normalize the bus to ~-1 dBFS. Near-silent stems are skipped."""
    if not stems:
        return None
    max_len = max(s.shape[1] for s in stems)
    acc = np.zeros((2, max_len), dtype=np.float32)
    for s in stems:
        if s.shape[0] == 1:
            s = np.stack([s[0], s[0]])  # mono -> stereo
        elif s.shape[0] >= 2:
            s = s[:2]
        rms = float(np.sqrt(np.mean(s.astype(np.float64) ** 2)))
        if rms < 1e-5:  # skip near-silent stems (would explode when normalized)
            continue
        s = s / (rms * 3.0)  # each stem contributes ~-10 dBFS RMS
        if s.shape[1] < max_len:
            s = np.pad(s, ((0, 0), (0, max_len - s.shape[1])))
        acc += s[:, :max_len]
    peak = float(np.max(np.abs(acc)))
    if peak < 1e-9:
        return None
    return acc / peak * 0.89  # -1 dBFS


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--stems-dir", required=True, help="dir of {songID}_{Instrument}.flac stems (extracted)")
    p.add_argument("--out-dir", required=True)
    p.add_argument("--sample-rate", type=int, default=48000)
    p.add_argument("--max-songs", type=int, default=0, help="0 = all (cap for testing)")
    args = p.parse_args()

    src = Path(args.stems_dir)
    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)

    songs: dict[str, list[Path]] = {}
    for f in sorted(src.glob("*.flac")):
        m = SONG_RE.match(f.name)
        if m:
            songs.setdefault(m.group(1), []).append(f)
    print(f"found {len(songs)} songs across {sum(len(v) for v in songs.values())} stems in {src}")

    n = 0
    for sid, stems in sorted(songs.items()):
        if args.max_songs and n >= args.max_songs:
            break
        try:
            loaded = [load_stem(s, args.sample_rate) for s in stems]
        except Exception as exc:  # noqa: BLE001
            print(f"  skip song {sid}: load failed ({exc})", file=sys.stderr)
            continue
        mix = sum_stems(loaded)
        if mix is None:
            continue
        sf.write(str(out / f"aam_{sid}.wav"), mix.T, args.sample_rate, subtype="FLOAT")
        n += 1
        if n % 25 == 0:
            print(f"  {n} songs -> roughmix")
    print(f"DONE: wrote {n} rough mixes to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
