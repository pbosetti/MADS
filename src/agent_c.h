/*
     _                    _
    / \   __ _  ___ _ __ | |_
   / _ \ / _` |/ _ \ '_ \| __|
  / ___ \ (_| |  __/ | | | |_
 /_/   \_\__, |\___|_| |_|\__|
         |___/
   ____   __        __
  / ___|  \ \      / / __ __ _ _ __  _ __   ___ _ __
 | |   ____\ \ /\ / / '__/ _` | '_ \| '_ \ / _ \ '__|
 | |__|_____\ V  V /| | | (_| | |_) | |_) |  __/ |
  \____|     \_/\_/ |_|  \__,_| .__/| .__/ \___|_|
                              |_|   |_|

Wrapper for Agent class in pure C
Author(s): Paolo Bosetti
*/
#ifndef AGENT_C_WRAPPER_HPP
#define AGENT_C_WRAPPER_HPP
#ifdef __cplusplus
extern "C" {
#endif
#include <stdbool.h>
#include <stdlib.h>

#ifdef _WIN32
#define MADS_EXPORT __declspec(dllexport)
#else 
#define MADS_EXPORT
#endif

/*
  _     _ _
 | |   (_) |__  _ __ __ _ _ __ _   _
 | |   | | '_ \| '__/ _` | '__| | | |
 | |___| | |_) | | | (_| | |  | |_| |
 |_____|_|_.__/|_|  \__,_|_|   \__, |
                               |___/
*/
/**
 * @brief Returns the MADS library version string.
 *
 * @return Pointer to an internal null-terminated string.
 */
MADS_EXPORT const char *mads_version();

/**
 * @brief Returns the default settings URI compiled into the library.
 *
 * @return Pointer to an internal null-terminated string.
 */
MADS_EXPORT const char *mads_default_settings_uri();

/*
   ____ _
  / ___| | __ _ ___ ___
 | |   | |/ _` / __/ __|
 | |___| | (_| \__ \__ \
  \____|_|\__,_|___/___/

Class-like opaque struct
*/
typedef void *agent_t;

typedef enum {
  mads_none = 0,
  mads_json = 1,
  mads_blob,
  mads_error
} message_type_t;

typedef enum {
  mads_marker = 0,
  mads_marker_in,
  mads_marker_out,
  mads_startup,
  mads_shutdown,
  mads_message
} event_type_t;

/*
  _     _  __                      _
 | |   (_)/ _| ___  ___ _   _  ___| | ___
 | |   | | |_ / _ \/ __| | | |/ __| |/ _ \
 | |___| |  _|  __/ (__| |_| | (__| |  __/
 |_____|_|_|  \___|\___|\__, |\___|_|\___|
                        |___/
*/
/**
 * @brief Creates a new agent instance.
 *
 * @param name Agent name.
 * @param settings_uri URI or path used to load the agent settings.
 * @return Opaque handle to the created agent. Destroy it with agent_destroy().
 */
MADS_EXPORT agent_t agent_create(const char *name, const char *settings_uri);

/**
 * @brief Initializes an agent before connecting it.
 *
 * @param agent Agent handle.
 * @param crypto Enables cryptographic setup when true.
 * @return `0` on success, `-1` on error.
 *
 * On failure, call agent_last_error() for details.
 */
MADS_EXPORT int agent_init(agent_t agent, bool crypto);

/**
 * @brief Destroys an agent instance.
 *
 * The agent is shut down before being deleted.
 *
 * @param agent Agent handle.
 */
MADS_EXPORT void agent_destroy(agent_t agent);

/**
 * @brief Sets the agent identifier.
 *
 * @param agent Agent handle.
 * @param id Null-terminated identifier string.
 */
MADS_EXPORT void agent_set_id(agent_t agent, const char *id);

/**
 * @brief Returns the current agent identifier.
 *
 * @param agent Agent handle.
 * @return Pointer to an internal null-terminated string.
 */
MADS_EXPORT const char *agent_id(agent_t agent);

/**
 * @brief Installs the internal loop watchdog for the agent.
 *
 * @param agent Agent handle.
 */
MADS_EXPORT void agent_install_loop_watchdog(agent_t agent);

/*
   ___                       _   _
  / _ \ _ __   ___ _ __ __ _| |_(_) ___  _ __  ___
 | | | | '_ \ / _ \ '__/ _` | __| |/ _ \| '_ \/ __|
 | |_| | |_) |  __/ | | (_| | |_| | (_) | | | \__ \
  \___/| .__/ \___|_|  \__,_|\__|_|\___/|_| |_|___/
       |_|
*/

// Crypto
/**
 * @brief Sets the directory containing authentication keys.
 *
 * @param agent Agent handle.
 * @param key_dir Filesystem path to the key directory.
 */
MADS_EXPORT void agent_set_key_dir(agent_t agent, const char *key_dir);

/**
 * @brief Sets the client key filename.
 *
 * @param agent Agent handle.
 * @param client_key_name Client key filename.
 */
MADS_EXPORT void agent_set_client_key_name(agent_t agent, const char *client_key_name);

/**
 * @brief Sets the server key filename.
 *
 * @param agent Agent handle.
 * @param server_key_name Server key filename.
 */
MADS_EXPORT void agent_set_server_key_name(agent_t agent, const char *server_key_name);

/**
 * @brief Enables or disables verbose authentication logging.
 *
 * @param agent Agent handle.
 * @param verbose Set to true to enable verbose output.
 */
MADS_EXPORT void agent_set_auth_verbose(agent_t agent, bool verbose);

// Std ops
/**
 * @brief Connects the agent to the MADS infrastructure.
 *
 * @param agent Agent handle.
 * @param delay_ms Delay in milliseconds applied by the underlying connect call.
 * @return `0` on success, `-1` on error.
 *
 * On failure, call agent_last_error() for details.
 */
MADS_EXPORT int agent_connect(agent_t agent, int delay_ms);

/**
 * @brief Registers an event for the connected agent.
 *
 * @param agent Agent handle.
 * @param event Event type to register.
 * @param info_json Optional JSON payload string. Pass `NULL` for no payload.
 * @return `0` on success, `-1` on error.
 *
 * On failure, call agent_last_error() for details.
 */
MADS_EXPORT int agent_register_event(agent_t agent, event_type_t event,
                         const char *info_json);

/**
 * @brief Disconnects the agent if it is connected.
 *
 * @param agent Agent handle.
 * @return `0` on success, `-1` on error.
 *
 * Returns `0` if the agent is already disconnected.
 */
MADS_EXPORT int agent_disconnect(agent_t agent);

/**
 * @brief Sets the receive timeout in milliseconds.
 *
 * @param agent Agent handle.
 * @param timeout Timeout in milliseconds.
 */
MADS_EXPORT void agent_set_receive_timeout(agent_t agent, int timeout);

/**
 * @brief Returns the current receive timeout.
 *
 * @param agent Agent handle.
 * @return Receive timeout in milliseconds.
 */
MADS_EXPORT int agent_receive_timeout(agent_t agent);

/**
 * @brief Returns the last error message produced by the wrapper.
 *
 * @return Pointer to an internal null-terminated string.
 */
MADS_EXPORT const char *agent_last_error();

// Settings
/**
 * @brief Serializes the current settings to JSON.
 *
 * @param agent Agent handle.
 * @param n Indentation passed to JSON dump formatting.
 * @return Pointer to an internal null-terminated JSON string.
 */
MADS_EXPORT const char *agent_get_settings(agent_t agent, int n);

/**
 * @brief Sets the timeout used when fetching settings.
 *
 * @param agent Agent handle.
 * @param to_ms Timeout in milliseconds.
 */
MADS_EXPORT void agent_set_settings_timeout(agent_t agent, int to_ms);

/**
 * @brief Returns the current settings timeout.
 *
 * @param agent Agent handle.
 * @return Timeout in milliseconds.
 */
MADS_EXPORT int agent_settings_timeout(agent_t agent);

/**
 * @brief Reads a boolean setting.
 *
 * @param agent Agent handle.
 * @param key Setting key.
 * @return Setting value, or `false` if the key is missing.
 */
MADS_EXPORT bool agent_setting_bool(agent_t agent, const char *key);

/**
 * @brief Reads an integer setting.
 *
 * @param agent Agent handle.
 * @param key Setting key.
 * @return Setting value, or `0` if the key is missing.
 */
MADS_EXPORT int agent_setting_int(agent_t agent, const char *key);

/**
 * @brief Reads a floating-point setting.
 *
 * @param agent Agent handle.
 * @param key Setting key.
 * @return Setting value, or `0.0` if the key is missing.
 */
MADS_EXPORT double agent_setting_dbl(agent_t agent, const char *key);

/**
 * @brief Reads a string setting.
 *
 * @param agent Agent handle.
 * @param key Setting key.
 * @return Pointer to an internal null-terminated string. Returns an empty
 * string if the key is missing.
 */
MADS_EXPORT const char *agent_setting_str(agent_t agent, const char *key);

/**
 * @brief Prints the current settings JSON to standard output.
 *
 * @param agent Agent handle.
 * @param tab Indentation passed to JSON dump formatting.
 */
MADS_EXPORT void agent_print_settings(agent_t agent, int tab);

/**
 * @brief Returns the current settings URI.
 *
 * @param agent Agent handle.
 * @return Pointer to an internal null-terminated string.
 */
MADS_EXPORT const char *agent_settings_uri(agent_t agent);

/**
 * @brief Sets the outgoing message high watermark.
 *
 * @param agent Agent handle.
 * @param n High watermark value.
 */
MADS_EXPORT void agent_set_high_watermark(agent_t agent, int n);

/**
 * @brief Returns the outgoing message high watermark.
 *
 * @param agent Agent handle.
 * @return High watermark value.
 */
MADS_EXPORT int agent_high_watermark(agent_t agent);

// Messaging
/**
 * @brief Publishes a JSON message on a topic.
 *
 * @param agent Agent handle.
 * @param message Null-terminated JSON string.
 * @param topic Topic name.
 * @return `0` on success, `-1` on error.
 *
 * On failure, call agent_last_error() for details.
 */
MADS_EXPORT int agent_publish(agent_t agent, const char *message, const char *topic);

/**
 * @brief Receives the next incoming message.
 *
 * @param agent Agent handle.
 * @param dont_block When true, performs a non-blocking receive.
 * @return Received message type, `mads_none` if no message is available, or
 * `mads_error` on failure.
 */
MADS_EXPORT message_type_t agent_receive(agent_t agent, bool dont_block);

/**
 * @brief Returns the topic and payload of the last received message.
 *
 * @param agent Agent handle.
 * @param topic Output pointer receiving the topic string.
 * @param message Output pointer receiving the message payload string.
 *
 * The returned pointers refer to internal storage that is overwritten by the
 * next call to this function.
 */
MADS_EXPORT void agent_last_message(agent_t agent, char **topic, char **message);

#ifdef __cplusplus
}
#endif
#endif // AGENT_C_WRAPPER_HPP
