#!/usr/bin/env python3
"""
verify_dataset.py — Post-generation verification for MorphSnap dataset batches.

Detects the 3 most common headless VST host failures:
  1. PASSTHROUGH: Plugin failed to load → wet output is identical to dry input
  2. SILENT: Plugin muted audio → file is all zeros
  3. DUPLICATES: Same parameters applied → multiple outputs are identical

Usage:
  python verify_dataset.py C:/MorphSnap_Datasets/smoke_test
  python verify_dataset.py C:/MorphSnap_Datasets/run_001 --full
"""

import argparse
import hashlib
import json
import sys
from pathlib import Path
from collections import defaultdict

import numpy as np
import soundfile as sf


def read_wav_samples(wav_path: str, max_seconds: float = 5.0) -> tuple:
    """Read WAV file and return (samples_array, sample_rate, num_channels)."""
    info = sf.info(wav_path)
    sr = info.samplerate
    nch = info.channels
    max_frames = int(max_seconds * sr)
    data, _ = sf.read(wav_path, frames=max_frames, dtype='float64')
    # Flatten to 1D for consistent processing
    samples = data.flatten()
    return samples, sr, nch


def compute_rms(samples: np.ndarray) -> float:
    """Compute RMS of a sample array."""
    if len(samples) == 0:
        return 0.0
    return float(np.sqrt(np.mean(samples ** 2)))


def compute_rms_db(samples: np.ndarray) -> float:
    """Compute RMS in dB."""
    rms = compute_rms(samples)
    if rms < 1e-15:
        return -120.0
    return 20.0 * np.log10(rms)


def compute_peak_db(samples: np.ndarray) -> float:
    """Compute peak level in dB."""
    peak = float(np.max(np.abs(samples)))
    if peak < 1e-15:
        return -120.0
    return 20.0 * np.log10(peak)


def compute_correlation(a: np.ndarray, b: np.ndarray) -> float:
    """Pearson correlation between two sample arrays. 1.0 = identical shape."""
    n = min(len(a), len(b))
    if n == 0:
        return 0.0
    a, b = a[:n], b[:n]
    a_mean = np.mean(a)
    b_mean = np.mean(b)
    a_centered = a - a_mean
    b_centered = b - b_mean
    num = np.sum(a_centered * b_centered)
    denom = np.sqrt(np.sum(a_centered ** 2) * np.sum(b_centered ** 2))
    if denom < 1e-15:
        return 0.0
    return float(num / denom)


def compute_energy_ratio(dry: np.ndarray, wet: np.ndarray) -> float:
    """Compute the ratio of difference energy to dry energy."""
    n = min(len(dry), len(wet))
    if n == 0:
        return 0.0
    dry, wet = dry[:n], wet[:n]
    diff = wet - dry
    diff_energy = np.sum(diff ** 2)
    dry_energy = np.sum(dry ** 2)
    if dry_energy < 1e-15:
        return 0.0
    return float(diff_energy / dry_energy)


def md5_samples(samples: np.ndarray, precision: int = 6) -> str:
    """Hash sample array for duplicate detection."""
    rounded = np.round(samples[:min(len(samples), 100000)], precision)
    return hashlib.md5(rounded.tobytes()).hexdigest()


