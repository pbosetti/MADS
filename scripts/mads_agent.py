"""
Python wrapper for the MADS Agent library using ctypes.

This module provides an Agent class that wraps all the functions
from the agent_c.h header file.
"""

import os, sys
import ctypes
from ctypes import c_void_p, c_char_p, c_int, c_bool, c_double, c_size_t
from enum import IntEnum
import platform
import subprocess
import json

# Determine the library extension based on the OS
system = platform.system()
lib_folder = "lib"
lib_prefix = "lib"
if system == "Darwin":
    lib_ext = ".dylib"
elif system == "Windows":
    lib_ext = ".dll"
    lib_folder = "bin"
    lib_prefix = ""
else:  # Linux and others
    lib_ext = ".so"

# Get the library path dynamically
try:
    if 'MADS_LIB_PATH' in os.environ:
        MADS_LIB_PATH = os.environ['MADS_LIB_PATH']
        sys.stderr.write(f"Loading MADS lib from {MADS_LIB_PATH}\n")
    else:
        mads_prefix = subprocess.check_output(["mads", "-p"], text=True).strip()
        MADS_LIB_PATH = os.path.join(mads_prefix, lib_folder, f"{lib_prefix}MadsCore{lib_ext}")
        sys.stderr.write(f"Loading MADS lib from default {MADS_LIB_PATH}\n")
except (FileNotFoundError, subprocess.CalledProcessError):
    sys.stderr.write("Cannot find MADS shared library. Is MADS installed?\n")
    exit


# Load the library
try:
    lib = ctypes.CDLL(MADS_LIB_PATH)
except OSError as e:
    raise OSError(f"Failed to load MADS library from {MADS_LIB_PATH}: {e}\n")

_TOPIC_BUFFER_SIZE = 256

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

lib.mads_free.argtypes = [c_void_p]
lib.mads_free.restype = None

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

lib.agent_setup_crypto.argtypes = [c_void_p, c_bool]
lib.agent_setup_crypto.restype = c_int

lib.agent_set_client_public_key.argtypes = [c_void_p, c_char_p]
lib.agent_set_client_public_key.restype = c_int

lib.agent_set_client_secret_key.argtypes = [c_void_p, c_char_p]
lib.agent_set_client_secret_key.restype = c_int

lib.agent_set_server_public_key.argtypes = [c_void_p, c_char_p]
lib.agent_set_server_public_key.restype = c_int

# Standard operations
lib.agent_connect.argtypes = [c_void_p, c_int]
lib.agent_connect.restype = c_int

lib.agent_register_event.argtypes = [c_void_p, c_int, c_char_p]
lib.agent_register_event.restype = c_int

lib.agent_disconnect.argtypes = [c_void_p]
lib.agent_disconnect.restype = c_int

lib.agent_set_receive_timeout.argtypes = [c_void_p, c_int]
lib.agent_set_receive_timeout.restype = None

lib.agent_receive_timeout.argtypes = [c_void_p]
lib.agent_receive_timeout.restype = c_int

lib.agent_last_error.argtypes = []
lib.agent_last_error.restype = c_char_p

lib.agent_set_pub_topic.argtypes = [c_void_p, c_char_p]
lib.agent_set_pub_topic.restype = None

lib.agent_set_sub_topics.argtypes = [c_void_p, ctypes.POINTER(c_char_p), c_int]
lib.agent_set_sub_topics.restype = None

lib.agent_pub_topic.argtypes = [c_void_p]
lib.agent_pub_topic.restype = c_char_p

lib.agent_sub_topics.argtypes = [
    c_void_p, ctypes.POINTER(c_char_p), ctypes.POINTER(c_size_t)
]
lib.agent_sub_topics.restype = c_int

lib.agent_topics.argtypes = [c_void_p, c_int]
lib.agent_topics.restype = c_char_p

# Settings functions
lib.agent_get_settings.argtypes = [c_void_p, c_int]
lib.agent_get_settings.restype = c_char_p

lib.agent_set_settings_timeout.argtypes = [c_void_p, c_int]
lib.agent_set_settings_timeout.restype = c_int

lib.agent_settings_timeout.argtypes = [c_void_p]
lib.agent_settings_timeout.restype = c_int

lib.agent_print_settings.argtypes = [c_void_p, c_int]
lib.agent_print_settings.restype = None

lib.agent_settings_uri.argtypes = [c_void_p]
lib.agent_settings_uri.restype = c_char_p

lib.discover_broker_settings.argtypes = [
    c_char_p, ctypes.POINTER(c_char_p), c_size_t
]
lib.discover_broker_settings.restype = c_int

lib.agent_set_high_watermark.argtypes = [c_void_p, c_int]
lib.agent_set_high_watermark.restype = c_int

