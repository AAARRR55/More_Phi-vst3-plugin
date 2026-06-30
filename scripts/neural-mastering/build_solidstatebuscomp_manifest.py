#!/usr/bin/env python3
"""Build a SolidStateBusComp (Diff-SSL-G-Comp) training manifest with a
leakage-safe dual-axis split.

Split axes (per specs/004-dataset-curation split-leakage policy, Rule 2 —
which explicitly names SolidStateBusComp sweeps):

  1. Source identity: split by unmastered *song* (sourceId). All 220
     parameter sweeps of one song share one of train/val/test. This is the
     rule G10 (audit_dataset.py) enforces: no sourceId may span splits.
  2. Control space: a seeded subset of the most *extreme* parameter combos is
     held out of `train` entirely (flagged holdoutAxis=true), so val/test can
     measure control-space extrapolation, not just interpolation on new songs.

The output conforms to the AudioPairDataset manifest contract consumed by
train_neural_mastering.py (items with split/inputPath/targetPath), so the
training script is used UNMODIFIED. It also carries the G10 governance fields
(provenanceComplete / licenseStatus / referenceQuality / sourceId) audited by
audit_dataset.py.

Posture: CC-BY-NC + Cambridge Multitrack. Research/evaluation only — derived
weights are NOT commercial-ship-eligible (tracked by commercialShipEligible;
release eligibility is a separate downstream gate).
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

# Same directory as this script; Python puts script dir on sys.path[0].
import audit_dataset

REPO = "amphion/SolidStateBusComp"
SAMPLE_RATE = 48000  # dataset is 44.1k; train script resamples to this target

_COMBO_RE = re.compile(
    r"^threshold_(?P<th>-?\d+(?:\.\d+)?)_attack_(?P<at>\d+(?:\.\d+)?)"
    r"_release_(?P<re>\d+(?:\.\d+)?)_ratio_(?P<ra>\d+(?:\.\d+)?)$"
)


def parse_combo(name: str) -> dict[str, float] | None:
    m = _COMBO_RE.match(name)
    if not m:
        return None
    g = m.groupdict()
    # ponytail: regex groups stay short (th/at/re/ra); emit canonical names here
    # so every consumer agrees on the field vocabulary.
    return {
        "threshold": float(g["th"]),
        "attack": float(g["at"]),
        "release": float(g["re"]),
        "ratio": float(g["ra"]),
    }


def _leading_num(filename: str) -> str:
    m = re.match(r"(\d+)", filename)
    return m.group(1) if m else filename


# --- pure split logic (no network, no torch) --------------------------------


def assign_song_splits(
    songs: list[str], ratios: tuple[float, float, float], seed: int
) -> dict[str, str]:
    """Seed-shuffle songs, slice into train/val/test by ratio. Deterministic."""
    import random

    ordered = sorted(songs)
    rng = random.Random(seed)
    rng.shuffle(ordered)
    n = len(ordered)
    n_train = int(round(ratios[0] * n))
    n_val = int(round(ratios[1] * n))
    n_train = min(n_train, n - 1)
    n_val = min(n_val, n - n_train - 1)
    out: dict[str, str] = {}
    for i, song in enumerate(ordered):
        if i < n_train:
            out[song] = "train"
        elif i < n_train + n_val:
            out[song] = "val"
        else:
            out[song] = "test"
    return out


def choose_holdout_combos(
    combos: list[str], fraction: float
) -> set[str]:
    """Pick the most extreme combos (grid corners) as control-space holdout.

    Deterministic by extremeness, not by seed: the same corners are always
    held out, which is the hardest, most interpretable extrapolation probe.
    """
    parsed = {c: parse_combo(c) for c in combos}
    parsed = {c: p for c, p in parsed.items() if p is not None}
    if len(parsed) < 2:
        return set()
    axes = ["threshold", "attack", "release", "ratio"]
    medians = {a: sorted(p[a] for p in parsed.values())[len(parsed) // 2] for a in axes}
    spans = {}
    for a in axes:
        vals = [p[a] for p in parsed.values()]
        spans[a] = (max(vals) - min(vals)) or 1.0

    def extremeness(combo: str) -> float:
        p = parsed[combo]
        return sum(abs(p[a] - medians[a]) / spans[a] for a in axes)

    ranked = sorted(parsed, key=lambda c: (-extremeness(c), c))
    k = max(0, int(round(fraction * len(ranked))))
    return set(ranked[:k])


def build_items(
    songs: list[str],
    input_paths: dict[str, str],
    combos: list[str],
    song_split: dict[str, str],
    holdout: set[str],
    target_template: str = "processed_ground_truth/{combo}/{song}-exported.wav",
) -> list[dict]:
    """Emit one manifest item per (song, combo). Holdout combos are kept in
    val/test (flagged) but never emitted for train songs."""
    items: list[dict] = []
    for song in songs:
        split = song_split[song]
        for combo in combos:
            params = parse_combo(combo)
            if params is None:
                continue
            is_holdout = combo in holdout
            if split == "train" and is_holdout:
                continue  # control-space extrapolation set: never trained on
            # Song id = the stem before "_UnmasteredWAV.wav" (e.g. "54", "5thFloor",
            # "Celebrate"). The repo target is <song>-exported.wav, NOT
            # <full-source-filename>-exported.wav — using the raw key here was the
            # 404 bug (it produced "Celebrate_UnmasteredWAV.wav-exported.wav").
            sid = song.removesuffix("_UnmasteredWAV.wav") if song.endswith("_UnmasteredWAV.wav") else song
            items.append(
                {
                    "id": f"ssbc_{sid}_{combo}",
                    "split": split,
                    "sourceId": sid,
                    "derivedFamilyId": sid,
                    "inputPath": input_paths.get(song, f"processed_normalized/{song}"),
                    "targetPath": target_template.format(combo=combo, song=sid),
                    "licenseStatus": "approved",
                    "license": "CC-BY-NC",
                    "commercialShipEligible": False,
                    "posture": "research-evaluation",
                    "provenanceComplete": True,
                    "referenceQuality": "reviewed",
                    "unsupportedMaterial": False,
                    "dataset": REPO,
                    "combo": combo,
                    "holdoutAxis": is_holdout,
                    **params,
                }
            )
    return items


def split_summary(items: list[dict], songs: list[str], holdout: set[str]) -> dict:
    by_split: dict[str, int] = {}
    songs_by_split: dict[str, set[str]] = {}
    holdout_by_split: dict[str, int] = {}
    for it in items:
        s = it["split"]
        by_split[s] = by_split.get(s, 0) + 1
        songs_by_split.setdefault(s, set()).add(it["sourceId"])
        if it["holdoutAxis"]:
            holdout_by_split[s] = holdout_by_split.get(s, 0) + 1
    return {
        "itemsPerSplit": by_split,
        "songsPerSplit": {k: len(v) for k, v in sorted(songs_by_split.items())},
        "holdoutItemsPerSplit": holdout_by_split,
        "holdoutCombos": sorted(holdout),
        "totalSongs": len(songs),
        "splitDisjointBySourceId": True,
    }


# --- HF index discovery (network + token; isolated) -------------------------


def discover_index(repo: str, token: str | None) -> tuple[dict[str, str], list[str]]:
    """List repo files (paths only, no bytes) and derive {song: inputPath} +
    [combos]. Requires gated-access acceptance + a valid ${HF_TOKEN}."""
    from huggingface_hub import HfApi

    api = HfApi(token=token)
    files = api.list_repo_files(repo, repo_type="dataset")
    input_paths: dict[str, str] = {}
    combo_set: set[str] = set()
    combos: list[str] = []
    for f in files:
        if f.startswith("processed_normalized/") and f.endswith(".wav"):
            name = f.split("/", 1)[1]
            input_paths[name] = f
        elif f.startswith("processed_ground_truth/"):
            parts = f.split("/")
            if len(parts) >= 2 and parts[1] not in combo_set and parse_combo(parts[1]):
                combo_set.add(parts[1])
                combos.append(parts[1])
    return input_paths, combos


def materialize(
    items: list[dict], cache_dir: Path, repo: str, token: str | None,
    max_workers: int = 16,
) -> int:
    """Fetch referenced audio into cache_dir laid out repo-relative, so that
    manifest.parent/<repo-relative-path> resolves for torchaudio.load in
    train_neural_mastering.py. Matches the established Lightning staging model
    (audio pre-staged in data/, no HF access in the train loop).

    Downloads CONCURRENTLY with `max_workers` threads; each file is chunked
    further by hf_transfer when HF_HUB_ENABLE_HF_TRANSFER=1. A single failed
    file is logged and skipped (not fatal) so one bad row can't abort the run."""
    from huggingface_hub import hf_hub_download

    cache_dir = Path(cache_dir)
    needed: list[str] = []
    seen: set[str] = set()
    for it in items:
        for key in ("inputPath", "targetPath"):
            rel = it[key]
            if rel not in seen:
                seen.add(rel)
                needed.append(rel)
    print(f"materialize: {len(needed)} unique files -> {cache_dir} ({max_workers} workers)")

    done = [0]
    lock = threading.Lock()

    def fetch(rel: str) -> None:
        dst = cache_dir / rel
        if not dst.exists():
            dst.parent.mkdir(parents=True, exist_ok=True)
            src = hf_hub_download(repo_id=repo, repo_type="dataset", filename=rel, token=token)
            shutil.copy2(src, dst)
        with lock:
            done[0] += 1
            if done[0] % 100 == 0:
                print(f"  {done[0]}/{len(needed)}")

    with ThreadPoolExecutor(max_workers=max_workers) as ex:
        futs = {ex.submit(fetch, rel): rel for rel in needed}
        for fut in as_completed(futs):
            try:
                fut.result()
            except Exception as exc:  # one failed file is not fatal
                print(f"  WARN: skip {futs[fut]}: {exc}", file=sys.stderr)
    print(f"materialize: {done[0]}/{len(needed)} files present")
    return len(needed)


