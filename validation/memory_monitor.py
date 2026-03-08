#!/usr/bin/env python3
"""
Memory monitor for dataset generation.
Tracks peak memory usage during generation.
"""

import json
import time
import sys
from pathlib import Path

try:
    import psutil
except ImportError:
    print("ERROR: psutil not installed. Run: pip install psutil")
    sys.exit(1)


class MemoryMonitor:
    """Monitors memory usage of a process."""

    def __init__(self, pid: int = None, interval: float = 1.0):
        """
        Initialize monitor.

        Args:
            pid: Process ID to monitor (None = current process)
            interval: Sampling interval in seconds
        """
        self.process = psutil.Process(pid) if pid else psutil.Process()
        self.interval = interval
        self.samples = []
        self.peak_gb = 0.0
        self.running = False

    def sample(self) -> float:
        """Take a memory sample, return GB."""
        try:
            mem_info = self.process.memory_info()
            rss_gb = mem_info.rss / (1024 ** 3)
            self.samples.append(rss_gb)
            self.peak_gb = max(self.peak_gb, rss_gb)
            return rss_gb
        except psutil.NoSuchProcess:
            return 0.0

    def monitor_loop(self, duration_seconds: float = None) -> dict:
        """
        Monitor memory for specified duration or until interrupted.

        Args:
            duration_seconds: Duration to monitor (None = until KeyboardInterrupt)

        Returns:
            Dictionary with monitoring results
        """
        self.running = True
        start_time = time.time()

        try:
            while self.running:
                current_gb = self.sample()
                elapsed = time.time() - start_time

                print(f"\rMemory: {current_gb:.2f} GB | Peak: {self.peak_gb:.2f} GB | "
                      f"Time: {elapsed:.0f}s", end='', flush=True)

                if duration_seconds and elapsed >= duration_seconds:
                    break

                time.sleep(self.interval)

        except KeyboardInterrupt:
            print("\nMonitoring stopped by user")

        print()  # New line after progress display

        return {
            "peak_gb": self.peak_gb,
            "samples": len(self.samples),
            "duration_seconds": time.time() - start_time,
            "average_gb": sum(self.samples) / len(self.samples) if self.samples else 0,
            "min_gb": min(self.samples) if self.samples else 0,
            "max_gb": self.peak_gb,
        }

    def stop(self):
        """Stop monitoring."""
        self.running = False


def monitor_generation_process(process_name: str = "python") -> dict:
    """
    Find and monitor a generation process by name.

    Args:
        process_name: Name of process to find and monitor

    Returns:
        Monitoring results
    """
    # Find the process
    target_process = None
    for proc in psutil.process_iter(['pid', 'name', 'cmdline']):
        try:
            if process_name.lower() in proc.info['name'].lower():
                # Check if it's a dataset generator
                cmdline = proc.info.get('cmdline', [])
                if any('dataset' in str(c).lower() for c in cmdline):
                    target_process = proc
                    break
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue

    if target_process is None:
        print(f"No {process_name} process found running dataset generation")
        print("Monitoring current process instead...")
        monitor = MemoryMonitor()
    else:
        print(f"Found process: PID {target_process.pid}")
        monitor = MemoryMonitor(pid=target_process.pid)

    return monitor.monitor_loop()


def main():
    """Main entry point."""
    print("=" * 60)
    print("Memory Monitor for Dataset Generation")
    print("=" * 60)
    print("\nMonitoring memory usage... (Ctrl+C to stop)\n")

    # Check if we should monitor a specific PID
    if len(sys.argv) > 1:
        try:
            pid = int(sys.argv[1])
            monitor = MemoryMonitor(pid=pid)
        except ValueError:
            print(f"Invalid PID: {sys.argv[1]}")
            sys.exit(1)
    else:
        monitor = MemoryMonitor()

    results = monitor.monitor_loop()

    # Generate report
    report = {
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
        "results": results,
        "criteria": {
            "max_memory_gb": 4.0,
            "passed": results["peak_gb"] < 4.0
        }
    }

    # Save report
    report_path = Path(__file__).parent / 'memory_report.json'
    with open(report_path, 'w') as f:
        json.dump(report, f, indent=2)

    print("\n" + "=" * 60)
    print("Memory Monitoring Results")
    print("=" * 60)
    print(f"Peak Memory: {results['peak_gb']:.2f} GB")
    print(f"Average Memory: {results['average_gb']:.2f} GB")
    print(f"Duration: {results['duration_seconds']:.1f} seconds")
    print(f"Status: {'PASS' if report['criteria']['passed'] else 'FAIL'} (< 4 GB)")
    print(f"\nReport saved to: {report_path}")

    sys.exit(0 if report['criteria']['passed'] else 1)


if __name__ == '__main__':
    main()
