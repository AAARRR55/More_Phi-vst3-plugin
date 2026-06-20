#!/usr/bin/env python3
"""Merge multiple {feature,delta} JSONL manifests into one leakage-safe train/val set.

Reads train.jsonl + val.jsonl from each input manifest dir, pools ALL records,
and re-splits ONCE per sourceId. build_manifest.py already splits per-source
within a corpus, but it is blind to a source spanning corpora (e.g. an artist in
both FMA and AAM). This closes that cross-corpus leakage gap (G10 requirement)
and produces a single unified train/val set for training.

Each record must be {"feature":[63], "delta":[72], "sourceId", ...}. Records with
wrong lengths or non-finite values are dropped (counted). Output: train.jsonl,
val.jsonl, manifest.json (referenceQuality stays "synthetic" — honest G10 posture).

Example:
  python merge_manifests.py --inputs data/manifest_fma data/manifest_aam \\
      --out-dir data/manifest_merged --train-ratio 0.9 --val-ratio 0.05
"""
from __future__ import annotations

import argparse
import json
import math
import random
from pathlib import Path


def _valid(record: dict) -> bool:
    f, d = record.get("feature"), record.get("delta")
    if not (isinstance(f, list) and isinstance(d, list) and len(f) == 63 and len(d) == 72):
        return False
    try:
        return all(math.isfinite(float(x)) for x in f + d)
    except (TypeError, ValueError):
        return False


def load_records(manifest_dir: Path) -> list[dict]:
    recs: list[dict] = []
    for name in ("train.jsonl", "val.jsonl"):
        p = manifest_dir / name
        if not p.exists():
            continue
        for line in p.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            if _valid(r):
                recs.append(r)
    return recs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--inputs", nargs="+", required=True, help="manifest dirs (each with train.jsonl[/val.jsonl])")
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--train-ratio", type=float, default=0.9)
    ap.add_argument("--val-ratio", type=float, default=0.05)
    ap.add_argument("--seed", type=int, default=1337)
    args = ap.parse_args()

    rng = random.Random(args.seed + 1)
    by_source: dict[str, list[dict]] = {}
    total_in = 0
    for md in args.inputs:
        recs = load_records(Path(md))
        total_in += len(recs)
        for r in recs:
            sid = str(r.get("sourceId") or r.get("id") or "unknown")
            by_source.setdefault(sid, []).append(r)
        print(f"  {md}: {len(recs)} valid records, {len({r.get('sourceId') for r in recs})} sources")
    print(f"pooled: {total_in} records, {len(by_source)} unique sources")

    splits = {"train": [], "val": []}
    for sid, recs in by_source.items():
        r = rng.random()
        if r < args.train_ratio:
            splits["train"].extend(recs)
        elif r < args.train_ratio + args.val_ratio:
            splits["val"].extend(recs)
        # else: hold-out test (not written)
    rng.shuffle(splits["train"])
    rng.shuffle(splits["val"])

    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    for name, rows in (("train", splits["train"]), ("val", splits["val"])):
        text = "".join(json.dumps(r, sort_keys=True) + "\n" for r in rows)
        (out / f"{name}.jsonl").write_text(text, encoding="utf-8")

    items = (
        [{"id": r.get("id"), "sourceId": r.get("sourceId"), "split": "train"} for r in splits["train"]]
        + [{"id": r.get("id"), "sourceId": r.get("sourceId"), "split": "val"} for r in splits["val"]]
    )
    manifest = {
        "schemaVersion": 1,
        "createdBy": "merge_manifests.py",
        "corpusName": "merged",
        "sources": args.inputs,
        "trainRatio": args.train_ratio,
        "valRatio": args.val_ratio,
        "featureExtractor": "features.py (parity with C++ analyzers)",
        "labelSource": "per-corpus teacher (T1 labels.py; T2 if --teacher t2 was used)",
        "referenceQuality": "synthetic",
        "lufsDependency": "in-house BS.1770-4 (features.compute_loudness)",
        "items": items,
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"DONE: {len(splits['train'])} train, {len(splits['val'])} val "
          f"({len(by_source) - len(splits['train']) - len(splits['val']) and len(by_source)} sources, test held out) -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
