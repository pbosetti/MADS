/*
  ____  _
 |  _ \| | __ _ _   _
 | |_) | |/ _` | | | |
 |  __/| | (_| | |_| |
 |_|   |_|\__,_|\__, |
                |___/

mads-play: reads a bag file (src/bag.hpp) and republishes its records via
Agent::publish_raw_message(), byte-identical to what was recorded (topic +
parts are sent exactly as read back from the file).

--topics applies an MQTT-style filter (Mads::topic_match, P2) against each
record's stored topic; records that don't match any given pattern are
skipped (default: replay everything). --rate paces replay using the gaps
between the recorded timestamps of the records actually being replayed
(records dropped by --topics don't contribute a gap), scaled by the given
factor; without --rate, records are republished back-to-back as fast as
possible.

--restamp is a narrow, opt-in convenience built on top of the raw
publish_raw_message() primitive (it does not change that primitive, which
stays a pure byte passthrough): for a record whose payload is the legacy,
header-less [topic][snappy(json)] frame -- the shape Agent::publish() emits
whenever the payload ends up Snappy-compressed, unconditionally under
Compression::Snappy or above ~256 bytes under the default Compression::Auto
-- the recorded "timestamp"/"timecode" fields are replaced with fresh ones
before republishing. Every other field, and every other frame shape (an
uncompressed small JSON payload, which carries a self-describing header;
MsgPack; or a blob's meta+bytes parts), is republished byte-for-byte
unchanged. This is intentionally conservative: parsing the self-describing
header format here would mean duplicating Agent::publish()'s internal wire
framing outside of agent.cpp, which is not worth the fragility for a
best-effort convenience flag.

Author(s): Paolo Bosetti
*/
#include "../agent_app.hpp"
#include "../bag.hpp"
#include "../topic_match.hpp"
#include "play_restamp.hpp"
#include <cxxopts.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace std;
using namespace Mads;

int main(int argc, char *argv[]) {
  AgentApp player(argv[0], SETTINGS_URI);
  // clang-format off
  player.options()
    ("i,input", "Bag file to replay (required)", cxxopts::value<string>())
    ("restamp", "Rewrite timestamp/timecode fields of plain JSON frames to now")
    ("topics", "MQTT-style topic filter (repeatable); default replays every topic",
     cxxopts::value<vector<string>>())
    ("rate", "Pace replay using recorded gaps, scaled by this factor "
             "(default: no pacing, replay as fast as possible)",
     cxxopts::value<double>())
    ("x,cross", "Cross-connect sockets (no broker)")
    // Not add_agent_identity_options(): its "-i,agent-id" collides with
    // "-i,input" above, and --input is the more central flag for this tool.
    ("n,name", "Agent/section name", cxxopts::value<string>())
    ("agent-id", "Agent ID (no short flag here: -i is --input)",
     cxxopts::value<string>());
  // clang-format on
  player.add_common_options();

  auto options_parsed = player.parse_options(argc, argv);
  if (int rc = AgentApp::handle_standard_exit_options<AgentApp>(
          options_parsed, player.raw_options(), argv);
      rc >= 0) {
    return rc;
  }

  if (options_parsed.count("input") == 0) {
    cerr << fg::red << "Error: --input is required" << fg::reset << endl;
    cerr << player.raw_options().help() << endl;
    return EXIT_FAILURE;
  }
  string input_path = options_parsed["input"].as<string>();
  bool restamp = options_parsed.count("restamp") != 0;
  vector<string> topic_filters;
  if (options_parsed.count("topics"))
    topic_filters = options_parsed["topics"].as<vector<string>>();
  optional<double> rate;
  if (options_parsed.count("rate")) {
    rate = options_parsed["rate"].as<double>();
    if (*rate <= 0) {
      cerr << fg::red << "Error: --rate must be > 0" << fg::reset << endl;
      return EXIT_FAILURE;
    }
  }

  unique_ptr<BagReader> bag;
  try {
    bag = make_unique<BagReader>(input_path);
  } catch (const BagError &e) {
    cerr << fg::red << "Error opening bag file: " << e.what() << fg::reset
         << endl;
    return EXIT_FAILURE;
  }
  if (bag->truncated()) {
    cerr << fg::yellow << "Warning: bag file is truncated; replaying the "
                          "recovered prefix ("
         << bag->record_count() << " records)" << fg::reset << endl;
  }

  try {
    player.init(options_parsed);
  } catch (const std::exception &e) {
    cerr << fg::red << "Error initializing agent: " << e.what() << fg::reset
         << endl;
    return EXIT_FAILURE;
  }
  if (options_parsed.count("cross"))
    player.set_cross(true);
  player.set_sub_topic({}); // pure publisher: no subscribe socket needed
  player.enable_events();
  player.connect();
  player.info();

  auto matches_filter = [&](const string &topic) {
    if (topic_filters.empty())
      return true;
    for (auto const &pat : topic_filters) {
      if (Mads::topic_match(pat, topic))
        return true;
    }
    return false;
  };

  size_t published = 0, skipped = 0;
  bool have_prev_ts = false;
  int64_t prev_ts = 0;
  cout << fg::green << "Replaying " << input_path << fg::reset << endl;
  bag->rewind();
  while (player.runtime()->running()) {
    auto rec_opt = bag->next();
    if (!rec_opt)
      break;
    BagRecord rec = std::move(*rec_opt);
    if (!matches_filter(rec.topic)) {
      ++skipped;
      continue;
    }
    if (rate && have_prev_ts) {
      int64_t gap_ns = rec.timestamp_ns - prev_ts;
      if (gap_ns > 0) {
        auto sleep_ns = chrono::nanoseconds(
            static_cast<int64_t>(static_cast<double>(gap_ns) / *rate));
        this_thread::sleep_for(sleep_ns);
      }
    }
    have_prev_ts = true;
    prev_ts = rec.timestamp_ns;

    if (restamp)
      Mads::Play::try_restamp(rec.parts);

    player.publish_raw_message(rec.topic, rec.parts);
    ++published;
    cerr << "\r\x1b[0KReplayed: " << fg::green << published << fg::reset
         << " (" << skipped << " skipped by --topics) ";
    cerr.flush();
  }
  cerr << endl;

  cout << fg::green << "Replay complete: " << published
       << " messages published" << fg::reset;
  if (skipped)
    cout << fg::yellow << ", " << skipped << " skipped by --topics"
         << fg::reset;
  cout << endl;

  player.disconnect();
  player.restart_if_requested(argv);
  return 0;
}
