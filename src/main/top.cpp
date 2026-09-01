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
#include "../clock_offset.hpp"
#include "../keypress.hpp"
#include "../topic_stats.hpp"
#include <cxxopts.hpp>
#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <map>
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

string human_ms(double ms) {
  ostringstream os;
  os << (ms >= 0 ? "+" : "") << fixed << setprecision(1) << ms << "ms";
  return os.str();
}

// Parses Agent::publish()'s own get_ISODate_time() output --
// "YYYY-MM-DDThh:mm:ss.mmm+hhmm" (always ms, always a numeric +/-hhmm
// offset, never a trailing "Z") -- back to microseconds since the Unix
// epoch. Best-effort: only ever fed this agent's own wire format, so a
// caller-supplied non-standard `timestamp` (or its absence) simply skips
// that message's contribution to the passive skew display below rather
// than guessing.
bool parse_mads_iso8601_us(const string &s, int64_t &out_us) {
  int Y, Mo, D, h, mi, se, ms = 0, oh = 0, om = 0;
  char sign = '+';
  if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d.%d%c%2d%2d", &Y, &Mo, &D, &h, &mi,
             &se, &ms, &sign, &oh, &om) != 10) {
    return false;
  }
  using namespace chrono;
  const sys_days days = year{Y} / Mo / D;
  const auto tz = minutes{(sign == '-' ? -1 : 1) * (oh * 60 + om)};
  const auto tp = days + hours{h} + minutes{mi} + seconds{se} +
                  milliseconds{ms} - tz;
  out_us = duration_cast<microseconds>(tp.time_since_epoch()).count();
  return true;
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
    // last_handshake rather than last_event, which by now is almost always
    // the DISCONNECTED libzmq fires right after a rejection.
    if (link.last_handshake == LinkEvent::HandshakeFailedAuth)
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

// Passive skew (no protocol, works against any existing MADS deployment):
// `timestamp`/`hostname` are already stamped on every published message,
// so this is fed from the same receive() loop that already drives
// TopicStats, at no extra network cost. The windowed MINIMUM is a tight
// upper bound on (offset + one-way delay) -- never a clean offset on its
// own, hence "skew" throughout, never "offset" (Mads::HostSkewStats).
void render_skew(const vector<HostSkewStat> &skew) {
  if (skew.empty())
    return;
  cout << "\n" << style::bold << "  Host clock skew"
       << style::reset << style::dim
       << " (passive, vs. this machine's local time -- see 'mads top "
          "--probe' for a real measurement)"
       << style::reset << "\n";
  vector<HostSkewStat> sorted = skew;
  stable_sort(sorted.begin(), sorted.end(),
             [](HostSkewStat const &a, HostSkewStat const &b) {
               return a.host < b.host;
             });
  for (auto const &s : sorted) {
    cout << "    " << left << setw(20) << truncate(s.host, 20) << "  ";
    if (s.has_value) {
      cout << fg::magenta << setw(10) << human_ms(s.min_skew_us / 1000.0)
          << fg::reset;
    } else {
      cout << style::dim << setw(10) << "?" << style::reset;
    }
    cout << "\n";
  }
}

string clock_source_label(ClockSource s) {
  switch (s) {
  case ClockSource::Broker:
    return "broker";
  case ClockSource::Peer:
    return "peer";
  default:
    return "none";
  }
}

// `--probe` mode: an active fleet-wide view (Agent::broadcast_clock_probe(),
// clock_offset.hpp source B), grouped by clock domain -- every member of a
// domain showing the same OFFSET/REF is the at-a-glance confirmation that
// they actually agree (Mads::ClockConsensus). Replaces the topic-activity
// table rather than sharing the screen with it: the two have unrelated
// refresh sources (one is a passive per-message tally, the other an active
// blocking probe) and cramming both together would make neither legible.
void render_probe(const string &my_domain, const ClockOffsetResult &mine,
                  const vector<ClockPeerObservation> &peers,
                  chrono::steady_clock::time_point now,
                  const LinkState &link) {
  cout << "\x1b[H\x1b[J";
  cout << style::bold << fg::green << "mads top --probe" << fg::reset
       << style::reset << "  " << peers.size() << " responder(s)";
  render_link(link, now);
  cout << "   (press q to quit)\n\n";

  cout << "  " << style::bold << "(this agent)" << style::reset
       << "  domain " << truncate(my_domain, 12);
  if (mine.valid) {
    cout << "  offset " << fg::magenta << human_ms(mine.offset_us / 1000.0)
        << fg::reset << "  (" << clock_source_label(mine.source) << ", "
        << static_cast<int>(mine.hops) << " hops, ref " << mine.clock_ref()
        << ")";
  } else {
    cout << style::dim << "  not yet measured" << style::reset;
  }
  cout << "\n\n";

  if (peers.empty()) {
    cout << style::italic
        << "  (no responders heard within the probe window)"
        << style::reset << "\n";
    cout.flush();
    return;
  }

  // Group by the responder's own clock domain, in first-seen order (stable
  // across redraws since map iteration order follows domain id, not
  // arrival), each preceded by that domain's own header line.
  map<string, vector<const ClockPeerObservation *>> by_domain;
  for (auto const &p : peers) {
    by_domain[p.responder_domain].push_back(&p);
  }

  for (auto const &[domain, members] : by_domain) {
    cout << "  " << style::bold << "DOMAIN " << truncate(domain, 12)
        << style::reset << "\n";
    for (auto const *p : members) {
      const auto theta = estimate(p->sample);
      cout << "    " << left << setw(16) << truncate(p->responder_name, 16)
          << "  " << setw(14) << truncate(p->responder_hostname, 14) << "  ";
      if (p->responder_adopted.valid) {
        cout << fg::magenta << setw(10)
            << human_ms(p->responder_adopted.offset_us / 1000.0)
            << fg::reset << "  " << setw(6)
            << clock_source_label(p->responder_adopted.source) << "  "
            << static_cast<int>(p->responder_adopted.hops) << " hops";
      } else {
        cout << style::dim << setw(10) << "?" << style::reset << "  "
            << setw(6) << "none" << "  " << "-";
      }
      cout << "  delay " << fixed << setprecision(1)
          << (theta.delay_us / 1000.0) << "ms\n";
    }
  }
  cout.flush();
}

