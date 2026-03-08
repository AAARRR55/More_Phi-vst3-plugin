#!/usr/bin/env python3
"""
Output validator for dataset generation.
Validates audio files, feature files, and parameter files.
"""

import json
import wave
import struct
import sys
from pathlib import Path
from dataclasses import dataclass
from typing import List


@dataclass
class ValidationResult:
    """Result of a single validation check."""
    check_name: str
    passed: bool
    message: str = ""


@dataclass
class SampleValidation:
    """Validation results for a single sample."""
    sample_path: Path
    checks: List[ValidationResult]

    @property
    def passed(self) -> bool:
        return all(c.passed for c in self.checks)

    @property
    def failed_checks(self) -> List[ValidationResult]:
        return [c for c in self.checks if not c.passed]


class OutputValidator:
    """Validates generated dataset samples."""

    def __init__(self, expected_sample_rate: int = 48000,
                 expected_channels: int = 2,
                 expected_duration: float = 5.0,
                 expected_feature_dims: int = 31):
        self.expected_sample_rate = expected_sample_rate
        self.expected_channels = expected_channels
        self.expected_duration = expected_duration
        self.expected_feature_dims = expected_feature_dims

    def validate_sample(self, sample_dir: Path) -> SampleValidation:
        """Validate a single sample directory."""
        checks = []

        # Check audio file
        audio_path = sample_dir / 'audio.wav'
        if audio_path.exists():
            checks.extend(self._validate_audio(audio_path))
        else:
            checks.append(ValidationResult(
                "audio_exists", False, f"Audio file not found: {audio_path}"
            ))

        # Check features file
        features_path = sample_dir / 'features.json'
        if features_path.exists():
            checks.extend(self._validate_features(features_path))
        else:
            checks.append(ValidationResult(
                "features_exists", False, f"Features file not found: {features_path}"
            ))

        # Check parameters file
        params_path = sample_dir / 'parameters.json'
        if params_path.exists():
            checks.extend(self._validate_parameters(params_path))
        else:
            checks.append(ValidationResult(
                "params_exists", False, f"Parameters file not found: {params_path}"
            ))

        return SampleValidation(sample_dir, checks)

    def _validate_audio(self, audio_path: Path) -> List[ValidationResult]:
        """Validate audio file format and content."""
        checks = []

        try:
            with wave.open(str(audio_path), 'rb') as wf:
                channels = wf.getnchannels()
                sample_rate = wf.getframerate()
                num_frames = wf.getnframes()
                duration = num_frames / sample_rate

                # Check sample rate
                checks.append(ValidationResult(
                    "audio_sample_rate",
                    sample_rate == self.expected_sample_rate,
                    f"Expected {self.expected_sample_rate}, got {sample_rate}"
                ))

                # Check channels
                checks.append(ValidationResult(
                    "audio_channels",
                    channels == self.expected_channels,
                    f"Expected {self.expected_channels}, got {channels}"
                ))

                # Check duration (allow 10% tolerance)
                duration_ok = abs(duration - self.expected_duration) < self.expected_duration * 0.1
                checks.append(ValidationResult(
                    "audio_duration",
                    duration_ok,
                    f"Expected ~{self.expected_duration}s, got {duration:.2f}s"
                ))

                # Check for silence
                frames = wf.readframes(min(num_frames, 48000))  # Read first second
                samples = struct.unpack(f'{len(frames)//2}h', frames)
                rms = (sum(s*s for s in samples) / len(samples)) ** 0.5
                normalized_rms = rms / 32768.0

                checks.append(ValidationResult(
                    "audio_not_silent",
                    normalized_rms > 1e-6,
                    f"RMS: {normalized_rms:.6f}"
                ))

        except Exception as e:
            checks.append(ValidationResult(
                "audio_readable", False, str(e)
            ))

        return checks

    def _validate_features(self, features_path: Path) -> List[ValidationResult]:
        """Validate features JSON file."""
        checks = []

        try:
            with open(features_path, 'r') as f:
                features = json.load(f)

            # Check MFCC dimensions
            mfcc = features.get('mfcc', features.get('spectral', {}).get('mfcc', []))
            checks.append(ValidationResult(
                "mfcc_count",
                len(mfcc) == 13,
                f"Expected 13 MFCCs, got {len(mfcc)}"
            ))

            # Check chroma dimensions
            chroma = features.get('chroma', features.get('spectral', {}).get('chroma', []))
            checks.append(ValidationResult(
                "chroma_count",
                len(chroma) == 12,
                f"Expected 12 chroma values, got {len(chroma)}"
            ))

            # Check total feature count (flattened)
            def count_features(obj, count=0):
                if isinstance(obj, (int, float)):
                    return 1
                elif isinstance(obj, list):
                    return sum(count_features(item) for item in obj)
                elif isinstance(obj, dict):
                    return sum(count_features(v) for v in obj.values())
                return 0

            total_features = count_features(features)
            checks.append(ValidationResult(
                "feature_dimensions",
                total_features >= self.expected_feature_dims,
                f"Expected >={self.expected_feature_dims} features, got {total_features}"
            ))

        except json.JSONDecodeError as e:
            checks.append(ValidationResult(
                "features_json_valid", False, str(e)
            ))
        except Exception as e:
            checks.append(ValidationResult(
                "features_readable", False, str(e)
            ))

        return checks

    def _validate_parameters(self, params_path: Path) -> List[ValidationResult]:
        """Validate parameters JSON file."""
        checks = []

        try:
            with open(params_path, 'r') as f:
                params = json.load(f)

            # Handle different parameter formats
            if isinstance(params, dict):
                param_values = params.get('parameters', params.get('values', list(params.values())))
            else:
                param_values = params

            if isinstance(param_values, list):
                # Check all parameters are in [0, 1]
                all_in_range = all(0 <= p <= 1 for p in param_values if isinstance(p, (int, float)))
                checks.append(ValidationResult(
                    "params_in_range",
                    all_in_range,
                    "Parameters should be in [0, 1]"
                ))

                # Check we have some parameters
                checks.append(ValidationResult(
                    "params_count",
                    len(param_values) > 0,
                    f"Got {len(param_values)} parameters"
                ))
            else:
                checks.append(ValidationResult(
                    "params_format", False, "Unexpected parameter format"
                ))

        except json.JSONDecodeError as e:
            checks.append(ValidationResult(
                "params_json_valid", False, str(e)
            ))
        except Exception as e:
            checks.append(ValidationResult(
                "params_readable", False, str(e)
            ))

        return checks


