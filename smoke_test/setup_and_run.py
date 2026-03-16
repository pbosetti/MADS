#!/usr/bin/env python3
"""
MADS Smoke Test: Full Setup and Run Pipeline

Performs the complete smoke test workflow:
  1. Install MADS from its build directory
  2. Configure the smoke test project
  3. Build the smoke test project
  4. Run all smoke tests (with broker)

Cross-platform (macOS, Linux, Windows).

Usage:
  python setup_and_run.py [options]

Options:
  --mads-build-dir PATH  MADS build directory (default: ../build)
  --prefix PATH          MADS install prefix (default: ../products)
  --build-dir PATH       Smoke test build directory (default: build)
    --build-config NAME    Build/CTest configuration (default: Release on Windows)
  --skip-install         Skip the install step (use existing install)
  --skip-build           Skip configure+build (use existing build)
  --label LABEL          Only run tests matching this CTest label
  --no-broker            Skip broker, run only no_broker tests
"""
import argparse
import os
import subprocess
import sys


def detect_mads_prefix():
    """Auto-detect MADS install prefix via 'mads -p'."""
    try:
        result = subprocess.run(
            ["mads", "-p"], capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip()
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass
    return None


def run_cmd(cmd, description, cwd=None):
    """Run a command with error checking."""
    print(f"\n{'='*60}")
    print(f"  {description}")
    print(f"  {' '.join(cmd)}")
    print(f"{'='*60}")
    result = subprocess.run(cmd, cwd=cwd)
    if result.returncode != 0:
        print(f"\nFAILED: {description} (exit code {result.returncode})")
        sys.exit(result.returncode)
    return result


def main():
    parser = argparse.ArgumentParser(
        description="MADS Smoke Test: full pipeline (install → configure → build → test)"
    )
    parser.add_argument(
        "--mads-build-dir", default=None,
        help="MADS build directory (default: ../build)"
    )
    parser.add_argument(
        "--prefix", default=None,
        help="MADS install prefix (default: auto-detect via 'mads -p')"
    )
    parser.add_argument(
        "--build-dir", default="build",
        help="Smoke test build directory (default: build)"
    )
    parser.add_argument(
        "--build-config", default=("Release" if sys.platform == "win32" else None),
        help="Build/CTest configuration (default: Release on Windows)"
    )
    parser.add_argument(
        "--skip-install", action="store_true",
        help="Skip the MADS install step"
    )
    parser.add_argument(
        "--skip-build", action="store_true",
        help="Skip configure + build steps"
    )
    parser.add_argument(
        "--label", default=None,
        help="Only run tests matching this CTest label"
    )
    parser.add_argument(
        "--no-broker", action="store_true",
        help="Skip broker, run only no_broker tests"
    )
    args = parser.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(script_dir)

    mads_build_dir = args.mads_build_dir or os.path.join(repo_root, "build")
    prefix = args.prefix or detect_mads_prefix()
    if not prefix:
        print("Error: cannot detect MADS prefix. Is 'mads' in PATH?")
        print("Use --prefix to specify the install location.")
        sys.exit(1)
    smoke_build_dir = os.path.join(script_dir, args.build_dir)

    # Resolve to absolute paths
    mads_build_dir = os.path.abspath(mads_build_dir)
    prefix = os.path.abspath(prefix)
    smoke_build_dir = os.path.abspath(smoke_build_dir)

    print(f"MADS build dir:       {mads_build_dir}")
    print(f"MADS install prefix:  {prefix}")
    print(f"Smoke test build dir: {smoke_build_dir}")
    print(f"Smoke test source:    {script_dir}")

    # Step 1: Install MADS
    if not args.skip_install:
        if not os.path.isdir(mads_build_dir):
            print(f"\nError: MADS build directory not found: {mads_build_dir}")
            print("Build MADS first, or pass --mads-build-dir")
            return 1
        run_cmd(
            ["cmake", "--install", mads_build_dir, "--prefix", prefix],
            "Installing MADS"
        )

    # Verify install
    broker_exe = os.path.join(prefix, "bin", "mads-broker")
    if sys.platform == "win32":
        broker_exe += ".exe"
    if not os.path.exists(broker_exe):
        print(f"\nError: broker not found at {broker_exe}")
        print("MADS install may have failed. Check the install step.")
        return 1

    # Step 2: Configure smoke test project
    if not args.skip_build:
        run_cmd(
            [
                "cmake",
                "-B", smoke_build_dir,
                "-S", script_dir,
                f"-DCMAKE_PREFIX_PATH={prefix}",
                "-DCMAKE_BUILD_TYPE=Release",
            ],
            "Configuring smoke test project"
        )

        # Step 3: Build smoke test project
        run_cmd(
            [
                "cmake",
                "--build",
                smoke_build_dir,
                *(["--config", args.build_config] if args.build_config else []),
            ],
            "Building smoke test project"
        )

    # Step 4: Run smoke tests
    runner_args = [
        sys.executable,
        os.path.join(script_dir, "run_smoke_tests.py"),
        "--mads-prefix", prefix,
        "--build-dir", args.build_dir,
    ]
    if args.build_config:
        runner_args += ["--build-config", args.build_config]
    if args.label:
        runner_args += ["--label", args.label]
    if args.no_broker:
        runner_args += ["--no-broker"]

    print(f"\n{'='*60}")
    print(f"  Running smoke tests")
    print(f"  {' '.join(runner_args)}")
    print(f"{'='*60}")
    result = subprocess.run(runner_args, cwd=script_dir)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
