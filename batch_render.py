#!/usr/bin/env python3
"""
batch_render.py — Run MorphSnapCLI in batches to work around plugin memory limits.

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


def main():
    parser = argparse.ArgumentParser(description="Batch MorphSnapCLI runner")
    parser.add_argument("--plugin", required=True, help="Path to VST3 plugin")
    parser.add_argument("--input", required=True, help="Input audio file")
    parser.add_argument("-n", "--variations", type=int, required=True, help="Total variations")
    parser.add_argument("-o", "--output", required=True, help="Output directory")
    parser.add_argument("--batch-size", type=int, default=100, help="Variations per batch (default: 100)")
    parser.add_argument("--cli", default=None, help="Path to MorphSnapCLI.exe")
    args = parser.parse_args()

    cli = args.cli or str(Path(__file__).parent / "build" / "Release" / "MorphSnapCLI.exe")
    if not Path(cli).exists():
        print(f"[ERROR] CLI not found: {cli}")
        sys.exit(1)

    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)

    total = args.variations
    batch_size = args.batch_size
    num_batches = (total + batch_size - 1) // batch_size
    global_index = 0
    dry_copied = False

    print(f"Rendering {total} variations in {num_batches} batches of {batch_size}")
    print(f"Plugin: {args.plugin}")
    print(f"Input: {args.input}")
    print(f"Output: {output_dir}")
    print()

    for batch_num in range(num_batches):
        remaining = total - global_index
        batch_count = min(batch_size, remaining)
        if batch_count <= 0:
            break

        print(f"--- Batch {batch_num + 1}/{num_batches}: {batch_count} variations (global {global_index}-{global_index + batch_count - 1}) ---")

        with tempfile.TemporaryDirectory(prefix="morphsnap_batch_") as tmp_dir:
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
                new_name = f"variation_{global_index:04d}.wav"
                shutil.copy2(wav_path, output_dir / new_name)

                # Copy matching JSON if exists
                json_path = wav_path.with_suffix(".json")
                if json_path.exists():
                    new_json = f"variation_{global_index:04d}.json"
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
