#!/usr/bin/env python3
"""
MADS Smoke Test Runner

Starts the broker, runs CTest, and cleans up.
Cross-platform (macOS, Linux, Windows).

Usage:
  python run_smoke_tests.py [options]

Options:
  --mads-prefix PATH   Path to MADS install prefix (default: auto-detect)
  --config PATH        Path to smoke test INI (default: mads_smoke.ini)
  --build-dir PATH     CTest build directory (default: build)
    --build-config NAME  CTest configuration to run (default: Release on Windows)
  --label LABEL        Only run tests with this CTest label
  --no-broker          Skip broker start (for no_broker tests only)
  --verbose            Verbose CTest output
"""
import argparse
import atexit
import os
import signal
import subprocess
import sys
import time


def find_mads_prefix(build_dir):
    """Try to auto-detect MADS prefix from CMakeCache.txt or 'mads -p'."""
    cache_file = os.path.join(build_dir, "CMakeCache.txt")
    if os.path.exists(cache_file):
        with open(cache_file, "r") as f:
            for line in f:
                if line.startswith("MADS_PREFIX:"):
                    return line.split("=", 1)[1].strip()
                if line.startswith("CMAKE_PREFIX_PATH:"):
                    paths = line.split("=", 1)[1].strip()
                    if paths:
                        return paths.split(";")[0]
    # Fallback: ask the mads executable
    try:
        result = subprocess.run(
            ["mads", "-p"], capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip()
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass
    return None


def wait_for_broker(timeout=5):
    """Wait for broker to be ready (simple sleep-based approach)."""
    if sys.platform == "win32":
        # Windows needs more time for process startup and ZMQ socket binding
        time.sleep(min(timeout, 4))
    else:
        time.sleep(min(timeout, 2))


def main():
    parser = argparse.ArgumentParser(
        description="MADS Smoke Test Runner — starts broker, runs CTest, cleans up"
    )
    parser.add_argument(
        "--mads-prefix", default=None,
        help="Path to MADS install prefix"
    )
    parser.add_argument(
        "--config", default=None,
        help="Path to smoke test INI file"
    )
    parser.add_argument(
        "--build-dir", default="build",
        help="CTest build directory (default: build)"
    )
    parser.add_argument(
        "--build-config", default=("Release" if sys.platform == "win32" else None),
        help="CTest configuration to run (default: Release on Windows)"
    )
    parser.add_argument(
        "--label", default=None,
        help="Only run tests matching this CTest label"
    )
    parser.add_argument(
        "--no-broker", action="store_true",
        help="Skip broker start (run only no_broker tests)"
    )
    parser.add_argument(
        "--verbose", action="store_true",
        help="Verbose CTest output"
    )
    args = parser.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    build_dir = os.path.join(script_dir, args.build_dir)

    if not os.path.isdir(build_dir):
        print(f"Error: build directory '{build_dir}' does not exist.")
        print(f"Run setup_and_run.py first, or configure manually:")
        print(f"  cmake -B {args.build_dir} -S {script_dir} -DCMAKE_PREFIX_PATH=<prefix>")
        return 1

    # Determine config path
    config_path = args.config or os.path.join(script_dir, "mads_smoke.ini")
    if not os.path.exists(config_path):
        print(f"Error: config file '{config_path}' not found")
        return 1

    # Determine MADS prefix for finding broker executable
    mads_prefix = args.mads_prefix or find_mads_prefix(build_dir)
    if not mads_prefix:
        print("Error: cannot determine MADS prefix. Use --mads-prefix.")
        return 1

    broker_exe = os.path.join(mads_prefix, "bin", "mads-broker")
    if sys.platform == "win32":
        broker_exe += ".exe"

    broker_proc = None

    def cleanup():
        nonlocal broker_proc
        if broker_proc and broker_proc.poll() is None:
            print("\nStopping broker...")
            if sys.platform == "win32":
                # Use CTRL_BREAK_EVENT for cleaner shutdown on Windows.
                # The default console control handler calls ExitProcess(), which
                # is much cleaner than TerminateProcess() (used by terminate()).
                # Requires CREATE_NEW_PROCESS_GROUP in the Popen call above.
                try:
                    broker_proc.send_signal(signal.CTRL_BREAK_EVENT)
                except OSError:
                    broker_proc.terminate()
            else:
                broker_proc.terminate()
            try:
                broker_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                broker_proc.kill()
                broker_proc.wait()
            print("Broker stopped.")

    atexit.register(cleanup)

    # Handle SIGINT/SIGTERM gracefully
    def signal_handler(signum, frame):
        cleanup()
        sys.exit(128 + signum)

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # Build CTest command
    ctest_cmd = ["ctest", "--test-dir", build_dir, "--output-on-failure"]
    if args.build_config:
        ctest_cmd += ["-C", args.build_config]

    if args.no_broker:
        # Only run tests that don't need a broker
        ctest_cmd += ["-L", "no_broker"]
        print("Running no-broker tests only...")
        result = subprocess.run(ctest_cmd)
        return result.returncode

    # Start broker
    if not os.path.exists(broker_exe):
        print(f"Error: broker not found at '{broker_exe}'")
        return 1

    print(f"Starting broker: {broker_exe} -s {config_path}")
    # Use DEVNULL for broker output to prevent pipe buffer deadlock on Windows.
    # Windows pipe buffers are only 4KB; if the broker's stdout/stderr fill up
    # (from startup messages, settings logging, etc.) and the parent never reads,
    # the broker's settings thread blocks on cout and can't serve agent requests.
    popen_kwargs = {
        "stdout": subprocess.DEVNULL,
        "stderr": subprocess.DEVNULL,
    }
    if sys.platform == "win32":
        # Create broker in its own process group so we can send CTRL_BREAK_EVENT
        # for clean shutdown (default handler calls ExitProcess) instead of
        # TerminateProcess which is an instant kill with no cleanup.
        popen_kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    broker_proc = subprocess.Popen(
        [broker_exe, "-s", config_path],
        **popen_kwargs,
    )
    wait_for_broker()

    if broker_proc.poll() is not None:
        print(f"Error: broker exited immediately (code {broker_proc.returncode})")
        return 1

    print(f"Broker started (PID {broker_proc.pid})")

    # Add label filter if specified
    if args.label:
        ctest_cmd += ["-L", args.label]

    if args.verbose:
        ctest_cmd += ["-V"]

    # Run CTest
    print(f"Running: {' '.join(ctest_cmd)}")
    result = subprocess.run(ctest_cmd)

    # Cleanup is handled by atexit
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
