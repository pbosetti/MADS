/*
  _____     _
 | ____|___| |__   ___
 |  _| / __| '_ \ / _ \
 | |__| (__| | | | (_) |
 |_____\___|_| |_|\___/

Zero-config CLI peek at live MADS traffic: subscribes as an ephemeral,
read-only sink Agent and pretty-prints each message as it arrives -- the
MADS analogue of `ros2 topic echo` / `rostopic echo` / `mosquitto_sub`.

Two independent ways to reach a broker, both supported at once:
  - Zero-config (default): no mads.ini section needed. `--broker` points
    straight at the broker's backend (XPUB) endpoint; topics are given
    directly on the command line.
  - `-s/--settings` (for consistency with every other mads-* executable):
    the normal settings-file/broker-query path, reading a `[echo]` section
    (or `[name]` with `-n`/`--name`) exactly like mads-logger etc.

Topic arguments accept the MQTT-style filter grammar from P2
(Mads::topic_match()/literal_prefix(), src/topic_match.hpp): Agent::
set_sub_topic() already wires the two-stage literal-prefix-subscribe +
in-process filter end to end (src/agent.cpp Agent::connect_sub()), so this
file only ever has to pass the raw topic strings through -- no extra
filtering logic needed here.

Author(s): Paolo Bosetti
*/
#include "../agent_app.hpp"
#include "../echo_format.hpp"
#include <cxxopts.hpp>
#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace std;
using namespace Mads;
using json = nlohmann::json;
using namespace std::chrono_literals;

int main(int argc, char *argv[]) {
  AgentApp echo(argv[0], SETTINGS_URI);
  // clang-format off
  echo.options()
    ("topic", "MQTT-style topic filter(s) to subscribe to (default: all "
              "topics); see the wildcard grammar in CONTEXT.md",
     cxxopts::value<vector<string>>())
    ("b,broker", "Sub(scribe) endpoint URI, bypassing --settings for "
                 "zero-config use (default: " BACKEND_URI ")",
     cxxopts::value<string>())
    ("raw", "Show exact blob bytes (base64) instead of a one-line summary")
    ("count", "Exit after N messages", cxxopts::value<size_t>())
    ("jsonl", "Emit one compact JSON line per message, for piping into jq");
  // clang-format on
  // Also brings in -s/--settings, --room, --crypto, -v/-h, ...
  echo.add_common_options();
  echo.add_agent_identity_options();
  echo.raw_options().parse_positional({"topic"});
  echo.raw_options().positional_help("[topic ...]");

  auto options_parsed = echo.parse_options(argc, argv);
  if (int rc = AgentApp::handle_standard_exit_options<AgentApp>(
          options_parsed, echo.raw_options(), argv);
      rc >= 0) {
    return rc;
  }

  try {
    // Zero-config by default: settings_uri "none" builds an in-memory
    // config (sub_topic = [""], default localhost endpoints) with no
    // network round-trip and no mads.ini section required (Agent::init(),
    // src/agent.cpp). Passing -s/--settings overrides this back to the
    // normal broker-query/local-file path other mads-* executables use.
    echo.init(options_parsed, "none");
  } catch (const std::exception &e) {
    cerr << fg::red << "Error initializing agent: " << e.what() << fg::reset
        << endl;
    return EXIT_FAILURE;
  }

  // Read-only sink: never publish, so connect() skips connect_pub()
  // entirely (Agent::connect(), src/agent.cpp) and no frontend endpoint is
  // ever required.
  echo.set_pub_topic("");
  if (options_parsed.count("topic")) {
    // Raw pass-through: Agent::set_sub_topic()/connect_sub() already handle
    // literal and MQTT-wildcard entries transparently (P2).
    echo.set_sub_topic(options_parsed["topic"].as<vector<string>>());
  }
  if (options_parsed.count("broker")) {
    echo.set_sub_endpoint(options_parsed["broker"].as<string>());
  }

  EchoRenderOptions render_opts;
  render_opts.raw = options_parsed.count("raw") != 0;
  render_opts.jsonl = options_parsed.count("jsonl") != 0;
  // jsonl output is meant to be piped into jq/similar: never interleave ANSI
  // color codes with it, regardless of whether stdout is a tty.
  render_opts.color = !render_opts.jsonl;

  optional<size_t> count_limit;
  if (options_parsed.count("count")) {
    count_limit = options_parsed["count"].as<size_t>();
    if (*count_limit == 0) {
      // Nothing to do; avoid connecting at all.
      return EXIT_SUCCESS;
    }
  }

  try {
    echo.connect(0ms);
  } catch (const std::exception &e) {
    cerr << fg::red << "Error connecting agent: " << e.what() << fg::reset
        << endl;
    return EXIT_FAILURE;
  }
  echo.info(cerr);

  size_t received = 0;
  echo.loop([&]() -> chrono::milliseconds {
    const auto mt = echo.receive();
    if (mt == message_type::json) {
      auto [topic, doc] = echo.last_json();
      cout << format_echo_json(topic, doc, doc.dump().size(), render_opts);
      ++received;
    } else if (mt == message_type::blob) {
      auto [topic, meta_text, bytes] = echo.last_blob_view();
      string format = "raw";
      try {
        format = json::parse(string(meta_text)).value("format", "raw");
      } catch (...) {
        // Malformed/absent metadata: fall back to "raw".
      }
      cout << format_echo_blob(string(topic), format, bytes.data(),
                               bytes.size(), render_opts);
      ++received;
    }
    if (count_limit && received >= *count_limit) {
      echo.runtime()->stop();
    }
    return 0ms;
  });

  echo.disconnect();
  return 0;
}
