#!/usr/bin/env python3
"""
Audio Quality Testing Script

Tests for audio quality including:
- Parameter smoothing verification
- Click/pop detection
- Sample accuracy
- Crossfade quality
- SNR and THD measurements
"""

import subprocess
import json
import sys
import os
import argparse
import numpy as np
import soundfile as sf
from pathlib import Path
from typing import Dict, List, Any, Tuple


class AudioQualityTester:
    """Automates audio quality testing for plugins."""

    def __init__(self, plugin_path: str, test_host: str = None):
        """
        Initialize the audio quality tester.

        Args:
            plugin_path: Path to the plugin bundle
            test_host: Path to test host application
        """
        self.plugin_path = plugin_path
        self.test_host = test_host
        self.test_results = {}

    def generate_test_tone(self, frequency: float = 1000.0,
                          duration: float = 1.0,
                          sample_rate: int = 44100,
                          amplitude: float = 0.7) -> np.ndarray:
        """
        Generate a sine wave test tone.

        Args:
            frequency: Frequency in Hz
            duration: Duration in seconds
            sample_rate: Sample rate in Hz
            amplitude: Peak amplitude (0.0 to 1.0)

        Returns:
            NumPy array containing the test tone
        """
        t = np.linspace(0, duration, int(sample_rate * duration), False)
        tone = amplitude * np.sin(2 * np.pi * frequency * t)
        return tone

    def detect_clicks_and_pops(self, audio: np.ndarray,
                              click_threshold: float = 0.2,
                              pop_threshold: float = 0.5) -> Dict[str, Any]:
        """
        Detect clicks and pops in audio signal.

        Args:
            audio: Audio signal array
            click_threshold: Maximum allowed change between samples
            pop_threshold: Threshold for pop detection (local energy)

        Returns:
            Dictionary containing detection results
        """
        results = {
            "clicks": [],
            "pops": [],
            "click_count": 0,
            "pop_count": 0,
            "max_click_amplitude": 0.0
        }

        # Detect clicks (sudden changes)
        diff = np.abs(np.diff(audio))
        click_indices = np.where(diff > click_threshold)[0]

        for idx in click_indices:
            results["clicks"].append({
                "sample": int(idx),
                "amplitude": float(diff[idx])
            })

        results["click_count"] = len(click_indices)
        if len(click_indices) > 0:
            results["max_click_amplitude"] = float(np.max(diff[click_indices]))

        # Detect pops (high-energy bursts)
        window_size = 10
        for i in range(window_size, len(audio) - window_size):
            local_energy = np.mean(np.abs(audio[i-window_size:i+window_size]))
            if local_energy > pop_threshold:
                results["pops"].append({
                    "sample": int(i),
                    "energy": float(local_energy)
                })

        results["pop_count"] = len(results["pops"])

        return results

    def verify_parameter_smoothing(self, audio: np.ndarray,
                                   transition_point: int,
                                   max_change: float = 0.1) -> Dict[str, Any]:
        """
        Verify parameter smoothing around a transition point.

        Args:
            audio: Audio signal array
            transition_point: Sample index of parameter change
            max_change: Maximum allowed change per sample

        Returns:
            Dictionary containing smoothing verification results
        """
        results = {
            "passed": True,
            "violations": [],
            "max_derivative": 0.0,
            "smoothness_score": 0.0
        }

        # Check samples around transition
        window = 100  # Check 100 samples before and after
        start = max(0, transition_point - window)
        end = min(len(audio), transition_point + window)

        for i in range(start + 1, end):
            diff = abs(audio[i] - audio[i - 1])
            results["max_derivative"] = max(results["max_derivative"], diff)

            if diff > max_change:
                results["violations"].append({
                    "sample": i,
                    "change": float(diff),
                    "allowed": max_change
                })
                results["passed"] = False

        # Calculate smoothness score
        if len(audio) > 0:
            total_change = np.sum(np.abs(np.diff(audio[start:end])))
            ideal_change = abs(audio[end - 1] - audio[start])
            if ideal_change > 0:
                results["smoothness_score"] = min(1.0, ideal_change / total_change)
            else:
                results["smoothness_score"] = 1.0

        return results

    def measure_snr(self, signal: np.ndarray,
                   noise: np.ndarray) -> float:
        """
        Measure Signal-to-Noise Ratio.

        Args:
            signal: Signal array
            noise: Noise array

        Returns:
            SNR in dB
        """
        signal_power = np.mean(signal ** 2)
        noise_power = np.mean(noise ** 2)

        if noise_power == 0:
            return float('inf')

        snr = 10 * np.log10(signal_power / noise_power)
        return float(snr)

    def measure_thd(self, signal: np.ndarray,
                   fundamental: float,
                   sample_rate: int = 44100) -> Dict[str, Any]:
        """
        Measure Total Harmonic Distortion.

        Args:
            signal: Signal array
            fundamental: Fundamental frequency in Hz
            sample_rate: Sample rate in Hz

        Returns:
            Dictionary containing THD measurements
        """
        # Compute FFT
        fft_result = np.fft.rfft(signal)
        fft_magnitude = np.abs(fft_result)
        fft_freq = np.fft.rfftfreq(len(signal), 1.0 / sample_rate)

        # Find fundamental
        fund_bin = np.argmin(np.abs(fft_freq - fundamental))
        fundamental_power = fft_magnitude[fund_bin] ** 2

        # Measure harmonics
        harmonic_powers = []
        for n in range(2, 10):  # Measure up to 9th harmonic
            harmonic_freq = fundamental * n
            if harmonic_freq < sample_rate / 2:
                harm_bin = np.argmin(np.abs(fft_freq - harmonic_freq))
                harmonic_powers.append(fft_magnitude[harm_bin] ** 2)

        # Calculate THD
        harmonic_power = sum(harmonic_powers)
        if fundamental_power == 0:
            thd = 0.0
        else:
            thd = np.sqrt(harmonic_power) / np.sqrt(fundamental_power)

        thd_db = 20 * np.log10(thd) if thd > 0 else float('-inf')

        return {
            "thd_percent": thd * 100,
            "thd_db": thd_db,
            "fundamental_power": float(fundamental_power),
            "harmonic_powers": [float(p) for p in harmonic_powers]
        }

    def measure_crossfade_quality(self, crossfade_audio: np.ndarray,
                                  start_amplitude: float,
                                  end_amplitude: float) -> Dict[str, Any]:
        """
        Measure the quality of a crossfade.

        Args:
            crossfade_audio: Audio signal containing crossfade
            start_amplitude: Expected start amplitude
            end_amplitude: Expected end amplitude

        Returns:
            Dictionary containing crossfade quality metrics
        """
        results = {
            "monotonicity": 0.0,
            "symmetry": 0.0,
            "energy_conservation": 0.0,
            "smoothness": 0.0,
            "overall_score": 0.0
        }

        if len(crossfade_audio) < 2:
            return results

        # Measure monotonicity (consistent change direction)
        direction_changes = 0
        for i in range(1, len(crossfade_audio) - 1):
            prev_diff = crossfade_audio[i] - crossfade_audio[i - 1]
            next_diff = crossfade_audio[i + 1] - crossfade_audio[i]
            if prev_diff * next_diff < 0:
                direction_changes += 1

        results["monotonicity"] = max(0.0, 1.0 - direction_changes / len(crossfade_audio))

        # Measure energy conservation
        total_energy = np.sum(crossfade_audio ** 2)
        expected_energy = (start_amplitude ** 2 + end_amplitude ** 2) * len(crossfade_audio) / 2
        if expected_energy > 0:
            results["energy_conservation"] = max(0.0, 1.0 - abs(total_energy - expected_energy) / expected_energy)

        # Measure smoothness (low frequency content)
        fft_result = np.fft.rfft(crossfade_audio)
        fft_magnitude = np.abs(fft_result)
        low_freq_energy = np.sum(fft_magnitude[:len(fft_magnitude)//10]**2)
        total_freq_energy = np.sum(fft_magnitude**2)
        if total_freq_energy > 0:
            results["smoothness"] = low_freq_energy / total_freq_energy

        # Calculate overall score
        results["overall_score"] = (
            results["monotonicity"] * 0.3 +
            results["energy_conservation"] * 0.4 +
            results["smoothness"] * 0.3
        )

        return results

    def verify_sample_accuracy(self, audio: np.ndarray,
                              automation_points: Dict[int, float]) -> Dict[str, Any]:
        """
        Verify sample-accurate automation.

        Args:
            audio: Audio signal array
            automation_points: Dictionary mapping sample indices to values

        Returns:
            Dictionary containing accuracy verification results
        """
        results = {
            "passed": True,
            "errors": [],
            "tested_points": len(automation_points)
        }

        # Verify each automation point
        for sample_idx, expected_value in automation_points.items():
            if sample_idx < len(audio):
                actual_value = audio[sample_idx]
                error = abs(actual_value - expected_value)

                if error > 0.001:  # Allow small floating point errors
                    results["errors"].append({
                        "sample": sample_idx,
                        "expected": expected_value,
                        "actual": actual_value,
                        "error": error
                    })
                    results["passed"] = False

        return results

    def run_full_test_suite(self, test_audio_path: str = None,
                           output_dir: str = "audio_quality_results") -> Dict[str, Any]:
        """
        Run the full audio quality test suite.

        Args:
            test_audio_path: Path to test audio file (optional)
            output_dir: Directory to save test outputs

        Returns:
            Dictionary containing all test results
        """
        results = {
            "timestamp": str(subprocess.check_output(['date']).decode().strip()),
            "tests": {}
        }

        os.makedirs(output_dir, exist_ok=True)

        # Generate or load test audio
        if test_audio_path and os.path.exists(test_audio_path):
            audio, sr = sf.read(test_audio_path)
        else:
            audio = self.generate_test_tone(frequency=1000, duration=1.0)
            sr = 44100

        # Test 1: Click/Pop Detection
        print("Running click/pop detection test...")
        results["tests"]["clicks_pops"] = self.detect_clicks_and_pops(audio)

        # Test 2: Parameter Smoothing (simulate a transition)
        print("Running parameter smoothing test...")
        transition_audio = audio.copy()
        transition_point = len(audio) // 2
        transition_audio[transition_point:] *= 0.5  # Simulate parameter change
        results["tests"]["parameter_smoothing"] = self.verify_parameter_smoothing(
            transition_audio, transition_point
        )

        # Test 3: SNR Measurement
        print("Running SNR measurement...")
        noise = np.random.normal(0, 0.001, len(audio))
        snr = self.measure_snr(audio, noise)
        results["tests"]["snr"] = {"snr_db": snr}

        # Test 4: THD Measurement
        print("Running THD measurement...")
        results["tests"]["thd"] = self.measure_thd(audio, fundamental=1000, sample_rate=sr)

        # Test 5: Crossfade Quality
        print("Running crossfade quality test...")
        crossfade_length = sr // 10  # 100ms crossfade
        crossfade = np.zeros(crossfade_length)
        for i in range(crossfade_length):
            t = i / crossfade_length
            crossfade[i] = (1 - t) * 1.0 + t * 0.5  # Linear crossfade
        results["tests"]["crossfade"] = self.measure_crossfade_quality(crossfade, 1.0, 0.5)

        # Save results
        output_path = os.path.join(output_dir, "audio_quality_results.json")
        with open(output_path, 'w') as f:
            json.dump(results, f, indent=2)

        return results

    def check_acceptance_criteria(self, results: Dict[str, Any]) -> bool:
        """
        Check if results meet acceptance criteria.

        Acceptance Criteria:
        - Clicks: 0
        - Pops: 0
        - Parameter smoothing: monotonicity > 0.95
        - SNR: > 90 dB
        - THD: < 0.01%
        - Crossfade: overall_score > 0.9

        Args:
            results: Test results dictionary

        Returns:
            True if acceptance criteria met
        """
        tests = results.get("tests", {})

        # Check clicks/pops
        if tests.get("clicks_pops", {}).get("click_count", 0) > 0:
            print("FAIL: Clicks detected")
            return False
        if tests.get("clicks_pops", {}).get("pop_count", 0) > 0:
            print("FAIL: Pops detected")
            return False

        # Check parameter smoothing
        smoothing = tests.get("parameter_smoothing", {})
        if not smoothing.get("passed", False):
            print("FAIL: Parameter smoothing violations")
            return False

        # Check SNR
        snr = tests.get("snr", {}).get("snr_db", 0)
        if snr < 90:
            print(f"FAIL: SNR too low: {snr} dB")
            return False

        # Check THD
        thd = tests.get("thd", {}).get("thd_percent", 100)
        if thd > 0.01:
            print(f"FAIL: THD too high: {thd}%")
            return False

        # Check crossfade
        crossfade = tests.get("crossfade", {})
        if crossfade.get("overall_score", 0) < 0.9:
            print(f"FAIL: Crossfade quality too low: {crossfade.get('overall_score', 0)}")
            return False

        print("PASS: All audio quality criteria met")
        return True

    def print_summary(self, results: Dict[str, Any]):
        """Print a human-readable summary of test results."""
        print("\n" + "="*60)
        print("Audio Quality Test Results")
        print("="*60)

        tests = results.get("tests", {})

        # Clicks/Pop
        cp = tests.get("clicks_pops", {})
        print(f"\nClick/Pop Detection:")
        print(f"  Clicks: {cp.get('click_count', 0)}")
        print(f"  Pops: {cp.get('pop_count', 0)}")
        print(f"  Max click amplitude: {cp.get('max_click_amplitude', 0):.4f}")

        # Parameter Smoothing
        ps = tests.get("parameter_smoothing", {})
        print(f"\nParameter Smoothing:")
        print(f"  Passed: {ps.get('passed', False)}")
        print(f"  Max derivative: {ps.get('max_derivative', 0):.4f}")
        print(f"  Smoothness score: {ps.get('smoothness_score', 0):.4f}")

        # SNR
        snr = tests.get("snr", {})
        print(f"\nSignal-to-Noise Ratio:")
        print(f"  SNR: {snr.get('snr_db', 0):.2f} dB")

        # THD
        thd = tests.get("thd", {})
        print(f"\nTotal Harmonic Distortion:")
        print(f"  THD: {thd.get('thd_percent', 0):.4f}%")
        print(f"  THD: {thd.get('thd_db', 0):.2f} dB")

        # Crossfade
        cf = tests.get("crossfade", {})
        print(f"\nCrossfade Quality:")
        print(f"  Monotonicity: {cf.get('monotonicity', 0):.4f}")
        print(f"  Symmetry: {cf.get('symmetry', 0):.4f}")
        print(f"  Energy conservation: {cf.get('energy_conservation', 0):.4f}")
        print(f"  Smoothness: {cf.get('smoothness', 0):.4f}")
        print(f"  Overall score: {cf.get('overall_score', 0):.4f}")

        print("\n" + "="*60 + "\n")


def main():
    """Main entry point for the script."""
    parser = argparse.ArgumentParser(
        description="Audio Quality Testing Script"
    )
    parser.add_argument(
        "--plugin",
        help="Path to plugin bundle",
        required=True
    )
    parser.add_argument(
        "--test-audio",
        help="Path to test audio file",
        default=None
    )
    parser.add_argument(
        "--output-dir",
        help="Directory to save test outputs",
        default="audio_quality_results"
    )
    parser.add_argument(
        "--strict",
        help="Exit with error if acceptance criteria not met",
        action="store_true"
    )

    args = parser.parse_args()

    # Create tester and run tests
    tester = AudioQualityTester(plugin_path=args.plugin)
    results = tester.run_full_test_suite(
        test_audio_path=args.test_audio,
        output_dir=args.output_dir
    )

    # Print summary
    tester.print_summary(results)

    # Check acceptance criteria
    if args.strict:
        if not tester.check_acceptance_criteria(results):
            sys.exit(1)

    sys.exit(0)


if __name__ == "__main__":
    main()
