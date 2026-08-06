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

/// One `mads.ini` agent section's topics.
struct AgentTopicInfo {
  /// The topic this section publishes on. Empty means the section publishes
  /// nothing at all; see `pub_implicit` for the (common) case of a section
  /// that declares no `pub_topic` key but still publishes at runtime.
  std::string pub_topic;
  /// Empty means no `sub_topic` key (no subscriptions). A single empty-string
  /// entry (`sub_topic = [""]`) is the subscribe-all convention used
  /// throughout MADS (see Agent::connect_sub()) and is rendered/matched
  /// accordingly.
  std::vector<std::string> sub_topic;
  /// True when `pub_topic` was not declared in the settings file and was
  /// filled in with the section's own name, mirroring `Agent`'s own
  /// `cfg["pub_topic"].value_or(_name)` default. Such a topic still produces
  /// real edges -- a filter that never declares `pub_topic` genuinely
  /// publishes under its section name -- but it is drawn muted and is never
  /// reported as a dangling publisher, since a pure sink gets the same
  /// default and nobody listening to it is normal, not a misconfiguration.
  bool pub_implicit = false;
};

/// Rendering knobs for topology_graph().
struct GraphOptions {
  /// Draw one edge per publisher into every subscribe-all (`sub_topic =
  /// [""]`) subscriber, instead of collapsing them into a count shown on the
  /// subscriber's own `(all)` line. Off by default: a subscribe-all
  /// subscriber matches *every* publisher by definition, so in any real
  /// deployment (a logger, a monitor, a recorder...) those edges are both
  /// the least informative and the most numerous, and they bury the wiring
  /// the graph is actually being read for. Nothing is lost by collapsing
  /// them -- "receives everything" is exactly what the `(all)` line says.
  bool expand_catch_all = false;
};

/**
 * @brief Builds Graphviz DOT text describing the declared pub/sub topology
 * of a set of agent sections.
 *
 * One record-shaped node per entry, colored by inferred role (source =
 * declared `pub_topic` only -> darkred; filter = both -> darkgreen; sink =
 * `sub_topic` only -> darkblue; neither -> no color attribute; the role is
 * always read off the *declared* keys, so an implicit `pub_topic` never
 * turns a sink green), with a dashed contour on any agent that has a
 * dangling topic: a declared `pub_topic` nothing subscribes to, or a
 * `sub_topic` entry nothing ever publishes to. Every dangling topic is also
 * marked in the node's own label with a trailing `[!]`, so which entry is
 * dead is visible without cross-referencing the edges. An agent's own
 * `pub_topic` matching its own `sub_topic` -- a self-loop -- counts as
 * satisfied on both sides, matching real broker behavior.
 *
 * One edge is drawn per matching (publisher, subscriber) section pair, where
 * "matching" is Mads::subscription_match(), i.e. exactly what the agent
 * would receive on the wire: literal `sub_topic` entries match by byte
 * prefix (`sensors` receives `sensors/imu/raw`; `""` receives everything)
 * and wildcard entries match per MQTT rules. Because those looser matches
 * are precisely what a topology graph is for, how the edge came to exist is
 * encoded in it: solid for an exact match, dashed for a prefix catch, dotted
 * for a wildcard, and any non-exact edge names the responsible pattern(s) in
 * its label (`topic` over `via pattern, ...`, with the subscribe-all pattern
 * shown as `(all)`). An edge published under an implicit `pub_topic` is
 * additionally drawn in grey. Edges are only drawn between distinct sections
 * (a self-match never produces a self-loop edge, though it still counts
 * toward the dangling computation above).
 *
 * @param agents Map of section name -> declared topics. A `std::map` is
 * used (rather than `unordered_map`) so node/edge emission order is
 * deterministic (alphabetical by section name), which keeps output diffable
 * and unit tests exact-string-comparable.
 * @param options Rendering knobs; see GraphOptions.
 * @return The complete `digraph mads { ... }` DOT text, newline-terminated.
 */
std::string topology_graph(const std::map<std::string, AgentTopicInfo> &agents,
                           const GraphOptions &options = {});

} // namespace Mads
