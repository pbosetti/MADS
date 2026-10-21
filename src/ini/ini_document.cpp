// Comment-preserving mads.ini model: see ini_document.hpp for the contract.
#include "ini_document.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace Mads::Ini {

namespace {

// ---------------------------------------------------------------- text utils

constexpr const char *kWhitespace = " \t";

std::string_view ltrim(std::string_view s) {
  auto pos = s.find_first_not_of(kWhitespace);
  return pos == std::string_view::npos ? std::string_view{} : s.substr(pos);
}

std::string_view rtrim(std::string_view s) {
  auto pos = s.find_last_not_of(kWhitespace);
  return pos == std::string_view::npos ? std::string_view{}
                                       : s.substr(0, pos + 1);
}

std::string_view trim(std::string_view s) { return rtrim(ltrim(s)); }

bool is_blank(std::string_view s) { return trim(s).empty(); }

bool is_comment(std::string_view s) {
  s = ltrim(s);
  return !s.empty() && s.front() == '#';
}

bool starts_with(std::string_view s, std::string_view prefix) {
  return s.substr(0, prefix.size()) == prefix;
}

std::vector<std::string> split_lines(std::string_view text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < text.size()) {
    auto end = text.find('\n', start);
    auto stop = end == std::string_view::npos ? text.size() : end;
    auto line = text.substr(start, stop - start);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    lines.emplace_back(line);
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return lines;
}

std::string join_lines(const std::vector<std::string> &lines) {
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) {
      out += '\n';
    }
    out += lines[i];
  }
  return out;
}

// ------------------------------------------------------------- value scanner

// TOML lexical state carried across the lines of one value.
struct ScanState {
  enum class Str { None, Basic, Literal, MlBasic, MlLiteral };
  Str str = Str::None;
  int depth = 0; ///< open '[' / '{'
  bool complete() const { return depth == 0 && str == Str::None; }
};

// Advances `st` over `s` from `start`. Returns the position of the '#' that
// opens an inline comment (outside strings and brackets), or npos. A '#'
// inside brackets comments out the rest of the line, which stays in the value.
size_t scan_line(std::string_view s, size_t start, ScanState &st) {
  using Str = ScanState::Str;
  size_t i = start;
  while (i < s.size()) {
    char c = s[i];
    switch (st.str) {
    case Str::None:
      if (c == '"') {
        if (s.substr(i, 3) == "\"\"\"") {
          st.str = Str::MlBasic;
          i += 3;
        } else {
          st.str = Str::Basic;
          ++i;
        }
      } else if (c == '\'') {
        if (s.substr(i, 3) == "'''") {
          st.str = Str::MlLiteral;
          i += 3;
        } else {
          st.str = Str::Literal;
          ++i;
        }
      } else if (c == '[' || c == '{') {
        ++st.depth;
        ++i;
      } else if (c == ']' || c == '}') {
        st.depth = std::max(0, st.depth - 1);
        ++i;
      } else if (c == '#') {
        if (st.depth == 0) {
          return i;
        }
        i = s.size();
      } else {
        ++i;
      }
      break;
    case Str::Basic:
      if (c == '\\') {
        i += 2;
      } else {
        if (c == '"') {
          st.str = Str::None;
        }
        ++i;
      }
      break;
    case Str::Literal:
      if (c == '\'') {
        st.str = Str::None;
      }
      ++i;
      break;
    case Str::MlBasic:
      if (c == '\\') {
        i += 2;
      } else if (s.substr(i, 3) == "\"\"\"") {
        st.str = Str::None;
        i += 3;
        // up to two extra quotes belong to the string: """a"""" is valid
        for (int k = 0; k < 2 && i < s.size() && s[i] == '"'; ++k) {
          ++i;
        }
      } else {
        ++i;
      }
      break;
    case Str::MlLiteral:
      if (s.substr(i, 3) == "'''") {
        st.str = Str::None;
        i += 3;
        for (int k = 0; k < 2 && i < s.size() && s[i] == '\''; ++k) {
          ++i;
        }
      } else {
        ++i;
      }
      break;
    }
  }
  // Single-line strings cannot span lines: an unterminated one ends here.
  if (st.str == Str::Basic || st.str == Str::Literal) {
    st.str = Str::None;
  }
  return std::string_view::npos;
}

