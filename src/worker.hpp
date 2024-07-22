/*
 __        __         _             
 \ \      / /__  _ __| | _____ _ __ 
  \ \ /\ / / _ \| '__| |/ / _ \ '__|
   \ V  V / (_) | |  |   <  __/ |   
    \_/\_/ \___/|_|  |_|\_\___|_|   
                                    
An agent that works on PUSH-PULL messages received by a Dealer agent.
*/
#ifndef WORKER_HPP
#define WORKER_HPP

#include "agent.hpp"
#include "mads.hpp"
#include <zmqpp/zmqpp.hpp>

using json = nlohmann::json;

namespace Mads {

class Worker : public Agent {
public:
  Worker(string name, string settings_path) : 
    Agent(name, settings_path), 
    _receiver(_context, socket_type::pull) {
    load_settings();
  }

  void connect(chrono::milliseconds delay = chrono::milliseconds(0)) {
    Agent::connect(delay);
    _receiver.connect(_dealer_address);
  }

  void info(ostream &out = cout) override {
    Agent::info(out);
    out << "  Dealer Address:   " << style::bold << _dealer_address << style::reset << endl;
  }

  json pull() {
    string payload;
    message msg;
    json j;

    _receiver.receive(msg);
    if (msg.parts() == 0) return j;
      
    msg >> payload;
    try {
      j = json::parse(payload);
    } catch (const std::exception &e) {
      cerr << fg::red << "Error parsing JSON: " << e.what() << fg::reset << endl;
      j["error"] = e.what();
    }
    return j;
  }


private: 
  void load_settings() override {
    auto cfg = _config[_name];
    _dealer_address = cfg["dealer_address"].value_or("tcp://localhost:9093");
  };


private:
  string _dealer_address;
  zmqpp::socket _receiver;

};

} // namespace Mads
#endif // WORKER_HPP