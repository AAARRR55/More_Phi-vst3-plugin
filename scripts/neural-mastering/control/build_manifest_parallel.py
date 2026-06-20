#!/usr/bin/env python3
"""Parallel build_manifest: shards the source dir across N workers, runs
build_manifest.py per shard in parallel, then merges via merge_manifests.py.

On the 48-core Blackwell this turns feature extraction (the CPU bottleneck) from
hours into minutes. It reuses build_manifest.py + merge_manifests.py UNMODIFIED
(each shard builds with train_ratio=1.0; merge_manifests re-splits per sourceId
across all shards, so the result is leakage-safe and identical in shape to a
single-core build_manifest run).

Example:
  python build_manifest_parallel.py --source-dir data/raw/aam/roughmix \\
      --out-dir data/manifest_aam --workers 48 --license-status approved
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

EXTS = {".wav", ".flac", ".aif", ".aiff", ".ogg", ".mp3"}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source-dir", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--sample-rate", type=int, default=48000)
    ap.add_argument("--segment-seconds", type=float, default=10.0)
    ap.add_argument("--seed", type=int, default=1337)
    ap.add_argument("--corpus-name", default="parallel")
    ap.add_argument("--license-status", default="approved")
    ap.add_argument("--train-ratio", type=float, default=0.9)
    ap.add_argument("--val-ratio", type=float, default=0.05)
    args = ap.parse_args()

    here = Path(__file__).parent
    src = Path(args.source_dir)
    files = sorted(f for f in src.rglob("*") if f.is_file() and f.suffix.lower() in EXTS)
    if not files:
        print(f"ERROR: no audio files under {src}", file=sys.stderr)
        return 2
    n = min(args.workers, len(files))
    shards = [files[i::n] for i in range(n)]  # round-robin
    print(f"{len(files)} files -> {n} shards (~{len(shards[0])}/shard); running build_manifest.py x{n} in parallel")

    tmp = Path(tempfile.mkdtemp(prefix="bmpar_"))
    shard_outs: list[Path] = []
    try:
        shard_dirs: list[Path] = []
        for i, shard in enumerate(shards):
            sd = tmp / f"shard_{i:03d}"
            sd.mkdir()
            for f in shard:
                try:
                    os.symlink(f.resolve(), sd / f.name)  # ABSOLUTE target (relative symlinks resolve from the link's dir)
                except OSError:
                    pass
            shard_dirs.append(sd)
            shard_outs.append(tmp / f"out_{i:03d}")

        procs = []
        for i, sd in enumerate(shard_dirs):
            cmd = [
                sys.executable, str(here / "build_manifest.py"),
                "--source-dir", str(sd), "--out-dir", str(shard_outs[i]),
                "--sample-rate", str(args.sample_rate),
                "--segment-seconds", str(args.segment_seconds),
                "--train-ratio", "1.0", "--val-ratio", "0.0",  # merge re-splits per source
                "--seed", str(args.seed), "--corpus-name", f"{args.corpus_name}_s{i}",
                "--license-status", args.license_status,
            ]
            procs.append(subprocess.Popen(cmd))
        rcs = [p.wait() for p in procs]
        valid = [str(so) for so, rc in zip(shard_outs, rcs) if rc == 0 and (so / "train.jsonl").exists()]
        print(f"  {len(valid)}/{n} shards produced output")
        if not valid:
            print("ERROR: no shards succeeded", file=sys.stderr)
            return 1

        merge = [
            sys.executable, str(here / "merge_manifests.py"),
            "--inputs", *valid, "--out-dir", str(args.out_dir),
            "--train-ratio", str(args.train_ratio), "--val-ratio", str(args.val_ratio),
            "--seed", str(args.seed),
        ]
        subprocess.run(merge, check=True)
        print(f"DONE: merged {len(valid)} shards -> {args.out_dir}")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
