// GUI-free state and operations of the mads-ini editor: see ini_editor.hpp
// for the contract.
#include "ini_editor.hpp"

#include "remote_edit.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <set>
#include <utility>

namespace Mads::Ini {

namespace {

// Undo steps kept; the oldest are dropped beyond this.
constexpr size_t kMaxHistory = 200;
// Canvas spacing of auto_layout(), in pixels.
constexpr float kLayerSpacing = 260.0f;
constexpr float kRowSpacing = 140.0f;
constexpr int kMaxRows = 6; // nodes per column before a layer wraps

bool is_document_section(const std::string &name) {
  return name == "agents" || name == "broker";
}

// A TOML basic string literal for `s`, with quotes.
std::string toml_quote(const std::string &s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\t':
      out += "\\t";
      break;
    case '\r':
      out += "\\r";
      break;
    default:
      if (c < 0x20 || c == 0x7f) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04X", c);
        out += buf;
      } else {
        out += static_cast<char>(c);
      }
    }
  }
  return out + "\"";
}

// `["a", "b"]` from parsed strings.
std::string toml_string_array(const std::vector<std::string> &items) {
  std::string out = "[";
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) {
      out += ", ";
    }
    out += toml_quote(items[i]);
  }
  return out + "]";
}

// `base`, or `base_2`, `base_3`, ... whichever is the first free section name.
std::string unique_name(const Document &doc, const std::string &base) {
  std::string name = base;
  for (int n = 2; doc.find(name) || is_document_section(name); ++n) {
    name = base + "_" + std::to_string(n);
  }
  return name;
}

// Rewrites the header lines `[old.x]` / `[[old.x]]` of a verbatim child block
// to `[new.x]` (same rule as Document::rename_section()).
void rename_child_header(std::string &child, const std::string &old_name,
                         const std::string &new_name) {
  size_t line_start = 0;
  while (line_start < child.size()) {
    size_t line_end = child.find('\n', line_start);
    if (line_end == std::string::npos) {
      line_end = child.size();
    }
    size_t pos = child.find_first_not_of(" \t", line_start);
    if (pos != std::string::npos && pos < line_end && child[pos] == '[') {
      pos = child.find_first_not_of("[ \t", pos);
      if (pos != std::string::npos &&
          child.compare(pos, old_name.size(), old_name) == 0) {
        size_t after = pos + old_name.size();
        if (after < line_end && (child[after] == '.' || child[after] == ']')) {
          child.replace(pos, old_name.size(), new_name);
          line_end += new_name.size() - old_name.size();
        }
      }
    }
    line_start = line_end + 1;
  }
}

} // namespace

Editor::Editor(const Schema &schema) : _schema(schema) { new_document(""); }

// ------------------------------------------------------------- documents

void Editor::new_document(const std::string &text) {
  _doc = Document::parse(text);
  _source.reset();
  _etag.clear();
  finish_load();
}

bool Editor::open(const std::filesystem::path &path, std::string *error) {
  return open_source(std::make_unique<FileSource>(path), error);
}

bool Editor::open_source(std::unique_ptr<Source> source, std::string *error) {
  Loaded loaded;
  try {
    loaded = source->load();
  } catch (const Remote::AuthError &) {
    throw; // the caller can ask for a password and retry
  } catch (const std::exception &err) {
    if (error) {
      *error = err.what();
    }
    return false;
  }
  _doc = Document::parse(loaded.text);
  _source = std::move(source);
  _etag = loaded.etag;
  finish_load();
  return true;
}

bool Editor::reload(std::string *error) {
  if (!_source) {
    if (error) {
      *error = "document has no source";
    }
    return false;
  }
  Loaded loaded;
  try {
    loaded = _source->load();
  } catch (const Remote::AuthError &) {
    throw;
  } catch (const std::exception &err) {
    if (error) {
      *error = err.what();
    }
    return false;
  }
  _doc = Document::parse(loaded.text);
  _etag = loaded.etag;
  finish_load();
  return true;
}

SaveResult Editor::save() {
  if (!_source) {
    SaveResult r;
    r.message = "document has no source: use save_as()";
    return r;
  }
  std::string text = _doc.serialize();
  SaveResult r = _source->save(text, _etag);
  if (r.status == SaveResult::Status::Ok) {
    _etag = r.etag;
    _saved_text = text;
  }
  return r;
}

