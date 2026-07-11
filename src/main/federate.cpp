/*
  _____         _                _
 |  ___|__  ___| | ___ _ __ __ _| |_ ___
 | |_ / _ \/ __| |/ _ \ '__/ _` | __/ _ \
 |  _|  __/ (__| |  __/ | | (_| | ||  __/
 |_|  \___|\___|_|\___|_|  \__,_|\__\___|

Relays selected topics between two independent MADS networks (each with its
own broker). Owns two plain Agent instances, one per network, and pumps
messages between them with a non-blocking poll loop (Agent::loop() can only
drive a single lambda/socket, so it isn't used here).

Loop prevention: every relayed message is tagged with this relay's own id in a
"mads_relay_path" JSON array; a message that already carries this relay's id
is not forwarded again, which stops the immediate A<->B ping-pong.

Author(s): Paolo Bosetti
*/
#include "../agent.hpp"
#include "../mads.hpp"
#include <cxxopts.hpp>
#include <csignal>
#include <chrono>
#include <thread>

using namespace std;
using namespace Mads;
using namespace cxxopts;
using json = nlohmann::json;

namespace {

bool path_contains(const json &path, const string &id) {
  if (!path.is_array())
    return false;
  for (auto &el : path) {
    if (el.is_string() && el.get<string>() == id)
      return true;
  }
  return false;
}

// Relays one pending message (if any) from `from` to `to`, tagging it for
// loop prevention. Returns true if a message was actually relayed.
bool relay_one(Agent &from, Agent &to, const string &relay_id) {
  try {
    if (from.receive(/*dont_block=*/true) != message_type::json)
      return false;
    auto [topic, payload] = from.last_json();
    // Administrative channels are never federated automatically: relaying
    // remote-control or agent-lifecycle traffic across networks is rarely
    // intended and can have surprising effects (e.g. a shutdown command
    // meant for one network reaching agents on the other).
    if (topic == "control" || topic == METADATA_TOPIC)
      return false;
    if (!payload.is_object())
      return false;
    json &path = payload["mads_relay_path"];
    if (path_contains(path, relay_id))
      return false; // already went through this relay: stop the ping-pong
    if (!path.is_array())
      path = json::array();
    path.push_back(relay_id);
    to.publish(payload, topic);
    return true;
  } catch (const std::exception &e) {
    cerr << fg::yellow << "Warning: failed to relay message: " << e.what()
         << fg::reset << endl;
    return false;
  }
}

} // namespace

int main(int argc, char *argv[]) {
  string name_a = "federate_a", name_b = "federate_b";
  int poll_interval_us = 1000;
  size_t count_a_to_b = 0, count_b_to_a = 0;

  Options options(argv[0]);
  // clang-format off
  options.add_options()
    ("a,settings-a", "Settings URI/path for network A", value<string>())
    ("b,settings-b", "Settings URI/path for network B", value<string>())
    ("name-a", "Agent/section name on network A (default federate_a)", value<string>())
    ("name-b", "Agent/section name on network B (default federate_b)", value<string>())
    ("id", "Relay id stamped into relayed messages for loop-prevention "
           "(default <name-a>-<name-b>)", value<string>())
    ("poll-interval-us", "Idle poll interval in microseconds (default 1000)", value<int>())
    ("v,version", "Print version")
    ("h,help", "Print usage");
  // clang-format on
  auto options_parsed = options.parse(argc, argv);

  if (options_parsed.count("help")) {
    cout << argv[0] << " ver. " << LIB_VERSION << endl;
    cout << options.help() << endl;
    return 0;
  }
  if (options_parsed.count("version")) {
    cout << LIB_VERSION << endl;
    return 0;
  }
  if (options_parsed.count("settings-a") == 0 ||
      options_parsed.count("settings-b") == 0) {
    cerr << fg::red
         << "Error: --settings-a and --settings-b are both required"
         << fg::reset << endl;
    cerr << options.help() << endl;
    return EXIT_FAILURE;
  }
  string settings_a = options_parsed["settings-a"].as<string>();
  string settings_b = options_parsed["settings-b"].as<string>();
  if (options_parsed.count("name-a"))
    name_a = options_parsed["name-a"].as<string>();
  if (options_parsed.count("name-b"))
    name_b = options_parsed["name-b"].as<string>();
  if (options_parsed.count("poll-interval-us"))
    poll_interval_us = options_parsed["poll-interval-us"].as<int>();
  string relay_id = options_parsed.count("id")
                        ? options_parsed["id"].as<string>()
                        : (name_a + "-" + name_b);

  Agent side_a(name_a, settings_a);
  Agent side_b(name_b, settings_b);

  try {
    // No CURVE/watchdog on the relay's two legs: each side is a plain agent
    // connection, and a per-leg watchdog force-exiting the process would
    // force-exit the whole relay because of a single slow leg.
    side_a.init(/*crypto=*/false, /*install_watchdog=*/false);
    side_b.init(/*crypto=*/false, /*install_watchdog=*/false);
  } catch (const std::exception &e) {
    cerr << fg::red << "Error initializing federate agent: " << e.what()
         << fg::reset << endl;
    return EXIT_FAILURE;
  }

  side_a.connect();
  side_b.connect();

  signal(SIGINT, [](int) { Mads::Runtime::stop_process(); });
  signal(SIGTERM, [](int) { Mads::Runtime::stop_process(); });

  cerr << style::bold << "mads-federate" << style::reset
       << " relaying between two networks:" << endl;
  side_a.info(cerr);
  side_b.info(cerr);
  cerr << "  Relay id:         " << style::bold << relay_id << style::reset
       << endl;
  cerr << fg::green << "Federation started" << fg::reset << endl;

  while (Mads::Runtime::process_running()) {
    bool busy = false;
    if (relay_one(side_a, side_b, relay_id)) {
      count_a_to_b++;
      busy = true;
    }
    if (relay_one(side_b, side_a, relay_id)) {
      count_b_to_a++;
      busy = true;
    }
    if (!busy) {
      this_thread::sleep_for(chrono::microseconds(poll_interval_us));
    } else {
      cerr << "\r\x1b[0KRelayed: " << fg::green << count_a_to_b << fg::reset
           << " A->B, " << fg::green << count_b_to_a << fg::reset << " B->A ";
      cerr.flush();
    }
  }

  cerr << endl << fg::green << "Federation stopped" << fg::reset << endl;
  side_a.disconnect();
  side_b.disconnect();
  return 0;
}