// Position of the first `target` outside quotes, or npos.
size_t find_unquoted(std::string_view s, char target, size_t from = 0) {
  for (size_t i = from; i < s.size(); ++i) {
    char c = s[i];
    if (c == '"') {
      for (++i; i < s.size() && s[i] != '"'; ++i) {
        if (s[i] == '\\') {
          ++i;
        }
      }
    } else if (c == '\'') {
      for (++i; i < s.size() && s[i] != '\''; ++i) {
      }
    } else if (c == target) {
      return i;
    }
  }
  return std::string_view::npos;
}

// ------------------------------------------------------------ toml checking

bool toml_ok(const std::string &text) {
  try {
    (void)toml::parse(text);
    return true;
  } catch (...) {
    return false;
  }
}

// True if `key = raw` is a valid single TOML assignment.
bool is_valid_assignment(std::string_view key, std::string_view raw) {
  if (key.empty() || raw.empty()) {
    return false;
  }
  try {
    auto table = toml::parse(std::string(key) + " = " + std::string(raw));
    return table.size() == 1;
  } catch (...) {
    return false;
  }
}

// -------------------------------------------------------------- key helpers

bool is_bare_key_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}

// ------------------------------------------------------------- parsed lines

// A `key = value` assignment found by read_assignment().
struct Assignment {
  std::string key, raw_value, inline_comment;
  size_t line_count = 1; ///< source lines consumed
  bool ok = false;       ///< syntactically complete and valid TOML
};

// Reads the assignment starting at lines[first]; a value left open at the end
// of a line (bracket or multi-line string) pulls in the following lines.
Assignment read_assignment(const std::vector<std::string> &lines,
                           size_t first) {
  Assignment a;
  const std::string &line = lines[first];
  auto eq = find_unquoted(line, '=');
  if (eq == std::string_view::npos) {
    return a;
  }
  a.key = std::string(trim(std::string_view(line).substr(0, eq)));
  size_t vstart = line.find_first_not_of(kWhitespace, eq + 1);
  if (vstart == std::string::npos) {
    return a;
  }
  ScanState st;
  auto comment_at = scan_line(line, vstart, st);
  auto end = comment_at == std::string_view::npos ? line.size() : comment_at;
  a.raw_value = std::string(rtrim(std::string_view(line).substr(vstart, end - vstart)));
  size_t last = first;
  while (!st.complete() && comment_at == std::string_view::npos &&
         last + 1 < lines.size()) {
    ++last;
    const std::string &next = lines[last];
    comment_at = scan_line(next, 0, st);
    end = comment_at == std::string_view::npos ? next.size() : comment_at;
    a.raw_value += '\n';
    a.raw_value += std::string(rtrim(std::string_view(next).substr(0, end)));
  }
  a.line_count = last - first + 1;
  if (!st.complete()) {
    return a;
  }
  if (comment_at != std::string_view::npos) {
    a.inline_comment = std::string(
        trim(std::string_view(lines[last]).substr(comment_at + 1)));
  }
  a.ok = is_valid_assignment(a.key, a.raw_value);
  return a;
}

