// Unit tests for src/topology_graph.hpp/.cpp, the pure DOT-graph builder
// backing `mads doctor --graph` (src/main/doctor.cpp). Pure string-in/
// string-out: no sockets, no files, no live broker -- every case here is a
// hand-built std::map<std::string, Mads::AgentTopicInfo>.
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

#include "topology_graph.hpp"

using Mads::AgentTopicInfo;
using Mads::GraphOptions;
using Mads::topology_graph;

namespace {

using AgentMap = std::map<std::string, AgentTopicInfo>;

// The invariant header every graph starts with, so the exact-string cases
// below only have to spell out what is specific to them.
const std::string kHeader = "digraph mads {\n"
                            "  rankdir=LR;\n"
                            "  node [shape=record, fontname=\"monospace\", "
                            "fontsize=8];\n"
                            "  edge [fontname=\"monospace\", fontsize=8];\n";

// An agent that publishes `pub` (declared, not defaulted) and subscribes to
// `subs`.
AgentTopicInfo agent(std::string pub, std::vector<std::string> subs = {}) {
  AgentTopicInfo info;
  info.pub_topic = std::move(pub);
  info.sub_topic = std::move(subs);
  return info;
}

// An agent with no `pub_topic` key: `Agent` defaults it to the section name,
// which is what src/main/doctor.cpp fills in here.
AgentTopicInfo implicit_pub(std::string section,
                            std::vector<std::string> subs = {}) {
  AgentTopicInfo info = agent(std::move(section), std::move(subs));
  info.pub_implicit = true;
  return info;
}

} // namespace

// ---------------------------------------------------------------------------
// Node record-label formatting
// ---------------------------------------------------------------------------

TEST_CASE("topology_graph: subscribe-all sub_topic renders as a single (all) line",
          "[topology_graph]") {
  AgentMap agents = {{"logger", agent("", {""})}};
  std::string dot = topology_graph(agents);
  // Alone in the graph, "logger"'s subscribe-all pattern has nothing to
  // receive, so it is marked dead ([!]) and the node is dashed -- exercised
  // together here since the exact string is fully deterministic either way.
  std::string expected =
      kHeader + "\n" +
      "  logger [label=\"logger|{(all) [!]\\l}\", color=darkblue, style=dashed];\n"
      "}\n";
  CHECK(dot == expected);
}

TEST_CASE("topology_graph: no sub_topic entries renders a bare name node",
          "[topology_graph]") {
  AgentMap agents = {{"imu", agent("temp/raw")}};
  std::string dot = topology_graph(agents);
  // Nobody subscribes to temp/raw, so the otherwise-invisible pub_topic is
  // spelled out in the node itself rather than only implied by the dashed
  // contour.
  std::string expected =
      kHeader + "\n" +
      "  imu [label=\"imu\\lpub temp/raw [!]\", color=darkred, style=dashed];\n"
      "}\n";
  CHECK(dot == expected);
}

TEST_CASE("topology_graph: multiple sub_topic entries each render on their own line",
          "[topology_graph]") {
  AgentMap agents = {{"agg", agent("", {"a/b", "c/d"})}};
  std::string dot = topology_graph(agents);
  std::string expected =
      kHeader + "\n" +
      "  agg [label=\"agg|{a/b [!]\\lc/d [!]\\l}\", color=darkblue, style=dashed];\n"
      "}\n";
  CHECK(dot == expected);
}

// ---------------------------------------------------------------------------
// Role -> color assignment (source / filter / sink / neither) and edge
// generation for exactly-matching sub_topic patterns, checked together
// against one exact expected graph since the four-role fixture below is
// fully deterministic (every dangling/color/edge outcome is pinned down by
// hand).
// ---------------------------------------------------------------------------

TEST_CASE("topology_graph: role-based color assignment for all four cases",
          "[topology_graph]") {
  AgentMap agents = {
      {"flt", agent("topic/b", {"topic/a"})}, // has both pub and sub -> filter
      {"non", agent("")},                     // neither -> no color attribute
      {"snk", agent("", {"topic/b"})},        // sub only -> sink
      {"src", agent("topic/a")},              // pub only -> source
  };
  std::string dot = topology_graph(agents);
  std::string expected =
      kHeader + "\n" +
      "  flt [label=\"flt|{topic/a\\l}\", color=darkgreen];\n"
      "  non [label=\"non\"];\n"
      "  snk [label=\"snk|{topic/b\\l}\", color=darkblue];\n"
      "  src [label=\"src\", color=darkred];\n"
      "\n"
      "  flt -> snk [label=\"topic/b\"];\n"
      "  src -> flt [label=\"topic/a\"];\n"
      "}\n";
  CHECK(dot == expected);
}

