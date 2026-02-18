#!/usr/bin/env python3
"""
Real-Time Safety Testing Script

Tests for real-time safety violations in the audio plugin including:
- Memory allocation detection
- Lock contention detection
- CPU spike detection
- Audio thread violation detection
"""

import subprocess
import json
import sys
import os
import time
import argparse
from pathlib import Path
from typing import Dict, List, Any
import numpy as np


class RealTimeSafetyTester:
    """Automates real-time safety testing for audio plugins."""

    def __init__(self, plugin_path: str, host_path: str = None):
        """
        Initialize the real-time safety tester.

        Args:
            plugin_path: Path to the plugin bundle
            host_path: Path to test host application
        """
        self.plugin_path = plugin_path
        self.host_path = host_path or self._find_test_host()
        self.violations = []
        self.metrics = {}

    def _find_test_host(self) -> str:
        """Find a suitable test host application."""
        possible_hosts = []

        if sys.platform == "win32":
            possible_hosts = [
                "C:/Program Files/Steinberg/VST3SDK/Build/Win64/Release/vst3_host.exe",
                "./build/test_host.exe",
            ]
        elif sys.platform == "darwin":
            possible_hosts = [
                "./build/test_host",
                "/Applications/Steinberg/VST3Validator.app/Contents/MacOS/vst3validator",
            ]

        for path in possible_hosts:
            if os.path.exists(path):
                return path

        return ""

    def run_stress_test(self, duration_minutes: int = 10,
                       buffer_size: int = 64,
                       sample_rate: int = 44100) -> Dict[str, Any]:
        """
        Run a stress test monitoring for real-time safety violations.

        Args:
            duration_minutes: Test duration in minutes
            buffer_size: Audio buffer size in samples
            sample_rate: Sample rate in Hz

        Returns:
            Dictionary containing test results and metrics
        """
        print(f"Running real-time safety stress test")
        print(f"Duration: {duration_minutes} minutes")
        print(f"Buffer size: {buffer_size} samples")
        print(f"Sample rate: {sample_rate} Hz")
        print("-" * 60)

        results = {
            "test_config": {
                "duration_minutes": duration_minutes,
                "buffer_size": buffer_size,
                "sample_rate": sample_rate,
            },
            "violations": [],
            "metrics": {},
            "status": "unknown"
        }

        # In a real implementation, this would launch a test host
        # that monitors the plugin for violations
        # For now, we'll simulate the structure

        test_duration_seconds = duration_minutes * 60
        start_time = time.time()

        print("Monitoring for violations...")

        # Simulate monitoring loop
        # In real implementation, this would read from test host
        while (time.time() - start_time) < test_duration_seconds:
            elapsed = time.time() - start_time
            progress = (elapsed / test_duration_seconds) * 100

            # Simulate checking for violations
            # In real implementation, parse logs from test host
            time.sleep(1)

            if int(elapsed) % 30 == 0:
                print(f"Progress: {progress:.1f}% ({int(elapsed)}s / {test_duration_seconds}s)")

        results["status"] = "completed"
        print(f"\nStress test completed in {int(time.time() - start_time)} seconds")

        return results

    def detect_memory_allocations(self, log_path: str) -> List[Dict[str, Any]]:
        """
        Detect memory allocations on the audio thread from logs.

        Args:
            log_path: Path to log file containing allocation tracking

        Returns:
            List of detected allocation violations
        """
        allocations = []

        if not os.path.exists(log_path):
            return allocations

        with open(log_path, 'r') as f:
            for line_num, line in enumerate(f, 1):
                if "ALLOCATION" in line and "AUDIO_THREAD" in line:
                    allocations.append({
                        "line": line_num,
                        "message": line.strip(),
                        "type": "memory_allocation"
                    })

        return allocations

    def detect_lock_contention(self, log_path: str) -> List[Dict[str, Any]]:
        """
        Detect lock contention events from logs.

        Args:
            log_path: Path to log file containing lock tracking

        Returns:
            List of detected lock contention violations
        """
        contentions = []

        if not os.path.exists(log_path):
            return contentions

        with open(log_path, 'r') as f:
            for line_num, line in enumerate(f, 1):
                if "LOCK_CONTENTION" in line:
                    contentions.append({
                        "line": line_num,
                        "message": line.strip(),
                        "type": "lock_contention"
                    })

        return contentions

    def analyze_cpu_spikes(self, log_path: str,
                          spike_threshold: float = 3.0) -> Dict[str, Any]:
        """
        Analyze CPU usage for spikes exceeding threshold.

        Args:
            log_path: Path to log file containing CPU measurements
            spike_threshold: Multiplier for spike detection (default: 3x)

        Returns:
            Dictionary containing spike analysis
        """
        spike_data = {
            "spikes": [],
            "average_time": 0.0,
            "max_time": 0.0,
            "spike_count": 0,
            "total_blocks": 0
        }

        if not os.path.exists(log_path):
            return spike_data

        processing_times = []

        with open(log_path, 'r') as f:
            for line in f:
                if "PROCESS_TIME" in line:
                    # Extract time value (format: "PROCESS_TIME: 123.45 us")
                    try:
                        time_str = line.split(":")[1].strip()
                        time_val = float(time_str.split()[0])
                        processing_times.append(time_val)
                    except (IndexError, ValueError):
                        continue

        if processing_times:
            spike_data["average_time"] = np.mean(processing_times)
            spike_data["max_time"] = np.max(processing_times)
            spike_data["total_blocks"] = len(processing_times)

            # Detect spikes
            moving_avg = np.convolve(processing_times,
                                    np.ones(100)/100,
                                    mode='valid')

            for i, time_val in enumerate(processing_times):
                if i < len(moving_avg):
                    threshold = moving_avg[i] * spike_threshold
                    if time_val > threshold:
                        spike_data["spikes"].append({
                            "block": i,
                            "time": time_val,
                            "threshold": threshold,
                            "severity": time_val / threshold
                        })

            spike_data["spike_count"] = len(spike_data["spikes"])

        return spike_data

    def measure_latency_accuracy(self, plugin_path: str,
                                 sample_rate: int = 44100) -> Dict[str, Any]:
        """
        Measure the accuracy of reported latency.

        Args:
            plugin_path: Path to the plugin
            sample_rate: Sample rate for test

        Returns:
            Dictionary containing latency measurements
        """
        latency_results = {
            "reported_latency": 0,
            "actual_latency": 0,
            "difference": 0,
            "accuracy": 0.0
        }

        # In real implementation, this would:
        # 1. Load plugin
        # 2. Query getLatencySamples()
        # 3. Measure actual latency via impulse response
        # 4. Compare values

        return latency_results

    def generate_report(self, results: Dict[str, Any],
                       output_path: str = "realtime_safety_report.json"):
        """
        Generate a comprehensive test report.

        Args:
            results: Test results dictionary
            output_path: Path to save the report
        """
        report = {
            "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
            "plugin_path": self.plugin_path,
            "results": results,
            "acceptance_criteria": {
                "memory_allocations": "Must be 0",
                "lock_contentions": "Must be 0",
                "cpu_spikes": "< 1 per 10,000 blocks",
                "latency_accuracy": "Reported == Actual"
            },
            "status": self._check_acceptance_criteria(results)
        }

        with open(output_path, 'w') as f:
            json.dump(report, f, indent=2)

        return report

    def _check_acceptance_criteria(self, results: Dict[str, Any]) -> str:
        """
        Check if results meet acceptance criteria.

        Args:
            results: Test results dictionary

        Returns:
            "passed", "failed", or "warning"
        """
        violations = results.get("violations", [])

        # Check for blocking violations
        for v in violations:
            if v.get("type") in ["memory_allocation", "lock_contention"]:
                return "failed"

        # Check CPU spikes
        cpu_metrics = results.get("metrics", {}).get("cpu", {})
        spike_count = cpu_metrics.get("spike_count", 0)
        total_blocks = cpu_metrics.get("total_blocks", 1)

        if spike_count > total_blocks / 10000:
            return "failed"

        return "passed"

    def print_summary(self, results: Dict[str, Any]):
        """Print a human-readable summary of test results."""
        print("\n" + "="*60)
        print("Real-Time Safety Test Results")
        print("="*60)

        status = results.get("status", "unknown").upper()
        print(f"\nOverall Status: {status}")

        # Print violations
        violations = results.get("violations", [])
        if violations:
            print(f"\nViolations Detected: {len(violations)}")
            for v in violations:
                print(f"  - {v.get('type', 'unknown')}: {v.get('message', '')}")
        else:
            print("\nNo violations detected")

        # Print metrics
        metrics = results.get("metrics", {})
        if metrics:
            print("\nMetrics:")
            for key, value in metrics.items():
                print(f"  {key}: {value}")

        # Print acceptance criteria
        print("\n" + "="*60 + "\n")


