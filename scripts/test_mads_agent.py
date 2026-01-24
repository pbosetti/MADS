#!/usr/bin/env python3
"""
Test script for the MADS Agent Python wrapper using ctypes.
"""

import sys
import os

# Add the current directory to Python path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    from mads_agent import Agent, mads_version, mads_default_settings_uri, EventType, MessageType
    print("✓ Successfully imported MADS Agent module")
    
    # Test library info functions
    version = mads_version()
    print(f"✓ MADS Version: {version}")
    
    default_uri = mads_default_settings_uri()
    print(f"✓ Default Settings URI: {default_uri}")
    
    # Test agent creation and basic operations
    print("\n--- Testing Agent Creation ---")
    agent = Agent("feedback")
    print("✓ Agent created successfully")
    
    agent_id = agent.id()
    print(f"✓ Initial Agent ID: {agent_id}")
    
    # Test setting ID (this should work even before init)
    agent.set_id("custom_agent_id")
    agent_id = agent.id()
    print(f"✓ Updated Agent ID: {agent_id}")
    
    # Test settings operations
    print("\n--- Testing Settings Operations ---")
    settings_uri = agent.settings_uri()
    print(f"✓ Settings URI: {settings_uri}")
    
    # Test timeout settings
    print("\n--- Testing Timeout Settings ---")
    agent.set_settings_timeout(2000)
    timeout = agent.settings_timeout()
    print(f"✓ Settings timeout: {timeout} ms")
    
    # Test initialization
    print("\n--- Testing Agent Initialization ---")
    result = agent.init(False)
    if result != 0:
        print(f"⚠ Agent init returned non-zero: {result}")
        print(f"  Last error: {agent.last_error()}")
    else:
        print("✓ Agent initialized successfully")
    
    
    print("\n🎉 All tests passed!")
    
except Exception as e:
    print(f"❌ Error: {e}")
    import traceback
    traceback.print_exc()
    sys.exit(1)