// ---------------------------------------------------------------------------
// Match semantics: the graph must draw exactly what the agent would receive
// on the wire (Mads::subscription_match()), which is NOT plain equality --
// literal sub_topic entries match by raw ZMQ byte prefix and wildcard ones by
// MQTT rules. How the edge came to be is encoded in its style and label.
// ---------------------------------------------------------------------------

TEST_CASE("topology_graph: an exact match draws a plain solid edge",
          "[topology_graph]") {
  AgentMap agents = {
      {"pub", agent("sensors/imu/raw")},
      {"sub", agent("", {"sensors/imu/raw"})},
  };
  std::string dot = topology_graph(agents);
  CHECK(dot.find("pub -> sub [label=\"sensors/imu/raw\"];\n") !=
        std::string::npos);
}

TEST_CASE("topology_graph: a literal prefix catch draws a dashed edge naming "
          "the pattern",
          "[topology_graph]") {
  // `sub_topic = ["sensors"]` is handed to ZeroMQ verbatim, and ZeroMQ
  // matches by byte prefix -- so this subscriber really does receive
  // sensors/imu/raw, even though the strings are not equal.
  AgentMap agents = {
      {"pub", agent("sensors/imu/raw")},
      {"sub", agent("", {"sensors"})},
  };
  std::string dot = topology_graph(agents);
  CHECK(dot.find("pub -> sub [label=\"sensors/imu/raw\\lvia sensors\\l\", "
                 "style=dashed];\n") != std::string::npos);
  // The catching pattern is live, so neither side is flagged: `dashed` here
  // is the edge's match kind, never a dangling-node contour.
  CHECK(dot.find("[!]") == std::string::npos);
}

TEST_CASE("topology_graph: wildcard patterns draw dotted edges naming the pattern",
          "[topology_graph]") {
  AgentMap agents = {
      {"hash_sub", agent("", {"sensors/#"})},
      {"plus_sub", agent("", {"sensors/+/raw"})},
      {"pub", agent("sensors/imu/raw")},
      {"unrelated_sub", agent("", {"other/+/raw"})},
  };
  std::string dot = topology_graph(agents);
  CHECK(dot.find("pub -> hash_sub [label=\"sensors/imu/raw\\lvia sensors/#\\l\", "
                 "style=dotted];") != std::string::npos);
  CHECK(dot.find("pub -> plus_sub [label=\"sensors/imu/raw\\lvia "
                 "sensors/+/raw\\l\", style=dotted];") != std::string::npos);
  CHECK(dot.find("-> unrelated_sub") == std::string::npos);
  // The pattern that catches nothing is the one marked, not the whole node's
  // subscription list.
  CHECK(dot.find("unrelated_sub|{other/+/raw [!]\\l}") != std::string::npos);
}

TEST_CASE("topology_graph: several patterns catching one topic are all listed, "
          "and the most explicit one sets the style",
          "[topology_graph]") {
  // "sensors" catches by prefix, "sensors/#" by wildcard, and both are worth
  // seeing -- a subscription list that overlaps with itself is exactly the
  // kind of thing this graph is read to find.
  AgentMap agents = {
      {"pub", agent("sensors/imu/raw")},
      {"sub", agent("", {"sensors", "sensors/#"})},
  };
  std::string dot = topology_graph(agents);
  CHECK(dot.find("pub -> sub [label=\"sensors/imu/raw\\lvia sensors, "
                 "sensors/#\\l\", style=dotted];") != std::string::npos);

  // An exact entry outranks both: the wiring was spelled out, so the edge is
  // solid and needs no "via" at all.
  AgentMap with_exact = {
      {"pub", agent("sensors/imu/raw")},
      {"sub", agent("", {"sensors", "sensors/imu/raw"})},
  };
  CHECK(topology_graph(with_exact).find("pub -> sub [label=\"sensors/imu/raw\"];") !=
        std::string::npos);
}

// ---------------------------------------------------------------------------
// Subscribe-all fan-in collapsing
// ---------------------------------------------------------------------------

