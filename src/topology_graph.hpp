/*
 _____                _                    ____                 _
|_   _|__  _ __   ___ | | ___   __ _ _   _ / ___|_ __ __ _ _ __ | |__
  | |/ _ \| '_ \ / _ \| |/ _ \ / _` | | | | |  _| '__/ _` | '_ \| '_ \
  | | (_) | |_) | (_) | | (_) | (_| | |_| | |_| | | | (_| | |_) | | | |
  |_|\___/| .__/ \___/|_|\___/ \__, |\__, |\____|_|  \__,_| .__/|_| |_|
          |_|                 |___/ |___/                 |_|

Pure DOT-graph builder backing `mads doctor --graph` (src/main/doctor.cpp).

Takes an already-parsed map of `section name -> {pub_topic, sub_topic[]}` --
one entry per `mads.ini` agent section -- and returns Graphviz DOT text
describing the declared pub/sub topology. No ZMQ, no `Agent`, no file I/O:
the ini-parsing and file/stdout writing both stay in src/main/doctor.cpp,
exactly like src/topic_match.hpp (from P2), which this module reuses
directly for edge/dangling detection so the graph is faithful to the exact
matching semantics `Agent::connect_sub()` applies at runtime.

Author(s): Paolo Bosetti
*/
#pragma once

#include <map>
#include <string>
#include <vector>

namespace Mads {

/// One `mads.ini` agent section's declared topics, as read straight out of
/// the `pub_topic`/`sub_topic` keys -- no runtime defaulting (e.g. `Agent`
/// itself defaults an absent `pub_topic` to the section's own name; this
/// struct deliberately does NOT, so an agent that never declares a
/// `pub_topic` key is correctly seen as "no pub_topic" rather than as a
/// filter/source of its own name).
struct AgentTopicInfo {
  /// Empty means the section has no `pub_topic` key at all.
  std::string pub_topic;
  /// Empty means no `sub_topic` key (no subscriptions). A single empty-string
  /// entry (`sub_topic = [""]`) is the subscribe-all convention used
  /// throughout MADS (see Agent::connect_sub()) and is rendered/matched
  /// accordingly.
  std::vector<std::string> sub_topic;
};

/**
 * @brief Builds Graphviz DOT text describing the declared pub/sub topology
 * of a set of agent sections.
 *
 * One record-shaped node per entry, colored by inferred role (source =
 * `pub_topic` only -> darkred; filter = both -> darkgreen; sink =
 * `sub_topic` only -> darkblue; neither -> no color attribute), with a
 * dashed contour on any agent that has a dangling topic: a `pub_topic`
 * nothing subscribes to, or a `sub_topic` pattern nothing ever publishes to
 * (an agent's own `pub_topic` matching its own `sub_topic` -- a self-loop --
 * counts as satisfied on both sides, matching real broker behavior). One
 * edge is drawn per matching (publisher, subscriber) section pair, labeled
 * with the publisher's `pub_topic`; edges are only drawn between distinct
 * sections (a self-match never produces a self-loop edge, though it still
 * counts toward the dangling computation above).
 *
 * @param agents Map of section name -> declared topics. A `std::map` is
 * used (rather than `unordered_map`) so node/edge emission order is
 * deterministic (alphabetical by section name), which keeps output diffable
 * and unit tests exact-string-comparable.
 * @return The complete `digraph mads { ... }` DOT text, newline-terminated.
 */
std::string topology_graph(const std::map<std::string, AgentTopicInfo> &agents);

} // namespace Mads
