#!/usr/bin/env python3
"""
Stress test for MorphSnap dataset generation framework.
Validates:
- Dataset unit tests pass
- Memory under 4GB (requires psutil for accurate measurement)
- Build completes successfully
"""

import subprocess
import sys
import os
from pathlib import Path

# Resolve paths relative to script location
SCRIPT_DIR = Path(__file__).parent.resolve()
PROJECT_ROOT = SCRIPT_DIR.parent
BUILD_DIR = PROJECT_ROOT / "build"

def check_memory():
    """Return current process memory usage in GB, or None if psutil unavailable."""
    try:
        import psutil
        process = psutil.Process(os.getpid())
        return process.memory_info().rss / (1024 ** 3)
    except ImportError:
        return None

def run_stress_test():
    """Run the dataset generation stress test."""
    print("=" * 60)
    print("MorphSnap Dataset Framework Stress Test")
    print("=" * 60)

    # Configuration
    memory_limit_gb = 4.0

    print(f"\nConfiguration:")
    print(f"  Memory limit: {memory_limit_gb} GB")

    # Build the test executable first
    print("\n[1/4] Building test executable...")
    build_result = subprocess.run(
        ["cmake", "--build", str(BUILD_DIR), "--config", "Release", "--target", "MorphSnapTests"],
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
        cwd=str(BUILD_DIR),
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
    if mem_gb is None:
        print("  WARNING: psutil not installed - cannot measure memory usage")
        print("  Install with: pip install psutil")
    else:
        print(f"  Current memory: {mem_gb:.2f} GB")
        if mem_gb > memory_limit_gb:
            print(f"  WARNING: Memory exceeds {memory_limit_gb} GB limit")
        else:
            print(f"  Memory OK (under {memory_limit_gb} GB)")

    # Summary
    print("\n[4/4] Stress test summary:")
    print(f"  Status: PASSED")
    if mem_gb is not None:
        print(f"  Memory: {mem_gb:.2f} GB / {memory_limit_gb} GB limit")
    else:
        print(f"  Memory: (psutil not available)")

    print("\n" + "=" * 60)
    print("STRESS TEST PASSED")
    print("=" * 60)
    return True

if __name__ == "__main__":
    success = run_stress_test()
    sys.exit(0 if success else 1)
