"""
Python wrapper for the MADS Agent library using ctypes.

This module provides an Agent class that wraps all the functions
from the agent_c.h header file.
"""

import os, sys
import ctypes
from ctypes import c_void_p, c_char_p, c_int, c_bool, c_double
from enum import IntEnum
import platform
import subprocess
import json

# Determine the library extension based on the OS
system = platform.system()
if system == "Darwin":
    lib_ext = ".dylib"
elif system == "Windows":
    lib_ext = ".dll"
else:  # Linux and others
    lib_ext = ".so"

# Get the library path dynamically
try:
    mads_prefix = subprocess.check_output(["mads", "-p"], text=True).strip()
    LIB_PATH = os.path.join(mads_prefix, "lib", f"libmads-lib{lib_ext}")
except (FileNotFoundError, subprocess.CalledProcessError):
    # Fallback to the hardcoded path if mads command is not available
    sys.stderr.write("Cannot find MADS shared library. Is MADS installed?")
    exit


# Load the library
try:
    lib = ctypes.CDLL(LIB_PATH)
except OSError as e:
    raise OSError(f"Failed to load MADS library from {LIB_PATH}: {e}")

# Define enums
class MessageType(IntEnum):
    NONE = 0
    JSON = 1
    BLOB = 2
    ERROR = 3

class EventType(IntEnum):
    MARKER = 0
    MARKER_IN = 1
    MARKER_OUT = 2
    STARTUP = 3
    SHUTDOWN = 4
    MESSAGE = 5

# Set function argument and return types
# Library functions
lib.mads_version.argtypes = []
lib.mads_version.restype = c_char_p

lib.mads_default_settings_uri.argtypes = []
lib.mads_default_settings_uri.restype = c_char_p

# Agent lifecycle functions
lib.agent_create.argtypes = [c_char_p, c_char_p]
lib.agent_create.restype = c_void_p

lib.agent_init.argtypes = [c_void_p, c_bool]
lib.agent_init.restype = c_int

lib.agent_destroy.argtypes = [c_void_p]
lib.agent_destroy.restype = None

lib.agent_set_id.argtypes = [c_void_p, c_char_p]
lib.agent_set_id.restype = None

lib.agent_id.argtypes = [c_void_p]
lib.agent_id.restype = c_char_p

# Crypto functions
lib.agent_set_key_dir.argtypes = [c_void_p, c_char_p]
lib.agent_set_key_dir.restype = None

lib.agent_set_client_key_name.argtypes = [c_void_p, c_char_p]
lib.agent_set_client_key_name.restype = None

lib.agent_set_server_key_name.argtypes = [c_void_p, c_char_p]
lib.agent_set_server_key_name.restype = None

lib.agent_set_auth_verbose.argtypes = [c_void_p, c_bool]
lib.agent_set_auth_verbose.restype = None

# Standard operations
lib.agent_connect.argtypes = [c_void_p, c_int]
lib.agent_connect.restype = c_int

lib.agent_register_event.argtypes = [c_void_p, c_int, c_char_p]
lib.agent_register_event.restype = c_int

lib.agent_disconnect.argtypes = [c_void_p]
lib.agent_disconnect.restype = c_int

lib.agent_set_receive_timeout.argtypes = [c_void_p, c_int]
lib.agent_set_receive_timeout.restype = None

lib.agent_last_error.argtypes = [c_void_p]
lib.agent_last_error.restype = c_char_p

# Settings functions
lib.agent_get_settings.argtypes = [c_void_p, c_int]
lib.agent_get_settings.restype = c_char_p

lib.agent_set_settings_timeout.argtypes = [c_void_p, c_int]
lib.agent_set_settings_timeout.restype = None

lib.agent_settings_timeout.argtypes = [c_void_p]
lib.agent_settings_timeout.restype = c_int

lib.agent_print_settings.argtypes = [c_void_p, c_int]
lib.agent_print_settings.restype = None

lib.agent_settings_uri.argtypes = [c_void_p]
lib.agent_settings_uri.restype = c_char_p

# Messaging functions
lib.agent_publish.argtypes = [c_void_p, c_char_p, c_char_p]
lib.agent_publish.restype = c_int

lib.agent_receive.argtypes = [c_void_p, c_bool]
lib.agent_receive.restype = c_int

lib.agent_last_message.argtypes = [c_void_p, ctypes.POINTER(ctypes.POINTER(c_char_p)), ctypes.POINTER(ctypes.POINTER(c_char_p))]
lib.agent_last_message.restype = None


