/*
  _____
 |_   _|__  _ __
   | |/ _ \| '_ \
   | | (_) | |_)
   |_|\___/| .__/
           |_|

`htop`-style live table of active MADS topics: subscribes as an ephemeral,
read-only sink Agent (same zero-config-by-default / -s-for-consistency
duality as `mads echo`, see src/main/echo.cpp) and redraws in place, showing
msg/s, bytes/s, last-seen age, and a short sample of the last payload per
topic.

The windowed msg/s and bytes/s aggregation lives in the pure, socket-free
Mads::TopicStats (src/topic_stats.hpp/.cpp) so it is independently unit
tested (tests/test_top_stats.cpp) with a synthetic timestamped message
stream; this file stays thin: CLI parsing, the subscribe/receive loop, and
table rendering.

Author(s): Paolo Bosetti
*/
#include <chrono>
// keypress.hpp's POSIX getch() default argument is the literal `500ms`,
// resolved by ordinary (non-ADL) unqualified lookup at the point the header
// is textually included -- so std::chrono_literals must already be in scope
// here, same as src/watcher.hpp does for its own (accidental) callers.
using namespace std::chrono_literals;

#include "../agent_app.hpp"
#include "../keypress.hpp"
#include "../topic_stats.hpp"
#include <cxxopts.hpp>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace std;
using namespace Mads;
using json = nlohmann::json;

namespace {

// Truncates `s` to at most `width` display columns, appending an ellipsis
// when it had to cut. Byte-length truncation (not full Unicode-aware), which
// is what every other table in this codebase already does (e.g. mads.cpp's
// room listing).
string truncate(const string &s, size_t width) {
  if (s.size() <= width) return s;
  if (width <= 1) return s.substr(0, width);
  return s.substr(0, width - 1) + "…";
}

string human_bytes(double bytes_per_sec) {
  static const char *units[] = {"B", "KB", "MB", "GB"};
  double v = bytes_per_sec;
  size_t u = 0;
  while (v >= 1024.0 && u + 1 < 4) {
    v /= 1024.0;
    ++u;
  }
  ostringstream os;
  os << fixed << setprecision(v >= 10 || u == 0 ? 0 : 1) << v << units[u];
  return os.str();
}

// The transport's own view of the broker connection, which is the one thing
// the topic table below cannot tell you: an empty table means "nobody is
// publishing" when the link is up, and "we lost the broker" when it is not
// (ZMQ_DEVELOPMENT.md §2.1).
void render_link(const LinkState &link,
                chrono::steady_clock::time_point now) {
  cout << "  link: ";
  switch (link.status) {
  case LinkStatus::Up:
    // Drops are worth showing even while up: at any single instant a link
    // that keeps flapping looks exactly like one that never dropped.
    if (link.drops > 0) {
      cout << fg::yellow << "up (" << link.drops << " drop"
          << (link.drops == 1 ? "" : "s") << ")" << fg::reset;
    } else {
      cout << fg::green << "up" << fg::reset;
    }
    break;
  case LinkStatus::Down:
    cout << fg::red << style::bold << "DOWN" << style::reset << fg::red;
    if (link.changed_at) {
      cout << " for " << fixed << setprecision(1)
          << chrono::duration<double>(now - *link.changed_at).count() << "s";
    }
    if (link.last_event == LinkEvent::HandshakeFailedAuth)
      cout << " (broker rejected our key)";
    cout << fg::reset;
    break;
  case LinkStatus::Unknown:
    // No monitor event yet -- still connecting, or a transport libzmq does
    // not report on (inproc://).
    cout << style::dim << "?" << style::reset;
    break;
  }
}

void render(const vector<TopicStat> &stats,
           chrono::steady_clock::time_point now,
           const vector<string> &sub_topic, const LinkState &link) {
  string filter_desc = "(all)";
  if (!sub_topic.empty() && !(sub_topic.size() == 1 && sub_topic[0].empty())) {
    filter_desc.clear();
    for (size_t i = 0; i < sub_topic.size(); ++i) {
      if (i) filter_desc += ", ";
      filter_desc += sub_topic[i];
    }
  }

  // Redraw in place: home cursor, clear from there to end of screen (avoids
  // the full-screen flash of clearing first, then redrawing).
  cout << "\x1b[H\x1b[J";
  cout << style::bold << fg::green << "mads top" << fg::reset << style::reset
       << "  " << stats.size() << " active topic(s)"
       << "  filter: " << style::italic << filter_desc << style::reset;
  render_link(link, now);
  cout << "   (press q to quit)\n\n";

  constexpr size_t kTopicMinW = 5;
  constexpr size_t kTopicMaxW = 40;
  constexpr size_t kSampleW = 40;
  size_t topic_w = kTopicMinW;
  for (auto const &s : stats) topic_w = max(topic_w, s.topic.size());
  topic_w = min(topic_w, kTopicMaxW);

  cout << style::bold << left << setw(static_cast<int>(topic_w)) << "TOPIC"
       << "  " << right << setw(8) << "MSG/S"
       << "  " << setw(10) << "BYTES/S"
       << "  " << setw(8) << "AGE"
       << "  " << left << "SAMPLE" << style::reset << "\n";

  if (stats.empty()) {
    cout << style::italic << "(no messages received yet)" << style::reset
        << "\n";
    // Flush here too, not just at the end: a broker that has gone away
    // produces exactly this empty table, and the link indicator above is
    // then the only thing still changing.
    cout.flush();
    return;
  }

  // Most active topics first (msg/s descending); ties keep the stable
  // alphabetical order TopicStats::snapshot() already returns.
  vector<TopicStat> sorted = stats;
  stable_sort(sorted.begin(), sorted.end(),
             [](TopicStat const &a, TopicStat const &b) {
               return a.messages_per_second > b.messages_per_second;
             });

  for (auto const &s : sorted) {
    const double age_s =
        chrono::duration<double>(now - s.last_seen).count();
    cout << left << setw(static_cast<int>(topic_w)) << truncate(s.topic, topic_w)
        << "  " << fg::cyan << right << fixed << setprecision(1) << setw(8)
        << s.messages_per_second << fg::reset << "  " << fg::yellow
        << setw(10) << human_bytes(s.bytes_per_second) << fg::reset << "  "
        << setw(7) << fixed << setprecision(1) << age_s << "s" << fg::reset
        << "  " << left << style::dim << truncate(s.last_sample, kSampleW)
        << style::reset << "\n";
  }
  cout.flush();
}

} // namespace