SaveResult Editor::save_as(const std::filesystem::path &path) {
  auto source = std::make_unique<FileSource>(path);
  std::string text = _doc.serialize();
  SaveResult r = source->save(text, "");
  if (r.status == SaveResult::Status::Ok) {
    _source = std::move(source);
    _etag = r.etag;
    _saved_text = text;
  }
  return r;
}

SaveResult Editor::save_overwrite() {
  if (!_source) {
    SaveResult r;
    r.message = "document has no source: use save_as()";
    return r;
  }
  std::string text = _doc.serialize();
  SaveResult r = _source->save(text, "");
  if (r.status == SaveResult::Status::Ok) {
    _etag = r.etag;
    _saved_text = text;
  }
  return r;
}

std::string Editor::location() const {
  return _source ? _source->describe() : "untitled";
}

bool Editor::dirty() const { return _doc.serialize() != _saved_text; }

bool Editor::changed_on_disk() {
  if (!_source) {
    return false;
  }
  try {
    return _source->current_etag() != _etag;
  } catch (...) {
    return false;
  }
}

// --------------------------------------------------------------- history

void Editor::reset_history() {
  _history.clear();
  _history.push_back({_doc.serialize(), ""});
  _cursor = 0;
}

void Editor::commit(const std::string &what) {
  std::string text = _doc.serialize();
  if (text != _history[_cursor].text) {
    // A new step discards whatever was left to redo.
    _history.resize(_cursor + 1);
    _history.push_back({std::move(text), what});
    if (_history.size() > kMaxHistory) {
      _history.erase(_history.begin());
    }
    _cursor = _history.size() - 1;
  }
  recompute();
}

bool Editor::can_undo() const { return _cursor > 0; }

bool Editor::can_redo() const { return _cursor + 1 < _history.size(); }

std::string Editor::undo_label() const {
  return can_undo() ? _history[_cursor].label : "";
}

std::string Editor::redo_label() const {
  return can_redo() ? _history[_cursor + 1].label : "";
}

void Editor::undo() {
  if (!can_undo()) {
    return;
  }
  --_cursor;
  _doc = Document::parse(_history[_cursor].text);
  recompute();
}

void Editor::redo() {
  if (!can_redo()) {
    return;
  }
  ++_cursor;
  _doc = Document::parse(_history[_cursor].text);
  recompute();
}

// --------------------------------------------------------------- derived

std::string Editor::type_of(const std::string &section) const {
  auto it = _types.find(section);
  return it == _types.end() ? "" : it->second;
}

void Editor::recompute() {
  _nodes.clear();
  _types.clear();
  for (const auto &s : _doc.sections()) {
    if (is_document_section(s.name)) {
      continue;
    }
    _nodes.push_back(s.name);
    _types[s.name] = _schema.infer_type(s);
  }

  // Topology comes from toml++'s view of the serialized text, so it is
  // exactly what the agents would read.
  _topics.clear();
  _edges.clear();
  _dangling = {};
  std::string parse_error;
  try {
    toml::table table = toml::parse(_doc.serialize());
    _topics = agent_topic_infos(table);
    _edges = topology_edges(_topics);
    _dangling = dangling_topics(_topics);
  } catch (const toml::parse_error &err) {
    parse_error = std::string(err.description());
  } catch (const std::exception &err) {
    parse_error = err.what();
  }

  // Parse problems of the current text, not of the text as loaded: the
  // offending lines are kept verbatim, so re-parsing reports them with their
  // current line numbers and drops them once the user fixes or removes them.
  std::string text = _doc.serialize();
  _diagnostics.clear();
  Document::parse(text, &_diagnostics);
  auto invalid = _doc.validate();
  _diagnostics.insert(_diagnostics.end(), invalid.begin(), invalid.end());
  if (!parse_error.empty() && invalid.empty()) {
    Diagnostic d;
    d.message = parse_error;
    _diagnostics.push_back(d);
  }
  auto lint = _schema.lint(_doc);
  _diagnostics.insert(_diagnostics.end(), lint.begin(), lint.end());
}

// -------------------------------------------------------------- node ops

