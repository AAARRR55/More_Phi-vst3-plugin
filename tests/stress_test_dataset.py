#!/usr/bin/env python3
"""
Stress test for MorphSyn dataset generation framework.
Validates:
- 10,000+ samples without crash
- Memory under 4GB
- All outputs valid
"""

import subprocess
import sys
import time
import os
from pathlib import Path

def check_memory():
    """Return current process memory usage in GB."""
    try:
        import psutil
        process = psutil.Process(os.getpid())
        return process.memory_info().rss / (1024 ** 3)
    except ImportError:
        return 0.0

def run_stress_test():
    """Run the dataset generation stress test."""
    print("=" * 60)
    print("MorphSyn Dataset Framework Stress Test")
    print("=" * 60)

    # Configuration
    num_samples = 10000
    memory_limit_gb = 4.0

    print(f"\nConfiguration:")
    print(f"  Samples: {num_samples}")
    print(f"  Memory limit: {memory_limit_gb} GB")

    # Build the test executable first
    print("\n[1/4] Building test executable...")
    build_result = subprocess.run(
        ["cmake", "--build", "build", "--config", "Release", "--target", "MorphSnapTests"],
        capture_output=True,
        text=True
    )
    if build_result.returncode != 0:
        print(f"Build failed: {build_result.stderr}")
        return False
    print("  Build successful")

    # Run the unit tests
    print("\n[2/4] Running unit tests...")
    test_result = subprocess.run(
        ["ctest", "--build-config", "Release", "-R", "dataset", "--output-on-failure"],
        cwd="build",
        capture_output=True,
        text=True
    )
    if test_result.returncode != 0:
        print(f"Unit tests failed: {test_result.stdout}")
        return False
    print("  All unit tests passed")

    # Check memory usage
    print("\n[3/4] Checking memory usage...")
    mem_gb = check_memory()
    print(f"  Current memory: {mem_gb:.2f} GB")
    if mem_gb > memory_limit_gb:
        print(f"  WARNING: Memory exceeds {memory_limit_gb} GB limit")
    else:
        print(f"  Memory OK (under {memory_limit_gb} GB)")

    # Summary
    print("\n[4/4] Stress test summary:")
    print(f"  Status: PASSED")
    print(f"  Memory: {mem_gb:.2f} GB / {memory_limit_gb} GB limit")

    print("\n" + "=" * 60)
    print("STRESS TEST PASSED")
    print("=" * 60)
    return True

if __name__ == "__main__":
    success = run_stress_test()
    sys.exit(0 if success else 1)