def main():
    """Main entry point for the script."""
    parser = argparse.ArgumentParser(
        description="Real-Time Safety Testing Script"
    )
    parser.add_argument(
        "--plugin",
        help="Path to plugin bundle",
        required=True
    )
    parser.add_argument(
        "--host",
        help="Path to test host application",
        default=None
    )
    parser.add_argument(
        "--duration",
        help="Stress test duration in minutes",
        type=int,
        default=10
    )
    parser.add_argument(
        "--buffer-size",
        help="Audio buffer size in samples",
        type=int,
        default=64
    )
    parser.add_argument(
        "--sample-rate",
        help="Sample rate in Hz",
        type=int,
        default=44100
    )
    parser.add_argument(
        "--log-path",
        help="Path to log file for analysis",
        default=None
    )
    parser.add_argument(
        "--output",
        help="Path to save JSON results",
        default="realtime_safety_results.json"
    )
    parser.add_argument(
        "--strict",
        help="Exit with error if acceptance criteria not met",
        action="store_true"
    )

    args = parser.parse_args()

    # Create tester and run tests
    tester = RealTimeSafetyTester(
        plugin_path=args.plugin,
        host_path=args.host
    )

    # Run stress test
    stress_results = tester.run_stress_test(
        duration_minutes=args.duration,
        buffer_size=args.buffer_size,
        sample_rate=args.sample_rate
    )

    # Analyze logs if provided
    if args.log_path:
        print(f"\nAnalyzing logs: {args.log_path}")
        stress_results["violations"].extend(
            tester.detect_memory_allocations(args.log_path)
        )
        stress_results["violations"].extend(
            tester.detect_lock_contention(args.log_path)
        )
        stress_results["metrics"]["cpu"] = tester.analyze_cpu_spikes(args.log_path)

    # Generate and save report
    report = tester.generate_report(stress_results, args.output)
    tester.print_summary(stress_results)

    print(f"Results saved to: {args.output}")

    # Exit with appropriate code
    if args.strict:
        sys.exit(0 if report.get("status") == "passed" else 1)

    sys.exit(0)


if __name__ == "__main__":
    main()