std::string Editor::add_node(const std::string &type_id, float x, float y) {
  const NodeType *type = _schema.type(type_id);
  if (!type || type->document) {
    return "";
  }
  // The first free alias (federate_a, then federate_b), else type_id[_N].
  std::string name;
  for (const auto &alias : type->aliases) {
    if (!_doc.find(alias)) {
      name = alias;
      break;
    }
  }
  if (name.empty()) {
    name = unique_name(_doc, type_id);
  }
  Section *s = _doc.add_section(name);
  if (!s) {
    return "";
  }
  // Only the meta: a source/filter without pub_topic publishes under its
  // section name, and nothing else needs a value yet.
  s->meta.type = type_id;
  s->meta.x = x;
  s->meta.y = y;
  commit("Add " + name);
  return name;
}

bool Editor::delete_node(const std::string &name) {
  if (is_document_section(name) || !_doc.remove_section(name)) {
    return false;
  }
  commit("Delete " + name);
  return true;
}

std::string Editor::duplicate_node(const std::string &name) {
  if (is_document_section(name)) {
    return "";
  }
  auto it = std::find_if(_doc.sections().begin(), _doc.sections().end(),
                         [&](const Section &s) { return s.name == name; });
  if (it == _doc.sections().end()) {
    return "";
  }
  Section copy = *it;
  std::string fresh = unique_name(_doc, name);
  copy.preamble.clear(); // banners belong to the original
  for (auto &child : copy.raw_children) {
    rename_child_header(child, name, fresh);
  }
  copy.name = fresh;
  if (copy.meta.x) {
    copy.meta.x = *copy.meta.x + 40.0f;
  }
  if (copy.meta.y) {
    copy.meta.y = *copy.meta.y + 40.0f;
  }
  // Right after the original, so the copy sits next to it in the file.
  _doc.sections().insert(it + 1, std::move(copy));
  commit("Duplicate " + name);
  return fresh;
}

bool Editor::rename_node(const std::string &old_name,
                         const std::string &new_name) {
  if (is_document_section(old_name) || is_document_section(new_name) ||
      !_doc.rename_section(old_name, new_name)) {
    return false;
  }
  commit("Rename " + old_name);
  return true;
}

void Editor::set_position(const std::string &name, float x, float y,
                          bool commit_now) {
  Section *s = _doc.find(name);
  if (!s) {
    return;
  }
  s->meta.x = x;
  s->meta.y = y;
  if (commit_now) {
    commit("Move " + name);
  }
}

void Editor::auto_layout(bool all) {
  if (layout_nodes(all)) {
    commit("Auto layout");
  }
}

void Editor::finish_load() {
  recompute();
  layout_nodes(false);
  _saved_text = _doc.serialize();
  reset_history();
  recompute();
}

bool Editor::layout_nodes(bool all) {
  const size_t count = _nodes.size();
  if (count == 0) {
    return false;
  }
  std::map<std::string, int> layer;
  for (const auto &n : _nodes) {
    layer[n] = 0;
  }

  // Longest path from the publishers. On a cycle the layers would grow
  // forever, so relax at most `count` times.
  for (size_t pass = 0; pass < count; ++pass) {
    bool changed = false;
    for (const auto &e : _edges) {
      if (e.catch_all || !layer.count(e.from) || !layer.count(e.to)) {
        continue;
      }
      int want = std::min<int>(layer[e.from] + 1, static_cast<int>(count) - 1);
      if (want > layer[e.to]) {
        layer[e.to] = want;
        changed = true;
      }
    }
    if (!changed) {
      break;
    }
  }

  // A node that only receives through a subscribe-all entry has no layer of
  // its own (catch-all edges are ignored); it is a sink, so put it right.
  int last = 0;
  for (const auto &kv : layer) {
    last = std::max(last, kv.second);
  }
  std::set<std::string> catch_all_sinks;
  std::set<std::string> wired;
  for (const auto &e : _edges) {
    (e.catch_all ? catch_all_sinks : wired).insert(e.to);
    if (!e.catch_all) {
      wired.insert(e.from);
    }
  }
  for (const auto &n : catch_all_sinks) {
    if (!wired.count(n) && layer.count(n)) {
      layer[n] = last + (last > 0 ? 0 : 1);
    }
  }

  // Rows: file order within a layer. A layer taller than kMaxRows wraps
  // into extra columns, which shift the following layers to the right.
  std::map<int, int> layer_size;
  for (const auto &n : _nodes) {
    ++layer_size[layer[n]];
  }
  std::map<int, int> first_column;
  int column = 0;
  for (const auto &[l, size] : layer_size) {
    first_column[l] = column;
    column += (size + kMaxRows - 1) / kMaxRows;
  }
  std::map<int, int> next_row;
  bool moved = false;
  for (const auto &n : _nodes) {
    const int index = next_row[layer[n]]++;
    Section *s = _doc.find(n);
    if (!s) {
      continue;
    }
    bool placed = s->meta.x && s->meta.y;
    if (placed && !all) {
      continue;
    }
    const float x = (first_column[layer[n]] + index / kMaxRows) * kLayerSpacing;
    const float y = (index % kMaxRows) * kRowSpacing;
    if (!placed || *s->meta.x != x || *s->meta.y != y) {
      s->meta.x = x;
      s->meta.y = y;
      moved = true;
    }
  }
  return moved;
}

