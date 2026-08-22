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
#include <zmq.hpp>
#include <zmq_addon.hpp>

using json = nlohmann::json;

namespace Mads {

class Worker : public Agent {
public:
  Worker(string name, string settings_path) : 
    Agent(name, settings_path), 
    _receiver(_context, zmq::socket_type::pull) {
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

  /**
   * @brief Waits up to `timeout` for one work item from the dealer, and
   * returns an empty object if none arrives.
   *
   * The PULL socket is polled rather than recv()'d blindly
   * (ZMQ_DEVELOPMENT.md §4.1). It used to be an unbounded blocking recv(),
   * so an idle worker -- one whose dealer simply had no work -- sat inside
   * pull() indefinitely and never got back to its subscriber socket: a
   * fleet-wide `control` shutdown/restart from the broker never arrived,
   * and neither did ordinary subscribed traffic. (A local Ctrl-C did get
   * through, but only because the signal interrupted the blocking recv()
   * with EINTR, which surfaced as an exception out of the agent's loop.)
   *
   * Returning empty on a timeout is the same contract receive() already
   * has, and callers already had to handle an empty result -- a zero-part
   * message produced one.
   *
   * Polled here on the application thread rather than moved into Agent's
   * _io_thread: that thread exists to take a socket *off* the application
   * thread when the application cannot poll it (LKV delivery, threaded
   * remote control). A worker's whole job is to block on its own work
   * queue, so draining it elsewhere would only add a queue and a handover
   * in front of the queue ZMQ already provides.
   *
   * @param timeout how long to wait; 0 polls without blocking.
   */
  json pull(chrono::milliseconds timeout) {
    json j;
    zmq::pollitem_t item;
    item = zmq::pollitem_t{_receiver.handle(), 0, ZMQ_POLLIN, 0};
    try {
      zmq::poll(&item, 1, timeout);
    } catch (const zmq::error_t &) {
      return j; // context terminating under us: no work, and none coming
    }
    if (!(item.revents & ZMQ_POLLIN)) return j;

    zmq::multipart_t msg;
    if (!msg.recv(_receiver, ZMQ_DONTWAIT)) return j;
    if (msg.size() == 0) return j;

    try {
      j = json::parse(msg.at(0).to_string());
    } catch (const std::exception &e) {
      cerr << fg::red << "Error parsing JSON: " << e.what() << fg::reset << endl;
      j["error"] = e.what();
    }
    return j;
  }

  /// As pull(timeout), bounded by the agent's own receive timeout so the
  /// PULL and SUB sockets stay on the same cadence.
  json pull() { return pull(chrono::milliseconds(receive_timeout())); }


private: 
  void load_settings() override {
    auto cfg = _config[_name];
    _dealer_address = cfg["dealer_address"].value_or("tcp://localhost:9093");
  };


private:
  string _dealer_address;
  zmq::socket_t _receiver;

};

} // namespace Mads
#endif // WORKER_HPP