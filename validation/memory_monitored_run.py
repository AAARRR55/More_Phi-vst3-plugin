#!/usr/bin/env python3
"""
Memory-monitored scale test runner.
Launches run_scale_test.py and monitors the ACTUAL generator process tree
(Python runner + MorePhiCLI.exe children) instead of the monitor's own RSS.
"""
import json
import subprocess
import sys
import time
from pathlib import Path

import psutil


def monitor_process_tree(proc: psutil.Process, interval: float = 0.5):
    """Monitor a process and all its children, returning peak combined RSS."""
    samples = []
    peak_gb = 0.0
    child_names_seen = set()

    while proc.is_running() and proc.status() != psutil.STATUS_ZOMBIE:
        try:
            children = proc.children(recursive=True)
            tree_rss = proc.memory_info().rss
            for child in children:
                try:
                    tree_rss += child.memory_info().rss
                    child_names_seen.add(child.name())
                except (psutil.NoSuchProcess, psutil.AccessDenied):
                    pass

            rss_gb = tree_rss / (1024 ** 3)
            samples.append(rss_gb)
            peak_gb = max(peak_gb, rss_gb)
            print(f"\rTree RSS: {rss_gb:.4f} GB | Peak: {peak_gb:.4f} GB | "
                  f"Children: {len(children)}", end='', flush=True)
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            break

        time.sleep(interval)

    print()
    return {
        "peak_gb": peak_gb,
        "samples": len(samples),
        "average_gb": sum(samples) / len(samples) if samples else 0,
        "min_gb": min(samples) if samples else 0,
        "max_gb": peak_gb,
        "child_processes_observed": sorted(child_names_seen),
    }


def main():
    script_dir = Path(__file__).parent
    scale_test = script_dir / "run_scale_test.py"

    print("=" * 60)
    print("Memory-Monitored Scale Test")
    print("=" * 60)
    print(f"Launching: {scale_test}")
    print("Monitoring process tree (runner + MorePhiCLI children)...\n")

    # Launch scale test as a subprocess
    proc = subprocess.Popen(
        [sys.executable, str(scale_test)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )

    ps = psutil.Process(proc.pid)
    start_time = time.time()

    # Monitor until the process exits
    results = monitor_process_tree(ps, interval=0.25)
    elapsed = time.time() - start_time

    # Capture stdout
    stdout, _ = proc.communicate()
    retcode = proc.returncode
    print(f"\nScale test exited with code {retcode}")
    sys.stdout.buffer.write(stdout)
    sys.stdout.flush()

    results["duration_seconds"] = elapsed

    # Write report
    report = {
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
        "scope": "generator_process_tree",
        "monitored_pid": proc.pid,
        "duration_configured_seconds": elapsed,
        "dataset_generator_pid_supplied": True,
        "monitoring_method": "psutil process tree (parent + recursive children)",
        "results": results,
        "criteria": {
            "max_memory_gb": 4.0,
            "passed": results["peak_gb"] < 4.0
        }
    }

    report_path = script_dir / "memory_report.json"
    with open(report_path, "w") as f:
        json.dump(report, f, indent=2)

    print("=" * 60)
    print("Memory Monitoring Results")
    print("=" * 60)
    print(f"Peak Memory (tree): {results['peak_gb']:.4f} GB "
          f"({results['peak_gb'] * 1024:.1f} MiB)")
    print(f"Average Memory:     {results['average_gb']:.4f} GB")
    print(f"Samples:            {results['samples']}")
    print(f"Child processes:    {results.get('child_processes_observed', [])}")
    print(f"Duration:           {elapsed:.1f}s")
    print(f"Status:             {'PASS' if report['criteria']['passed'] else 'FAIL'} (< 4 GB)")
    print(f"\nReport saved to: {report_path}")

    sys.exit(0 if report["criteria"]["passed"] and retcode == 0 else 1)


if __name__ == "__main__":
    main()