def validate_dataset(dataset_dir: Path, max_samples: int = None) -> dict:
    """
    Validate all samples in a dataset directory.

    Args:
        dataset_dir: Path to dataset root
        max_samples: Maximum samples to validate (None = all)

    Returns:
        Validation report dictionary
    """
    validator = OutputValidator()

    # Find all sample directories
    sample_dirs = sorted([
        d for d in dataset_dir.iterdir()
        if d.is_dir() and d.name.startswith('sample_')
    ])

    if max_samples:
        sample_dirs = sample_dirs[:max_samples]

    print(f"Validating {len(sample_dirs)} samples...")

    results = []
    failed_samples = []

    for i, sample_dir in enumerate(sample_dirs):
        if (i + 1) % 100 == 0:
            print(f"  Progress: {i+1}/{len(sample_dirs)}")

        validation = validator.validate_sample(sample_dir)
        results.append(validation)

        if not validation.passed:
            failed_samples.append({
                "sample": sample_dir.name,
                "failed_checks": [
                    {"check": c.check_name, "message": c.message}
                    for c in validation.failed_checks
                ]
            })

    # Generate report
    report = {
        "total_samples": len(results),
        "passed_samples": len([r for r in results if r.passed]),
        "failed_samples": len(failed_samples),
        "pass_rate": len([r for r in results if r.passed]) / len(results) if results else 0,
        "failures": failed_samples[:100],  # Limit to first 100 failures
        "all_passed": len(failed_samples) == 0
    }

    return report


def main():
    """Main entry point."""
    if len(sys.argv) < 2:
        print("Usage: python validate_outputs.py <dataset_dir> [max_samples]")
        sys.exit(1)

    dataset_dir = Path(sys.argv[1])
    max_samples = int(sys.argv[2]) if len(sys.argv) > 2 else None

    if not dataset_dir.exists():
        print(f"ERROR: Dataset directory not found: {dataset_dir}")
        sys.exit(1)

    print("=" * 60)
    print("Dataset Output Validation")
    print("=" * 60)
    print(f"Dataset directory: {dataset_dir}")
    print()

    report = validate_dataset(dataset_dir, max_samples)

    # Save report
    report_path = Path(__file__).parent / 'output_validation_report.json'
    with open(report_path, 'w') as f:
        json.dump(report, f, indent=2)

    print("\n" + "=" * 60)
    print("Validation Results")
    print("=" * 60)
    print(f"Total Samples: {report['total_samples']}")
    print(f"Passed: {report['passed_samples']}")
    print(f"Failed: {report['failed_samples']}")
    print(f"Pass Rate: {report['pass_rate']*100:.1f}%")
    print(f"\nStatus: {'PASS' if report['all_passed'] else 'FAIL'}")
    print(f"\nReport saved to: {report_path}")

    if report['failures']:
        print(f"\nFirst {min(5, len(report['failures']))} failures:")
        for failure in report['failures'][:5]:
            print(f"  - {failure['sample']}: {failure['failed_checks'][0]['check']}")

    sys.exit(0 if report['all_passed'] else 1)


if __name__ == '__main__':
    main()
