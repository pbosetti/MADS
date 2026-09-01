/*
  ____                       _
 |  _ \ ___  ___ ___  _ __ __| |
 | |_) / _ \/ __/ _ \| '__/ _` |
 |  _ <  __/ (_| (_) | | | (_| |
 |_| \_\___|\___\___/|_|  \__,_|

mads-record: subscribes per sub_topic (MQTT-style filters, P2) and writes
every received message to a bespoke binary bag file (src/bag.hpp) using
Agent::receive_raw_message(), so JSON payloads are never re-parsed/
re-serialized and blob bytes are copied exactly once, matching the wire
byte-for-byte (src/bag.hpp documents the on-disk format).

The administrative "control", "agent_event" and "clocksync" topics are never
recorded, even under a catch-all sub_topic (e.g. sub_topic = [""]), mirroring
mads-federate's own rationale for excluding them from automatic relaying:
capturing (and later replaying) a remote-control command such as "shutdown",
or a clock-sync ping/pong/announce, would be a footgun, not a feature.

Author(s): Paolo Bosetti
*/
#include "../agent_app.hpp"
#include "../bag.hpp"
#include <cxxopts.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

using namespace std;
using namespace Mads;

int main(int argc, char *argv[]) {
  AgentApp recorder(argv[0], SETTINGS_URI);
  // clang-format off
  recorder.options()
    ("o,output", "Bag file to write (required)", cxxopts::value<string>())
    ("no-crc", "Disable per-record CRC32 (enabled by default)")
    ("count", "Stop after N records (0 = unlimited)",
     cxxopts::value<size_t>()->default_value("0"))
    ("x,cross", "Cross-connect sockets (no broker)");
  // clang-format on
  recorder.add_common_options();
  recorder.add_agent_identity_options();

  auto options_parsed = recorder.parse_options(argc, argv);
  if (int rc = AgentApp::handle_standard_exit_options<AgentApp>(
          options_parsed, recorder.raw_options(), argv);
      rc >= 0) {
    return rc;
  }

  if (options_parsed.count("output") == 0) {
    cerr << fg::red << "Error: --output is required" << fg::reset << endl;
    cerr << recorder.raw_options().help() << endl;
    return EXIT_FAILURE;
  }
  string output_path = options_parsed["output"].as<string>();
  bool enable_crc = options_parsed.count("no-crc") == 0;
  size_t max_count = options_parsed["count"].as<size_t>();

  try {
    recorder.init(options_parsed);
  } catch (const std::exception &e) {
    cerr << fg::red << "Error initializing agent: " << e.what() << fg::reset
         << endl;
    return EXIT_FAILURE;
  }
  if (options_parsed.count("cross"))
    recorder.set_cross(true);
  recorder.enable_events();
  recorder.connect();
  recorder.info();

  unique_ptr<BagWriter> bag;
  try {
    bag = make_unique<BagWriter>(output_path, enable_crc);
  } catch (const BagError &e) {
    cerr << fg::red << "Error opening bag file: " << e.what() << fg::reset
         << endl;
    recorder.disconnect();
    return EXIT_FAILURE;
  }

  size_t count = 0;
  cout << fg::green << "Recording to " << output_path << fg::reset << " (CRC "
       << (enable_crc ? "on" : "off") << ")" << endl;
  recorder.loop([&]() -> chrono::milliseconds {
    string topic;
    vector<string> parts;
    if (!recorder.receive_raw_message(topic, parts, true))
      return 0ms;
    // Administrative channels are never recorded: see file header.
    // CLOCKSYNC_TOPIC in particular needs this explicit check --
    // receive_raw_message() bypasses receive()'s own clock-sync
    // interception (Agent::_handle_clocksync_message()), so without it
    // every ping/pong/announce would land in the bag file.
    if (topic == "control" || topic == METADATA_TOPIC ||
        topic == CLOCKSYNC_TOPIC)
      return 0ms;

    int64_t now_ns = chrono::duration_cast<chrono::nanoseconds>(
                         chrono::system_clock::now().time_since_epoch())
                         .count();
    try {
      bag->write(now_ns, topic, parts);
    } catch (const BagError &e) {
      cerr << endl
           << fg::red << "Error writing to bag file: " << e.what()
           << fg::reset << endl;
      recorder.runtime()->stop();
      return 0ms;
    }
    ++count;
    cerr << "\r\x1b[0KRecorded: " << fg::green << count << fg::reset << " ("
         << topic << ") ";
    cerr.flush();
    if (max_count > 0 && count >= max_count)
      recorder.runtime()->stop();
    return 0ms;
  });
  cerr << endl;

  try {
    bag->close();
  } catch (const BagError &e) {
    cerr << fg::red << "Error finalizing bag file: " << e.what() << fg::reset
         << endl;
  }
  cout << fg::green << "Recording stopped (" << count
       << " messages written to " << output_path << ")" << fg::reset << endl;

  recorder.disconnect();
  recorder.restart_if_requested(argv);
  return 0;
}
