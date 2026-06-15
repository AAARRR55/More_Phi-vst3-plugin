"""Audit dataset quality for ML readiness."""
import json, glob, os, sys
import numpy as np
from pathlib import Path

PARAM_VECTOR_LENGTH = 737
BAND_COUNT = 24
BAND_STRIDE = 24
BAND_PARAM_COUNT = BAND_COUNT * BAND_STRIDE
GLOBAL_PARAM_START = BAND_PARAM_COUNT

def audit(dataset_dir, spectro_dir):
    print("=" * 70)
    print("  ML DATASET QUALITY AUDIT")
    print("=" * 70)

    # 1. Load all JSON metadata
    jsons = sorted(glob.glob(os.path.join(dataset_dir, "variation_*.json")))
    print(f"\n  Variations: {len(jsons)}")

    peaks, rmses, norms = [], [], []
    all_params = []
    passthrough = 0
    silent = 0
    near_silent = 0
    clipping = 0

    for jf in jsons:
        with open(jf) as f:
            m = json.load(f)
        peaks.append(m["peak_db"])
        rmses.append(m["rms_db"])
        norms.append(m.get("normalization_gain_db", 0))
        if m.get("passthrough_detected", False):
            passthrough += 1
        if m.get("has_silence", False):
            silent += 1
        if m["rms_db"] < -40:
            near_silent += 1
        if m["rms_db"] > -0.1 or m.get("has_clipping", False):
            clipping += 1

        pdict = {p["index"]: p["value"] for p in m["parameters"]}
        param_vec = [pdict.get(i, 0.0) for i in range(PARAM_VECTOR_LENGTH)]
        all_params.append(param_vec)

    peaks = np.array(peaks)
    rmses = np.array(rmses)
    norms = np.array(norms)
    all_params = np.array(all_params)

    # 2. Basic health
    print("\n--- HEALTH CHECK ---")
    print(f"  Passthrough:   {passthrough} ({100*passthrough/len(jsons):.1f}%)")
    print(f"  Silent:        {silent}")
    print(f"  Near-silent:   {near_silent} ({100*near_silent/len(jsons):.1f}%)")
    print(f"  Clipping:      {clipping}")
    health_ok = passthrough == 0 and silent == 0 and near_silent == 0
    print(f"  Health:        {'PASS' if health_ok else 'WARN'}")

    # 3. Dynamic range
    print("\n--- DYNAMIC RANGE ---")
    print(f"  Peak dB:  min={peaks.min():.1f}  max={peaks.max():.1f}  mean={peaks.mean():.1f}  std={peaks.std():.1f}")
    print(f"  RMS dB:   min={rmses.min():.1f}  max={rmses.max():.1f}  mean={rmses.mean():.1f}  std={rmses.std():.1f}")
    print(f"  Normalized: {np.sum(norms < -0.1)} / {len(norms)} ({100*np.sum(norms < -0.1)/len(norms):.0f}%)")
    rms_spread = rmses.max() - rmses.min()
    print(f"  RMS spread: {rms_spread:.1f} dB  {'GOOD (>15 dB)' if rms_spread > 15 else 'LOW - may lack diversity'}")

    # 4. Parameter diversity
    print("\n--- PARAMETER DIVERSITY ---")
    # Check which parameters actually vary
    param_std = all_params.std(axis=0)
    varying = np.sum(param_std > 0.01)
    constant = np.sum(param_std <= 0.01)
    print(f"  Varying params:  {varying} / {PARAM_VECTOR_LENGTH}")
    print(f"  Constant params: {constant} / {PARAM_VECTOR_LENGTH}")

    # Band activation analysis
    # Pro-Q 4 layout used by this dataset: 24 bands * 24 parameters = 576 band
    # parameters (indices 0-575). Indices 576-736 are global/UI parameters.
    print(f"  Band/global layout: {BAND_PARAM_COUNT} band params + {PARAM_VECTOR_LENGTH - GLOBAL_PARAM_START} global/UI params")
    bands_active = []
    for row in all_params:
        count = 0
        for b in range(BAND_COUNT):
            base = b * BAND_STRIDE
            if row[base] > 0.5:  # "Used" param (offset +0 in band layout)
                count += 1
        bands_active.append(count)
    bands_active = np.array(bands_active)
    print(f"  Bands active:  min={bands_active.min()}  max={bands_active.max()}  mean={bands_active.mean():.1f}")
    for n in range(1, 5):
        pct = 100 * np.sum(bands_active == n) / len(bands_active)
        print(f"    {n} band(s): {pct:.1f}%")

    # Gain distribution (offset +3 per band)
    gains = []
    for row in all_params:
        for b in range(BAND_COUNT):
            base = b * BAND_STRIDE
            if row[base] > 0.5:  # only active bands
                gains.append(row[base + 3])
    gains = np.array(gains)
    if len(gains) > 0:
        print(f"  Active band gains: n={len(gains)} mean={gains.mean():.3f} std={gains.std():.3f}")
        print(f"    range: [{gains.min():.3f}, {gains.max():.3f}]")

    # Frequency distribution (offset +2 per band)
    freqs = []
    for row in all_params:
        for b in range(BAND_COUNT):
            base = b * BAND_STRIDE
            if row[base] > 0.5:
                freqs.append(row[base + 2])
    freqs = np.array(freqs)
    if len(freqs) > 0:
        print(f"  Active band freqs: n={len(freqs)} mean={freqs.mean():.3f} std={freqs.std():.3f}")
        lo = np.sum(freqs < 0.33)
        mid = np.sum((freqs >= 0.33) & (freqs < 0.66))
        hi = np.sum(freqs >= 0.66)
        print(f"    Low/Mid/High: {100*lo/len(freqs):.0f}% / {100*mid/len(freqs):.0f}% / {100*hi/len(freqs):.0f}%")

    # 5. Duplicate check (parameter vectors)
    print("\n--- DUPLICATE CHECK ---")
    param_hashes = set()
    dupes = 0
    for row in all_params:
        h = hash(row.tobytes())
        if h in param_hashes:
            dupes += 1
        param_hashes.add(h)
    print(f"  Duplicate param vectors: {dupes}")
    print(f"  Unique param vectors:    {len(param_hashes)}")

    # 6. Spectrogram spot check
    print("\n--- SPECTROGRAM CHECK ---")
    npzs = sorted(glob.glob(os.path.join(spectro_dir, "*.npz")))
    print(f"  NPZ files: {len(npzs)}")
    if npzs:
        sample_indices = [0, len(npzs)//4, len(npzs)//2, 3*len(npzs)//4, len(npzs)-1]
        shapes_ok = 0
        for idx in sample_indices:
            d = np.load(npzs[idx])
            dry = d["dry_mel"]
            wet = d["wet_mel"]
            params = d["parameters"]
            if dry.shape == wet.shape and dry.shape[0] == 128 and params.shape[0] == PARAM_VECTOR_LENGTH:
                shapes_ok += 1
            else:
                print(f"    BAD shape at {idx}: dry={dry.shape} wet={wet.shape} params={params.shape}")
        print(f"  Shape check: {shapes_ok}/{len(sample_indices)} OK (128 x T, {PARAM_VECTOR_LENGTH} params)")

        # Check for NaN/Inf
        nan_count = 0
        for idx in sample_indices:
            d = np.load(npzs[idx])
            if np.any(np.isnan(d["dry_mel"])) or np.any(np.isnan(d["wet_mel"])):
                nan_count += 1
            if np.any(np.isinf(d["dry_mel"])) or np.any(np.isinf(d["wet_mel"])):
                nan_count += 1
        nan_msg = "PASS" if nan_count == 0 else f"FAIL ({nan_count} bad files)"
        print(f"  NaN/Inf check: {nan_msg}")

        # Value range
        d0 = np.load(npzs[0])
        print(f"  Dry mel range: [{d0['dry_mel'].min():.1f}, {d0['dry_mel'].max():.1f}] dB")
        print(f"  Wet mel range: [{d0['wet_mel'].min():.1f}, {d0['wet_mel'].max():.1f}] dB")

    # 7. ML readiness score
    print("\n" + "=" * 70)
    print("  ML READINESS SUMMARY")
    print("=" * 70)
    score = 0
    checks = []

    c = len(jsons) >= 10000
    checks.append(("Dataset size >= 10k", c, f"{len(jsons)}"))
    score += c

    c = passthrough == 0
    checks.append(("Zero passthrough", c, f"{passthrough}"))
    score += c

    c = near_silent == 0
    checks.append(("Zero near-silent", c, f"{near_silent}"))
    score += c

    c = rms_spread > 15
    checks.append(("RMS spread > 15 dB", c, f"{rms_spread:.1f} dB"))
    score += c

    c = varying >= 20
    checks.append((">=20 varying params", c, f"{varying}"))
    score += c

    c = dupes == 0
    checks.append(("No duplicate params", c, f"{dupes} dupes"))
    score += c

    c = len(npzs) == len(jsons)
    checks.append(("Spectrograms complete", c, f"{len(npzs)}/{len(jsons)}"))
    score += c

    c = bands_active.mean() >= 1.5
    checks.append(("Avg bands >= 1.5", c, f"{bands_active.mean():.1f}"))
    score += c

    c = gains.std() >= 0.05 if len(gains) > 0 else False
    checks.append(("Gain diversity (std>=0.05)", c, f"{gains.std():.3f}" if len(gains) > 0 else "N/A"))
    score += c

    c = len(freqs) > 0 and (np.sum(freqs < 0.33) > 0.15 * len(freqs) and np.sum(freqs >= 0.66) > 0.15 * len(freqs))
    checks.append(("Freq coverage (L/M/H)", c, "balanced" if c else "skewed"))
    score += c

    for name, passed, detail in checks:
        status = "[PASS]" if passed else "[FAIL]"
        print(f"  {status} {name}: {detail}")

    print(f"\n  SCORE: {score}/10")
    if score >= 9:
        print("  VERDICT: READY FOR TRAINING")
    elif score >= 7:
        print("  VERDICT: USABLE - minor issues")
    else:
        print("  VERDICT: NEEDS IMPROVEMENT")

if __name__ == "__main__":
    ds = sys.argv[1] if len(sys.argv) > 1 else r"C:\MorePhi_Datasets\proq4_14k"
    sp = sys.argv[2] if len(sys.argv) > 2 else r"C:\MorePhi_Datasets\proq4_14k_spectrograms"
    audit(ds, sp)
