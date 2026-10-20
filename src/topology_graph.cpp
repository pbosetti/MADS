#include "topology_graph.hpp"

#include "topic_match.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace Mads {

namespace {

// Escapes DOT record-label metacharacters ('{', '}', '|', '<', '>') and
// backslash itself, so a topic string can never corrupt record-label syntax
// once embedded in a node's label. Applied to every topic string emitted
// (node sub_topic entries and edge pub_topic labels alike) rather than only
// inside record labels proper -- simpler than tracking two escaping rules,
// and harmless for a plain edge label since none of these characters occur
// in a real topic today (see topology_graph.hpp).
std::string escape_topic(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
    case '\\':
    case '{':
    case '}':
    case '|':
    case '<':
    case '>':
      out += '\\';
      out += c;
      break;
    default:
      out += c;
    }
  }
  return out;
}

// How a sub_topic entry reads in a label: the subscribe-all convention
// (`sub_topic = [""]`) has no text of its own, so it gets a name.
std::string pattern_text(const std::string &pattern) {
  return pattern.empty() ? "(all)" : escape_topic(pattern);
}

std::string plural(std::size_t n, const std::string &noun) {
  return std::to_string(n) + " " + noun + (n == 1 ? "" : "s");
}

enum class Role { Source, Filter, Sink, Neither };

// Role is inferred from what the section *declares*, never from the implicit
// pub_topic default: a sink that omits pub_topic still inherits its own name
// as a publish topic at runtime (see AgentTopicInfo::pub_implicit), and
// letting that recolor it green would make every sink look like a filter.
Role infer_role(const AgentTopicInfo &info) {
  const bool has_pub = !info.pub_topic.empty() && !info.pub_implicit;
  const bool has_sub = !info.sub_topic.empty();
  if (has_pub && has_sub) {
    return Role::Filter;
  }
  if (has_pub) {
    return Role::Source;
  }
  if (has_sub) {
    return Role::Sink;
  }
  return Role::Neither;
}

// nullptr means "omit the color attribute entirely" (Role::Neither).
const char *role_color(Role role) {
  switch (role) {
  case Role::Source:
    return "darkred";
  case Role::Filter:
    return "darkgreen";
  case Role::Sink:
    return "darkblue";
  case Role::Neither:
    return nullptr;
  }
  return nullptr;
}

// Everything the rendering pass needs to know about one agent, kept together
// so the node-emission loop below is a straight read.
struct NodeState {
  // Declared pub_topic reaches no subscriber (see DanglingTopics::pub).
  bool pub_dangling = false;
  // Entries of this section's sub_topic that no publisher reaches, stored by
  // text (see DanglingTopics::sub).
  std::set<std::string> dead_subs;
  // Per sub_topic entry: how many live subscribers the broker currently
  // reports for the prefix that entry subscribes. Left EMPTY when no live
  // overlay was requested, which is what every render path tests on.
  std::vector<int> sub_live;
  // Other sections reaching this one *only* through its subscribe-all entry,
  // i.e. the edges GraphOptions::expand_catch_all would draw and the default
  // rendering shows as a count on the `(all)` line instead.
  std::size_t catch_all_pubs = 0;
};

int match_rank(SubMatch kind) {
  switch (kind) {
  case SubMatch::Exact:
    return 3;
  case SubMatch::Wildcard:
    return 2;
  case SubMatch::Prefix:
    return 1;
  case SubMatch::None:
    return 0;
  }
  return 0;
}

// Solid / dashed / dotted for exact / prefix / wildcard. Returns nullptr for
// the exact case, i.e. "emit no style attribute at all".
const char *edge_style(SubMatch kind) {
  switch (kind) {
  case SubMatch::Prefix:
    return "dashed";
  case SubMatch::Wildcard:
    return "dotted";
  case SubMatch::Exact:
  case SubMatch::None:
    return nullptr;
  }
  return nullptr;
}