class Agent:
    """
    Python wrapper class for the MADS Agent.
    
    This class provides methods that correspond to all the functions
    in the agent_c.h header file.
    """
    
    def __init__(self, name: str, settings_uri: str = None):
        """
        Create a new Agent instance.
        
        Args:
            name: The name of the agent
            settings_uri: Optional settings URI, defaults to mads_default_settings_uri()
        """
        if settings_uri is None:
            settings_uri = mads_default_settings_uri()
        
        self._agent = lib.agent_create(
            name.encode('utf-8'),
            settings_uri.encode('utf-8')
        )
        if not self._agent:
            raise RuntimeError("Failed to create agent")
    
    def init(self, crypto: bool = False) -> int:
        """Initialize the agent."""
        return lib.agent_init(self._agent, crypto)
    
    def destroy(self):
        """Destroy the agent and free resources."""
        if self._agent:
            lib.agent_destroy(self._agent)
            self._agent = None
    
    def set_id(self, agent_id: str):
        """Set the agent ID."""
        lib.agent_set_id(self._agent, agent_id.encode('utf-8'))
    
    def id(self) -> str:
        """Get the agent ID."""
        result = lib.agent_id(self._agent)
        return result.decode('utf-8') if result else None
    
    # Crypto methods
    def set_key_dir(self, key_dir: str):
        """Set the key directory for crypto operations."""
        lib.agent_set_key_dir(self._agent, key_dir.encode('utf-8'))
    
    def set_client_key_name(self, client_key_name: str):
        """Set the client key name."""
        lib.agent_set_client_key_name(self._agent, client_key_name.encode('utf-8'))
    
    def set_server_key_name(self, server_key_name: str):
        """Set the server key name."""
        lib.agent_set_server_key_name(self._agent, server_key_name.encode('utf-8'))
    
    def set_auth_verbose(self, verbose: bool = True):
        """Set authentication verbosity."""
        lib.agent_set_auth_verbose(self._agent, verbose)
    
    # Standard operations
    def connect(self, delay_ms: int = 0) -> int:
        """Connect to the server."""
        return lib.agent_connect(self._agent, delay_ms)
    
    def register_event(self, event_type: EventType, info_json: str = None) -> int:
        """Register an event."""
        info = info_json.encode('utf-8') if info_json else None
        return lib.agent_register_event(self._agent, int(event_type), info)
    
    def disconnect(self) -> int:
        """Disconnect from the server."""
        return lib.agent_disconnect(self._agent)
    
    def set_receive_timeout(self, timeout: int):
        """Set the receive timeout in milliseconds."""
        lib.agent_set_receive_timeout(self._agent, timeout)
    
    def last_error(self) -> str:
        """Get the last error message."""
        result = lib.agent_last_error(self._agent)
        return result.decode('utf-8') if result else None
    
    # Settings methods
    def settings(self) -> str:
        """Retrieve settings from the server."""
        return json.loads(lib.agent_get_settings(self._agent, 0))
    
    def set_settings_timeout(self, timeout_ms: int):
        """Set the settings timeout in milliseconds."""
        lib.agent_set_settings_timeout(self._agent, timeout_ms)
    
    def settings_timeout(self) -> int:
        """Get the settings timeout."""
        return lib.agent_settings_timeout(self._agent)
    
    def print_settings(self, tab: int = 0):
        """Print all settings."""
        lib.agent_print_settings(self._agent, tab)
    
    def settings_uri(self) -> str:
        """Get the settings URI."""
        result = lib.agent_settings_uri(self._agent)
        return result.decode('utf-8') if result else None
    
    # Messaging methods
    def publish(self, topic: str, message: dict) -> int:
        """Publish a message to a topic."""
        return lib.agent_publish(
            self._agent,
            topic.encode('utf-8'),
            json.dumps(message).encode('utf-8')
        )
    
    def receive(self, dont_block: bool = False) -> MessageType:
        """Receive a message."""
        result = lib.agent_receive(self._agent, dont_block)
        return MessageType(result)
    
    def last_message(self) -> tuple:
        """Get the last received topic and message."""
        topic_ptr = ctypes.POINTER(c_char_p)()
        message_ptr = ctypes.POINTER(c_char_p)()
        
        lib.agent_last_message(self._agent, ctypes.byref(topic_ptr), ctypes.byref(message_ptr))
        
        topic = ctypes.string_at(topic_ptr).decode('utf-8') if topic_ptr else None
        message = ctypes.string_at(message_ptr).decode('utf-8') if message_ptr else None
        
        return topic, json.loads(message)
    
    def __del__(self):
        """Destructor to ensure proper cleanup."""
        self.disconnect()
        self.destroy()


# Module-level functions for library info
def mads_version() -> str:
    """Get the MADS library version."""
    result = lib.mads_version()
    return result.decode('utf-8') if result else None


def mads_default_settings_uri() -> str:
    """Get the default settings URI."""
    result = lib.mads_default_settings_uri()
    return result.decode('utf-8') if result else None