// The transport's own view of the broker connection, which is the one thing
// the topic table below cannot tell you: an empty table means "nobody is
// publishing" when the link is up, and "we lost the broker" when it is not
// (ZMQ_DEVELOPMENT.md §2.1).
void render(const vector<TopicStat> &stats,
           const vector<HostSkewStat> &skew,
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
    render_skew(skew);
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
  render_skew(skew);
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
               "(default: 5.0)", cxxopts::value<double>())
    ("probe", "Active clock-offset probe: broadcasts a clock-sync ping every "
              "--sample-rate seconds and shows every responder's offset, "
              "grouped by clock domain, instead of the topic-activity table");
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

  const bool probe_mode = options_parsed.count("probe") > 0;
  // Read-only sink: never publish -- except under --probe, which needs a
  // connected publisher to send its own clock-sync pings.
  if (!probe_mode) {
    top.set_pub_topic("");
  }
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

  if (probe_mode) {
    // An active blocking probe (Agent::broadcast_clock_probe()) replaces
    // the passive per-message loop entirely: it drives its own receive()
    // calls for the probe window, so top.receive() is never called
    // directly here. --sample-rate doubles as both the probe window and
    // the redraw interval.
    top.loop([&]() -> chrono::milliseconds {
      auto peers = top.broadcast_clock_probe(
          chrono::duration_cast<chrono::milliseconds>(redraw_every));
      render_probe(top.clock_domain(), top.clock_offset(), peers,
                  chrono::steady_clock::now(), top.link_state());
      if (char c = getch(0ms); c == 'q' || c == 'Q') {
        top.runtime()->stop();
      }
      return 0ms;
    });
  } else {
    HostSkewStats skew(
        chrono::milliseconds(static_cast<int64_t>(window_s * 1000.0)));
    auto last_draw = chrono::steady_clock::time_point::min();

    top.loop([&]() -> chrono::milliseconds {
      const auto mt = top.receive();
      const auto now = chrono::steady_clock::now();
      if (mt == message_type::json) {
        auto [topic, doc] = top.last_json();
        const string dumped = doc.dump();
        stats.record(topic, dumped.size(), truncate(dumped, 60), now);
        // Passive skew (§"Surfacing: mads top"): no protocol, just reads
        // the timestamp/hostname every published message already carries.
        const string host = doc.value("hostname", string());
        int64_t payload_us = 0;
        if (!host.empty() && doc.contains("timestamp") &&
            doc["timestamp"].is_object() &&
            doc["timestamp"].contains("$date") &&
            doc["timestamp"]["$date"].is_string() &&
            parse_mads_iso8601_us(
                doc["timestamp"]["$date"].get<string>(), payload_us)) {
          const int64_t local_us =
              chrono::duration_cast<chrono::microseconds>(
                  chrono::system_clock::now().time_since_epoch())
                  .count();
          skew.record(host, local_us - payload_us, now);
        }
      } else if (mt == message_type::blob) {
        auto [topic, meta_text, bytes] = top.last_blob_view();
        (void)meta_text;
        stats.record(string(topic), bytes.size(),
                    "<blob " + to_string(bytes.size()) + " bytes>", now);
      }

      if (last_draw == chrono::steady_clock::time_point::min() ||
          now - last_draw >= redraw_every) {
        render(stats.snapshot(now), skew.snapshot(now), now, sub_topic,
              top.link_state());
        last_draw = now;
      }

      if (char c = getch(0ms); c == 'q' || c == 'Q') {
        top.runtime()->stop();
      }
      return 0ms;
    });
  }

  cout << endl;
  top.disconnect();
  return 0;
}
