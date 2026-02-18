#!/usr/bin/env python3
"""
VST3 Validator Automation Script

Automates running the Steinberg VST3 validator and parsing results.
Generates JSON reports for CI/CD integration.
"""

import subprocess
import json
import sys
import os
import argparse
from pathlib import Path
from typing import Dict, List, Any
import xml.etree.ElementTree as ET


class VST3Validator:
    """Automates VST3 SDK validation testing."""

    def __init__(self, validator_path: str = None, plugin_path: str = None):
        """
        Initialize the VST3 validator.

        Args:
            validator_path: Path to vst3_validator executable
            plugin_path: Path to the plugin .vst3 bundle
        """
        self.validator_path = validator_path or self._find_validator()
        self.plugin_path = plugin_path
        self.results = {}

    def _find_validator(self) -> str:
        """Attempt to locate the VST3 validator automatically."""
        possible_paths = []

        if sys.platform == "win32":
            possible_paths = [
                "C:/Program Files/Steinberg/VST3SDK/vst3_validator.exe",
                "C:/Program Files (x86)/Steinberg/VST3SDK/vst3_validator.exe",
            ]
        elif sys.platform == "darwin":
            possible_paths = [
                "/Applications/VST3Validator.app/Contents/MacOS/vst3validator",
                "/Developer/Applications/Audio/VST3SDK/vst3validator",
            ]
        else:  # Linux
            possible_paths = [
                "/usr/bin/vst3_validator",
                "/usr/local/bin/vst3_validator",
            ]

        for path in possible_paths:
            if os.path.exists(path):
                return path

        return ""

    def validate(self, plugin_path: str = None, timeout: int = 300) -> Dict[str, Any]:
        """
        Run the VST3 validator on the plugin.

        Args:
            plugin_path: Path to the plugin bundle
            timeout: Maximum time to wait for validation (seconds)

        Returns:
            Dictionary containing validation results
        """
        plugin_path = plugin_path or self.plugin_path

        if not plugin_path:
            return {
                "status": "error",
                "error": "No plugin path specified"
            }

        if not os.path.exists(plugin_path):
            return {
                "status": "error",
                "error": f"Plugin not found: {plugin_path}"
            }

        if not self.validator_path:
            return {
                "status": "error",
                "error": "VST3 validator not found"
            }

        print(f"Validating plugin: {plugin_path}")
        print(f"Using validator: {self.validator_path}")

        try:
            # Run the validator
            cmd = [self.validator_path, "-r", plugin_path]
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=timeout
            )

            # Parse the output
            validation_result = self._parse_validator_output(
                result.stdout,
                result.stderr,
                result.returncode
            )

            return validation_result

        except subprocess.TimeoutExpired:
            return {
                "status": "timeout",
                "error": f"Validation timed out after {timeout} seconds"
            }
        except Exception as e:
            return {
                "status": "error",
                "error": str(e)
            }

    def _parse_validator_output(self, stdout: str, stderr: str, returncode: int) -> Dict[str, Any]:
        """
        Parse the validator output into a structured format.

        Args:
            stdout: Standard output from validator
            stderr: Standard error from validator
            returncode: Process return code

        Returns:
            Structured validation results
        """
        results = {
            "status": "passed" if returncode == 0 else "failed",
            "returncode": returncode,
            "tests": [],
            "warnings": [],
            "errors": [],
            "summary": {}
        }

        # Parse stdout for test results
        lines = stdout.split('\n')
        for line in lines:
            line = line.strip()
            if not line:
                continue

            # Look for test results
            if "PASS" in line:
                results["tests"].append({
                    "name": line,
                    "status": "passed"
                })
            elif "FAIL" in line:
                results["tests"].append({
                    "name": line,
                    "status": "failed"
                })
            elif "WARNING" in line:
                results["warnings"].append(line)
            elif "ERROR" in line:
                results["errors"].append(line)

        # Parse stderr for additional information
        if stderr:
            results["stderr"] = stderr

        # Generate summary
        results["summary"] = {
            "total_tests": len(results["tests"]),
            "passed": sum(1 for t in results["tests"] if t["status"] == "passed"),
            "failed": sum(1 for t in results["tests"] if t["status"] == "failed"),
            "warnings": len(results["warnings"]),
            "errors": len(results["errors"])
        }

        # Overall status
        if results["summary"]["failed"] > 0 or results["summary"]["errors"] > 0:
            results["status"] = "failed"
        elif results["summary"]["warnings"] > 0:
            results["status"] = "warning"

        return results

    def generate_junit_xml(self, results: Dict[str, Any], output_path: str):
        """
        Generate JUnit XML report from validation results.

        Args:
            results: Validation results dictionary
            output_path: Path to save the XML file
        """
        root = ET.Element("testsuite")
        root.set("name", "VST3 Validation")
        root.set("tests", str(len(results.get("tests", []))))
        root.set("failures", str(results.get("summary", {}).get("failed", 0)))
        root.set("errors", str(results.get("summary", {}).get("errors", 0)))

        # Add test cases
        for test in results.get("tests", []):
            testcase = ET.SubElement(root, "testcase")
            testcase.set("name", test["name"])
            testcase.set("classname", "VST3Validator")

            if test["status"] == "failed":
                failure = ET.SubElement(testcase, "failure")
                failure.set("message", "Test failed")

        # Add errors as test cases
        for error in results.get("errors", []):
            testcase = ET.SubElement(root, "testcase")
            testcase.set("name", f"Error: {error}")
            error_elem = ET.SubElement(testcase, "error")
            error_elem.text = error

        # Write XML file
        tree = ET.ElementTree(root)
        tree.write(output_path, encoding='unicode', xml_declaration=True)

    def print_summary(self, results: Dict[str, Any]):
        """Print a human-readable summary of validation results."""
        print("\n" + "="*60)
        print("VST3 Validation Results")
        print("="*60)

        status = results.get("status", "unknown")
        status_symbol = {
            "passed": "✓",
            "failed": "✗",
            "warning": "⚠",
            "error": "✗",
            "timeout": "⏱"
        }.get(status, "?")

        print(f"\nOverall Status: {status_symbol} {status.upper()}")

        summary = results.get("summary", {})
        if summary:
            print(f"\nTest Summary:")
            print(f"  Total Tests: {summary.get('total_tests', 0)}")
            print(f"  Passed: {summary.get('passed', 0)}")
            print(f"  Failed: {summary.get('failed', 0)}")
            print(f"  Warnings: {summary.get('warnings', 0)}")
            print(f"  Errors: {summary.get('errors', 0)}")

        # Show warnings if any
        if results.get("warnings"):
            print(f"\nWarnings ({len(results['warnings'])}):")
            for warning in results["warnings"]:
                print(f"  - {warning}")

        # Show errors if any
        if results.get("errors"):
            print(f"\nErrors ({len(results['errors'])}):")
            for error in results["errors"]:
                print(f"  - {error}")

        print("\n" + "="*60 + "\n")

    def check_acceptance_criteria(self, results: Dict[str, Any]) -> bool:
        """
        Check if results meet acceptance criteria.

        Acceptance Criteria:
        - 100% of mandatory tests pass
        - 0 critical, 0 major, 0 minor warnings
        - No errors

        Args:
            results: Validation results dictionary

        Returns:
            True if acceptance criteria met, False otherwise
        """
        if results.get("status") != "passed":
            return False

        summary = results.get("summary", {})

        # All tests must pass
        if summary.get("failed", 0) > 0:
            print("FAIL: Not all tests passed")
            return False

        # No warnings allowed
        if summary.get("warnings", 0) > 0:
            print("FAIL: Warnings detected")
            return False

        # No errors allowed
        if summary.get("errors", 0) > 0:
            print("FAIL: Errors detected")
            return False

        print("PASS: All acceptance criteria met")
        return True