std::string record_label(const std::string &name, const AgentTopicInfo &info,
                         const NodeState &state) {
  std::string top = escape_topic(name);
  // A declared pub_topic that reaches nobody is otherwise invisible: it never
  // becomes an edge, so without this line the node would be dashed with no
  // hint of which topic is falling on deaf ears.
  if (state.pub_dangling) {
    top += "\\lpub " + escape_topic(info.pub_topic) + " [!]";
  }
  if (info.sub_topic.empty()) {
    return top;
  }
  std::string label = top + "|{";
  for (std::size_t i = 0; i < info.sub_topic.size(); ++i) {
    label += pattern_text(info.sub_topic[i]);
    // The live marker is orthogonal to the dangling one and both can show
    // at once: "[!] [offline]" reads "nothing publishes here, and nobody is
    // listening either".
    if (!state.sub_live.empty()) {
      const int n = state.sub_live[i];
      label += n > 0 ? " [live " + std::to_string(n) + "]" : " [offline]";
    }
    if (state.dead_subs.count(info.sub_topic[i]) > 0) {
      label += " [!]";
    } else if (info.sub_topic[i].empty() && state.catch_all_pubs > 0) {
      // Collapsed fan-in: the arrows aren't drawn, so say how many there
      // would be. Only reachable when expand_catch_all is off, since the
      // counter is left at zero otherwise.
      label += " [" + plural(state.catch_all_pubs, "publisher") + "]";
    }
    label += "\\l";
  }
  label += "}";
  return label;
}

std::string edge_label(const TopologyEdge &edge) {
  std::string label = escape_topic(edge.topic);
  // An exact match adds nothing by repeating itself: the pattern *is* the
  // topic. Every looser match names what caught it, which is the whole point
  // of drawing the graph in the first place.
  if (edge.kind != SubMatch::Exact) {
    label += "\\lvia ";
    for (std::size_t i = 0; i < edge.via_patterns.size(); ++i) {
      if (i > 0) {
        label += ", ";
      }
      label += pattern_text(edge.via_patterns[i]);
    }
    label += "\\l";
  }
  return label;
}

// The prefix a declared sub_topic entry actually hands to
// zmq_setsockopt(ZMQ_SUBSCRIBE) -- and therefore the exact key it appears
// under in a live subscription table. A wildcard entry subscribes only its
// literal prefix at the ZMQ layer (Agent::connect_sub()), so comparing raw
// pattern strings against table keys would never match.
std::string zmq_subscribe_key(const std::string &sub_entry) {
  return Mads::has_wildcard(sub_entry) ? Mads::literal_prefix(sub_entry)
                                       : sub_entry;
}

// Absent from the table means nobody is subscribed: the broker erases an
// entry once its refcount reaches zero.
int live_count(const LiveSubscriptions &live, const std::string &key) {
  const auto it = live.find(key);
  return it == live.end() ? 0 : it->second;
}

std::string pad(const std::string &s, std::size_t width) {
  std::string r = s;
  if (r.size() < width) {
    r.append(width - r.size(), ' ');
  }
  return r;
}

} // namespace

bool is_implicitly_subscribed_topic(const std::string &topic) {
  // "control" is pushed onto _sub_topic by Agent::enable_remote_control(),
  // never declared in mads.ini; "subscriptions" is the table's own topic, so
  // whoever is reading the table shows up in it.
  return topic == "control" || topic == "subscriptions";
}

AgentTopicInfo agent_topic_info(std::string_view section,
                                const toml::table &table) {
  AgentTopicInfo info;
  if (auto pub = table["pub_topic"].value<std::string>()) {
    info.pub_topic = *pub;
  } else {
    // Agent::init() reads `cfg["pub_topic"].value_or(_name)`, so a section
    // with no pub_topic key still publishes -- under its own name. Filters
    // routinely rely on that (`[running_avg] sub_topic = ["serial_reader"]`
    // with no pub_topic publishes "running_avg"), and omitting it here used
    // to drop those edges from the graph entirely. Flagged as implicit so
    // topology_graph() can draw it muted and never call it dangling: a pure
    // sink inherits exactly the same default and nobody listening to it is
    // the normal case.
    info.pub_topic = std::string(section);
    info.pub_implicit = true;
  }
  const auto &sub_node = table["sub_topic"];
  if (sub_node.type() == toml::node_type::string) {
    info.sub_topic.push_back(sub_node.value_or(std::string("")));
  } else if (const auto *arr = sub_node.as_array()) {
    arr->for_each(
        [&](auto &&el) { info.sub_topic.push_back(el.value_or(std::string(""))); });
  }
  return info;
}

std::map<std::string, AgentTopicInfo>
agent_topic_infos(const toml::table &config) {
  // Every top-level table is an agent section, except the reserved 'agents'
  // and 'broker' sections; the plugin `attachment` scan in doctor follows the
  // same convention.
  std::map<std::string, AgentTopicInfo> agents;
  for (const auto &[key, node] : config) {
    const std::string section = std::string(key.str());
    if (section == "agents" || section == "broker") {
      continue;
    }
    const auto *table = node.as_table();
    if (!table) {
      continue;
    }
    agents.emplace(section, agent_topic_info(section, *table));
  }
  return agents;
}

