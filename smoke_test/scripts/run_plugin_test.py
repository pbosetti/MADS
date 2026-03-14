#!/usr/bin/env python3
"""
Run a plugin executable for a short time, then terminate it.
Success = process starts and runs for the specified duration without crashing.
"""
import subprocess
import sys
import time
import signal

def main():
    if len(sys.argv) < 2:
        print("Usage: run_plugin_test.py <command> [args...]", file=sys.stderr)
        return 1

    cmd = sys.argv[1:]
    try:
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
    except FileNotFoundError:
        print(f"FAIL: executable not found: {cmd[0]}", file=sys.stderr)
        return 1

    # Let it run for 3 seconds
    time.sleep(3)

    if proc.poll() is not None:
        # Process already exited — that's a failure (crashed or errored)
        stdout = proc.stdout.read().decode(errors="replace")
        stderr = proc.stderr.read().decode(errors="replace")
        print(f"FAIL: process exited prematurely with code {proc.returncode}",
              file=sys.stderr)
        if stdout:
            print(f"stdout: {stdout}", file=sys.stderr)
        if stderr:
            print(f"stderr: {stderr}", file=sys.stderr)
        return 1

    # Still running — send SIGTERM (or CTRL_BREAK on Windows)
    if sys.platform == "win32":
        proc.send_signal(signal.CTRL_BREAK_EVENT)
    else:
        proc.terminate()

    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()

    print(f"PASS: plugin ran successfully for 3s then terminated")
    return 0

if __name__ == "__main__":
    sys.exit(main())
