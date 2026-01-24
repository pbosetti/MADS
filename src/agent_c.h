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

/*
  _     _ _                          
 | |   (_) |__  _ __ __ _ _ __ _   _ 
 | |   | | '_ \| '__/ _` | '__| | | |
 | |___| | |_) | | | (_| | |  | |_| |
 |_____|_|_.__/|_|  \__,_|_|   \__, |
                               |___/ 
*/
const char *mads_version();
const char *mads_default_settings_uri();


/*
   ____ _
  / ___| | __ _ ___ ___
 | |   | |/ _` / __/ __|
 | |___| | (_| \__ \__ \
  \____|_|\__,_|___/___/

Class-like opaque struct
*/
typedef void *agent_t;

typedef enum { none = 0, json = 1, blob, error } message_type_t;

typedef enum {
  marker = 0,
  marker_in,
  marker_out,
  startup,
  shutdown,
  message
} event_type_t;

/*
  _     _  __                      _
 | |   (_)/ _| ___  ___ _   _  ___| | ___
 | |   | | |_ / _ \/ __| | | |/ __| |/ _ \
 | |___| |  _|  __/ (__| |_| | (__| |  __/
 |_____|_|_|  \___|\___|\__, |\___|_|\___|
                        |___/
*/
agent_t agent_create(const char *name, const char *settings_uri);
int agent_init(agent_t agent, bool crypto);
void agent_destroy(agent_t agent);
void agent_set_id(agent_t agent, const char *id);
const char *agent_id(agent_t agent);

/*
   ___                       _   _
  / _ \ _ __   ___ _ __ __ _| |_(_) ___  _ __  ___
 | | | | '_ \ / _ \ '__/ _` | __| |/ _ \| '_ \/ __|
 | |_| | |_) |  __/ | | (_| | |_| | (_) | | | \__ \
  \___/| .__/ \___|_|  \__,_|\__|_|\___/|_| |_|___/
       |_|
*/

// Crypto
void agent_set_key_dir(agent_t agent, const char *key_dir);
void agent_set_client_key_name(agent_t agent, const char *client_key_name);
void agent_set_server_key_name(agent_t agent, const char *server_key_name);
void agent_set_auth_verbose(agent_t agent, bool verbose);

// Std ops
int agent_connect(agent_t agent, int delay_ms);
int agent_register_event(agent_t agent, event_type_t event,
                         const char *info_json);
int agent_disconnect(agent_t agent);
void agent_set_receive_timeout(agent_t agent, int timeout);
const char *agent_last_error(agent_t agent);

// Settings
void agent_get_settings(agent_t agent);
void agent_set_settings_timeout(agent_t agent, int to_ms);
int agent_settings_timeout(agent_t agent);
bool agent_setting_bool(agent_t agent, const char *key);
int agent_setting_int(agent_t agent, const char *key);
double agent_setting_dbl(agent_t agent, const char *key);
const char *agent_setting_str(agent_t agent, const char *key);
void agent_print_settings(agent_t agent);
const char *agent_settings_uri(agent_t agent);

// Messaging
int agent_publish(agent_t agent, const char *topic, const char *message);
message_type_t agent_receive(agent_t agent, bool dont_block);
void agent_last_message(agent_t agent, char **topic, char **message);

#ifdef __cplusplus
}
#endif
#endif // AGENT_C_WRAPPER_HPP