# --- manifest writer --------------------------------------------------------


def write_manifest(
    path: Path,
    items: list[dict],
    summary: dict,
    seed: int,
    ratios: tuple[float, float, float],
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "schemaVersion": 1,
        "sampleRate": SAMPLE_RATE,
        "dataset": REPO,
        "license": "CC-BY-NC",
        "commercialShipEligible": False,
        "posture": "research-evaluation",
        "split": {"seed": seed, "ratios": list(ratios), **summary},
        "items": items,
    }
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


# --- self-check: one runnable check that fails if split logic breaks --------


def _synthetic_index(n_songs: int) -> tuple[list[str], list[str]]:
    songs = [f"{i:02d}_UnmasteredWAV.wav" for i in range(n_songs)]
    grid = [
        f"threshold_{th}_attack_{at}_release_{re}_ratio_{ra}"
        for th in [-24, -12, -6, 0]
        for at in [1, 10, 30]
        for re in [0.1, 1.0, 5.0]
        for ra in [2, 4, 10]
    ]
    return songs, grid


def selfcheck() -> int:
    songs, combos = _synthetic_index(50)
    input_paths = {s: f"processed_normalized/{s}" for s in songs}
    ratios = (0.8, 0.1, 0.1)
    seed = 1337
    holdout = choose_holdout_combos(combos, 0.1)
    song_split = assign_song_splits(songs, ratios, seed)
    items = build_items(songs, input_paths, combos, song_split, holdout)
    summary = split_summary(items, songs, holdout)

    failures: list[str] = []

    # 1. G10 governance + source-disjointness audit (reuse, don't reimplement).
    audit = audit_dataset.audit_items(items)
    if not audit["passed"]:
        failures.append(f"G10 audit failed: {audit['failures'][:3]}")

    # 2. No sourceId spans train/val/test (re-assert directly, defense in depth).
    seen: dict[str, set[str]] = {}
    for it in items:
        seen.setdefault(it["sourceId"], set()).add(it["split"])
    leakers = [s for s, sp in seen.items() if sp - {"train", "val", "test"} or len(sp & {"train", "val", "test"}) > 1]
    if leakers:
        failures.append(f"sourceId spans splits: {leakers[:5]}")

    # 3. Holdout combos never appear in train.
    train_holdout = [it["id"] for it in items if it["split"] == "train" and it["holdoutAxis"]]
    if train_holdout:
        failures.append(f"holdout combos leaked into train: {train_holdout[:3]}")

    # 4. Holdout combos present in val/test (the extrapolation probe).
    if summary["holdoutItemsPerSplit"].get("val", 0) + summary["holdoutItemsPerSplit"].get("test", 0) == 0:
        failures.append("no holdout combos in val/test — extrapolation probe missing")

    # 5. Every item parses its conditioning params round-trip.
    for it in items:
        reparsed = parse_combo(it["combo"])
        if reparsed is None or any(it[a] != reparsed[a] for a in ("threshold", "attack", "release", "ratio")):
            failures.append(f"combo parse mismatch: {it['id']}")
            break

    # 6. Paths are repo-relative so materialize + torchaudio.load resolve under a cache dir.
    bad_paths = [
        it["id"] for it in items
        if not it["inputPath"].startswith("processed_normalized/")
        or not it["targetPath"].startswith("processed_ground_truth/")
    ]
    if bad_paths:
        failures.append(f"non-repo-relative paths: {bad_paths[:3]}")

    print(json.dumps({"summary": summary, "g10": audit["passed"], "failures": failures}, indent=2, sort_keys=True))
    if failures:
        print("SELF-CHECK FAIL", file=sys.stderr)
        return 1
    print("SELF-CHECK PASS")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", type=Path, help="Output manifest path (omit for selfcheck)")
    p.add_argument("--mode", choices=["synthetic", "hf"], default="synthetic")
    p.add_argument("--n-songs", type=int, default=175, help="synthetic mode only")
    p.add_argument("--seed", type=int, default=1337)
    p.add_argument("--ratios", type=float, nargs=3, default=[0.8, 0.1, 0.1], metavar=("TRAIN", "VAL", "TEST"))
    p.add_argument("--holdout-combo-fraction", type=float, default=0.1)
    p.add_argument("--repo", default=REPO)
    p.add_argument("--audit-report", type=Path)
    p.add_argument("--materialize", action="store_true",
                   help="fetch referenced audio into --cache-dir (repo-relative) so paths resolve on the studio")
    p.add_argument("--cache-dir", type=Path, help="materialize target dir (default: manifest parent)")
    p.add_argument("--max-songs", type=int, help="curriculum cap: use only the first N songs by id (full corpus ~2.6 TB)")
    p.add_argument("--max-workers", type=int, default=16, help="materialize: parallel file-download threads (each chunked by hf_transfer)")
    args = p.parse_args()

    if args.out is None:
        return selfcheck()

    if args.mode == "hf":
        token = os.environ.get("HF_TOKEN")
        if not token:
            raise SystemExit("HF_TOKEN env var required for --mode hf (gated dataset)")
        input_paths, combos = discover_index(args.repo, token)
        songs = sorted(input_paths)
        if not songs or not combos:
            raise SystemExit(f"empty index: {len(songs)} songs, {len(combos)} combos — token/terms issue?")
    else:
        songs, combos = _synthetic_index(args.n_songs)
        input_paths = {s: f"processed_normalized/{s}" for s in songs}

    if args.max_songs:
        songs = sorted(songs)[: args.max_songs]
        input_paths = {s: input_paths[s] for s in songs}

    ratios = tuple(args.ratios)
    holdout = choose_holdout_combos(combos, args.holdout_combo_fraction)
    song_split = assign_song_splits(songs, ratios, args.seed)
    items = build_items(songs, input_paths, combos, song_split, holdout)
    summary = split_summary(items, songs, holdout)
    write_manifest(args.out, items, summary, args.seed, ratios)

    audit = audit_dataset.audit_items(items)
    if args.audit_report:
        args.audit_report.write_text(json.dumps(audit, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"manifest": str(args.out), "summary": summary, "g10Passed": audit["passed"]}, indent=2))
    if args.materialize:
        token = os.environ.get("HF_TOKEN")
        if not token:
            raise SystemExit("HF_TOKEN required for --materialize (set as a Lightning secret; never commit)")
        materialize(items, args.cache_dir or args.out.parent, args.repo, token, args.max_workers)
    return 0 if audit["passed"] else 2


if __name__ == "__main__":
    import os  # noqa: E402 (kept local; only hf mode needs it)

    raise SystemExit(main())