int main(int argc, char *argv[]) {
  AgentApp top(argv[0], SETTINGS_URI);
  // clang-format off
  top.options()
    ("topic", "MQTT-style topic filter(s) to subscribe to (default: all "
              "topics)", cxxopts::value<vector<string>>())
    ("b,broker", "Sub(scribe) endpoint URI, bypassing --settings for "
                 "zero-config use (default: " BACKEND_URI ")",
     cxxopts::value<string>())
    ("sample-rate", "Redraw interval in seconds (default: 1.0)",
     cxxopts::value<double>())
    ("window", "Sliding window in seconds used to average msg/s and bytes/s "
               "(default: 5.0)", cxxopts::value<double>());
  // clang-format on
  top.add_common_options();
  top.add_agent_identity_options();
  top.raw_options().parse_positional({"topic"});
  top.raw_options().positional_help("[topic ...]");

  auto options_parsed = top.parse_options(argc, argv);
  if (int rc = AgentApp::handle_standard_exit_options<AgentApp>(
          options_parsed, top.raw_options(), argv);
      rc >= 0) {
    return rc;
  }

  double sample_rate_s = 1.0;
  if (options_parsed.count("sample-rate")) {
    sample_rate_s = options_parsed["sample-rate"].as<double>();
  }
  double window_s = 5.0;
  if (options_parsed.count("window")) {
    window_s = options_parsed["window"].as<double>();
  }
  if (sample_rate_s <= 0.0 || window_s <= 0.0) {
    cerr << fg::red << "Error: --sample-rate and --window must be positive"
        << fg::reset << endl;
    return EXIT_FAILURE;
  }

  try {
    // Same zero-config-by-default duality as mads-echo: "none" needs no
    // mads.ini section unless -s/--settings is given.
    top.init(options_parsed, "none");
  } catch (const std::exception &e) {
    cerr << fg::red << "Error initializing agent: " << e.what() << fg::reset
        << endl;
    return EXIT_FAILURE;
  }

  top.set_pub_topic(""); // read-only sink: never publish
  if (options_parsed.count("topic")) {
    top.set_sub_topic(options_parsed["topic"].as<vector<string>>());
  }
  // Otherwise keep whatever init() resolved: sub_topic = [""] ("subscribe
  // all") in zero-config "none" mode, or the settings-section's own
  // sub_topic when -s/--settings was used.
  if (options_parsed.count("broker")) {
    top.set_sub_endpoint(options_parsed["broker"].as<string>());
  }

  // Bound the blocking receive() wait well below the redraw/keypress cadence
  // so both stay responsive regardless of how quiet the topic set is.
  const int poll_ms =
      max(20, min(200, static_cast<int>(sample_rate_s * 1000.0)));
  top.set_receive_timeout(poll_ms);

  TopicStats stats(chrono::milliseconds(
      static_cast<int64_t>(window_s * 1000.0)));

  try {
    top.connect(0ms);
  } catch (const std::exception &e) {
    cerr << fg::red << "Error connecting agent: " << e.what() << fg::reset
        << endl;
    return EXIT_FAILURE;
  }

  const auto sub_topic = top.sub_topic();
  const auto redraw_every =
      chrono::duration_cast<chrono::steady_clock::duration>(
          chrono::duration<double>(sample_rate_s));
  auto last_draw = chrono::steady_clock::time_point::min();

  top.loop([&]() -> chrono::milliseconds {
    const auto mt = top.receive();
    const auto now = chrono::steady_clock::now();
    if (mt == message_type::json) {
      auto [topic, doc] = top.last_json();
      const string dumped = doc.dump();
      stats.record(topic, dumped.size(), truncate(dumped, 60), now);
    } else if (mt == message_type::blob) {
      auto [topic, meta_text, bytes] = top.last_blob_view();
      (void)meta_text;
      stats.record(string(topic), bytes.size(),
                  "<blob " + to_string(bytes.size()) + " bytes>", now);
    }

    if (last_draw == chrono::steady_clock::time_point::min() ||
        now - last_draw >= redraw_every) {
      render(stats.snapshot(now), now, sub_topic, top.link_state());
      last_draw = now;
    }

    if (char c = getch(0ms); c == 'q' || c == 'Q') {
      top.runtime()->stop();
    }
    return 0ms;
  });

  cout << endl;
  top.disconnect();
  return 0;
}
