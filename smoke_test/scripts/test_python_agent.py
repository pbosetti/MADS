#!/usr/bin/env python3
"""
MADS Smoke Test: Python Agent Wrapper
Validates that the Python ctypes wrapper can load the MADS library,
create an agent, and perform basic operations.

Usage: test_python_agent.py [--settings-uri <uri>]

Requires:
  MADS_LIB_PATH env var pointing to libMadsCore
  PYTHONPATH including the installed python/ directory
"""
import sys
import os
import argparse

def main():
    parser = argparse.ArgumentParser(description="MADS Python wrapper smoke test")
    parser.add_argument("--settings-uri", default="tcp://localhost:19092",
                        help="Broker settings URI")
    args = parser.parse_args()

    # Import the wrapper
    try:
        from mads_agent import Agent, mads_version, mads_default_settings_uri, MessageType
        print(f"OK: Imported mads_agent module")
    except ImportError as e:
        print(f"FAIL: Cannot import mads_agent: {e}")
        return 1

    # Library info
    version = mads_version()
    print(f"OK: MADS version: {version}")

    default_uri = mads_default_settings_uri()
    print(f"OK: Default URI: {default_uri}")

    # Create agent
    try:
        agent = Agent("feedback", args.settings_uri)
        print("OK: Agent created")
    except Exception as e:
        print(f"FAIL: Agent creation: {e}")
        return 1

    # Set/get ID
    agent.set_id("smoke_python")
    agent_id = agent.id()
    if agent_id != "smoke_python":
        print(f"FAIL: ID mismatch: expected 'smoke_python', got '{agent_id}'")
        return 1
    print(f"OK: Agent ID: {agent_id}")

    # Settings timeout
    agent.set_settings_timeout(5000)
    timeout = agent.settings_timeout()
    if timeout != 5000:
        print(f"FAIL: Timeout mismatch: expected 5000, got {timeout}")
        return 1
    print(f"OK: Settings timeout: {timeout}")

    # Initialize (requires broker running)
    result = agent.init(False)
    if result != 0:
        error = agent.last_error()
        print(f"FAIL: Agent init returned {result}: {error}")
        return 1
    print("OK: Agent initialized")

    # Queue size (must be set before connect)
    agent.set_queue_size(500)
    qs = agent.queue_size()
    if qs != 500:
        print(f"FAIL: Queue size mismatch: expected 500, got {qs}")
        return 1
    print(f"OK: Queue size: {qs}")

    # Connect
    result = agent.connect(500)
    if result != 0:
        print(f"FAIL: Agent connect returned {result}")
        return 1
    print("OK: Agent connected")

    # Publish
    for i in range(3):
        msg = {"source": "smoke_python", "seq": i}
        result = agent.publish(msg, "feedback")
        if result != 0:
            print(f"FAIL: Publish returned {result}")
            return 1
    print("OK: Published 3 messages")

    # Non-blocking receive
    agent.set_receive_timeout(500)
    mt = agent.receive(True)  # non-blocking
    print(f"OK: Non-blocking receive returned type {mt}")

    # Disconnect
    agent.disconnect()
    print("OK: Agent disconnected")

    print("PASS: Python wrapper smoke test completed successfully")
    return 0

if __name__ == "__main__":
    sys.exit(main())