TEST_CASE("topology_graph: subscribe-all fan-in collapses into a count by default",
          "[topology_graph]") {
  AgentMap agents = {
      {"a", agent("topic/a")},
      {"b", agent("topic/b")},
      {"logger", agent("", {""})},
  };
  std::string dot = topology_graph(agents);
  CHECK(dot.find("logger [label=\"logger|{(all) [2 publishers]\\l}\", "
                 "color=darkblue];") != std::string::npos);
  CHECK(dot.find("->") == std::string::npos);
  CHECK(dot.find("// 2 fan-in edges into subscribe-all subscribers collapsed") !=
        std::string::npos);
}

TEST_CASE("topology_graph: expand_catch_all draws the fan-in edges instead",
          "[topology_graph]") {
  AgentMap agents = {
      {"a", agent("topic/a")},
      {"logger", agent("", {""})},
  };
  GraphOptions options;
  options.expand_catch_all = true;
  std::string dot = topology_graph(agents, options);
  CHECK(dot.find("logger [label=\"logger|{(all)\\l}\", color=darkblue];") !=
        std::string::npos);
  CHECK(dot.find("a -> logger [label=\"topic/a\\lvia (all)\\l\", style=dashed];") !=
        std::string::npos);
  CHECK(dot.find("collapsed") == std::string::npos);
}

