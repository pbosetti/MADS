/*
  __  __      _            _       _               _
 |  \/  | ___| |_ __ _  __| | __ _| |_ __ _    ___| | __ _ ___ ___
 | |\/| |/ _ \ __/ _` |/ _` |/ _` | __/ _` |  / __| |/ _` / __/ __|
 | |  | |  __/ || (_| | (_| | (_| | || (_| | | (__| | (_| \__ \__ \
 |_|  |_|\___|\__\__,_|\__,_|\__,_|\__\__,_|  \___|_|\__,_|___/___/

This class is a pure frontend class: it is expected to stream metadata, as
triggers, events, and any other field-related information.

Author(s): Paolo Bosetti
*/

#ifndef METADATA_HPP
#define METADATA_HPP


#include "mads.hpp"
#include "agent.hpp"
#include <vector>
#ifdef USE_GPIOD_2
#include <gpiod.hpp>
#else
#include <gpiod.h>
#endif

namespace Mads {

/**
 * @brief The Metadata class represents metadata for an agent.
 *
 * This class inherits from the Agent class and provides additional
 * functionality for loading settings and managing metadata.
 *
 */
class Metadata : public Agent {
public:
  /**
   * @brief Constructs a Metadata object with the given name and settings path.
   *
   * @param name The name of the metadata.
   * @param settings_path The path to the settings file.
   */
  Metadata(std::string name, std::string settings_path)
      : Agent(name, settings_path) {
    load_settings();
    setup_lines();
  }


#ifdef USE_GPIOD_2

  ~Metadata() {
    _lines.release();
    _chip.reset();
  }

  /**
   * @brief Read the GPIO lines selected in the settings file
   *
   * This function reads the GPIO lines selected in the settings file and stores
   * the values internally. Use the values() function to get the values.
   */
  bool read_lines() {
    bool changed = false;
    for (auto &line : _lines) {
      auto off = line.offset();
      auto value = line.get_value();
      if (value != _values[to_string(off)]) {
        changed = true;
        _values[to_string(off)] = value;
      }
    }
    return changed;
  }


#else // USE_GPIOD_2

  ~Metadata() {
    for (auto &[off, line] : _lines) {
      gpiod_line_release(line);
    }
    gpiod_chip_close(_chip);
  }

  bool read_lines() {
    bool changed = false;
    for (auto &[off, line] : _lines) {
      int val = gpiod_line_get_value(line);
      if (val != _values[to_string(off)]) {
        changed = true;
      }
      _values[to_string(off)] = val;
    }
    return changed;
  }

#endif // USE_GPIOD_2

  /**
   * @brief Returns the values read from the GPIO lines.
   *
   * This function returns the values read from the GPIO lines. The values are
   * stored internally and updated by the read_lines() function.
   *
   * @return nlohmann::json The values read from the GPIO lines as a JSON map.
   */
  nlohmann::json values() { return _values; }

  void info(ostream &out = cout) override {
    Agent::info(out);
    out << "  GPIO chip:        " << style::bold << _chip_path << style::reset
        << endl;
    out << "  GPIO lines:       " << style::bold;
    for (auto &line : _offsets) {
      out << line << " ";
    }
    out << style::reset << endl;
    out << "  GPIOD version:    " << style::bold << gpiod_version_string() 
        << style::reset << endl;
  }

private:
  /**
   * @brief Loads the settings for the metadata.
   *
   * This function is called internally to load the settings from the specified
   * settings file.
   */
  void load_settings() override {
    auto cfg = _config[_name];
    _chip_number = cfg["gpio_chip"].value_or(0);
    _chip_path = cfg["gpio_chip_path"].value_or("/dev/gpiochip0");
    if (cfg["gpio_lines"].type() == toml::node_type::integer) {
      _offsets.push_back(cfg["gpio_lines"].value_or(0));
    } else if (cfg["gpio_lines"].type() == toml::node_type::array) {
      toml::array *a = cfg["gpio_lines"].as_array();
      a->for_each([&](auto &&el) { _offsets.push_back(el.value_or(0)); });
    }
  }


#ifdef USE_GPIOD_2

  void setup_lines() {
    _chip = gpiod::chip(_chip_path);
    _lines = _chip.get_lines(_offsets);
    _lines.request({_name.c_str(), ::gpiod::line_request::DIRECTION_INPUT, 0});
  }

#else  // USE_GPIOD_2

  void setup_lines() {
    int ret = 0;
    _chip = gpiod_chip_open_by_number(_chip_number);
    for (auto &off : _offsets) {
      struct gpiod_line *line = gpiod_chip_get_line(_chip, off);
      ret = gpiod_line_request_input(line, _name.c_str());
      if (ret) {
        cerr << "Error reserving GPIO line " << off << ": " << strerror(errno)
             << endl;
        continue;
      }
      _lines[off] = line;
    }
  }

#endif // USE_GPIOD_2

  int _chip_number = 0;
  string _chip_path = "/dev/gpiochip0";
  vector<unsigned int> _offsets;
  nlohmann::json _values;
#ifdef USE_GPIOD_2
  gpiod::chip _chip;
  gpiod::line_bulk _lines;
#else
  struct gpiod_chip *_chip = NULL;
  map<int, struct gpiod_line *> _lines;
#endif
};

} // namespace Mads

#endif // METADATA_HPP