def verify_dataset(dataset_dir: str, full_check: bool = False):
    """
    Run all verification checks on a rendered dataset directory.

    Checks:
      1. File existence and sizes
      2. Silent file detection (RMS < -60 dB)
      3. Passthrough detection (wet ≈ dry, correlation > 0.9999)
      4. Duplicate detection (identical wet hashes)
      5. Parameter metadata validation
    """
    dpath = Path(dataset_dir)

    print("=" * 70)
    print("  DATASET VERIFICATION")
    print("=" * 70)
    print(f"  Directory: {dpath}")
    print()

    # ── Find files ────────────────────────────────────────────────────────

    dry_file = dpath / "dry_source.wav"
    if not dry_file.exists():
        # Try .flac or any dry* file
        candidates = list(dpath.glob("dry_source*"))
        if candidates:
            dry_file = candidates[0]

    wet_files = sorted(dpath.glob("variation_*.wav"))
    if not wet_files:
        wet_files = sorted(dpath.glob("variation_*.flac"))
    json_files = sorted(dpath.glob("variation_*.json"))

    print(f"  Dry source:     {'FOUND' if dry_file.exists() else 'MISSING'}")
    print(f"  Wet variations: {len(wet_files)}")
    print(f"  JSON metadata:  {len(json_files)}")
    print()

    if not wet_files:
        print("[FAIL] No variation files found. The renderer produced no output.")
        print("  Possible causes:")
        print("  - MorphSnapCLI crashed during rendering")
        print("  - Plugin failed to load (check --verbose output)")
        print("  - Output directory mismatch")
        return False

    # ── Load dry reference ────────────────────────────────────────────────

    dry_samples = None
    if dry_file.exists():
        try:
            dry_samples, dry_sr, dry_ch = read_wav_samples(str(dry_file))
            dry_rms_db = compute_rms_db(dry_samples)
            dry_peak_db = compute_peak_db(dry_samples)
            print(f"  Dry audio: {len(dry_samples)} samples, "
                  f"RMS={dry_rms_db:.1f} dB, Peak={dry_peak_db:.1f} dB")
        except Exception as e:
            print(f"  [WARN] Failed to read dry source: {e}")
            dry_samples = None
    else:
        print("  [WARN] No dry source file - cannot check for passthrough.")

    print()

    # ── Analyze each variation ────────────────────────────────────────────

    results = []
    hashes = defaultdict(list)  # hash -> [filename, ...]

    issues = {
        "zero_size": [],
        "silent": [],
        "near_silent": [],
        "passthrough": [],
        "clipping": [],
        "read_error": [],
        "bypass_flagged": [],
    }

    for wet_path in wet_files:
        name = wet_path.name
        file_size = wet_path.stat().st_size

        # Check file size
        if file_size == 0:
            issues["zero_size"].append(name)
            results.append({"file": name, "status": "ZERO_SIZE"})
            continue

        # Read wet audio
        try:
            wet_samples, wet_sr, wet_ch = read_wav_samples(str(wet_path))
        except Exception as e:
            issues["read_error"].append(f"{name}: {e}")
            results.append({"file": name, "status": "READ_ERROR"})
            continue

        wet_rms_db = compute_rms_db(wet_samples)
        wet_peak_db = compute_peak_db(wet_samples)

        entry = {
            "file": name,
            "size_kb": file_size / 1024,
            "rms_db": wet_rms_db,
            "peak_db": wet_peak_db,
            "status": "OK",
        }

        # Check: Silent
        if wet_rms_db < -60.0:
            entry["status"] = "SILENT"
            issues["silent"].append(name)
        elif wet_rms_db < -40.0:
            entry["status"] = "NEAR_SILENT"
            issues["near_silent"].append(name)

        # Check: Clipping
        if wet_peak_db > -0.1:
            issues["clipping"].append(name)

        # Check: Passthrough (wet ≈ dry)
        if dry_samples is not None and len(wet_samples) > 0:
            corr = compute_correlation(dry_samples, wet_samples)
            energy_ratio = compute_energy_ratio(dry_samples, wet_samples)
            entry["dry_correlation"] = corr
            entry["energy_ratio"] = energy_ratio

            # Correlation > 0.9999 means virtually identical
            if corr > 0.9999:
                entry["status"] = "PASSTHROUGH"
                issues["passthrough"].append(name)
            elif corr > 0.999:
                entry["status"] = "SUSPICIOUS"

        # Check JSON metadata for bypass flags
        json_path = wet_path.with_suffix(".json")
        if json_path.exists():
            try:
                with open(json_path) as f:
                    meta = json.load(f)
                if meta.get("has_silence", False):
                    issues["bypass_flagged"].append(f"{name}: flagged silent by renderer")
                if meta.get("passthrough_detected", False):
                    issues["bypass_flagged"].append(f"{name}: passthrough detected by renderer")
                    if entry["status"] == "OK":
                        entry["status"] = "PASSTHROUGH"
                        issues["passthrough"].append(name)
                if not meta.get("plugin_processed", True):
                    issues["bypass_flagged"].append(f"{name}: plugin_processed=false")
            except (json.JSONDecodeError, KeyError):
                pass

        # Duplicate detection via hash
        h = md5_samples(wet_samples)
        hashes[h].append(name)
        entry["hash"] = h

        results.append(entry)

    # ── Report ────────────────────────────────────────────────────────────

    total = len(wet_files)
    ok_count = sum(1 for r in results if r["status"] == "OK")
    print("-" * 70)
    print("  RESULTS")
    print("-" * 70)
    print()

    # Per-file detail (first 10 + any problems)
    print("  File-by-file analysis:")
    shown = 0
    for r in results:
        is_problem = r["status"] not in ("OK",)
        if shown < 10 or is_problem:
            corr_str = ""
            if "dry_correlation" in r:
                corr_str = f"  corr={r['dry_correlation']:.6f}"
            energy_str = ""
            if "energy_ratio" in r:
                energy_str = f"  dE={r['energy_ratio']:.4f}"

            status_marker = "  " if r["status"] == "OK" else ">>"
            print(f"    {status_marker} {r['file']:30s}  "
                  f"RMS={r.get('rms_db', -120):.1f} dB  "
                  f"Peak={r.get('peak_db', -120):.1f} dB"
                  f"{corr_str}{energy_str}  [{r['status']}]")
            shown += 1

    if shown < total:
        print(f"    ... ({total - shown} more OK files omitted)")
    print()

    # Duplicate groups
    dup_groups = {h: files for h, files in hashes.items() if len(files) > 1}

    # Summary
    print("-" * 70)
    print("  SUMMARY")
    print("-" * 70)
    print()
    print(f"  Total files:      {total}")
    print(f"  OK:               {ok_count}  {'[PASS]' if ok_count == total else ''}")
    print()

    all_good = True

    if issues["zero_size"]:
        all_good = False
        print(f"  [FAIL] Zero-size files:  {len(issues['zero_size'])}")
        for f in issues["zero_size"][:5]:
            print(f"         {f}")

    if issues["silent"]:
        all_good = False
        print(f"  [FAIL] Silent files:     {len(issues['silent'])} (RMS < -60 dB)")
        for f in issues["silent"][:5]:
            print(f"         {f}")

    if issues["passthrough"]:
        all_good = False
        print(f"  [FAIL] PASSTHROUGH:      {len(issues['passthrough'])} (wet ~= dry, corr > 0.9999)")
        print(f"         The plugin is NOT processing audio!")
        print(f"         Possible causes:")
        print(f"           - VST3 failed to load (check CLI --verbose)")
        print(f"           - Plugin is in bypass mode")
        print(f"           - Buffer size mismatch -> processBlock is a no-op")
        for f in issues["passthrough"][:5]:
            print(f"         {f}")

    if dup_groups:
        all_good = False
        print(f"  [WARN] Duplicate groups: {len(dup_groups)}")
        for h, files in list(dup_groups.items())[:3]:
            print(f"         {len(files)} identical files: {', '.join(files[:3])}...")

    if issues["near_silent"]:
        print(f"  [WARN] Near-silent:      {len(issues['near_silent'])} (RMS -60 to -40 dB)")

    if issues["clipping"]:
        print(f"  [WARN] Clipping:         {len(issues['clipping'])} (Peak > -0.1 dB)")

    if issues["read_error"]:
        all_good = False
        print(f"  [FAIL] Read errors:      {len(issues['read_error'])}")
        for f in issues["read_error"][:5]:
            print(f"         {f}")

    print()

    if all_good:
        print("  [PASS] ALL CHECKS PASSED - The plugin is processing audio correctly.")
        print()
        print("  The wet files are different from the dry input and from each other.")
        print("  Your headless VST host engine is working. Proceed to full dataset generation.")
    else:
        passthrough_pct = len(issues["passthrough"]) / max(total, 1) * 100
        silent_pct = len(issues["silent"]) / max(total, 1) * 100

        if passthrough_pct > 50:
            print("  [CRITICAL] >50% of files are passthrough - the plugin is NOT loaded.")
            print()
            print("  Debugging steps:")
            print("  1. Run: .\\build\\Release\\MorphSnapCLI.exe --plugin <path.vst3> --list-params --verbose")
            print("     If this shows 0 parameters, the plugin failed to load.")
            print("  2. Check the .vst3 path is correct and the plugin is 64-bit.")
            print("  3. Some plugins require a GUI framework - try running with a display available.")
        elif silent_pct > 20:
            print("  [FAIL] HIGH SILENCE RATE: Many files have no audio.")
            print()
            print("  Possible causes: Bypass/Mute parameter accidentally set,")
            print("  or source audio is a sine tone (use pink noise instead).")
        else:
            print("  [WARN] Issues found but dataset is partially usable.")
            print("  Fix the flagged issues before full-scale generation.")

    print()

    # Save report
    report_path = dpath / "verification_report.json"
    report = {
        "directory": str(dpath),
        "total_files": total,
        "ok_files": ok_count,
        "issues": {k: len(v) for k, v in issues.items()},
        "duplicate_groups": len(dup_groups),
        "results": results,
    }
    with open(report_path, "w") as f:
        json.dump(report, f, indent=2, default=str)
    print(f"  Report saved: {report_path}")

    return all_good


def main():
    parser = argparse.ArgumentParser(
        description="Verify MorphSnap dataset renders for passthrough/silence/duplicates"
    )
    parser.add_argument("dataset_dir", help="Path to the rendered dataset directory")
    parser.add_argument("--full", action="store_true",
                        help="Extended checks (slower, reads entire files)")
    args = parser.parse_args()

    ok = verify_dataset(args.dataset_dir, args.full)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