// A commented-out knob: `#key = value` or `# key = value`.
std::optional<Entry> read_disabled(std::string_view line) {
  auto t = ltrim(line);
  if (t.empty() || t.front() != '#') {
    return std::nullopt;
  }
  t.remove_prefix(1);
  if (!t.empty() && t.front() == ' ') {
    t.remove_prefix(1);
  }
  size_t k = 0;
  while (k < t.size() && is_bare_key_char(t[k])) {
    ++k;
  }
  if (k == 0) {
    return std::nullopt;
  }
  auto key = t.substr(0, k);
  size_t eq = t.find_first_not_of(kWhitespace, k);
  if (eq == std::string_view::npos || t[eq] != '=') {
    return std::nullopt;
  }
  size_t vstart = t.find_first_not_of(kWhitespace, eq + 1);
  if (vstart == std::string_view::npos) {
    return std::nullopt;
  }
  ScanState st;
  auto comment_at = scan_line(t, vstart, st);
  if (!st.complete()) {
    return std::nullopt;
  }
  auto end = comment_at == std::string_view::npos ? t.size() : comment_at;
  auto raw = rtrim(t.substr(vstart, end - vstart));
  if (!is_valid_assignment(key, raw)) {
    return std::nullopt;
  }
  Entry e;
  e.key = std::string(key);
  e.raw_value = std::string(raw);
  if (comment_at != std::string_view::npos) {
    e.inline_comment = std::string(trim(t.substr(comment_at + 1)));
  }
  e.disabled = true;
  return e;
}

// A parsed `[name]` / `[[name]]` line.
struct Header {
  std::string name;    ///< raw text between the brackets, trimmed
  std::string first;   ///< first dotted component (raw, trimmed)
  std::string comment; ///< text after a '#' that follows the header
  bool array = false;
  bool dotted = false;
  bool ok = false;
};

Header read_header(std::string_view line) {
  Header h;
  auto t = trim(line);
  if (t.empty() || t.front() != '[') {
    return h;
  }
  h.array = starts_with(t, "[[");
  size_t open = h.array ? 2 : 1;
  size_t close = find_unquoted(t, ']', open);
  if (close == std::string_view::npos) {
    return h;
  }
  size_t after = close + 1;
  if (h.array) {
    if (after >= t.size() || t[after] != ']') {
      return h;
    }
    ++after;
  }
  auto rest = ltrim(t.substr(after));
  if (!rest.empty()) {
    if (rest.front() != '#') {
      return h;
    }
    h.comment = std::string(trim(rest.substr(1)));
  }
  h.name = std::string(trim(t.substr(open, close - open)));
  if (h.name.empty()) {
    return h;
  }
  auto dot = find_unquoted(h.name, '.');
  h.dotted = dot != std::string::npos;
  h.first = std::string(trim(std::string_view(h.name).substr(0, dot)));
  h.ok = toml_ok(std::string(t.substr(0, after)) + "\n");
  return h;
}

// Meta line `#@ type=X pos=X,Y k=v ...`.
std::optional<NodeMeta> read_meta(std::string_view line) {
  auto t = ltrim(line);
  if (!starts_with(t, "#@")) {
    return std::nullopt;
  }
  t.remove_prefix(2);
  NodeMeta meta;
  std::istringstream in{std::string(t)};
  std::string token;
  while (in >> token) {
    auto eq = token.find('=');
    std::string k = token.substr(0, eq);
    std::string v = eq == std::string::npos ? "" : token.substr(eq + 1);
    if (k == "type") {
      meta.type = v;
    } else if (k == "pos") {
      auto comma = v.find(',');
      char *end_x = nullptr, *end_y = nullptr;
      float x = std::strtof(v.c_str(), &end_x);
      float y = comma == std::string::npos
                    ? 0.0F
                    : std::strtof(v.c_str() + comma + 1, &end_y);
      if (comma != std::string::npos && end_x == v.c_str() + comma &&
          end_y == v.c_str() + v.size()) {
        meta.x = x;
        meta.y = y;
      } else {
        meta.extra[k] = v;
      }
    } else {
      meta.extra[k] = v;
    }
  }
  return meta;
}

