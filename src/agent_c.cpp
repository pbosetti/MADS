/*
   ____      _                    _
  / ___|    / \   __ _  ___ _ __ | |_
 | |       / _ \ / _` |/ _ \ '_ \| __|
 | |___   / ___ \ (_| |  __/ | | | |_
  \____| /_/   \_\__, |\___|_| |_|\__|
                 |___/
C wrapper for the core methods of the Agent class
Paolo Bosetti, 2026
*/

#include "agent_c.h"
#include "agent.hpp"
#include "mads.hpp"
#include <iostream>
#define ERR_MSG_SIZE 256

using namespace Mads;

const char *mads_version() {
  static string v = LIB_VERSION;
  return v.c_str();
}

const char *mads_default_settings_uri() {
  static string s = SETTINGS_URI;
  return s.c_str();
}

static char _err_msg[ERR_MSG_SIZE];

agent_t agent_create(const char *name, const char *settings_uri) {
  Agent *agent = new Agent(string(name), string(settings_uri));
  return reinterpret_cast<agent_t *>(agent);
}

void agent_destroy(agent_t agent) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  delete ag;
}

int agent_init(agent_t agent, bool crypto) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  try {
    ag->init(crypto);
  } catch (const std::exception &e) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error initializing agent: %s", e.what());
    return -1;
  } catch (...) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error initializing agent: Unexpected");
    return -1;
  }
  return 0;
}

void agent_set_id(agent_t agent, const char *id) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  ag->set_agent_id(string(id));
}

const char *agent_id(agent_t agent) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  static string id = ag->get_agent_id();
  return id.c_str();
}

void agent_set_receive_timeout(agent_t agent, int timeout) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  ag->set_receive_timeout(timeout);
}

int agent_receive_timeout(agent_t agent) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  return ag->receive_timeout();
}

const char *agent_last_error(agent_t agent) { return _err_msg; }

// Crypto
void agent_set_key_dir(agent_t agent, const char *key_dir) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  ag->set_key_dir(filesystem::path(key_dir));
}

void agent_set_client_key_name(agent_t agent, const char *client_key_name) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  ag->client_key_name = string(client_key_name);
}

void agent_set_server_key_name(agent_t agent, const char *server_key_name) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  ag->server_key_name = string(server_key_name);
}

void agent_set_auth_verbose(agent_t agent, bool verbose) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  if (verbose)
    ag->auth_verbose = auth_verbose::on;
  else
    ag->auth_verbose = auth_verbose::off;
}

// Std ops
int agent_connect(agent_t agent, int delay_ms) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  try {
    ag->connect(chrono::milliseconds(delay_ms));
  } catch (const std::exception &e) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error connecting agent: %s", e.what());
    return -1;
  } catch (...) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error connecting agent: Unexpected");
    return -1;
  }
  return 0;
}

int agent_register_event(agent_t agent, event_type_t event,
                         const char *info_json) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  try {
    if (info_json == nullptr) {
      ag->register_event(static_cast<Mads::event_type>(event));
    } else {
      ag->register_event(static_cast<Mads::event_type>(event),
                         nlohmann::json::parse(string(info_json)));
    }
  } catch (const std::exception &e) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error registering event: %s", e.what());
    return -1;
  } catch (...) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error registering event: Unexpected");
    return -1;
  }
  return 0;
}

int agent_disconnect(agent_t agent) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  try {
    ag->disconnect();
  } catch (const std::exception &e) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error disconnecting agent: %s", e.what());
    return -1;
  } catch (...) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error disconnecting agent: Unexpected");
    return -1;
  }
  return 0;
}

// Settings
const char *agent_get_settings(agent_t agent, int n) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  static string s = ag->get_settings().dump(n);
  return s.c_str();
}

void agent_set_settings_timeout(agent_t agent, int to_ms) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  ag->set_settings_timeout(to_ms);
}

int agent_settings_timeout(agent_t agent) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  return ag->settings_timeout();
}

bool agent_setting_bool(agent_t agent, const char *key) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  return ag->get_settings().value(string(key), false);
}

int agent_setting_int(agent_t agent, const char *key) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  return ag->get_settings().value(string(key), 0);
}

double agent_setting_dbl(agent_t agent, const char *key) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  return ag->get_settings().value(string(key), 0.0);
}

const char *agent_setting_str(agent_t agent, const char *key) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  static string value;
  value = ag->get_settings().value(string(key), "");
  return value.c_str();
}

void agent_print_settings(agent_t agent, int tab) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  std::cout << ag->get_settings().dump(tab) << std::endl;
}

const char *agent_settings_uri(agent_t agent) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  static string s = ag->settings_uri();
  return s.c_str();
}

int agent_publish(agent_t agent, const char *topic, const char *message) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(string(message));
  } catch (...) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error publishing message: Invalid JSON");
    return -1;
  }
  try {
    ag->publish(j, string(topic));
  } catch (const std::exception &e) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error publishing message: %s", e.what());
    return -1;
  } catch (...) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error publishing message: Unexpected");
    return -1;
  }
  return 0;
}

message_type_t agent_receive(agent_t agent, bool dont_block) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  message_type type = message_type::none;
  try {
    type = ag->receive(dont_block);
  } catch (const std::exception &e) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error receiving message: %s", e.what());
    return mads_error;
  } catch (...) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error receiving message: Unexpected");
    return mads_error;
  }
  return static_cast<message_type_t>(type);
}

void agent_last_message(agent_t agent, char **topic, char **message) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  tuple<string, string> last_msg = ag->last_message();
  static string t, m;
  t = get<0>(last_msg);
  m = get<1>(last_msg);
  *topic = const_cast<char *>(t.c_str());
  *message = const_cast<char *>(m.c_str());
}