#!/usr/bin/env python3
"""
dataset_pipeline.py — End-to-end dataset generation and feature extraction.

Workflow:
  1. RENDER: Calls MorePhiCLI to generate dry/wet audio pairs with random
     safe parameter variations (Frequency, Gain, Q, Shape, Slope).
  2. EXTRACT: Converts every WAV to a Mel-Spectrogram (2D image tensor)
     and saves the paired (dry_spec, wet_spec, params) for ML training.

Usage:
  # Step 1: Smoke test (5 variations)
  python dataset_pipeline.py smoke --plugin "C:/Path/To/Pro-Q 4.vst3" --input pink_noise.wav

  # Step 2: Full generation (100k samples)
  python dataset_pipeline.py generate --plugin "C:/Path/To/Pro-Q 4.vst3" --input pink_noise.wav -n 100000

  # Step 3: Extract spectrograms from existing renders
  python dataset_pipeline.py extract --dataset-dir C:/MorePhi_Datasets/run_001

  # All-in-one: generate + extract
  python dataset_pipeline.py full --plugin "C:/Path/To/Pro-Q 4.vst3" --input pink_noise.wav -n 10000

Prerequisites:
  - MorePhiCLI.exe built in build/Release/
  - Pink noise or drum loop WAV as source audio
  - pip install numpy scipy librosa matplotlib
"""

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from datetime import datetime
from concurrent.futures import ProcessPoolExecutor, as_completed

import numpy as np

# ── Constants ────────────────────────────────────────────────────────────────

SCRIPT_DIR = Path(__file__).parent.resolve()
CLI_EXE = SCRIPT_DIR / "build" / "Release" / "MorePhiCLI.exe"
DEFAULT_OUTPUT = Path("C:/MorePhi_Datasets")

# Mel-Spectrogram defaults (matching standard audio ML configs)
MEL_SAMPLE_RATE = 48000
MEL_N_FFT = 2048
MEL_HOP_LENGTH = 512
MEL_N_MELS = 128
MEL_FMIN = 20.0
MEL_FMAX = 20000.0

# ── CLI Runner ───────────────────────────────────────────────────────────────

def find_cli_exe() -> Path:
    """Locate MorePhiCLI.exe, checking multiple known locations."""
    candidates = [
        CLI_EXE,
        SCRIPT_DIR / "build_cli_fix" / "Release" / "MorePhiCLI.exe",
        SCRIPT_DIR / "build" / "Debug" / "MorePhiCLI.exe",
    ]
    for p in candidates:
        if p.exists():
            return p
    print("[ERROR] MorePhiCLI.exe not found. Searched:")
    for p in candidates:
        print(f"  {p}")
    print("\nBuild it with: cmake --build build --config Release --target MorePhiCLI")
    sys.exit(1)


def run_cli(plugin_path: str, input_path: str, output_dir: str,
            variations: int, verbose: bool = False) -> bool:
    """Run MorePhiCLI.exe to render parameter variations."""
    exe = find_cli_exe()
    cmd = [
        str(exe),
        "--plugin", plugin_path,
        "--input", input_path,
        "-o", output_dir,
        "-n", str(variations),
    ]
    if verbose:
        cmd.append("--verbose")

    print(f"[CLI] Running: {' '.join(cmd)}")
    print(f"[CLI] Output dir: {output_dir}")
    print()

    try:
        process = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1
        )
        for line in iter(process.stdout.readline, ''):
            print(f"  {line.rstrip()}")
        process.wait()

        if process.returncode != 0:
            print(f"\n[ERROR] MorePhiCLI exited with code {process.returncode}")
            return False

        print(f"\n[OK] Render complete.")
        return True

    except FileNotFoundError:
        print(f"[ERROR] Cannot execute: {exe}")
        return False


# ── Mel-Spectrogram Extraction ───────────────────────────────────────────────