std::string meta_line(const NodeMeta &meta) {
  std::string out = "#@";
  if (!meta.type.empty()) {
    out += " type=" + meta.type;
  }
  if (meta.x || meta.y) {
    out += " pos=" + std::to_string(std::lround(meta.x.value_or(0.0F))) + "," +
           std::to_string(std::lround(meta.y.value_or(0.0F)));
  }
  for (const auto &[k, v] : meta.extra) {
    out += " " + k + "=" + v;
  }
  return out;
}

// '#' and one following space stripped.
std::string comment_text(std::string_view line) {
  auto t = ltrim(line);
  t.remove_prefix(1);
  if (!t.empty() && t.front() == ' ') {
    t.remove_prefix(1);
  }
  return std::string(t);
}

// ------------------------------------------------------------------ parsing

class Parser {
public:
  Parser(Document &doc, std::vector<Diagnostic> *diagnostics)
      : _doc(doc), _diagnostics(diagnostics) {}

  void run(std::string_view text) {
    auto lines = split_lines(text);
    for (size_t i = 0; i < lines.size();) {
      i += consume(lines, i);
    }
    finish();
  }

private:
  // Where the current non-header lines go.
  struct Target {
    int section = -1; ///< index in sections(), -1 for the root table
    int child = -1;   ///< index in raw_children, -1 for the section body
  };

  Section &section_at(int index) {
    return index < 0 ? _doc.root() : _doc.sections()[index];
  }
  Section &current() { return section_at(_target.section); }

  void report(int line, const std::string &message) {
    if (_diagnostics) {
      Diagnostic d;
      d.line = line;
      d.section = current().name;
      d.message = message;
      _diagnostics->push_back(std::move(d));
    }
  }

  // Splits the pending comment/blank lines: the run before the first blank
  // line stays with the block that precedes, the rest is returned.
  std::vector<std::string> split_pending(std::vector<std::string> &head) {
    size_t cut = 0;
    while (cut < _pending.size() && !is_blank(_pending[cut])) {
      ++cut;
    }
    head.assign(_pending.begin(), _pending.begin() + cut);
    std::vector<std::string> rest(_pending.begin() + cut, _pending.end());
    _pending.clear();
    return rest;
  }

  void append_to_target(const std::vector<std::string> &lines) {
    if (lines.empty()) {
      return;
    }
    Section &s = current();
    if (_target.child < 0) {
      s.trailing.insert(s.trailing.end(), lines.begin(), lines.end());
    } else {
      auto &block = s.raw_children[_target.child];
      block += '\n' + join_lines(lines);
    }
  }

  // Lines consumed verbatim from a block that is not parsed (children).
  void append_raw(const std::string &line) {
    auto &block = current().raw_children[_target.child];
    block += '\n' + line;
  }

  // Handles `lines[i]`; returns the number of lines consumed.
  size_t consume(const std::vector<std::string> &lines, size_t i) {
    const std::string &line = lines[i];
    int lineno = static_cast<int>(i) + 1;

    if (_in_head) {
      if (!_head_started && starts_with(ltrim(line), "#@")) {
        _head_started = true;
        if (auto meta = read_meta(line)) {
          current().meta = *meta;
          return 1;
        }
      }
      _head_started = true;
      if (is_comment(line) && !read_disabled(line)) {
        auto &comment = current().comment;
        comment += (comment.empty() && !_head_has_comment ? "" : "\n") +
                   comment_text(line);
        _head_has_comment = true;
        return 1;
      }
      _in_head = false;
    }

    if (is_blank(line)) {
      _pending.emplace_back();
      return 1;
    }
    if (ltrim(line).front() == '#') {
      if (_target.child < 0) {
        if (auto entry = read_disabled(line)) {
          entry->leading_comments = std::move(_pending);
          _pending.clear();
          current().entries.push_back(std::move(*entry));
          return 1;
        }
      }
      _pending.emplace_back(line);
      return 1;
    }
    if (ltrim(line).front() == '[') {
      auto header = read_header(line);
      if (header.ok) {
        on_header(header, line);
        return 1;
      }
      report(lineno, "invalid table header");
      keep_verbatim(line);
      return 1;
    }

    auto a = read_assignment(lines, i);
    if (_target.child >= 0) {
      // Children are kept verbatim; only the extent of the value matters.
      flush_pending_into_child();
      size_t n = a.line_count;
      if (a.key.empty()) {
        n = 1;
      }
      for (size_t k = 0; k < n; ++k) {
        append_raw(lines[i + k]);
      }
      return n;
    }
    if (!a.ok) {
      report(lineno, a.key.empty() || find_unquoted(line, '=') ==
                                          std::string_view::npos
                         ? "expected 'key = value'"
                         : "invalid value for key '" + a.key + "'");
      // Only the offending first line is kept raw; resume right after it.
      keep_verbatim(line);
      return 1;
    }
    Entry e;
    e.key = std::move(a.key);
    e.raw_value = std::move(a.raw_value);
    e.inline_comment = std::move(a.inline_comment);
    e.leading_comments = std::move(_pending);
    _pending.clear();
    current().entries.push_back(std::move(e));
    return a.line_count;
  }

