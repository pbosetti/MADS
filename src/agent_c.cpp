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
#include "service_discovery.hpp"
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
  ag->shutdown();
  delete ag;
}

int agent_init(agent_t agent, bool crypto) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  if(ag->is_connected()) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error initializing agent: Agent already connected");
    return -1;
  }
  try {
    ag->init(crypto, false);
  } catch (const std::exception &e) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error initializing agent: %s", e.what());
    return -1;
  } catch (...) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error initializing agent: Unexpected");
    return -1;
  }
  return 0;
}

void agent_install_loop_watchdog(agent_t agent) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  ag->install_loop_watchdog();
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

const char *agent_last_error() { return _err_msg; }

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

int agent_set_client_public_key(agent_t agent, const char *key) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  if (!ag->curve_auth()) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error setting client public key: CurveAuth not initialized");
    return -1;
  }
  ag->curve_auth()->get()->set_client_public_key(string(key));
  return 0;
}

int agent_set_client_secret_key(agent_t agent, const char *key) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  if (!ag->curve_auth()) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error setting client secret key: CurveAuth not initialized");
    return -1;
  }
  ag->curve_auth()->get()->set_client_secret_key(string(key));
  return 0;
}

int agent_set_server_public_key(agent_t agent, const char *key) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  if (!ag->curve_auth()) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error setting server public key: CurveAuth not initialized");
    return -1;
  }
  ag->curve_auth()->get()->set_server_public_key(string(key));
  return 0;
}

int agent_setup_crypto(agent_t agent, bool verbose) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  try {
    ag->setup_crypto(verbose ? auth_verbose::on : auth_verbose::off);
  } catch (const std::exception &e) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error setting up CURVE: %s", e.what());
    return -1;
  } catch (...) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error setting up CURVE: Unexpected");
    return -1;
  }
  return 0;
}

// Std ops
int agent_connect(agent_t agent, int delay_ms) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  if(ag->is_connected()) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error connecting agent: Agent already connected");
    return -1;
  }
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
  if(!ag->is_connected()) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error registering event: Agent not connected");
    return -1;
  }
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
  if (!ag->is_connected()) return 0;
  Mads::running = false;
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

int agent_set_settings_timeout(agent_t agent, int to_ms) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  try {
    ag->set_settings_timeout(to_ms);
  } catch (const std::exception &e) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error setting settings timeout: %s", e.what());
    return -1;
  } catch (...) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error setting settings timeout: Unexpected");
    return -1;
  }
  return 0;
}

int agent_settings_timeout(agent_t agent) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  return ag->settings_timeout();
}

int discover_broker_settings(const char *room, char **url, size_t url_size) {
  if (url == nullptr) {
    snprintf(_err_msg, ERR_MSG_SIZE,
             "Error discovering settings: Invalid output buffer pointer");
    return -1;
  }
  if (*url != nullptr && url_size == 0) {
    snprintf(_err_msg, ERR_MSG_SIZE,
             "Error discovering settings: Invalid output buffer size");
    return -1;
  }
  if (*url != nullptr) {
    (*url)[0] = '\0';
  }

  const string room_name =
      room == nullptr || room[0] == '\0' ? string(MADS_SERVICE_ROOM)
                                         : string(room);
  try {
    ServiceDiscovery discovery(MADS_SERVICE_PORT);
    const auto service =
        discovery.discover(room_name, chrono::milliseconds(5000));
    const auto settings = service.ports.find("settings");
    if (settings == service.ports.end()) {
      snprintf(_err_msg, ERR_MSG_SIZE,
               "Error discovering settings: Advertisement has no settings port");
      return -1;
    }
    const string discovered_url =
        "tcp://" + service.ip + ":" + std::to_string(settings->second);
    if (*url == nullptr) {
      url_size = discovered_url.size() + 1;
      *url = static_cast<char *>(malloc(url_size));
      if (*url == nullptr) {
        snprintf(_err_msg, ERR_MSG_SIZE,
                 "Error discovering settings: Unable to allocate output buffer");
        return -1;
      }
    }
    const int written = snprintf(*url, url_size, "%s", discovered_url.c_str());
    if (written < 0 || static_cast<size_t>(written) >= url_size) {
      (*url)[0] = '\0';
      snprintf(_err_msg, ERR_MSG_SIZE,
               "Error discovering settings: Output buffer too small");
      return -1;
    }
  } catch (const std::exception &e) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error discovering settings: %s",
             e.what());
    return -1;
  } catch (...) {
    snprintf(_err_msg, ERR_MSG_SIZE,
             "Error discovering settings: Unexpected");
    return -1;
  }
  return 0;
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

int agent_set_high_watermark(agent_t agent, int n) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  try {
    ag->set_high_watermark(n);
  } catch (const std::exception &e) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error setting high watermark: %s", e.what());
    return -1;
  } catch (...) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error setting high watermark: Unexpected");
    return -1;
  }
  return 0;
}

int agent_high_watermark(agent_t agent) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  return ag->high_watermark();
}

void agent_set_pub_topic(agent_t agent, const char *topic) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  ag->set_pub_topic(string(topic));
}

void agent_set_sub_topics(agent_t agent, const char **topics, int n_topics) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  vector<string> t;
  for (int i = 0; i < n_topics; i++) {
    t.push_back(string(topics[i]));
  }
  ag->set_sub_topic(t);
}

int agent_publish(agent_t agent, const char *message, const char *topic) {
  Agent *ag = reinterpret_cast<Agent *>(agent);
  nlohmann::json j;
  if(!ag->is_connected()) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error publishing message: Agent not connected");
    return -1;
  }
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
  if(!ag->is_connected()) {
    snprintf(_err_msg, ERR_MSG_SIZE, "Error receiving message: Agent not connected");
    return static_cast<message_type_t>(type);
  }
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