def wav_to_mel_spectrogram(wav_path: str, sr: int = MEL_SAMPLE_RATE,
                           n_fft: int = MEL_N_FFT,
                           hop_length: int = MEL_HOP_LENGTH,
                           n_mels: int = MEL_N_MELS,
                           fmin: float = MEL_FMIN,
                           fmax: float = MEL_FMAX) -> np.ndarray:
    """
    Convert a WAV file to a log-scaled Mel-Spectrogram.

    Returns:
        np.ndarray of shape (n_mels, time_frames) — log-power Mel-Spectrogram in dB.
        X-axis = time, Y-axis = Mel-frequency bands, values = amplitude in dB.
    """
    import librosa

    # Load audio (mono mix, native sample rate then resample)
    y, file_sr = librosa.load(wav_path, sr=sr, mono=True)

    # Compute Mel-Spectrogram (power spectrum)
    mel_spec = librosa.feature.melspectrogram(
        y=y, sr=sr, n_fft=n_fft, hop_length=hop_length,
        n_mels=n_mels, fmin=fmin, fmax=fmax, power=2.0
    )

    # Convert to log scale (dB) — this is what the neural network "sees"
    mel_spec_db = librosa.power_to_db(mel_spec, ref=np.max)

    return mel_spec_db


def extract_single_sample(args_tuple):
    """
    Worker function for parallel extraction.
    args_tuple = (variation_index, wet_wav, dry_wav, params_json, output_dir, mel_config)
    """
    idx, wet_wav, dry_wav, params_json, output_dir, mel_config = args_tuple

    try:
        # Load parameter metadata
        params = {}
        if params_json and os.path.exists(params_json):
            with open(params_json, 'r') as f:
                params = json.load(f)

        # Generate Mel-Spectrograms
        wet_mel = wav_to_mel_spectrogram(wet_wav, **mel_config)
        dry_mel = wav_to_mel_spectrogram(dry_wav, **mel_config)

        # Save as compressed numpy archive
        out_path = os.path.join(output_dir, f"sample_{idx:06d}.npz")
        np.savez_compressed(
            out_path,
            dry_mel=dry_mel.astype(np.float32),
            wet_mel=wet_mel.astype(np.float32),
            parameters=np.array(
                [p["value"] for p in params.get("parameters", [])],
                dtype=np.float32
            ),
            variation_index=np.int32(idx),
            peak_db=np.float32(params.get("peak_db", 0.0)),
            rms_db=np.float32(params.get("rms_db", -100.0)),
        )

        return idx, True, ""
    except Exception as e:
        return idx, False, str(e)


