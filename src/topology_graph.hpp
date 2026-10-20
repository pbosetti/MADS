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

#include "topic_match.hpp"

#include <toml++/toml.hpp>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
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

/**
 * @brief Extracts one `mads.ini` agent section's topics.
 *
 * Reads `pub_topic` and `sub_topic` from `table`, the body of the section
 * named `section`. A missing `pub_topic` key falls back to the section name
 * and sets `pub_implicit`, mirroring `Agent`'s own
 * `cfg["pub_topic"].value_or(_name)` default. `sub_topic` may be a single
 * string or an array of strings; any other type leaves `sub_topic` empty.
 *
 * @param section Section name, i.e. the key the table sits under.
 * @param table The section's table.
 * @return The section's topics.
 */
AgentTopicInfo agent_topic_info(std::string_view section,
                                const toml::table &table);

/**
 * @brief Extracts the topics of every agent section in a settings file.
 *
 * Loops the top-level nodes of `config`, skipping the reserved `agents` and
 * `broker` sections and any node that is not a table. Each remaining table
 * is handed to agent_topic_info().
 *
 * @param config The whole parsed settings file.
 * @return Map of section name -> topics, alphabetically ordered.
 */
std::map<std::string, AgentTopicInfo>
agent_topic_infos(const toml::table &config);

/// A live topic -> subscriber-count table as published by a broker running
/// with `[broker] subscription_table = true` (see
/// Mads::fetch_subscription_table()).
///
/// Keys are the prefixes agents actually passed to
/// `zmq_setsockopt(ZMQ_SUBSCRIBE)`, i.e. a declared `sub_topic` entry after
/// Mads::literal_prefix() -- so a wildcard entry appears under its literal
/// prefix, never as the pattern. The table carries NO peer identity (ZMQ
/// subscription frames have none), so it can only ever answer "is anyone
/// subscribed to this prefix", never "which agent".
using LiveSubscriptions = std::map<std::string, int>;

/// Topics MADS subscribes to programmatically rather than through a declared
/// `sub_topic`, plus the one this very query creates. They appear in a live
/// table with nobody having declared them, and reporting them as undeclared
/// subscribers would cry wolf on every single run:
///
/// - `control`   -- pushed onto _sub_topic by Agent::enable_remote_control()
/// - `subscriptions` -- the reader of the table is itself a subscriber
bool is_implicitly_subscribed_topic(const std::string &topic);

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

  /// When set, overlay live subscriber counts onto the declared graph
  /// (`mads doctor --graph --graph-live`). This annotates, never replaces:
  /// the nodes and edges still come from the settings file, because an
  /// anonymous topic -> count table cannot name agents or identify
  /// publishers and so cannot draw a graph by itself.
  ///
  /// Three states become distinguishable that are identical without it:
  /// a declared subscription with someone listening, a declared
  /// subscription with nobody listening, and a live subscription nobody
  /// declared (invisible entirely on a settings-only graph).
  ///
  /// Note the overlay speaks only about *subscribers*. A pure publisher's
  /// liveness leaves no trace in a subscription table, so a source is never
  /// reported as offline -- only as unheard, which the declared graph
  /// already shows.
  std::optional<LiveSubscriptions> live;
};

/// One publisher -> subscriber arrow of the declared topology, as
/// topology_graph() draws it. Produced by topology_edges(), which is the
/// structured form a GUI can reuse without parsing DOT text.
struct TopologyEdge {
  /// Publisher section.
  std::string from;
  /// Subscriber section.
  std::string to;
  /// The publisher's (possibly implicit) pub_topic.
  std::string topic;
  /// The strongest match kind among the subscriber's matching sub_topic
  /// entries, ranked Exact > Wildcard > Prefix; the same rule DOT uses to
  /// pick solid / dashed / dotted.
  SubMatch kind = SubMatch::None;
  /// Every matching sub_topic entry of the subscriber, in declaration order,
  /// filled only when `kind` is not Exact (an exact edge needs no "via"
  /// line). Entries are kept verbatim: the subscribe-all entry is "" here,
  /// and DOT renders it as "(all)".
  std::vector<std::string> via_patterns;
  /// True when `topic` came from the implicit pub_topic default
  /// (AgentTopicInfo::pub_implicit). DOT draws such edges in grey.
  bool implicit_pub = false;
  /// True when the subscriber matched ONLY through its subscribe-all ("")
  /// entry. topology_graph() collapses these into a count on the
  /// subscriber's `(all)` line unless GraphOptions::expand_catch_all is set.
  bool catch_all = false;
};

/**
 * @brief Computes every publisher -> subscriber edge of the declared
 * topology, in the order topology_graph() emits them.
 *
 * Outer loop over publishers (section name order), inner loop over
 * subscribers (section name order). A publisher must have a non-empty
 * pub_topic and a subscriber must have at least one sub_topic entry. Each
 * pair is tested with Mads::subscription_match() against every entry, so
 * prefix, wildcard and exact matches all count, exactly as on the wire.
 *
 * Self-pairs (a section matching its own sub_topic) never produce an edge;
 * they do count toward satisfaction, which dangling_topics() accounts for.
 * Catch-all edges are returned too (with `catch_all` set): callers that
 * want the default collapsed rendering filter them out themselves.
 *
 * @param agents Map of section name -> declared topics.
 * @return All edges, possibly empty.
 */
std::vector<TopologyEdge>
topology_edges(const std::map<std::string, AgentTopicInfo> &agents);

/// The dangling topics of a declared topology: the declarations that no
/// counterpart on the wire reaches. This is exactly what topology_graph()
/// marks with `[!]` and what turns a node's contour dashed.
struct DanglingTopics {
  /// Sections whose declared (non-implicit) pub_topic reaches no subscriber.
  /// Implicit pub_topic defaults are never listed here.
  std::set<std::string> pub;
  /// Section -> its dead sub_topic entries (entries no publisher reaches).
  /// Sections with no dead entries have no key. Entries are stored by text:
  /// satisfaction depends only on the text, so duplicate entries share one
  /// verdict.
  std::map<std::string, std::set<std::string>> sub;
};

/**
 * @brief Computes the dangling pub_topic and sub_topic declarations of a
 * declared topology.
 *
 * A declared pub_topic is satisfied when at least one subscriber entry
 * matches it, a sub_topic entry when at least one publisher's pub_topic
 * matches it. Both checks run over every section pair, self-pairs included,
 * so an agent whose pub_topic matches its own sub_topic is satisfied on
 * both sides, matching real broker behavior.
 *
 * @param agents Map of section name -> declared topics.
 * @return The dangling declarations; both members are empty for a fully
 * wired topology.
 */
DanglingTopics
dangling_topics(const std::map<std::string, AgentTopicInfo> &agents);

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