TEST_CASE("topology_graph: an edge that also matches a named pattern is never "
          "collapsed",
          "[topology_graph]") {
  // "" and "topic/a" both match, so the arrow carries information the (all)
  // line does not -- the subscriber asked for this topic by name. It stays
  // drawn (solid, since the named pattern is an exact one) and the count
  // excludes it.
  AgentMap agents = {
      {"a", agent("topic/a")},
      {"b", agent("topic/b")},
      {"sink", agent("", {"", "topic/a"})},
  };
  std::string dot = topology_graph(agents);
  CHECK(dot.find("a -> sink [label=\"topic/a\"];") != std::string::npos);
  CHECK(dot.find("-> sink [label=\"topic/b") == std::string::npos);
  CHECK(dot.find("(all) [1 publisher]") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Implicit pub_topic (the `cfg["pub_topic"].value_or(_name)` default)
// ---------------------------------------------------------------------------

TEST_CASE("topology_graph: an implicit pub_topic still produces edges, drawn grey",
          "[topology_graph]") {
  // A filter that declares no pub_topic publishes under its own section name
  // at runtime; dropping that edge would hide a real link in the pipeline.
  AgentMap agents = {
      {"running_avg", implicit_pub("running_avg", {"serial_reader"})},
      {"serial_reader", implicit_pub("serial_reader")},
  };
  std::string dot = topology_graph(agents);
  CHECK(dot.find("serial_reader -> running_avg [label=\"serial_reader\", "
                 "color=gray50, fontcolor=gray50];") != std::string::npos);
}

TEST_CASE("topology_graph: an implicit pub_topic is never dangling and never "
          "recolors the node",
          "[topology_graph]") {
  // A pure sink gets the same defaulted pub_topic as a filter does, so
  // treating it as a publish declaration would paint every sink green and
  // flag every one of them as dangling.
  AgentMap agents = {{"logger", implicit_pub("logger", {"metadata"})}};
  std::string dot = topology_graph(agents);
  CHECK(dot.find("logger [label=\"logger|{metadata [!]\\l}\", color=darkblue, "
                 "style=dashed];") != std::string::npos);
  CHECK(dot.find("pub logger") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Dangling detection: unmatched pub_topic, unsatisfied sub_topic, and the
// self-loop-does-not-count-as-dangling case, all in one deterministic fixture.
// ---------------------------------------------------------------------------

TEST_CASE("topology_graph: dangling pub/sub get a dashed contour, a matched self-loop does not",
          "[topology_graph]") {
  AgentMap agents = {
      // pub_topic nobody (including itself) subscribes to -> dangling.
      {"orphan_pub", agent("a/b")},
      // sub_topic nobody (including itself) ever publishes -> dangling.
      {"orphan_sub", agent("", {"z/z"})},
      // Its own pub_topic matches its own sub_topic: satisfied on both
      // sides by the self-loop, so NOT dangling, per spec.
      {"self_loop", agent("loop/topic", {"loop/topic"})},
  };
  std::string dot = topology_graph(agents);
  std::string expected =
      kHeader + "\n" +
      "  orphan_pub [label=\"orphan_pub\\lpub a/b [!]\", color=darkred, style=dashed];\n"
      "  orphan_sub [label=\"orphan_sub|{z/z [!]\\l}\", color=darkblue, style=dashed];\n"
      "  self_loop  [label=\"self_loop|{loop/topic\\l}\", color=darkgreen];\n"
      "}\n";
  CHECK(dot == expected);
}

TEST_CASE("topology_graph: a matched pub_topic and a satisfied sub_topic are not dangling",
          "[topology_graph]") {
  AgentMap agents = {
      {"a", agent("x/y")},
      {"b", agent("", {"x/y"})},
  };
  std::string dot = topology_graph(agents);
  CHECK(dot.find("style=dashed") == std::string::npos);
  CHECK(dot.find("[!]") == std::string::npos);
}

TEST_CASE("topology_graph: a pub_topic heard only by a collapsed subscribe-all "
          "subscriber is still not dangling",
          "[topology_graph]") {
  // The arrow is not drawn, but the topic does have a listener -- whether an
  // edge is rendered must not feed back into the dangling verdict.
  AgentMap agents = {
      {"src", agent("x/y")},
      {"logger", agent("", {""})},
  };
  std::string dot = topology_graph(agents);
  CHECK(dot.find("style=dashed") == std::string::npos);
  CHECK(dot.find("[!]") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Metacharacter escaping ('{', '}', '|', '<', '>', backslash)
// ---------------------------------------------------------------------------

TEST_CASE("topology_graph: record-label metacharacters in a sub_topic entry are escaped",
          "[topology_graph]") {
  const std::vector<char> metachars = {'{', '}', '|', '<', '>', '\\'};
  for (char ch : metachars) {
    INFO("metachar='" << ch << "'");
    AgentMap agents = {{"sink", agent("", {std::string(1, ch)})}};
    std::string dot = topology_graph(agents);
    // escape_topic() emits a backslash immediately followed by the original
    // character -- built programmatically (not as a literal) to avoid
    // hand-transcribing backslash-heavy expected strings.
    std::string expected_segment = std::string("\\") + ch + " [!]\\l";
    CHECK(dot.find(expected_segment) != std::string::npos);
  }
}

TEST_CASE("topology_graph: record-label metacharacters in a matching edge's pub_topic label are escaped",
          "[topology_graph]") {
  const std::vector<char> metachars = {'{', '}', '|', '<', '>', '\\'};
  for (char ch : metachars) {
    INFO("metachar='" << ch << "'");
    // The raw (unescaped) topic is used for matching -- a literal sub_topic
    // entry equal to the raw pub_topic must still match -- only the emitted
    // DOT text is escaped.
    std::string raw_topic = std::string("a") + ch + "b";
    AgentMap agents = {
        {"pub", agent(raw_topic)},
        {"sub", agent("", {raw_topic})},
    };
    std::string dot = topology_graph(agents);
    CHECK(dot.find("pub -> sub") != std::string::npos);
    std::string expected_label =
        std::string("label=\"a") + '\\' + ch + "b\"";
    CHECK(dot.find(expected_label) != std::string::npos);
  }
}

// ---------------------------------------------------------------------------
// Integration-style check against an imu/filter/logger/debug_sink pipeline:
// one wildcard hop, one catch-all sink, one unsatisfiable subscription.
// ---------------------------------------------------------------------------

TEST_CASE("topology_graph: imu/filter/logger/debug_sink worked example",
          "[topology_graph]") {
  AgentMap agents = {
      {"debug_sink", agent("", {"diagnostics/#"})},
      {"filter", agent("sensors/imu/filtered", {"sensors/imu/#"})},
      {"imu", agent("sensors/imu/raw")},
      {"logger", agent("", {""})},
  };
  std::string dot = topology_graph(agents);

  // Node shapes, colors and dashed states.
  CHECK(dot.find("imu        [label=\"imu\", color=darkred];") != std::string::npos);
  CHECK(dot.find("filter     [label=\"filter|{sensors/imu/#\\l}\", color=darkgreen];") !=
        std::string::npos);
  CHECK(dot.find("logger     [label=\"logger|{(all) [2 publishers]\\l}\", color=darkblue];") !=
        std::string::npos);
  CHECK(dot.find("debug_sink [label=\"debug_sink|{diagnostics/# [!]\\l}\", color=darkblue, "
                 "style=dashed];") != std::string::npos);

  // The one informative arrow: imu's raw topic caught by the filter's
  // wildcard. filter's own output is heard only by the catch-all logger, so
  // that arrow is collapsed into logger's count (both publishers reach it),
  // and filter's pub_topic is not dangling despite having no drawn edge.
  CHECK(dot.find("imu -> filter [label=\"sensors/imu/raw\\lvia sensors/imu/#\\l\", "
                 "style=dotted];") != std::string::npos);
  CHECK(dot.find("-> logger") == std::string::npos);
  CHECK(dot.find("-> debug_sink") == std::string::npos);

  // With the fan-in expanded, both catch-all arrows appear.
  GraphOptions expanded;
  expanded.expand_catch_all = true;
  std::string full = topology_graph(agents, expanded);
  CHECK(full.find("imu    -> logger [label=\"sensors/imu/raw\\lvia (all)\\l\", "
                  "style=dashed];") != std::string::npos);
  CHECK(full.find("filter -> logger [label=\"sensors/imu/filtered\\lvia (all)\\l\", "
                  "style=dashed];") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Live subscription overlay (GraphOptions::live) -- ZMQ_DEVELOPMENT.md §2.2.
//
// The overlay annotates the declared graph with a topic -> subscriber-count
// table read off a live broker. It is deliberately anonymous (ZMQ
// subscription frames carry no peer identity), so every assertion below is
// about a *prefix* being subscribed or not, never about which agent did it.
// ---------------------------------------------------------------------------

TEST_CASE("topology_graph: no live overlay leaves every label untouched",
          "[topology_graph]") {
  AgentMap agents{{"src", agent("imu")}, {"sink", agent("", {"imu"})}};
  // Same graph, once without and once with an overlay that reports the
  // subscription as live: the no-overlay rendering must not gain a marker.
  const std::string plain = topology_graph(agents, {});
  REQUIRE(plain.find("[live") == std::string::npos);
  REQUIRE(plain.find("[offline]") == std::string::npos);
  REQUIRE(plain.find("live overlay:") == std::string::npos);
}

TEST_CASE("topology_graph: a declared sub_topic with subscribers is marked live",
          "[topology_graph]") {
  AgentMap agents{{"src", agent("imu")}, {"sink", agent("", {"imu"})}};
  GraphOptions opts;
  opts.live = Mads::LiveSubscriptions{{"imu", 2}};
  const std::string dot = topology_graph(agents, opts);
  REQUIRE(dot.find("sink [label=\"sink|{imu [live 2]\\l}\"") !=
          std::string::npos);
  // The legend is emitted only in live mode.
  REQUIRE(dot.find("live overlay:") != std::string::npos);
}

TEST_CASE("topology_graph: a declared sub_topic nobody subscribes is offline",
          "[topology_graph]") {
  AgentMap agents{{"src", agent("imu")}, {"sink", agent("", {"imu"})}};
  GraphOptions opts;
  opts.live = Mads::LiveSubscriptions{}; // broker reports nothing subscribed
  const std::string dot = topology_graph(agents, opts);
  REQUIRE(dot.find("sink [label=\"sink|{imu [offline]\\l}\"") !=
          std::string::npos);
}

TEST_CASE("topology_graph: a wildcard sub_topic is looked up under its "
          "literal prefix, not its pattern",
          "[topology_graph]") {
  // Agent::connect_sub() subscribes literal_prefix() for a wildcard entry, so
  // that -- and not the raw pattern -- is the key the broker's table carries.
  // Matching on the pattern string would report a live subscriber as offline.
  AgentMap agents{{"src", agent("sensors/imu/raw")},
                  {"sink", agent("", {"sensors/+/raw"})}};
  GraphOptions opts;
  opts.live = Mads::LiveSubscriptions{{"sensors/", 1}};
  const std::string dot = topology_graph(agents, opts);
  // Asserted on the label form: the legend comment itself contains the bare
  // text "[offline]", so a naive whole-document search always matches.
  REQUIRE(dot.find("[label=\"sink|{sensors/+/raw [live 1]\\l}\"") !=
          std::string::npos);
  REQUIRE(dot.find("[offline]\\l") == std::string::npos);
}

TEST_CASE("topology_graph: a live subscription nobody declared is surfaced",
          "[topology_graph]") {
  // The case a settings-only graph cannot show at all.
  AgentMap agents{{"src", agent("imu")}};
  GraphOptions opts;
  opts.live = Mads::LiveSubscriptions{{"mystery", 3}};
  const std::string dot = topology_graph(agents, opts);
  REQUIRE(dot.find("__live_0 [label=\"mystery\\lundeclared subscribers [3]\\l\"") !=
          std::string::npos);
  REQUIRE(dot.find("shape=note, color=darkorange") != std::string::npos);
}

TEST_CASE("topology_graph: an undeclared subscriber is linked to the "
          "declared publishers it actually receives from",
          "[topology_graph]") {
  // "sensors" as a subscribe prefix catches "sensors/imu" by the same
  // byte-prefix rule the wire uses, so the arrow is real information: it
  // names who that unknown listener is hearing.
  AgentMap agents{{"src", agent("sensors/imu")}, {"other", agent("unrelated")}};
  GraphOptions opts;
  opts.live = Mads::LiveSubscriptions{{"sensors", 1}};
  const std::string dot = topology_graph(agents, opts);
  REQUIRE(dot.find("src -> __live_0 [style=dotted, color=darkorange];") !=
          std::string::npos);
  REQUIRE(dot.find("other -> __live_0") == std::string::npos);
}

TEST_CASE("topology_graph: MADS's own programmatic subscriptions are never "
          "reported as undeclared",
          "[topology_graph]") {
  // "control" is pushed on by Agent::enable_remote_control() and
  // "subscriptions" is created by whoever reads the table, so both appear in
  // every real table with no mads.ini section behind them. Reporting them
  // would cry wolf on every single run.
  REQUIRE(Mads::is_implicitly_subscribed_topic("control"));
  REQUIRE(Mads::is_implicitly_subscribed_topic("subscriptions"));
  REQUIRE_FALSE(Mads::is_implicitly_subscribed_topic("imu"));

  AgentMap agents{{"src", agent("imu")}};
  GraphOptions opts;
  opts.live = Mads::LiveSubscriptions{{"control", 4}, {"subscriptions", 1}};
  const std::string dot = topology_graph(agents, opts);
  REQUIRE(dot.find("__live_") == std::string::npos);
}

TEST_CASE("topology_graph: a zero count is treated as nobody subscribed",
          "[topology_graph]") {
  // The broker erases an entry at refcount zero, but a table that still
  // carries one must not be read as a live subscriber, nor surfaced as an
  // undeclared one.
  AgentMap agents{{"src", agent("imu")}, {"sink", agent("", {"imu"})}};
  GraphOptions opts;
  opts.live = Mads::LiveSubscriptions{{"imu", 0}, {"ghost", 0}};
  const std::string dot = topology_graph(agents, opts);
  REQUIRE(dot.find("imu [offline]") != std::string::npos);
  REQUIRE(dot.find("__live_") == std::string::npos);
}

TEST_CASE("topology_graph: offline and dangling markers coexist on one entry",
          "[topology_graph]") {
  // Orthogonal facts: "nothing publishes here" ([!]) and "nobody is
  // listening" ([offline]) can both be true, and a reader needs both.
  AgentMap agents{{"sink", agent("", {"nobody_publishes_this"})}};
  GraphOptions opts;
  opts.live = Mads::LiveSubscriptions{};
  const std::string dot = topology_graph(agents, opts);
  REQUIRE(dot.find("nobody_publishes_this [offline] [!]") != std::string::npos);
}

TEST_CASE("topology_graph: a pure publisher is never marked offline",
          "[topology_graph]") {
  // A subscription table says nothing about publishers, so a source's
  // liveness is simply not knowable this way -- and must not be guessed at.
  AgentMap agents{{"src", agent("imu")}, {"sink", agent("", {"imu"})}};
  GraphOptions opts;
  opts.live = Mads::LiveSubscriptions{{"imu", 1}};
  const std::string dot = topology_graph(agents, opts);
  // src subscribes to nothing, so it carries no live marker of any kind --
  // neither [live N] nor [offline].
  REQUIRE(dot.find("[label=\"src\", color=darkred]") != std::string::npos);
  REQUIRE(dot.find("[offline]\\l") == std::string::npos);
}
