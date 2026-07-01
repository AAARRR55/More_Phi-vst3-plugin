#!/usr/bin/env python3
r"""
batch_render.py — Run MorePhiCLI in batches to work around plugin memory limits.

Splits a large render into chunks of --batch-size, each in a fresh CLI process.
Output files are renumbered contiguously.

Usage:
  python batch_render.py --plugin "C:\...\Pro-Q 4.vst3" --input pink_noise.wav -n 500 -o output_dir
  python batch_render.py --plugin "C:\...\Pro-Q 4.vst3" --input pink_noise.wav -n 2000 -o output_dir --batch-size 100
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def parse_variation_index(path: Path):
    stem = path.stem
    if not stem.startswith("variation_"):
        return None
    try:
        return int(stem.split("_", 1)[1])
    except ValueError:
        return None


def main():
    parser = argparse.ArgumentParser(description="Batch MorePhiCLI runner")
    parser.add_argument("--plugin", required=True, help="Path to VST3 plugin")
    parser.add_argument("--input", required=True, help="Input audio file")
    parser.add_argument("-n", "--variations", type=int, required=True, help="Total variations")
    parser.add_argument("-o", "--output", required=True, help="Output directory")
    parser.add_argument("--batch-size", type=int, default=100, help="Variations per batch (default: 100)")
    parser.add_argument("--cli", default=None, help="Path to MorePhiCLI.exe")
    parser.add_argument("--resume", action="store_true", help="Resume into an existing output directory")
    args = parser.parse_args()

    cli = args.cli or str(Path(__file__).parent / "build" / "Release" / "MorePhiCLI.exe")
    if not Path(cli).exists():
        print(f"[ERROR] CLI not found: {cli}")
        sys.exit(1)

    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)

    total = args.variations
    batch_size = args.batch_size
    num_batches = (total + batch_size - 1) // batch_size
    pad_width = max(4, len(str(max(0, total - 1))))

    existing_wavs = sorted(output_dir.glob("variation_*.wav"))
    existing_indices = sorted(
        idx for idx in (parse_variation_index(path) for path in existing_wavs)
        if idx is not None
    )

    if existing_indices and not args.resume:
        print(f"[ERROR] Output directory already contains {len(existing_indices)} variation WAV files.")
        print("Use --resume to continue an interrupted render, or choose a new output directory.")
        sys.exit(1)

    if args.resume and existing_indices:
        global_index = existing_indices[-1] + 1
        existing_index_set = set(existing_indices)
        missing_indices = [idx for idx in range(global_index) if idx not in existing_index_set]
        if missing_indices:
            preview = ", ".join(str(idx) for idx in missing_indices[:10])
            print("[WARN] Output directory has missing variation indices before the resume point.")
            print(f"       Missing indices (first 10): {preview}")
            print("       Resume will continue after the highest existing index.")
    else:
        global_index = 0

    dry_copied = (output_dir / "dry_source.wav").exists()
    remaining_total = max(0, total - global_index)
    num_batches = (remaining_total + batch_size - 1) // batch_size if remaining_total else 0

    print(f"Rendering {total} target variations in {num_batches} remaining batches of {batch_size}")
    print(f"Plugin: {args.plugin}")
    print(f"Input: {args.input}")
    print(f"Output: {output_dir}")
    print(f"Filename pad width: {pad_width}")
    print(f"Resume mode: {'on' if args.resume else 'off'}")
    print(f"Starting at variation index: {global_index}")
    print()

    for batch_num in range(num_batches):
        remaining = total - global_index
        batch_count = min(batch_size, remaining)
        if batch_count <= 0:
            break

        print(f"--- Batch {batch_num + 1}/{num_batches}: {batch_count} variations (global {global_index}-{global_index + batch_count - 1}) ---")

        with tempfile.TemporaryDirectory(prefix="morephi_batch_") as tmp_dir:
            cmd = [
                cli,
                "--plugin", args.plugin,
                "--input", args.input,
                "-o", tmp_dir,
                "-n", str(batch_count),
            ]

            result = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)

            if result.returncode != 0:
                # Check if it partially succeeded
                partial_wavs = sorted(Path(tmp_dir).glob("variation_*.wav"))
                if not partial_wavs:
                    print(f"  [FAIL] Batch {batch_num + 1} produced 0 files. Stderr: {result.stderr[:200]}")
                    continue
                print(f"  [WARN] Batch {batch_num + 1} exited with code {result.returncode}, "
                      f"but produced {len(partial_wavs)} files. Using them.")

            # Copy dry source (once)
            dry_src = Path(tmp_dir) / "dry_source.wav"
            if dry_src.exists() and not dry_copied:
                shutil.copy2(dry_src, output_dir / "dry_source.wav")
                dry_copied = True

            # Rename and copy variation files
            batch_wavs = sorted(Path(tmp_dir).glob("variation_*.wav"))
            batch_jsons = sorted(Path(tmp_dir).glob("variation_*.json"))

            for wav_path in batch_wavs:
                new_name = f"variation_{global_index:0{pad_width}d}.wav"
                shutil.copy2(wav_path, output_dir / new_name)

                # Copy matching JSON if exists
                json_path = wav_path.with_suffix(".json")
                if json_path.exists():
                    new_json = f"variation_{global_index:0{pad_width}d}.json"
                    shutil.copy2(json_path, output_dir / new_json)

                global_index += 1

            print(f"  Copied {len(batch_wavs)} files (total: {global_index})")

    print()
    print(f"Done. {global_index} variations in {output_dir}")
    final_count = len(list(output_dir.glob("variation_*.wav")))
    print(f"Final WAV count: {final_count}")

    if final_count < total:
        print(f"[WARN] Generated {final_count}/{total} ({final_count*100//total}%). "
              f"Plugin may have crashed in some batches.")
        sys.exit(1)


if __name__ == "__main__":
    main()