  // Unparseable text goes out unchanged, together with the pending lines.
  void keep_verbatim(const std::string &line) {
    if (_target.child >= 0) {
      flush_pending_into_child();
      append_raw(line);
    } else {
      _pending.push_back(line);
    }
  }

  void flush_pending_into_child() {
    if (_pending.empty()) {
      return;
    }
    auto &block = current().raw_children[_target.child];
    block += '\n' + join_lines(_pending);
    _pending.clear();
  }

  void on_header(const Header &h, const std::string &line) {
    std::vector<std::string> head;
    auto rest = split_pending(head);
    bool root_bare = _target.section < 0 && _target.child < 0 &&
                     _doc.root().entries.empty();
    if (root_bare) {
      // Comments above the first header with no root keys: all banner.
      rest.insert(rest.begin(), head.begin(), head.end());
      head.clear();
    }
    append_to_target(head);
    _in_head = false;

    if (h.array || h.dotted) {
      int owner = -1;
      auto &sections = _doc.sections();
      for (int k = static_cast<int>(sections.size()) - 1; k >= 0; --k) {
        if (sections[k].name == h.first) {
          owner = k;
          break;
        }
      }
      Section &s = section_at(owner);
      std::string block(rtrim(line));
      if (!rest.empty()) {
        block = join_lines(rest) + '\n' + block;
      }
      s.raw_children.push_back(std::move(block));
      _target = {owner, static_cast<int>(s.raw_children.size()) - 1};
      return;
    }

    Section s;
    s.name = h.name;
    s.preamble = std::move(rest);
    if (!h.comment.empty()) {
      s.comment = h.comment;
      _head_has_comment = true;
    } else {
      _head_has_comment = false;
    }
    _doc.sections().push_back(std::move(s));
    _target = {static_cast<int>(_doc.sections().size()) - 1, -1};
    _in_head = true;
    _head_started = false;
  }

  void finish() {
    append_to_target(_pending);
    _pending.clear();
  }

  Document &_doc;
  std::vector<Diagnostic> *_diagnostics;
  Target _target;
  std::vector<std::string> _pending; ///< comment/blank lines not yet attached
  bool _in_head = false;             ///< right under a header: meta, comment
  bool _head_started = false;
  bool _head_has_comment = false;
};

// ------------------------------------------------------------ serialization

// Builds the output text, collapsing blank lines on the fly.
class Writer {
public:
  void line(std::string_view s) {
    _text.append(s);
    _text += '\n';
    _last_blank = false;
    _at_start = false;
  }
  // A blank line, unless it would double one or open a block.
  void blank() {
    if (_last_blank || _at_start) {
      return;
    }
    _text += '\n';
    _last_blank = true;
  }
  // Starts a block: a blank line separates it from the previous text.
  void separate() {
    trim_end();
    if (!_text.empty()) {
      _text += '\n';
      _last_blank = true;
    }
    _at_start = true;
  }
  void trim_end() {
    if (_last_blank) {
      _text.pop_back();
      _last_blank = false;
    }
  }
  void begin_block() { _at_start = true; }
  bool empty() const { return _text.empty(); }
  std::string take() {
    trim_end();
    return std::move(_text);
  }

private:
  std::string _text;
  bool _last_blank = false;
  bool _at_start = true;
};

