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

// Everything the match pass works out about one agent, kept together so the
// node-emission loop below is a straight read with no second matching sweep.
struct NodeState {
  // Per sub_topic entry: was it satisfied by at least one publisher? A
  // section can declare several entries and only one need be dead to be
  // worth flagging, so this is tracked per entry rather than per agent.
  std::vector<bool> sub_satisfied;
  // Declared pub_topic reached at least one subscriber. Implicit pub topics
  // are left `true` -- nobody listening to a sink's defaulted topic is
  // normal, not a misconfiguration.
  bool pub_matched = true;
  // Other sections reaching this one *only* through its subscribe-all entry,
  // i.e. the edges GraphOptions::expand_catch_all would draw and the default
  // rendering shows as a count on the `(all)` line instead.
  std::size_t catch_all_pubs = 0;
};

// One drawn arrow, plus how it came to be. `patterns` lists every sub_topic
// entry of the target that accepts this topic (usually one; more than one is
// itself worth seeing, e.g. `["sensors", "sensors/#"]` both catching the same
// message), and `kind` is the most explicit of them, ranked
// Exact > Wildcard > Prefix: an exact entry means the wiring was spelled out,
// a wildcard means it was asked for on purpose, and a bare prefix catch is
// the incidental one, so it only wins when nothing else matched.
struct Edge {
  std::string from;
  std::string to;
  std::string topic;
  std::vector<std::string> patterns;
  SubMatch kind = SubMatch::None;
  bool implicit_pub = false;
  // Every matching pattern is the subscribe-all one: this arrow carries no
  // information the target's `(all)` line doesn't already state, so it is
  // collapsed into a count unless GraphOptions::expand_catch_all is set.
  bool catch_all_only = false;
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
  if (!state.pub_matched) {
    top += "\\lpub " + escape_topic(info.pub_topic) + " [!]";
  }
  if (info.sub_topic.empty()) {
    return top;
  }
  std::string label = top + "|{";
  for (std::size_t i = 0; i < info.sub_topic.size(); ++i) {
    label += pattern_text(info.sub_topic[i]);
    if (!state.sub_satisfied[i]) {
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

std::string edge_label(const Edge &edge) {
  std::string label = escape_topic(edge.topic);
  // An exact match adds nothing by repeating itself: the pattern *is* the
  // topic. Every looser match names what caught it, which is the whole point
  // of drawing the graph in the first place.
  if (edge.kind != SubMatch::Exact) {
    label += "\\lvia ";
    for (std::size_t i = 0; i < edge.patterns.size(); ++i) {
      if (i > 0) {
        label += ", ";
      }
      label += pattern_text(edge.patterns[i]);
    }
    label += "\\l";
  }
  return label;
}

std::string pad(const std::string &s, std::size_t width) {
  std::string r = s;
  if (r.size() < width) {
    r.append(width - r.size(), ' ');
  }
  return r;
}

} // namespace

std::string topology_graph(const std::map<std::string, AgentTopicInfo> &agents,
                           const GraphOptions &options) {
  // Single combined pass over every (publisher, subscriber) pair --
  // INCLUDING self-pairs, so an agent's own pub_topic matching its own
  // sub_topic satisfies both sides without ever becoming a drawn edge --
  // that simultaneously: (a) records which sections' pub_topic matched at
  // least one subscriber (for pub-side dangling detection), (b) records,
  // per subscriber and per sub_topic entry, whether that specific pattern
  // was satisfied by at least one publisher (for sub-side dangling
  // detection), and (c) collects the cross-section edges to draw, together
  // with the patterns and match kind that produced each one. This is the one
  // match pass everything downstream reads, rather than a separate O(n^2)
  // sweep per question.
  std::map<std::string, NodeState> states;
  for (const auto &[name, info] : agents) {
    NodeState state;
    state.sub_satisfied.assign(info.sub_topic.size(), false);
    state.pub_matched = info.pub_topic.empty() || info.pub_implicit;
    states.emplace(name, std::move(state));
  }

  std::vector<Edge> edges;

  for (const auto &[a_name, a_info] : agents) {
    if (a_info.pub_topic.empty()) {
      continue;
    }
    for (const auto &[b_name, b_info] : agents) {
      if (b_info.sub_topic.empty()) {
        continue;
      }
      Edge edge;
      edge.from = a_name;
      edge.to = b_name;
      edge.topic = a_info.pub_topic;
      edge.implicit_pub = a_info.pub_implicit;

      auto &b_state = states[b_name];
      bool matched_named_pattern = false;
      for (std::size_t i = 0; i < b_info.sub_topic.size(); ++i) {
        const SubMatch kind =
            Mads::subscription_match(b_info.sub_topic[i], a_info.pub_topic);
        if (kind == SubMatch::None) {
          continue;
        }
        b_state.sub_satisfied[i] = true;
        edge.patterns.push_back(b_info.sub_topic[i]);
        matched_named_pattern =
            matched_named_pattern || !b_info.sub_topic[i].empty();
        if (match_rank(kind) > match_rank(edge.kind)) {
          edge.kind = kind;
        }
      }
      if (edge.kind == SubMatch::None) {
        continue;
      }
      // Set before any collapsing: whether the arrow ends up drawn has no
      // bearing on whether the topic found a listener.
      states[a_name].pub_matched = true;
      if (a_name == b_name) {
        continue;
      }
      edge.catch_all_only = !matched_named_pattern;
      if (edge.catch_all_only && !options.expand_catch_all) {
        ++b_state.catch_all_pubs;
        continue;
      }
      edges.push_back(std::move(edge));
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
  out << "\n";

  for (const auto &[name, info] : agents) {
    const NodeState &state = states.at(name);
    const bool sub_dangling =
        std::find(state.sub_satisfied.begin(), state.sub_satisfied.end(),
                  false) != state.sub_satisfied.end();

    out << "  " << pad(name, node_name_width) << " [label=\""
        << record_label(name, info, state) << "\"";
    if (const char *color = role_color(infer_role(info))) {
      out << ", color=" << color;
    }
    if (!state.pub_matched || sub_dangling) {
      out << ", style=dashed";
    }
    out << "];\n";
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
