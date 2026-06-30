#!/usr/bin/env python3
"""Parallel-download the files referenced by an EXISTING manifest, skipping the
slow repo index (list_repo_files). Use after build_solidstatebuscomp_manifest.py
has written the manifest — to materialize, or to change the worker count without
re-paying the ~1-2 min index phase on every restart.

Reads HF_TOKEN from the environment (or the cached token if unset). dst.exists()
makes it resumable/incremental. One failed file is logged + skipped (not fatal).

Usage:
  HF_TOKEN=... HF_XET_HIGH_PERFORMANCE=1 python materialize_from_manifest.py \\
    --manifest data/ssbc/manifest.json --cache-dir data/ssbc --max-workers 42
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

import build_solidstatebuscomp_manifest as B  # reuse materialize() + REPO


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--manifest", type=Path, required=True)
    p.add_argument("--cache-dir", type=Path, required=True)
    p.add_argument("--repo", default=B.REPO)
    p.add_argument("--max-workers", type=int, default=16)
    args = p.parse_args()

    token = os.environ.get("HF_TOKEN")
    payload = json.loads(args.manifest.read_text(encoding="utf-8"))
    items = payload.get("items", payload if isinstance(payload, list) else [])
    if not items:
        raise SystemExit(f"no items in {args.manifest}")
    B.materialize(items, args.cache_dir, args.repo, token, args.max_workers)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