void write_lines(Writer &w, const std::vector<std::string> &lines) {
  for (const auto &l : lines) {
    if (is_blank(l)) {
      w.blank();
    } else {
      w.line(l);
    }
  }
}

void write_entry(Writer &w, const Entry &e) {
  write_lines(w, e.leading_comments);
  std::string text = e.disabled ? "# " : "";
  text += e.key + " = " + e.raw_value;
  if (!e.inline_comment.empty()) {
    text += " # " + e.inline_comment;
  }
  w.line(text);
}

void write_children(Writer &w, const Section &s) {
  for (const auto &child : s.raw_children) {
    auto lines = split_lines(child);
    size_t first = 0, last = lines.size();
    while (first < last && is_blank(lines[first])) {
      ++first;
    }
    while (last > first && is_blank(lines[last - 1])) {
      --last;
    }
    if (first == last) {
      continue;
    }
    w.separate();
    for (size_t i = first; i < last; ++i) {
      w.line(lines[i]);
    }
  }
}

// Body of a section: meta, comment, entries, trailing lines, children.
void write_body(Writer &w, const Section &s) {
  if (!s.meta.empty()) {
    w.line(meta_line(s.meta));
  }
  if (!s.comment.empty()) {
    for (auto &l : split_lines(s.comment + "\n")) {
      w.line(l.empty() ? "#" : "# " + l);
    }
  }
  for (const auto &e : s.entries) {
    write_entry(w, e);
  }
  write_lines(w, s.trailing);
  w.trim_end();
  write_children(w, s);
}

std::string_view fnv_hex_digits() { return "0123456789abcdef"; }

} // namespace

// ------------------------------------------------------------------ Section

Entry *Section::find(std::string_view key) {
  for (auto &e : entries) {
    if (!e.disabled && e.key == key) {
      return &e;
    }
  }
  return nullptr;
}

const Entry *Section::find(std::string_view key) const {
  return const_cast<Section *>(this)->find(key);
}

Entry *Section::find_disabled(std::string_view key) {
  for (auto &e : entries) {
    if (e.disabled && e.key == key) {
      return &e;
    }
  }
  return nullptr;
}

const Entry *Section::find_disabled(std::string_view key) const {
  return const_cast<Section *>(this)->find_disabled(key);
}

// ----------------------------------------------------------------- Document

Document Document::parse(std::string_view text,
                         std::vector<Diagnostic> *diagnostics) {
  Document doc;
  Parser(doc, diagnostics).run(text);
  return doc;
}

std::string Document::serialize() const {
  Writer w;
  write_body(w, _root);
  for (const auto &s : _sections) {
    w.separate();
    auto preamble = s.preamble;
    while (!preamble.empty() && is_blank(preamble.back())) {
      preamble.pop_back();
    }
    write_lines(w, preamble);
    w.line("[" + s.name + "]");
    w.begin_block();
    write_body(w, s);
  }
  return w.take();
}

std::vector<Diagnostic> Document::validate() const {
  std::vector<Diagnostic> out;
  try {
    (void)toml::parse(serialize());
  } catch (const toml::parse_error &err) {
    Diagnostic d;
    d.severity = Diagnostic::Severity::Error;
    d.line = static_cast<int>(err.source().begin.line);
    d.message = std::string(err.description());
    out.push_back(std::move(d));
  } catch (const std::exception &err) {
    Diagnostic d;
    d.message = err.what();
    out.push_back(std::move(d));
  }
  return out;
}