lib.agent_high_watermark.argtypes = [c_void_p]
lib.agent_high_watermark.restype = c_int

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
            self.disconnect()
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

    def setup_crypto(self, verbose: bool = False) -> int:
        """Setup crypto with optional verbosity."""
        return lib.agent_setup_crypto(self._agent, verbose)
    
    def set_client_public_key(self, public_key: str) -> int:
        """Set the client public key."""
        return lib.agent_set_client_public_key(self._agent, public_key.encode('utf-8'))
    
    def set_client_secret_key(self, secret_key: str) -> int:
        """Set the client secret key."""
        return lib.agent_set_client_secret_key(self._agent, secret_key.encode('utf-8'))
    
    def set_server_public_key(self, public_key: str) -> int:
        """Set the server public key."""
        return lib.agent_set_server_public_key(self._agent, public_key.encode('utf-8'))

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
        if not self._agent:
            return 0
        return lib.agent_disconnect(self._agent)
    
    def set_receive_timeout(self, timeout: int):
        """Set the receive timeout in milliseconds."""
        lib.agent_set_receive_timeout(self._agent, timeout)
    
    def receive_timeout(self):
        """get the receive timeout in milliseconds."""
        return lib.agent_receive_timeout(self._agent)
    
    def last_error(self) -> str:
        """Get the last error message."""
        result = lib.agent_last_error()
        return result.decode('utf-8') if result else None
    
    def set_pub_topic(self, topic: str):
        """Set the publish topic."""
        lib.agent_set_pub_topic(self._agent, topic.encode('utf-8'))
    
    def set_sub_topics(self, topics: list):
        """Set the subscribe topics."""
        topic_ptrs = (c_char_p * len(topics))()
        for i, topic in enumerate(topics):
            topic_ptrs[i] = topic.encode('utf-8')
        lib.agent_set_sub_topics(self._agent, topic_ptrs, len(topics))

    def pub_topic(self) -> str:
        """Get the publish topic."""
        result = lib.agent_pub_topic(self._agent)
        return result.decode('utf-8') if result else None

    def sub_topics(self) -> list:
        """Get the subscribe topics."""
        topics = c_char_p()
        n_topics = c_int(0)
        result = lib.agent_sub_topics(
            self._agent, ctypes.byref(topics), ctypes.byref(n_topics)
        )
        try:
            if result == ctypes.c_size_t(-1).value:
                raise RuntimeError(self.last_error())

            base_ptr = ctypes.cast(topics, c_void_p)
            if not base_ptr.value:
                return []

            return [
                ctypes.string_at(
                    base_ptr.value + i * _TOPIC_BUFFER_SIZE
                ).decode('utf-8')
                for i in range(n_topics.value)
            ]
        finally:
            topics_ptr = ctypes.cast(topics, c_void_p)
            if topics_ptr.value:
                lib.mads_free(topics_ptr)

    def topics(self) -> str:
        """Get all topics as a JSON string."""
        result = lib.agent_topics(self._agent, 0)
        return json.loads(result.decode('utf-8')) if result else None
    
    # Settings methods
    def settings(self) -> str:
        """Retrieve settings from the server."""
        return json.loads(lib.agent_get_settings(self._agent, 0))
    
    def set_settings_timeout(self, timeout_ms: int):
        """Set the settings timeout in milliseconds."""
        return lib.agent_set_settings_timeout(self._agent, timeout_ms)
    
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

    @classmethod
    def discover_settings(cls, room: str = None, buffer_size: int = 0) -> str:
        """
        Discover the broker settings URI advertised in a service room.

        Args:
            room: Optional discovery room. If None or empty, the C wrapper uses
                the compiled MADS default room.
            buffer_size: Optional explicit C buffer size. If 0, the C wrapper
                allocates the buffer internally and this method frees it.

        Returns:
            The discovered settings URI, e.g. tcp://host:port.

        Raises:
            RuntimeError: If discovery fails.
        """
        room_arg = room.encode('utf-8') if room else None
        if buffer_size > 0:
            buffer = ctypes.create_string_buffer(buffer_size)
            url = c_char_p(ctypes.addressof(buffer))
            result = lib.discover_broker_settings(
                room_arg, ctypes.byref(url), buffer_size
            )
            if result != 0:
                raise RuntimeError(cls._last_error())
            return buffer.value.decode('utf-8')

        url = c_char_p()
        result = lib.discover_broker_settings(room_arg, ctypes.byref(url), 0)
        if result != 0:
            raise RuntimeError(cls._last_error())
        try:
            return ctypes.string_at(url).decode('utf-8') if url else None
        finally:
            url_ptr = ctypes.cast(url, c_void_p)
            if url_ptr.value:
                lib.mads_free(url_ptr)

    @staticmethod
    def _last_error() -> str:
        result = lib.agent_last_error()
        return result.decode('utf-8') if result else None
    
    def set_queue_size(self, size: 1000):
        """Set the receive queue size"""
        return lib.agent_set_high_watermark(self._agent, size)
    
    def queue_size(self) -> int:
        """Get the receive queue size"""
        return lib.agent_high_watermark(self._agent)
    
    # Messaging methods
    def publish(self, message: dict, topic: str = "") -> int:
        """Publish a message to a topic."""
        return lib.agent_publish(
            self._agent,
            json.dumps(message).encode('utf-8'),
            topic.encode('utf-8')
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
