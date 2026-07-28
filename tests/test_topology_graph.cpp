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
using Mads::topology_graph;

namespace {
using AgentMap = std::map<std::string, AgentTopicInfo>;
}

// ---------------------------------------------------------------------------
// Node record-label formatting
// ---------------------------------------------------------------------------

TEST_CASE("topology_graph: subscribe-all sub_topic renders as a single (all) line",
          "[topology_graph]") {
  AgentMap agents = {{"logger", {"", {""}}}};
  std::string dot = topology_graph(agents);
  // Alone in the graph, "logger"'s subscribe-all pattern is unsatisfied (no
  // publisher anywhere), so the node is also dashed -- exercised together
  // here since the exact string is fully deterministic either way.
  std::string expected =
      "digraph mads {\n"
      "  rankdir=LR;\n"
      "  node [shape=record, fontname=\"monospace\"];\n\n"
      "  logger [label=\"{logger|(all)\\l}\", color=darkblue, style=dashed];\n"
      "}\n";
  CHECK(dot == expected);
}

TEST_CASE("topology_graph: no sub_topic entries renders an empty bottom compartment",
          "[topology_graph]") {
  AgentMap agents = {{"imu", {"temp/raw", {}}}};
  std::string dot = topology_graph(agents);
  std::string expected =
      "digraph mads {\n"
      "  rankdir=LR;\n"
      "  node [shape=record, fontname=\"monospace\"];\n\n"
      "  imu [label=\"{imu|}\", color=darkred, style=dashed];\n"
      "}\n";
  CHECK(dot == expected);
}

TEST_CASE("topology_graph: multiple sub_topic entries each render on their own line",
          "[topology_graph]") {
  AgentMap agents = {{"agg", {"", {"a/b", "c/d"}}}};
  std::string dot = topology_graph(agents);
  std::string expected =
      "digraph mads {\n"
      "  rankdir=LR;\n"
      "  node [shape=record, fontname=\"monospace\"];\n\n"
      "  agg [label=\"{agg|a/b\\lc/d\\l}\", color=darkblue, style=dashed];\n"
      "}\n";
  CHECK(dot == expected);
}

// ---------------------------------------------------------------------------
// Role -> color assignment (source / filter / sink / neither) and edge
// generation for literal sub_topic patterns, checked together against one
// exact expected graph since the four-role fixture below is fully
// deterministic (every dangling/color/edge outcome is pinned down by hand).
// ---------------------------------------------------------------------------

TEST_CASE("topology_graph: role-based color assignment for all four cases",
          "[topology_graph]") {
  AgentMap agents = {
      {"flt", {"topic/b", {"topic/a"}}}, // has both pub and sub -> filter
      {"non", {"", {}}},                 // neither -> no color attribute
      {"snk", {"", {"topic/b"}}},        // sub only -> sink
      {"src", {"topic/a", {}}},          // pub only -> source
  };
  std::string dot = topology_graph(agents);
  std::string expected =
      "digraph mads {\n"
      "  rankdir=LR;\n"
      "  node [shape=record, fontname=\"monospace\"];\n\n"
      "  flt [label=\"{flt|topic/a\\l}\", color=darkgreen];\n"
      "  non [label=\"{non|}\"];\n"
      "  snk [label=\"{snk|topic/b\\l}\", color=darkblue];\n"
      "  src [label=\"{src|}\", color=darkred];\n"
      "\n"
      "  flt -> snk [label=\"topic/b\"];\n"
      "  src -> flt [label=\"topic/a\"];\n"
      "}\n";
  CHECK(dot == expected);
}

// ---------------------------------------------------------------------------
// Edge generation across literal and wildcard sub_topic patterns
// ---------------------------------------------------------------------------