def main():
    """Main entry point for the script."""
    parser = argparse.ArgumentParser(
        description="VST3 Validator Automation Script"
    )
    parser.add_argument(
        "--validator",
        help="Path to vst3_validator executable",
        default=None
    )
    parser.add_argument(
        "--plugin",
        help="Path to plugin .vst3 bundle",
        required=True
    )
    parser.add_argument(
        "--timeout",
        help="Validation timeout in seconds",
        type=int,
        default=300
    )
    parser.add_argument(
        "--output",
        help="Path to save JSON results",
        default="vst3_validation_results.json"
    )
    parser.add_argument(
        "--junit",
        help="Path to save JUnit XML results",
        default=None
    )
    parser.add_argument(
        "--strict",
        help="Exit with error if acceptance criteria not met",
        action="store_true"
    )

    args = parser.parse_args()

    # Create validator and run
    validator = VST3Validator(
        validator_path=args.validator,
        plugin_path=args.plugin
    )

    results = validator.validate(
        plugin_path=args.plugin,
        timeout=args.timeout
    )

    # Print summary
    validator.print_summary(results)

    # Save JSON results
    with open(args.output, 'w') as f:
        json.dump(results, f, indent=2)
    print(f"Results saved to: {args.output}")

    # Generate JUnit XML if requested
    if args.junit:
        validator.generate_junit_xml(results, args.junit)
        print(f"JUnit XML saved to: {args.junit}")

    # Check acceptance criteria
    if args.strict:
        if not validator.check_acceptance_criteria(results):
            sys.exit(1)

    # Exit with appropriate code
    sys.exit(0 if results.get("status") == "passed" else 1)


if __name__ == "__main__":
    main()