std::vector<TopologyEdge>
topology_edges(const std::map<std::string, AgentTopicInfo> &agents) {
  std::vector<TopologyEdge> edges;
  for (const auto &[a_name, a_info] : agents) {
    if (a_info.pub_topic.empty()) {
      continue;
    }
    for (const auto &[b_name, b_info] : agents) {
      // A self-match is satisfied wiring, not an arrow (see
      // dangling_topics()), so it never reaches the edge list.
      if (b_info.sub_topic.empty() || a_name == b_name) {
        continue;
      }
      TopologyEdge edge;
      edge.from = a_name;
      edge.to = b_name;
      edge.topic = a_info.pub_topic;
      edge.implicit_pub = a_info.pub_implicit;

      bool matched_named_pattern = false;
      std::vector<std::string> matched;
      for (const auto &entry : b_info.sub_topic) {
        const SubMatch kind = Mads::subscription_match(entry, a_info.pub_topic);
        if (kind == SubMatch::None) {
          continue;
        }
        matched.push_back(entry);
        matched_named_pattern = matched_named_pattern || !entry.empty();
        if (match_rank(kind) > match_rank(edge.kind)) {
          edge.kind = kind;
        }
      }
      if (edge.kind == SubMatch::None) {
        continue;
      }
      edge.catch_all = !matched_named_pattern;
      if (edge.kind != SubMatch::Exact) {
        edge.via_patterns = std::move(matched);
      }
      edges.push_back(std::move(edge));
    }
  }
  return edges;
}

DanglingTopics
dangling_topics(const std::map<std::string, AgentTopicInfo> &agents) {
  // One pass over every (publisher, subscriber) pair -- INCLUDING self-pairs,
  // so an agent's own pub_topic matching its own sub_topic satisfies both
  // sides -- recording which declarations were reached at least once.
  std::set<std::string> pub_reached;
  std::map<std::string, std::vector<bool>> sub_reached;
  for (const auto &[name, info] : agents) {
    sub_reached[name].assign(info.sub_topic.size(), false);
  }
  for (const auto &[a_name, a_info] : agents) {
    if (a_info.pub_topic.empty()) {
      continue;
    }
    for (const auto &[b_name, b_info] : agents) {
      auto &reached = sub_reached[b_name];
      for (std::size_t i = 0; i < b_info.sub_topic.size(); ++i) {
        if (Mads::subscription_match(b_info.sub_topic[i], a_info.pub_topic) !=
            SubMatch::None) {
          reached[i] = true;
          pub_reached.insert(a_name);
        }
      }
    }
  }

  DanglingTopics result;
  for (const auto &[name, info] : agents) {
    // An implicit pub_topic is never dangling: a pure sink inherits the same
    // default and nobody listening to it is the normal case.
    if (!info.pub_topic.empty() && !info.pub_implicit &&
        pub_reached.count(name) == 0) {
      result.pub.insert(name);
    }
    const auto &reached = sub_reached[name];
    for (std::size_t i = 0; i < info.sub_topic.size(); ++i) {
      if (!reached[i]) {
        result.sub[name].insert(info.sub_topic[i]);
      }
    }
  }
  return result;
}

