/*
  ____             _
 |  _ \  ___  __ _| | ___ _ __
 | | | |/ _ \/ _` | |/ _ \ '__|
 | |_| |  __/ (_| | |  __/ |
 |____/ \___|\__,_|_|\___|_|

An agent that collects messages from the mads network and sends them to
listening workers, in a round-robin fashion.
*/
#ifndef DEALER_HPP
#define DEALER_HPP

#include "agent.hpp"
#include "mads.hpp"
#include <zmq.hpp>
#include <zmq_addon.hpp>

using json = nlohmann::json;

namespace Mads {

class Dealer : public Agent {
public:
  Dealer(string name, string settings_path) : 
    Agent(name, settings_path), 
    _sender(_context, zmq::socket_type::push) {
    load_settings();
  }

  void connect(chrono::milliseconds delay = chrono::milliseconds(0)) {
    Agent::connect(delay);
    _sender.bind(_dealer_address);
  }

  void info(ostream &out = cout) override {
    Agent::info(out);
    out << "  Dealer Address:   " << style::bold << _dealer_address << style::reset << endl;
  }

  void push(string message) {
    _sender.send(zmq::buffer(message), zmq::send_flags::none);
  }

  void push(json j) {
    const string payload = j.dump();
    _sender.send(zmq::buffer(payload), zmq::send_flags::none);
  }

private: 
  void load_settings() override {
    auto cfg = _config[_name];
    _dealer_address = cfg["dealer_address"].value_or("tcp://*:9093");
  };


private:
  string _dealer_address;
  zmq::socket_t _sender;

};

} // namespace Mads
#endif // DEALER_HPP