def extract_spectrograms(dataset_dir: str, output_dir: str = None,
                         max_workers: int = 4, mel_config: dict = None):
    """
    Extract Mel-Spectrograms from all rendered variations in a dataset directory.

    Expected directory structure (from MorePhiCLI):
      dataset_dir/
        dry_source.wav          <- dry input audio
        variation_0000.wav      <- wet output (variation 0)
        variation_0000.json     <- parameter metadata (variation 0)
        variation_0001.wav
        variation_0001.json
        ...

    Output:
      output_dir/
        sample_000000.npz       <- {dry_mel, wet_mel, parameters}
        sample_000001.npz
        ...
        manifest.json           <- dataset metadata
    """
    dataset_path = Path(dataset_dir)
    if output_dir is None:
        output_dir = str(dataset_path / "spectrograms")

    os.makedirs(output_dir, exist_ok=True)

    if mel_config is None:
        mel_config = {
            "sr": MEL_SAMPLE_RATE,
            "n_fft": MEL_N_FFT,
            "hop_length": MEL_HOP_LENGTH,
            "n_mels": MEL_N_MELS,
            "fmin": MEL_FMIN,
            "fmax": MEL_FMAX,
        }

    # Find dry source
    dry_wav = dataset_path / "dry_source.wav"
    if not dry_wav.exists():
        # Fallback: look for any file matching dry*
        dry_candidates = list(dataset_path.glob("dry*.*"))
        if dry_candidates:
            dry_wav = dry_candidates[0]
        else:
            print("[ERROR] No dry source audio found in dataset directory.")
            print(f"  Looked for: {dataset_path / 'dry_source.wav'}")
            return False

    # Collect all wet variation files
    wet_files = sorted(dataset_path.glob("variation_*.wav"))
    if not wet_files:
        wet_files = sorted(dataset_path.glob("variation_*.flac"))
    if not wet_files:
        print("[ERROR] No rendered variation files found.")
        return False

    print(f"[EXTRACT] Found {len(wet_files)} variations + dry source")
    print(f"[EXTRACT] Dry source: {dry_wav}")
    print(f"[EXTRACT] Output: {output_dir}")
    print(f"[EXTRACT] Mel config: {MEL_N_MELS} bands, {MEL_N_FFT} FFT, {MEL_HOP_LENGTH} hop")
    print()

    # Build work items
    tasks = []
    for wet_path in wet_files:
        # Parse variation index from filename
        stem = wet_path.stem  # "variation_0000"
        idx = int(stem.split("_")[-1])

        # Find matching parameter JSON
        json_path = wet_path.with_suffix(".json")
        if not json_path.exists():
            json_path = None

        tasks.append((idx, str(wet_path), str(dry_wav), str(json_path) if json_path else None,
                       output_dir, mel_config))

    # Process in parallel
    start_time = time.time()
    success_count = 0
    fail_count = 0

    with ProcessPoolExecutor(max_workers=max_workers) as executor:
        futures = {executor.submit(extract_single_sample, t): t[0] for t in tasks}

        for i, future in enumerate(as_completed(futures)):
            idx, ok, err = future.result()
            if ok:
                success_count += 1
            else:
                fail_count += 1
                print(f"  [FAIL] variation_{idx:04d}: {err}")

            if (i + 1) % 100 == 0 or (i + 1) == len(tasks):
                elapsed = time.time() - start_time
                rate = (i + 1) / elapsed if elapsed > 0 else 0
                print(f"  Progress: {i+1}/{len(tasks)} ({rate:.1f} samples/sec)")

    elapsed = time.time() - start_time
    print()
    print(f"[EXTRACT] Done: {success_count} ok, {fail_count} failed, {elapsed:.1f}s total")

    # Write manifest
    manifest = {
        "created": datetime.now().isoformat(),
        "source_dir": str(dataset_path),
        "dry_source": str(dry_wav),
        "total_samples": success_count,
        "failed_samples": fail_count,
        "mel_config": {
            "sample_rate": MEL_SAMPLE_RATE,
            "n_fft": MEL_N_FFT,
            "hop_length": MEL_HOP_LENGTH,
            "n_mels": MEL_N_MELS,
            "fmin": MEL_FMIN,
            "fmax": MEL_FMAX,
        },
        "spectrogram_shape_note": f"(n_mels={MEL_N_MELS}, time_frames=variable)",
    }
    with open(os.path.join(output_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)

    return True


def visualize_sample(npz_path: str, save_path: str = None):
    """
    Visualize a single dry/wet spectrogram pair for sanity checking.
    """
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt

    data = np.load(npz_path)
    dry_mel = data["dry_mel"]
    wet_mel = data["wet_mel"]
    params = data["parameters"]

    fig, axes = plt.subplots(1, 3, figsize=(18, 5))

    # Dry spectrogram
    im0 = axes[0].imshow(dry_mel, aspect='auto', origin='lower',
                         cmap='magma', vmin=-80, vmax=0)
    axes[0].set_title("Dry (Input)")
    axes[0].set_ylabel("Mel Band")
    axes[0].set_xlabel("Time Frame")
    plt.colorbar(im0, ax=axes[0], label="dB")

    # Wet spectrogram
    im1 = axes[1].imshow(wet_mel, aspect='auto', origin='lower',
                         cmap='magma', vmin=-80, vmax=0)
    axes[1].set_title("Wet (EQ Applied)")
    axes[1].set_xlabel("Time Frame")
    plt.colorbar(im1, ax=axes[1], label="dB")

    # Difference (what the EQ changed)
    diff = wet_mel - dry_mel
    im2 = axes[2].imshow(diff, aspect='auto', origin='lower',
                         cmap='RdBu_r', vmin=-20, vmax=20)
    axes[2].set_title("Difference (Wet - Dry)")
    axes[2].set_xlabel("Time Frame")
    plt.colorbar(im2, ax=axes[2], label="dB change")

    plt.suptitle(f"Sample: {Path(npz_path).stem} | {len(params)} parameters", fontsize=12)
    plt.tight_layout()

    if save_path:
        plt.savefig(save_path, dpi=150, bbox_inches='tight')
        print(f"  Saved visualization: {save_path}")
    else:
        save_path = str(Path(npz_path).with_suffix(".png"))
        plt.savefig(save_path, dpi=150, bbox_inches='tight')
        print(f"  Saved visualization: {save_path}")
    plt.close()


# ── Smoke Test ───────────────────────────────────────────────────────────────

def run_smoke_test(plugin_path: str, input_path: str, output_base: str = None):
    """Quick 5-variation test to verify the full pipeline works."""
    if output_base is None:
        output_base = str(DEFAULT_OUTPUT / "smoke_test")

    print("=" * 65)
    print("  SMOKE TEST — Verify CLI + Spectrogram Pipeline")
    print("=" * 65)
    print()

    # Step 1: Render 5 variations
    print("[1/3] Rendering 5 variations...")
    if not run_cli(plugin_path, input_path, output_base, variations=5, verbose=True):
        print("[FAIL] CLI render failed. Check plugin path and input file.")
        return False

    # Step 2: Verify output files
    print("\n[2/3] Verifying output files...")
    output_path = Path(output_base)
    wav_files = list(output_path.glob("variation_*.wav"))
    json_files = list(output_path.glob("variation_*.json"))
    dry_file = output_path / "dry_source.wav"

    print(f"  WAV files: {len(wav_files)}")
    print(f"  JSON files: {len(json_files)}")
    print(f"  Dry source: {'OK' if dry_file.exists() else 'MISSING'}")

    if len(wav_files) == 0:
        print("[FAIL] No rendered WAV files found.")
        return False

    # Check for silent files
    silent_count = 0
    for jf in sorted(json_files):
        with open(jf) as f:
            meta = json.load(f)
        if meta.get("has_silence", False):
            silent_count += 1
            print(f"  [WARN] {jf.name}: SILENT (rms={meta.get('rms_db', '?')} dB)")

    if silent_count > 0:
        print(f"\n  {silent_count}/{len(json_files)} variations are silent!")
        print("  Check: Is pink noise (not a test tone) playing through the plugin?")

    # Step 3: Extract spectrograms
    print("\n[3/3] Extracting Mel-Spectrograms...")
    spec_dir = str(output_path / "spectrograms")
    if not extract_spectrograms(output_base, spec_dir, max_workers=2):
        print("[FAIL] Spectrogram extraction failed.")
        return False

    # Visualize first sample
    npz_files = sorted(Path(spec_dir).glob("sample_*.npz"))
    if npz_files:
        print("\n  Generating visualization...")
        visualize_sample(str(npz_files[0]))

    print()
    print("=" * 65)
    print("  SMOKE TEST PASSED")
    print("=" * 65)
    print(f"  Output:         {output_base}")
    print(f"  Spectrograms:   {spec_dir}")
    print(f"  Variations:     {len(wav_files)}")
    if npz_files:
        data = np.load(str(npz_files[0]))
        print(f"  Mel shape:      {data['dry_mel'].shape}")
        print(f"  Params vector:  {data['parameters'].shape}")
    print()
    print("  Next: Run 'python dataset_pipeline.py full' for the real dataset.")
    return True


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="MorePhi Dataset Generation + Mel-Spectrogram Pipeline",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Quick smoke test
  python dataset_pipeline.py smoke --plugin "C:/VST3/Pro-Q 4.vst3" --input pink_noise.wav

  # Generate 10,000 variations
  python dataset_pipeline.py generate --plugin "C:/VST3/Pro-Q 4.vst3" --input pink_noise.wav -n 10000

  # Extract spectrograms from existing renders
  python dataset_pipeline.py extract --dataset-dir C:/MorePhi_Datasets/run_001

  # Full pipeline: generate + extract
  python dataset_pipeline.py full --plugin "C:/VST3/Pro-Q 4.vst3" --input pink_noise.wav -n 10000
""")

    subparsers = parser.add_subparsers(dest="command", help="Pipeline stage to run")

    # ── smoke ──
    smoke_p = subparsers.add_parser("smoke", help="Quick 5-variation smoke test")
    smoke_p.add_argument("--plugin", required=True, help="Path to VST3 plugin")
    smoke_p.add_argument("--input", required=True, help="Input WAV (use pink_noise.wav)")
    smoke_p.add_argument("-o", "--output", default=None, help="Output directory")

    # ── generate ──
    gen_p = subparsers.add_parser("generate", help="Generate dry/wet audio pairs via CLI")
    gen_p.add_argument("--plugin", required=True, help="Path to VST3 plugin")
    gen_p.add_argument("--input", required=True, help="Input WAV (use pink_noise.wav)")
    gen_p.add_argument("-n", "--variations", type=int, default=10000)
    gen_p.add_argument("-o", "--output", default=None, help="Output directory")
    gen_p.add_argument("--verbose", action="store_true")

    # ── extract ──
    ext_p = subparsers.add_parser("extract", help="Extract Mel-Spectrograms from rendered audio")
    ext_p.add_argument("--dataset-dir", required=True, help="Directory with rendered variations")
    ext_p.add_argument("-o", "--output", default=None, help="Spectrogram output dir")
    ext_p.add_argument("--workers", type=int, default=4, help="Parallel workers (default: 4)")

    # ── full ──
    full_p = subparsers.add_parser("full", help="Generate + extract in one step")
    full_p.add_argument("--plugin", required=True, help="Path to VST3 plugin")
    full_p.add_argument("--input", required=True, help="Input WAV (use pink_noise.wav)")
    full_p.add_argument("-n", "--variations", type=int, default=10000)
    full_p.add_argument("-o", "--output", default=None, help="Output directory")
    full_p.add_argument("--workers", type=int, default=4, help="Parallel workers (default: 4)")
    full_p.add_argument("--verbose", action="store_true")

    # ── visualize ──
    vis_p = subparsers.add_parser("visualize", help="Visualize a single spectrogram sample")
    vis_p.add_argument("npz_file", help="Path to a sample .npz file")
    vis_p.add_argument("-o", "--output", default=None, help="Save PNG to this path")

    args = parser.parse_args()

    if args.command is None:
        parser.print_help()
        return

    if args.command == "smoke":
        ok = run_smoke_test(args.plugin, args.input, args.output)
        sys.exit(0 if ok else 1)

    elif args.command == "generate":
        run_name = datetime.now().strftime("run_%Y%m%d_%H%M%S")
        output_dir = args.output or str(DEFAULT_OUTPUT / run_name)
        os.makedirs(output_dir, exist_ok=True)
        ok = run_cli(args.plugin, args.input, output_dir, args.variations, args.verbose)
        if ok:
            print(f"\n[DONE] {args.variations} variations rendered to {output_dir}")
            print(f"Next: python dataset_pipeline.py extract --dataset-dir \"{output_dir}\"")
        sys.exit(0 if ok else 1)

    elif args.command == "extract":
        ok = extract_spectrograms(args.dataset_dir, args.output, args.workers)
        sys.exit(0 if ok else 1)

    elif args.command == "full":
        run_name = datetime.now().strftime("run_%Y%m%d_%H%M%S")
        output_dir = args.output or str(DEFAULT_OUTPUT / run_name)
        os.makedirs(output_dir, exist_ok=True)

        print("=" * 65)
        print(f"  FULL PIPELINE — {args.variations} variations")
        print("=" * 65)
        print()

        # Phase 1: Render
        print("[Phase 1] Rendering audio variations...")
        if not run_cli(args.plugin, args.input, output_dir, args.variations, args.verbose):
            print("[FAIL] Render phase failed.")
            sys.exit(1)

        # Phase 2: Extract
        print()
        print("[Phase 2] Extracting Mel-Spectrograms...")
        spec_dir = str(Path(output_dir) / "spectrograms")
        if not extract_spectrograms(output_dir, spec_dir, args.workers):
            print("[FAIL] Extraction phase failed.")
            sys.exit(1)

        # Visualize a sample
        npz_files = sorted(Path(spec_dir).glob("sample_*.npz"))
        if npz_files:
            visualize_sample(str(npz_files[0]))

        print()
        print("=" * 65)
        print("  PIPELINE COMPLETE")
        print("=" * 65)
        print(f"  Audio:          {output_dir}")
        print(f"  Spectrograms:   {spec_dir}")
        print(f"  Total samples:  {len(npz_files)}")
        if npz_files:
            data = np.load(str(npz_files[0]))
            print(f"  Mel shape:      {data['dry_mel'].shape} (n_mels × time_frames)")
            print(f"  Params vector:  {data['parameters'].shape}")
        print()
        print("  Ready for PyTorch training!")
        print("  Each .npz file contains: dry_mel, wet_mel, parameters")

    elif args.command == "visualize":
        visualize_sample(args.npz_file, args.output)


if __name__ == "__main__":
    main()
