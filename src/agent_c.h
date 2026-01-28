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
MADS_EXPORT const char *mads_version();
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
MADS_EXPORT agent_t agent_create(const char *name, const char *settings_uri);
MADS_EXPORT int agent_init(agent_t agent, bool crypto);
MADS_EXPORT void agent_destroy(agent_t agent);
MADS_EXPORT void agent_set_id(agent_t agent, const char *id);
MADS_EXPORT const char *agent_id(agent_t agent);

/*
   ___                       _   _
  / _ \ _ __   ___ _ __ __ _| |_(_) ___  _ __  ___
 | | | | '_ \ / _ \ '__/ _` | __| |/ _ \| '_ \/ __|
 | |_| | |_) |  __/ | | (_| | |_| | (_) | | | \__ \
  \___/| .__/ \___|_|  \__,_|\__|_|\___/|_| |_|___/
       |_|
*/

// Crypto
MADS_EXPORT void agent_set_key_dir(agent_t agent, const char *key_dir);
MADS_EXPORT void agent_set_client_key_name(agent_t agent, const char *client_key_name);
MADS_EXPORT void agent_set_server_key_name(agent_t agent, const char *server_key_name);
MADS_EXPORT void agent_set_auth_verbose(agent_t agent, bool verbose);

// Std ops
MADS_EXPORT int agent_connect(agent_t agent, int delay_ms);
MADS_EXPORT int agent_register_event(agent_t agent, event_type_t event,
                         const char *info_json);
MADS_EXPORT int agent_disconnect(agent_t agent);
MADS_EXPORT void agent_set_receive_timeout(agent_t agent, int timeout);
MADS_EXPORT int agent_receive_timeout(agent_t agent);
MADS_EXPORT const char *agent_last_error(agent_t agent);

// Settings
MADS_EXPORT const char *agent_get_settings(agent_t agent, int n);
MADS_EXPORT void agent_set_settings_timeout(agent_t agent, int to_ms);
MADS_EXPORT int agent_settings_timeout(agent_t agent);
MADS_EXPORT bool agent_setting_bool(agent_t agent, const char *key);
MADS_EXPORT int agent_setting_int(agent_t agent, const char *key);
MADS_EXPORT double agent_setting_dbl(agent_t agent, const char *key);
MADS_EXPORT const char *agent_setting_str(agent_t agent, const char *key);
MADS_EXPORT void agent_print_settings(agent_t agent, int tab);
MADS_EXPORT const char *agent_settings_uri(agent_t agent);

// Messaging
MADS_EXPORT int agent_publish(agent_t agent, const char *topic, const char *message);
MADS_EXPORT message_type_t agent_receive(agent_t agent, bool dont_block);
MADS_EXPORT void agent_last_message(agent_t agent, char **topic, char **message);

#ifdef __cplusplus
}
#endif
#endif // AGENT_C_WRAPPER_HPP