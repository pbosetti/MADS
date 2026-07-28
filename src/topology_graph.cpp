#include "topology_graph.hpp"

#include "topic_match.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
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

// Subscribe-all convention (`sub_topic = [""]`): at the ZMQ layer this is
// issued as an empty-prefix SUBSCRIBE frame, which byte-prefix-matches every
// topic -- NOT the same thing as Mads::topic_match()'s own literal-equality
// fallback for an empty pattern (which only matches an empty topic; see
// test_topic_match.cpp's `{"", "sensors", false}` case). This wrapper
// restores that runtime fidelity for the graph, exactly mirroring
// Agent::connect_sub()'s two paths (raw ZMQ subscribe for literals,
// Mads::topic_match() only for '+'/'#' patterns).
bool topic_or_subscribe_all_matches(const std::string &sub_pattern,
                                    const std::string &pub_topic) {
  if (sub_pattern.empty()) {
    return true;
  }
  return Mads::topic_match(sub_pattern, pub_topic);
}

enum class Role { Source, Filter, Sink, Neither };

Role infer_role(const AgentTopicInfo &info) {
  const bool has_pub = !info.pub_topic.empty();
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

std::string record_label(const std::string &name, const AgentTopicInfo &info) {
  std::string label = "{" + name + "|";
  if (info.sub_topic.size() == 1 && info.sub_topic[0].empty()) {
    label += "(all)\\l";
  } else {
    for (const auto &t : info.sub_topic) {
      label += escape_topic(t) + "\\l";
    }
  }
  label += "}";
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

std::string topology_graph(const std::map<std::string, AgentTopicInfo> &agents) {
  // Single combined pass over every (publisher, subscriber) pair --
  // INCLUDING self-pairs, so an agent's own pub_topic matching its own
  // sub_topic satisfies both sides without ever becoming a drawn edge --
  // that simultaneously: (a) records which sections' pub_topic matched at
  // least one subscriber (for pub-side dangling detection), (b) records,
  // per subscriber and per sub_topic entry, whether that specific pattern
  // was satisfied by at least one publisher (for sub-side dangling
  // detection, since a section can have several sub_topic entries and only
  // one need be dangling to warrant a dashed contour), and (c) collects the
  // cross-section edges to draw. This is the one match pass the dangling
  // computation reuses, rather than a separate O(n^2) sweep.
  std::set<std::string> pub_matched;
  std::map<std::string, std::vector<bool>> sub_pattern_satisfied;
  for (const auto &[name, info] : agents) {
    if (!info.sub_topic.empty()) {
      sub_pattern_satisfied[name] = std::vector<bool>(info.sub_topic.size(), false);
    }
  }

  std::vector<std::tuple<std::string, std::string, std::string>> edges;

  for (const auto &[a_name, a_info] : agents) {
    if (a_info.pub_topic.empty()) {
      continue;
    }
    for (const auto &[b_name, b_info] : agents) {
      if (b_info.sub_topic.empty()) {
        continue;
      }
      bool pair_matched = false;
      auto &flags = sub_pattern_satisfied[b_name];
      for (std::size_t i = 0; i < b_info.sub_topic.size(); ++i) {
        if (topic_or_subscribe_all_matches(b_info.sub_topic[i], a_info.pub_topic)) {
          pair_matched = true;
          flags[i] = true;
        }
      }
      if (!pair_matched) {
        continue;
      }
      pub_matched.insert(a_name);
      if (a_name != b_name) {
        edges.emplace_back(a_name, b_name, a_info.pub_topic);
      }
    }
  }

  std::size_t node_name_width = 0;
  for (const auto &[name, info] : agents) {
    node_name_width = std::max(node_name_width, name.size());
  }

  std::ostringstream out;
  out << "digraph mads {\n";
  out << "  rankdir=LR;\n";
  out << "  node [shape=record, fontname=\"monospace\"];\n\n";

  for (const auto &[name, info] : agents) {
    const bool pub_dangling = !info.pub_topic.empty() && !pub_matched.count(name);
    bool sub_dangling = false;
    if (auto it = sub_pattern_satisfied.find(name); it != sub_pattern_satisfied.end()) {
      for (bool satisfied : it->second) {
        if (!satisfied) {
          sub_dangling = true;
          break;
        }
      }
    }

    out << "  " << pad(name, node_name_width) << " [label=\""
        << record_label(name, info) << "\"";
    if (const char *color = role_color(infer_role(info))) {
      out << ", color=" << color;
    }
    if (pub_dangling || sub_dangling) {
      out << ", style=dashed";
    }
    out << "];\n";
  }

  if (!edges.empty()) {
    std::size_t src_width = 0;
    for (const auto &[a, b, label] : edges) {
      src_width = std::max(src_width, a.size());
    }
    out << "\n";
    for (const auto &[a, b, label] : edges) {
      out << "  " << pad(a, src_width) << " -> " << b << " [label=\""
          << escape_topic(label) << "\"];\n";
    }
  }

  out << "}\n";
  return out.str();
}

} // namespace Mads