Section *Document::find(std::string_view name) {
  for (auto &s : _sections) {
    if (s.name == name) {
      return &s;
    }
  }
  return nullptr;
}

const Section *Document::find(std::string_view name) const {
  return const_cast<Document *>(this)->find(name);
}

Section *Document::add_section(const std::string &name) {
  if (!is_bare_key(name) || find(name)) {
    return nullptr;
  }
  Section s;
  s.name = name;
  _sections.push_back(std::move(s));
  return &_sections.back();
}

bool Document::remove_section(std::string_view name) {
  auto it = std::find_if(_sections.begin(), _sections.end(),
                         [&](const Section &s) { return s.name == name; });
  if (it == _sections.end()) {
    return false;
  }
  _sections.erase(it);
  return true;
}

bool Document::rename_section(std::string_view old_name,
                              const std::string &new_name) {
  Section *s = find(old_name);
  if (!s || !is_bare_key(new_name) || find(new_name)) {
    return false;
  }
  // Child headers: `[old.x]` / `[[ old.x ]]` -> `[new.x]`.
  for (auto &child : s->raw_children) {
    auto lines = split_lines(child);
    for (auto &l : lines) {
      auto t = ltrim(l);
      if (t.empty() || t.front() != '[') {
        continue;
      }
      size_t pos = starts_with(t, "[[") ? 2 : 1;
      pos = t.find_first_not_of(kWhitespace, pos);
      if (pos == std::string_view::npos || !starts_with(t.substr(pos), old_name)) {
        continue;
      }
      size_t after = t.find_first_not_of(kWhitespace, pos + old_name.size());
      if (after == std::string_view::npos ||
          (t[after] != '.' && t[after] != ']')) {
        continue;
      }
      auto indent = l.size() - t.size();
      l = l.substr(0, indent) + std::string(t.substr(0, pos)) + new_name +
          std::string(t.substr(pos + old_name.size()));
    }
    child = join_lines(lines);
  }
  s->name = new_name;
  return true;
}

std::optional<std::string> Document::raw_value(std::string_view section,
                                               std::string_view key) const {
  if (const Section *s = find(section)) {
    if (const Entry *e = s->find(key)) {
      return e->raw_value;
    }
  }
  return std::nullopt;
}

std::optional<std::string>
Document::effective_raw_value(std::string_view section,
                              std::string_view key) const {
  if (auto v = raw_value(section, key)) {
    return v;
  }
  return raw_value("agents", key);
}

bool Document::set_raw_value(std::string_view section, std::string_view key,
                             std::string_view raw) {
  Section *s = find(section);
  raw = trim(raw);
  if (!s || !is_valid_value(raw)) {
    return false;
  }
  if (Entry *e = s->find(key)) {
    e->raw_value = std::string(raw);
  } else if (Entry *d = s->find_disabled(key)) {
    d->raw_value = std::string(raw);
    d->disabled = false;
  } else {
    Entry entry;
    entry.key = std::string(key);
    entry.raw_value = std::string(raw);
    s->entries.push_back(std::move(entry));
  }
  return true;
}

bool Document::set_enabled(std::string_view section, std::string_view key,
                           bool enabled) {
  Section *s = find(section);
  if (!s) {
    return false;
  }
  Entry *active = s->find(key);
  Entry *disabled = s->find_disabled(key);
  if (enabled) {
    if (!disabled) {
      return false;
    }
    if (active) {
      if (active->raw_value.find('\n') != std::string::npos) {
        return false; // a multi-line value cannot live in a comment
      }
      active->disabled = true;
    }
    disabled->disabled = false;
    return true;
  }
  if (!active || active->raw_value.find('\n') != std::string::npos) {
    return false;
  }
  active->disabled = true;
  return true;
}

bool Document::remove_key(std::string_view section, std::string_view key) {
  Section *s = find(section);
  if (!s) {
    return false;
  }
  auto it = std::remove_if(s->entries.begin(), s->entries.end(),
                           [&](const Entry &e) { return e.key == key; });
  bool removed = it != s->entries.end();
  s->entries.erase(it, s->entries.end());
  return removed;
}

