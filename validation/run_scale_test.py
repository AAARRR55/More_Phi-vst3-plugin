#!/usr/bin/env python3
"""
Smoke/scale test for dataset generation framework.
Generates the configured sample count and validates completion. The checked-in
config is a 5-sample smoke run, not a 10,000-sample production run.
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
    Run dataset generation via the local smoke pipeline.
    """
    import shutil
    output_dir = Path(config['outputDirectory'])
    output_dir.mkdir(parents=True, exist_ok=True)

    total_samples = config['totalSamples']
    existing = count_output_files(output_dir)

    print(f"Output directory: {output_dir}")
    print(f"Target samples: {total_samples}")
    print(f"Existing samples: {existing}")

    if existing >= total_samples:
        print("[OK] Already have enough samples")
        return True

    # Run the dataset pipeline smoke test to generate flat files
    pipeline_script = Path(__file__).parent.parent / 'dataset_pipeline.py'
    plugin_path = "C:\\Program Files\\Common Files\\VST3\\FabFilter\\FabFilter Pro-Q 4.vst3"
    input_wav = Path(__file__).parent.parent / 'pink_noise.wav'
    temp_flat_dir = Path("C:/MorePhi_Datasets/smoke_test")

    print(f"Running dataset pipeline script: {pipeline_script}")
    try:
        result = subprocess.run(
            [sys.executable, str(pipeline_script), 'smoke', '--plugin', plugin_path, '--input', str(input_wav)],
            capture_output=True,
            text=True,
            check=True
        )
        print(result.stdout)
    except subprocess.CalledProcessError as e:
        print("Pipeline script failed!")
        print("STDOUT:", e.stdout)
        print("STDERR:", e.stderr)
        return False

    # Perform structuring conversion
    print(f"Structuring dataset to {output_dir}...")
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    for idx in range(total_samples):
        sample_dir = output_dir / f"sample_{idx:06d}"
        sample_dir.mkdir(parents=True, exist_ok=True)

        # Copy audio.wav
        shutil.copy(temp_flat_dir / f"variation_{idx:04d}.wav", sample_dir / "audio.wav")

        # Copy parameters.json
        shutil.copy(temp_flat_dir / f"variation_{idx:04d}.json", sample_dir / "parameters.json")

        # Create placeholder features.json for structural validation only.
        # These values are not measured FeatureExtractor output.
        with open(temp_flat_dir / f"variation_{idx:04d}.json") as f:
            v_meta = json.load(f)

        features = {
            "spectral": {
                "mfcc": [0.0] * 13,
                "chroma": [0.0] * 12,
                "spectralCentroid": 1000.0,
                "spectralRolloff": 5000.0,
                "spectralFlux": 0.5,
                "spectralSpread": 100.0,
                "spectralFlatness": 0.1
            },
            "temporal": {
                "rmsEnergy": v_meta.get("rms_db", -14.0),
                "peakAmplitude": v_meta.get("peak_db", -0.3),
                "crestFactor": 12.0,
                "attackTime": 10.0,
                "transientDensity": 0.5,
                "zeroCrossingRate": 0.1
            },
            "perceptual": {
                "lufs": v_meta.get("rms_db", -14.0) - 2.0,
                "truePeakDb": v_meta.get("peak_db", -0.3),
                "roughness": 0.2,
                "sharpness": 1.5,
                "brightness": 0.6,
                "dynamicRange": 8.0
            }
        }

        with open(sample_dir / "features.json", "w") as f:
            json.dump(features, f, indent=2)

    final_count = count_output_files(output_dir)
    print(f"Final structured sample count: {final_count}")
    return final_count >= total_samples


def generate_report(config: dict, success: bool, elapsed: float) -> dict:
    """Generate validation report."""
    output_dir = Path(config['outputDirectory'])

    report = {
        "testName": config.get("datasetName", "scale_test_smoke"),
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
        "success": success,
        "scope": "production-scale" if config['totalSamples'] >= 10000 else "smoke",
        "productionScale": config['totalSamples'] >= 10000,
        "featureSource": "placeholder_schema_values_not_measured_extractor_output",
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
    print("Dataset Framework Smoke/Scale Test")
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
