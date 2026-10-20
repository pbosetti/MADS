// Unit tests for the structured edge/dangling API in src/topology_graph.hpp:
// topology_edges(), dangling_topics() and agent_topic_info(s)(). Every case
// is a hand-built std::map<std::string, Mads::AgentTopicInfo> (or a small
// settings string parsed with toml++); no sockets, no files.
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "topology_graph.hpp"

using Mads::AgentTopicInfo;
using Mads::DanglingTopics;
using Mads::SubMatch;
using Mads::TopologyEdge;

namespace {

using AgentMap = std::map<std::string, AgentTopicInfo>;

// An agent that publishes `pub` (declared, not defaulted) and subscribes to
// `subs`.
AgentTopicInfo agent(std::string pub, std::vector<std::string> subs = {}) {
  AgentTopicInfo info;
  info.pub_topic = std::move(pub);
  info.sub_topic = std::move(subs);
  return info;
}

// An agent with no `pub_topic` key: it publishes under its section name.
AgentTopicInfo implicit_pub(std::string section,
                            std::vector<std::string> subs = {}) {
  AgentTopicInfo info = agent(std::move(section), std::move(subs));
  info.pub_implicit = true;
  return info;
}

} // namespace

TEST_CASE("topology_edges: exact match produces a solid edge with no via",
          "[topology_edges]") {
  AgentMap agents = {{"imu", agent("t/x")}, {"log", agent("", {"t/x"})}};
  auto edges = Mads::topology_edges(agents);
  REQUIRE(edges.size() == 1);
  const TopologyEdge &e = edges[0];
  CHECK(e.from == "imu");
  CHECK(e.to == "log");
  CHECK(e.topic == "t/x");
  CHECK(e.kind == SubMatch::Exact);
  CHECK(e.via_patterns.empty());
  CHECK_FALSE(e.implicit_pub);
  CHECK_FALSE(e.catch_all);
}

TEST_CASE("topology_edges: prefix match names the literal entry as via",
          "[topology_edges]") {
  // Byte-prefix delivery: "sensors" receives "sensors/imu".
  AgentMap agents = {{"imu", agent("sensors/imu")},
                     {"fan", agent("", {"sensors"})}};
  auto edges = Mads::topology_edges(agents);
  REQUIRE(edges.size() == 1);
  CHECK(edges[0].kind == SubMatch::Prefix);
  CHECK(edges[0].via_patterns == std::vector<std::string>{"sensors"});
  CHECK_FALSE(edges[0].catch_all);
}

TEST_CASE("topology_edges: wildcard match carries the pattern as via",
          "[topology_edges]") {
  AgentMap agents = {{"imu", agent("sensors/imu")},
                     {"fan", agent("", {"sensors/+"})}};
  auto edges = Mads::topology_edges(agents);
  REQUIRE(edges.size() == 1);
  CHECK(edges[0].kind == SubMatch::Wildcard);
  CHECK(edges[0].via_patterns == std::vector<std::string>{"sensors/+"});
}

TEST_CASE("topology_edges: implicit pub_topic edge is flagged", "[topology_edges]") {
  // "filter" publishes under its own name because it declares no pub_topic.
  AgentMap agents = {{"filter", implicit_pub("filter")},
                     {"log", agent("", {"filter"})}};
  auto edges = Mads::topology_edges(agents);
  REQUIRE(edges.size() == 1);
  CHECK(edges[0].from == "filter");
  CHECK(edges[0].topic == "filter");
  CHECK(edges[0].kind == SubMatch::Exact);
  CHECK(edges[0].implicit_pub);
}

TEST_CASE("topology_edges: subscribe-all entry sets the catch_all flag",
          "[topology_edges]") {
  AgentMap agents = {{"imu", agent("t/x")}, {"logger", agent("", {""})}};
  auto edges = Mads::topology_edges(agents);
  REQUIRE(edges.size() == 1);
  CHECK(edges[0].catch_all);
  CHECK(edges[0].kind == SubMatch::Prefix);
  // The subscribe-all entry is kept verbatim; DOT renders it as "(all)".
  CHECK(edges[0].via_patterns == std::vector<std::string>{""});
}

TEST_CASE("topology_edges: catch_all is false when a named entry also matches",
          "[topology_edges]") {
  AgentMap agents = {{"imu", agent("t/x")},
                     {"mon", agent("", {"", "t"})}};
  auto edges = Mads::topology_edges(agents);
  REQUIRE(edges.size() == 1);
  CHECK_FALSE(edges[0].catch_all);
  CHECK(edges[0].via_patterns == std::vector<std::string>{"", "t"});
}

TEST_CASE("topology_edges: self-match produces no edge", "[topology_edges]") {
  AgentMap agents = {{"loop", agent("t/x", {"t/x"})}};
  CHECK(Mads::topology_edges(agents).empty());
  // Yet the self-match satisfies both sides, so nothing dangles.
  DanglingTopics d = Mads::dangling_topics(agents);
  CHECK(d.pub.empty());
  CHECK(d.sub.empty());
}

TEST_CASE("topology_edges: dangling_topics: unreached pub_topic and sub_topic are reported",
          "[topology_edges]") {
  AgentMap agents = {{"src", agent("t/out")},
                     {"sink", agent("", {"t/in"})}};
  CHECK(Mads::topology_edges(agents).empty());
  DanglingTopics d = Mads::dangling_topics(agents);
  CHECK(d.pub == std::set<std::string>{"src"});
  REQUIRE(d.sub.count("sink") == 1);
  CHECK(d.sub.at("sink") == std::set<std::string>{"t/in"});
  CHECK(d.sub.count("src") == 0);
}

TEST_CASE("topology_edges: dangling_topics: implicit pub_topic is never dangling",
          "[topology_edges]") {
  AgentMap agents = {{"sink", implicit_pub("sink", {"t/in"})}};
  DanglingTopics d = Mads::dangling_topics(agents);
  CHECK(d.pub.empty());
  CHECK(d.sub.at("sink") == std::set<std::string>{"t/in"});
}

TEST_CASE("topology_edges: agent_topic_info: implicit pub_topic falls back to section name",
          "[topology_edges]") {
  auto cfg = toml::parse("[filter]\nsub_topic = \"s/out\"\n");
  const auto *table = cfg["filter"].as_table();
  REQUIRE(table != nullptr);
  AgentTopicInfo info = Mads::agent_topic_info("filter", *table);
  CHECK(info.pub_topic == "filter");
  CHECK(info.pub_implicit);
  CHECK(info.sub_topic == std::vector<std::string>{"s/out"});
}

TEST_CASE("topology_edges: agent_topic_infos: skips agents and broker, keeps tables only",
          "[topology_edges]") {
  auto cfg = toml::parse(R"(
title = "not a table"

[agents]
x = 1

[broker]
backend_address = "tcp://127.0.0.1:5556"

[sensor]
pub_topic = "s/out"
sub_topic = ["a", "b"]

[filter]
sub_topic = "s/out"
)");
  auto infos = Mads::agent_topic_infos(cfg);
  REQUIRE(infos.size() == 2);
  REQUIRE(infos.count("sensor") == 1);
  REQUIRE(infos.count("filter") == 1);
  CHECK(infos.at("sensor").pub_topic == "s/out");
  CHECK_FALSE(infos.at("sensor").pub_implicit);
  CHECK(infos.at("sensor").sub_topic == std::vector<std::string>{"a", "b"});
  CHECK(infos.at("filter").pub_topic == "filter");
  CHECK(infos.at("filter").pub_implicit);
}
