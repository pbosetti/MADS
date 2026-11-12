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

--loop replays the bag again and again until stopped. On every pass after
the first, the timestamp/timecode of each data record is moved forward by
the pass number times the bag's span (first-to-last record time plus one
mean gap), so they keep increasing across passes (Mads::Play::try_shift(),
which handles both the legacy and the self-describing JSON/MsgPack data
frames, re-encoded in their own format). Blobs and shared-memory references
cannot be rewritten and keep their recorded times. With --restamp the times
are "now" and need no shift.

Author(s): Paolo Bosetti
*/
#include "../agent_app.hpp"
#include "../bag.hpp"
#include "../topic_match.hpp"
#include "../precise_wait.hpp"
#include "play_pacing.hpp"
#include "play_restamp.hpp"
#include <cxxopts.hpp>
#include <chrono>
#include <iomanip>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace std;
using namespace Mads;

int main(int argc, char *argv[]) {
  AgentApp player(argv[0], SETTINGS_URI);
  // clang-format off
  player.options()
    ("i,input", "Bag file to replay (required)", cxxopts::value<string>())
    ("restamp", "Rewrite timestamp/timecode fields of plain JSON frames to now")
    ("l,loop", "Replay the bag over and over, shifting timestamps/timecodes "
               "forward on each pass so they keep increasing")
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
  bool loop = options_parsed.count("loop") != 0;
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
  // The replay loop below is not Agent::loop(), which is what installs these:
  // without them Ctrl-C killed mads-play abruptly (no summary, no clean
  // disconnect) -- and --loop never ends on its own.
  Agent::install_signal_handlers();
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
  // --rate: every record is due at a fixed offset from the start of replay
  // (play_pacing.hpp); one that is already late goes out at once, so late
  // wake-ups (a whole 15.6 ms tick on Windows) don't add up.
  optional<Mads::Play::Pacer> pacer;
  if (rate)
    pacer.emplace(*rate);
  constexpr auto max_lag = chrono::seconds(1);
  chrono::steady_clock::time_point t_start, t_first, last_draw;
  chrono::nanoseconds last_due{0}; // of the last record published
  bool started = false;
  size_t reanchors = 0;
  // --loop: `pass` counts completed passes. A pass lasts `span_ns`: the
  // first-to-last record time of the first pass plus its mean gap, so the
  // seam between passes keeps the bag's cadence (and --rate's pacing).
  size_t pass = 0, pass_records = 0, unshifted = 0;
  int64_t first_ts = 0, last_ts = 0, span_ns = 0;
  auto draw_progress = [&] {
    cerr << "\r\x1b[0KReplayed: " << fg::green << published << fg::reset
         << " (" << skipped << " skipped by --topics) ";
    if (loop)
      cerr << "pass " << pass + 1 << " ";
    cerr.flush();
  };
  cout << fg::green << "Replaying " << input_path << fg::reset << endl;
  bag->rewind();
  while (player.runtime()->running()) {
    auto rec_opt = bag->next();
    if (!rec_opt) {
      if (!loop || pass_records == 0) // nothing replayed: don't spin
        break;
      if (pass == 0) {
        const int64_t length = last_ts - first_ts;
        span_ns = length + (pass_records > 1
                                ? length / static_cast<int64_t>(pass_records - 1)
                                : 0);
        if (span_ns <= 0)
          span_ns = 1'000'000'000; // one instant recorded: step 1 s a pass
      }
      ++pass;
      pass_records = 0;
      if (pacer)
        pacer->seam(span_ns); // the seam is one mean gap long
      bag->rewind();
      continue;
    }
    BagRecord rec = std::move(*rec_opt);
    if (!matches_filter(rec.topic)) {
      if (pass == 0)
        ++skipped;
      continue;
    }
    if (pass == 0) {
      if (pass_records == 0)
        first_ts = rec.timestamp_ns;
      last_ts = rec.timestamp_ns;
    }
    ++pass_records;
    if (pacer) {
      auto due = pacer->offset(rec.timestamp_ns);
      auto now = chrono::steady_clock::now();
      if (!started) {
        t_start = now - due;
        t_first = now;
        started = true;
      }
      auto deadline = t_start + due;
      if (now - deadline > max_lag) {
        // Stalled (suspend, debugger, slow broker): restart the schedule
        // here rather than flood subscribers with the backlog.
        t_start += now - deadline;
        ++reanchors;
      } else if (!Mads::precise_wait_until(deadline, [&] {
                   return player.runtime()->running();
                 })) {
        break; // stopped during a gap: don't publish
      }
      last_due = due;
    }

    if (restamp)
      Mads::Play::try_restamp(rec.parts); // "now": monotonic on every pass
    else if (pass > 0 &&
             !Mads::Play::try_shift(rec.parts,
                                    chrono::nanoseconds(span_ns * pass)))
      ++unshifted;

    player.publish_raw_message(rec.topic, rec.parts);
    ++published;
    // Redrawn at most every 100 ms: per-record console writes are slow.
    if (auto now = chrono::steady_clock::now(); now - last_draw >= 100ms) {
      draw_progress();
      last_draw = now;
    }
  }
  if (published)
    draw_progress();
  cerr << endl;

  cout << fg::green << "Replay complete: " << published
       << " messages published" << fg::reset;
  if (skipped)
    cout << fg::yellow << ", " << skipped << " skipped by --topics"
         << fg::reset;
  if (loop)
    cout << " in " << pass + 1 << " passes";
  cout << endl;
  if (started) {
    chrono::duration<double> took = chrono::steady_clock::now() - t_first;
    cout << fixed << setprecision(2) << "Paced replay took " << took.count()
         << " s (expected " << chrono::duration<double>(last_due).count()
         << " s at --rate " << defaultfloat << *rate << ")";
    if (reanchors)
      cout << fg::yellow << "; " << reanchors
           << " schedule restarts after stalls over 1 s" << fg::reset;
    cout << endl;
  }
  if (unshifted)
    cout << fg::yellow << unshifted
         << " records kept their recorded timestamp/timecode on later passes "
            "(blobs, or no such fields)"
         << fg::reset << endl;

  player.disconnect();
  player.restart_if_requested(argv);
  return 0;
}