TEST_CASE("topology_graph: edges are drawn for both literal and wildcard sub_topic matches",
          "[topology_graph]") {
  AgentMap agents = {
      {"lit_sub", {"", {"sensors/acc/x"}}},   // literal, matches pub1 exactly
      {"no_match_sub", {"", {"sensors/gyro/x"}}}, // literal, does not match
      {"pub1", {"sensors/acc/x", {}}},
      {"wild_sub", {"", {"sensors/+/x"}}},    // wildcard, matches pub1
  };
  std::string dot = topology_graph(agents);
  CHECK(dot.find("pub1 -> lit_sub [label=\"sensors/acc/x\"]") != std::string::npos);
  CHECK(dot.find("pub1 -> wild_sub [label=\"sensors/acc/x\"]") != std::string::npos);
  CHECK(dot.find("-> no_match_sub") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Dangling detection: unmatched pub_topic, unsatisfied sub_topic, and the
// self-loop-does-not-count-as-dangling case, all in one deterministic fixture.
// ---------------------------------------------------------------------------

TEST_CASE("topology_graph: dangling pub/sub get a dashed contour, a matched self-loop does not",
          "[topology_graph]") {
  AgentMap agents = {
      // pub_topic nobody (including itself) subscribes to -> dangling.
      {"orphan_pub", {"a/b", {}}},
      // sub_topic nobody (including itself) ever publishes -> dangling.
      {"orphan_sub", {"", {"z/z"}}},
      // Its own pub_topic matches its own sub_topic: satisfied on both
      // sides by the self-loop, so NOT dangling, per spec.
      {"self_loop", {"loop/topic", {"loop/topic"}}},
  };
  std::string dot = topology_graph(agents);
  std::string expected =
      "digraph mads {\n"
      "  rankdir=LR;\n"
      "  node [shape=record, fontname=\"monospace\"];\n\n"
      "  orphan_pub [label=\"{orphan_pub|}\", color=darkred, style=dashed];\n"
      "  orphan_sub [label=\"{orphan_sub|z/z\\l}\", color=darkblue, style=dashed];\n"
      "  self_loop  [label=\"{self_loop|loop/topic\\l}\", color=darkgreen];\n"
      "}\n";
  CHECK(dot == expected);
}

TEST_CASE("topology_graph: a matched pub_topic and a satisfied sub_topic are not dangling",
          "[topology_graph]") {
  AgentMap agents = {
      {"a", {"x/y", {}}},
      {"b", {"", {"x/y"}}},
  };
  std::string dot = topology_graph(agents);
  CHECK(dot.find("style=dashed") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Metacharacter escaping ('{', '}', '|', '<', '>', backslash)
// ---------------------------------------------------------------------------

TEST_CASE("topology_graph: record-label metacharacters in a sub_topic entry are escaped",
          "[topology_graph]") {
  const std::vector<char> metachars = {'{', '}', '|', '<', '>', '\\'};
  for (char ch : metachars) {
    INFO("metachar='" << ch << "'");
    AgentMap agents = {{"sink", {"", {std::string(1, ch)}}}};
    std::string dot = topology_graph(agents);
    // escape_topic() emits a backslash immediately followed by the original
    // character, immediately followed by the '\l' record line terminator --
    // built programmatically (not as a literal) to avoid hand-transcribing
    // backslash-heavy expected strings.
    std::string expected_segment = std::string("\\") + ch + "\\l";
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
        {"pub", {raw_topic, {}}},
        {"sub", {"", {raw_topic}}},
    };
    std::string dot = topology_graph(agents);
    CHECK(dot.find("pub -> sub") != std::string::npos);
    std::string expected_label =
        std::string("label=\"a") + '\\' + ch + "b\"";
    CHECK(dot.find(expected_label) != std::string::npos);
  }
}

// ---------------------------------------------------------------------------
// Integration-style check against the imu/filter/logger/debug_sink pipeline
// from NEW_FEATURES.md's "Topology graph" worked example: same node shapes,
// colors, and dashed states. Note: that illustration only shows the
// imu->filter and filter->logger edges, but logger's declared subscribe-all
// pattern (sub_topic = [""]) genuinely matches every publisher, including
// imu directly -- exactly mirroring real broker fan-out (a subscribe-all
// subscriber receives from every publisher, not just the "obvious" pipeline
// stage) -- so a complete graph also draws imu->logger. This is asserted
// explicitly below rather than silently reproduced.
// ---------------------------------------------------------------------------

TEST_CASE("topology_graph: imu/filter/logger/debug_sink worked example",
          "[topology_graph]") {
  AgentMap agents = {
      {"debug_sink", {"", {"diagnostics/#"}}},
      {"filter", {"sensors/imu/filtered", {"sensors/imu/#"}}},
      {"imu", {"sensors/imu/raw", {}}},
      {"logger", {"", {""}}},
  };
  std::string dot = topology_graph(agents);

  // Node shapes, colors and dashed states match the worked example exactly.
  CHECK(dot.find("imu        [label=\"{imu|}\", color=darkred];") != std::string::npos);
  CHECK(dot.find("filter     [label=\"{filter|sensors/imu/#\\l}\", color=darkgreen];") !=
        std::string::npos);
  CHECK(dot.find("logger     [label=\"{logger|(all)\\l}\", color=darkblue];") !=
        std::string::npos);
  CHECK(dot.find("debug_sink [label=\"{debug_sink|diagnostics/#\\l}\", color=darkblue, "
                 "style=dashed];") != std::string::npos);

  // Edges: the two the doc illustrates, plus the genuine imu->logger edge
  // implied by logger's subscribe-all pattern (see comment above).
  CHECK(dot.find("imu    -> filter [label=\"sensors/imu/raw\"];") != std::string::npos);
  CHECK(dot.find("filter -> logger [label=\"sensors/imu/filtered\"];") != std::string::npos);
  CHECK(dot.find("imu    -> logger [label=\"sensors/imu/raw\"];") != std::string::npos);
  // debug_sink's unsatisfiable subscription never receives an edge.
  CHECK(dot.find("-> debug_sink") == std::string::npos);
}