std::string topology_graph(const std::map<std::string, AgentTopicInfo> &agents,
                           const GraphOptions &options) {
  const std::vector<TopologyEdge> all_edges = topology_edges(agents);
  const DanglingTopics dangling = dangling_topics(agents);

  std::map<std::string, NodeState> states;
  for (const auto &[name, info] : agents) {
    NodeState state;
    state.pub_dangling = dangling.pub.count(name) > 0;
    const auto dead = dangling.sub.find(name);
    if (dead != dangling.sub.end()) {
      state.dead_subs = dead->second;
    }
    if (options.live) {
      state.sub_live.reserve(info.sub_topic.size());
      for (const auto &entry : info.sub_topic) {
        state.sub_live.push_back(
            live_count(*options.live, zmq_subscribe_key(entry)));
      }
    }
    states.emplace(name, std::move(state));
  }

  // Catch-all edges are collapsed into a count on the subscriber's `(all)`
  // line unless the caller asked for every arrow.
  std::vector<TopologyEdge> edges;
  for (const auto &edge : all_edges) {
    if (edge.catch_all && !options.expand_catch_all) {
      ++states[edge.to].catch_all_pubs;
      continue;
    }
    edges.push_back(edge);
  }

  // Live subscriptions nobody declared: the one case a settings-only graph
  // cannot show at all. A table key is "declared" when some section's
  // sub_topic entry subscribes exactly that prefix; MADS's own programmatic
  // subscriptions are excluded so they do not cry wolf on every run.
  std::vector<std::string> undeclared_live;
  if (options.live) {
    std::set<std::string> declared_keys;
    for (const auto &[name, info] : agents) {
      for (const auto &entry : info.sub_topic) {
        declared_keys.insert(zmq_subscribe_key(entry));
      }
    }
    for (const auto &[topic, count] : *options.live) {
      if (count <= 0 || declared_keys.count(topic) > 0 ||
          is_implicitly_subscribed_topic(topic)) {
        continue;
      }
      undeclared_live.push_back(topic);
    }
  }

  std::size_t node_name_width = 0;
  for (const auto &[name, info] : agents) {
    node_name_width = std::max(node_name_width, name.size());
  }

  std::size_t collapsed = 0;
  for (const auto &[name, state] : states) {
    collapsed += state.catch_all_pubs;
  }

  std::ostringstream out;
  out << "digraph mads {\n";
  out << "  rankdir=LR;\n";
  out << "  node [shape=record, fontname=\"monospace\", fontsize=8];\n";
  out << "  edge [fontname=\"monospace\", fontsize=8];\n";
  // A DOT comment: invisible in the rendered image, but right there for
  // anyone reading (or diffing) the text and wondering where the arrows into
  // the subscribe-all subscribers went.
  if (collapsed > 0) {
    out << "  // " << plural(collapsed, "fan-in edge")
        << " into subscribe-all subscribers collapsed into the '(all)'\n"
        << "  // counts below; re-run with --graph-fanout to draw them.\n";
  }
  if (options.live) {
    out << "  // live overlay: [live N] = N subscriber(s) on the wire now,\n"
        << "  // [offline] = declared but nobody subscribed. Counts are\n"
        << "  // anonymous, so they name no agent; publishers leave no trace\n"
        << "  // in a subscription table and are never marked offline.\n";
  }
  out << "\n";

  for (const auto &[name, info] : agents) {
    const NodeState &state = states.at(name);
    out << "  " << pad(name, node_name_width) << " [label=\""
        << record_label(name, info, state) << "\"";
    if (const char *color = role_color(infer_role(info))) {
      out << ", color=" << color;
    }
    if (state.pub_dangling || !state.dead_subs.empty()) {
      out << ", style=dashed";
    }
    out << "];\n";
  }

  // Synthetic nodes for live subscriptions with no declaration behind them.
  // Deliberately not named after an agent: the table is anonymous, so all
  // that can honestly be said is that *something* is listening.
  if (!undeclared_live.empty()) {
    out << "\n";
    for (std::size_t i = 0; i < undeclared_live.size(); ++i) {
      const std::string &topic = undeclared_live[i];
      out << "  __live_" << i << " [label=\"" << pattern_text(topic)
          << "\\lundeclared subscriber"
          << (live_count(*options.live, topic) == 1 ? "" : "s") << " ["
          << live_count(*options.live, topic) << "]\\l\""
          << ", shape=note, color=darkorange, fontcolor=darkorange];\n";
    }
    // Context for each: which declared publisher(s) that listener is
    // actually receiving from, inferred with the same matching rule the
    // wire uses. Dotted and orange to keep them visibly distinct from the
    // declared wiring.
    for (std::size_t i = 0; i < undeclared_live.size(); ++i) {
      for (const auto &[name, info] : agents) {
        if (info.pub_topic.empty()) {
          continue;
        }
        if (Mads::subscription_match(undeclared_live[i], info.pub_topic) ==
            SubMatch::None) {
          continue;
        }
        out << "  " << name << " -> __live_" << i
            << " [style=dotted, color=darkorange];\n";
      }
    }
  }

  if (!edges.empty()) {
    std::size_t src_width = 0;
    for (const auto &edge : edges) {
      src_width = std::max(src_width, edge.from.size());
    }
    out << "\n";
    for (const auto &edge : edges) {
      out << "  " << pad(edge.from, src_width) << " -> " << edge.to
          << " [label=\"" << edge_label(edge) << "\"";
      if (const char *style = edge_style(edge.kind)) {
        out << ", style=" << style;
      }
      // Grey marks "this arrow exists only because of the implicit
      // pub_topic default", which is a weaker claim than a declared topic.
      if (edge.implicit_pub) {
        out << ", color=gray50, fontcolor=gray50";
      }
      out << "];\n";
    }
  }

  out << "}\n";
  return out.str();
}

} // namespace Mads
