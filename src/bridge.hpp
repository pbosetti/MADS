/*
  ____       _     _                   _
 | __ ) _ __(_) __| | __ _  ___    ___| | __ _ ___ ___
 |  _ \| '__| |/ _` |/ _` |/ _ \  / __| |/ _` / __/ __|
 | |_) | |  | | (_| | (_| |  __/ | (__| | (_| \__ \__ \
 |____/|_|  |_|\__,_|\__, |\___|  \___|_|\__,_|___/___/
                     |___/

This class is a pure frontend class: it acts as a bridge between the output of
an external program and the mads framework.

Author(s): Paolo Bosetti
*/

#ifndef BRIDGE_HPP
#define BRIDGE_HPP


#include "mads.hpp"
#include "agent.hpp"
#include <regex>
#include <iostream>

using json = nlohmann::json;

namespace Mads {

/**
 * @brief The Bridge class represents metadata for an agent.
 *
 * This class inherits from the Agent class and provides additional
 * functionality for loading settings and managing metadata.
 */
class Bridge : public Agent {
public:
  /**
   * @brief Constructs a Bridge object with the given name and settings path.
   *
   * @param name The name of the Bridge.
   * @param settings_path The path to the settings file.
   */
  Bridge(std::string name, std::string settings_path)
      : Agent(name, settings_path) {
    load_settings();
  }

  /**
   * @brief Routes any line received via STDIN to the mads network.
   * 
   * @note The input data must be a valid JSON string, one document per line, 
   * i.e. no newline within a single JSON document. Optionally, the input string
   * may start with a topic name, followed by a colon and zero or more spaces,
   *  e.g.: "my_topic: {...}". In this case, this topic name will be used to
   * publish the message, otherwise the default topic will be used. 
   * This allowas a single CLI client to publish to multiple topics.
   * 
   * @note This function is blocking. 
   */
  void route() {
    message message;
    string payload, compressed;
    regex re("^(\\w+):\\s*(\\{.*\\})\\s*$");
    smatch match;
    while (getline(cin, payload)) {
      if (payload == "exit") {
        Mads::running = false;
        break;
      }
      if (regex_search(payload, match, re) && match.size() > 1) {
        _pub_topic = match[1];
        payload = match[2];
      }
      cout << "Topic: " << _pub_topic << ", Payload: " << payload << endl;
      if (_compress) {
        snappy::Compress(payload.data(), payload.length(), &compressed);
        message << _pub_topic << compressed;
      } else {
        message << _pub_topic << payload;
      }
      _publisher.send(message);
    }
  }

private:
  /**
   * @brief Loads the settings for the Bridge.
   *
   * This function is called internally to load the settings from the specified
   * settings file.
   */
  void load_settings() override {
    // NONE
  }

};

} // namespace Mads

#endif // BRIDGE_HPP