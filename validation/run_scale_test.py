#!/usr/bin/env python3
"""
Scale test for dataset generation framework.
Generates 10,000 samples and validates completion.
"""

import json
import subprocess
import sys
import time
from pathlib import Path


def load_config(config_path: str) -> dict:
    """Load configuration from JSON file."""
    with open(config_path, 'r') as f:
        return json.load(f)


def count_output_files(output_dir: Path) -> int:
    """Count generated sample directories."""
    if not output_dir.exists():
        return 0

    # Count directories that match sample pattern
    sample_dirs = [d for d in output_dir.iterdir()
                   if d.is_dir() and d.name.startswith('sample_')]
    return len(sample_dirs)


def run_generation(config: dict) -> bool:
    """
    Run dataset generation via Python generator.
    Falls back to checking if output already exists.
    """
    output_dir = Path(config['outputDirectory'])
    output_dir.mkdir(parents=True, exist_ok=True)

    total_samples = config['totalSamples']
    existing = count_output_files(output_dir)

    print(f"Output directory: {output_dir}")
    print(f"Target samples: {total_samples}")
    print(f"Existing samples: {existing}")

    if existing >= total_samples:
        print("✓ Already have enough samples")
        return True

    # Try to run the dataset generator
    # Check for available generators in order of preference
    generators = [
        'auto_dataset_generator.py',
        'big_dataset_generator.py',
        'dataset_crashproof.py',
        'generate_rendered_dataset.py',
    ]

    generator_path = None
    for gen in generators:
        if Path(gen).exists():
            generator_path = Path(gen)
            break

    if generator_path is None:
        print("ERROR: No dataset generator found")
        print("Available generators checked:", generators)
        return False

    print(f"Using generator: {generator_path}")

    # Save config for the generator
    config_file = output_dir / 'scale_test_config.json'
    with open(config_file, 'w') as f:
        json.dump(config, f, indent=2)

    # Run the generator
    start_time = time.time()

    try:
        result = subprocess.run(
            [sys.executable, str(generator_path), '--config', str(config_file)],
            cwd=Path(__file__).parent.parent,
            capture_output=True,
            text=True,
            timeout=36000  # 10 hour timeout
        )

        if result.returncode != 0:
            print(f"Generator failed with code {result.returncode}")
            print("STDOUT:", result.stdout[-2000:] if len(result.stdout) > 2000 else result.stdout)
            print("STDERR:", result.stderr[-2000:] if len(result.stderr) > 2000 else result.stderr)
            return False

    except subprocess.TimeoutExpired:
        print("Generator timed out after 10 hours")
        return False
    except Exception as e:
        print(f"Generator error: {e}")
        return False

    elapsed = time.time() - start_time
    final_count = count_output_files(output_dir)

    print(f"Generation completed in {elapsed:.1f} seconds")
    print(f"Final sample count: {final_count}")

    return final_count >= total_samples


def generate_report(config: dict, success: bool, elapsed: float) -> dict:
    """Generate validation report."""
    output_dir = Path(config['outputDirectory'])

    report = {
        "testName": "scale_test_10k",
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
        "success": success,
        "config": {
            "targetSamples": config['totalSamples'],
            "sampleRate": config['sampleRate'],
            "duration": config['fullDuration'],
            "chainType": config['chainType'],
        },
        "results": {
            "samplesGenerated": count_output_files(output_dir),
            "elapsedSeconds": elapsed,
        },
        "criteria": {
            "samplesTarget": config['totalSamples'],
            "passed": success,
        }
    }

    if elapsed > 0 and report["results"]["samplesGenerated"] > 0:
        report["results"]["samplesPerHour"] = (
            report["results"]["samplesGenerated"] / (elapsed / 3600)
        )

    return report


def main():
    """Main entry point."""
    script_dir = Path(__file__).parent
    config_path = script_dir / 'scale_test_config.json'

    print("=" * 60)
    print("Dataset Framework Scale Test")
    print("=" * 60)

    # Load configuration
    if not config_path.exists():
        print(f"ERROR: Config file not found: {config_path}")
        sys.exit(1)

    config = load_config(config_path)
    print(f"Loaded config from: {config_path}")

    # Run generation
    start_time = time.time()
    success = run_generation(config)
    elapsed = time.time() - start_time

    # Generate report
    report = generate_report(config, success, elapsed)

    # Save report
    report_path = script_dir / 'scale_test_report.json'
    with open(report_path, 'w') as f:
        json.dump(report, f, indent=2)

    print("\n" + "=" * 60)
    print("Scale Test Results")
    print("=" * 60)
    print(f"Status: {'PASS' if success else 'FAIL'}")
    print(f"Samples Generated: {report['results']['samplesGenerated']}")
    print(f"Target Samples: {report['criteria']['samplesTarget']}")
    print(f"Elapsed Time: {elapsed:.1f}s ({elapsed/60:.1f} min)")
    if 'samplesPerHour' in report['results']:
        print(f"Rate: {report['results']['samplesPerHour']:.1f} samples/hour")
    print(f"\nReport saved to: {report_path}")

    sys.exit(0 if success else 1)


if __name__ == '__main__':
    main()