// ------------------------------------------------------------ free functions

bool is_valid_value(std::string_view raw) {
  raw = trim(raw);
  if (raw.empty()) {
    return false;
  }
  // A '#' outside strings and brackets would make toml++ accept `1 # note`.
  auto lines = split_lines(raw);
  ScanState st;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (scan_line(lines[i], 0, st) != std::string_view::npos) {
      return false;
    }
  }
  return st.complete() && is_valid_assignment("v", raw);
}

bool is_bare_key(std::string_view key) {
  return !key.empty() && std::all_of(key.begin(), key.end(), is_bare_key_char);
}

std::string content_etag(std::string_view text) {
  uint64_t hash = 14695981039346656037ULL;
  for (unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[i] = fnv_hex_digits()[hash & 0xF];
    hash >>= 4;
  }
  return out;
}

// --------------------------------------------------------------- FileSource

namespace {

bool read_whole_file(const std::filesystem::path &path, std::string &out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

SaveResult save_error(SaveResult::Status status, std::string message) {
  SaveResult r;
  r.status = status;
  r.message = std::move(message);
  return r;
}

} // namespace

FileSource::FileSource(std::filesystem::path path) : _path(std::move(path)) {}

Loaded FileSource::load() {
  Loaded loaded;
  if (!read_whole_file(_path, loaded.text)) {
    throw std::runtime_error("cannot read " + _path.string());
  }
  loaded.etag = content_etag(loaded.text);
  return loaded;
}

SaveResult FileSource::save(const std::string &text,
                            const std::string &base_etag) {
  namespace fs = std::filesystem;
  try {
    try {
      (void)toml::parse(text);
    } catch (const toml::parse_error &err) {
      return save_error(SaveResult::Status::Error,
                        "not valid TOML: " + std::string(err.description()) +
                            " (line " +
                            std::to_string(err.source().begin.line) + ")");
    }

    std::error_code ec;
    const bool exists = fs::exists(_path, ec);
    if (exists && !base_etag.empty()) {
      std::string current;
      if (!read_whole_file(_path, current)) {
        return save_error(SaveResult::Status::Error,
                          "cannot read " + _path.string());
      }
      if (content_etag(current) != base_etag) {
        return save_error(SaveResult::Status::Conflict,
                          _path.string() + " changed since it was loaded");
      }
    }

    fs::path tmp = _path;
    tmp += ".tmp";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      out.write(text.data(), static_cast<std::streamsize>(text.size()));
      out.close();
      if (!out) {
        fs::remove(tmp, ec);
        return save_error(SaveResult::Status::Error,
                          "cannot write " + tmp.string());
      }
    }

    if (exists) {
      fs::path bak = _path;
      bak += ".bak";
      fs::copy_file(_path, bak, fs::copy_options::overwrite_existing, ec);
      if (ec) {
        fs::remove(tmp, ec);
        return save_error(SaveResult::Status::Error,
                          "cannot write backup " + bak.string());
      }
    }

    fs::rename(tmp, _path, ec);
    if (ec && exists) {
      // Some platforms refuse to rename over an existing file.
      fs::remove(_path, ec);
      fs::rename(tmp, _path, ec);
    }
    if (ec) {
      std::error_code ignored;
      fs::remove(tmp, ignored);
      return save_error(SaveResult::Status::Error,
                        "cannot replace " + _path.string() + ": " +
                            ec.message());
    }

    SaveResult ok;
    ok.status = SaveResult::Status::Ok;
    ok.etag = content_etag(text);
    return ok;
  } catch (const std::exception &err) {
    return save_error(SaveResult::Status::Error, err.what());
  } catch (...) {
    return save_error(SaveResult::Status::Error, "unknown error");
  }
}

std::string FileSource::describe() const { return _path.string(); }

} // namespace Mads::Ini