// -------------------------------------------------------------- link ops

Editor::ConnectResult Editor::connect(const std::string &from,
                                      const std::string &to, std::string *via) {
  auto pub = _topics.find(from);
  auto sub = _topics.find(to);
  if (from == to || pub == _topics.end() || sub == _topics.end() ||
      !_doc.find(to) || pub->second.pub_topic.empty()) {
    return ConnectResult::Invalid;
  }
  const std::string &topic = pub->second.pub_topic;
  const auto &entries = sub->second.sub_topic;

  // Already received? Report the strongest matching entry (Exact first).
  const std::string *best = nullptr;
  SubMatch best_kind = SubMatch::None;
  for (const auto &entry : entries) {
    SubMatch m = subscription_match(entry, topic);
    if (m == SubMatch::None) {
      continue;
    }
    if (!best || (m == SubMatch::Exact && best_kind != SubMatch::Exact)) {
      best = &entry;
      best_kind = m;
    }
  }
  if (best) {
    if (via) {
      *via = *best;
    }
    return ConnectResult::AlreadyMatched;
  }

  auto updated = entries;
  updated.push_back(topic);
  std::string raw = toml_string_array(updated);
  Section *s = _doc.find(to);
  if (!s->find("sub_topic") && s->find_disabled("sub_topic")) {
    // Keep the commented-out knob and add an active one next to it.
    Entry e;
    e.key = "sub_topic";
    e.raw_value = raw;
    s->entries.push_back(std::move(e));
  } else if (!_doc.set_raw_value(to, "sub_topic", raw)) {
    return ConnectResult::Invalid;
  }
  commit("Connect " + from + " -> " + to);
  return ConnectResult::Added;
}

Editor::DisconnectResult Editor::disconnect(const std::string &from,
                                            const std::string &to) {
  auto edge =
      std::find_if(_edges.begin(), _edges.end(), [&](const TopologyEdge &e) {
        return e.from == from && e.to == to;
      });
  auto sub = _topics.find(to);
  if (edge == _edges.end() || sub == _topics.end()) {
    return DisconnectResult::NotFound;
  }
  const std::string &topic = edge->topic;

  // Any entry other than an exact one that also reaches the topic keeps the
  // edge alive (and is not ours to rewrite). An exact-kind edge can hide
  // such entries (via_patterns is empty then), so look at the entries.
  std::vector<std::string> kept;
  bool removed = false;
  for (const auto &entry : sub->second.sub_topic) {
    SubMatch m = subscription_match(entry, topic);
    if (m == SubMatch::Prefix || m == SubMatch::Wildcard) {
      return DisconnectResult::ViaPattern;
    }
    if (m == SubMatch::Exact) {
      removed = true;
    } else {
      kept.push_back(entry);
    }
  }
  if (!removed) {
    return DisconnectResult::NotFound;
  }
  if (!_doc.set_raw_value(to, "sub_topic", toml_string_array(kept))) {
    return DisconnectResult::NotFound;
  }
  commit("Disconnect " + from + " -> " + to);
  return DisconnectResult::Removed;
}

} // namespace Mads::Ini
