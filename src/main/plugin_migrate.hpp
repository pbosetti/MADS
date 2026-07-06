/*
  ____  _             _         __  __ _                 _
 |  _ \| |_   _  __ _(_)_ __   |  \/  (_) __ _ _ __ __ _| |_ ___
 | |_) | | | | |/ _` | | '_ \  | |\/| | |/ _` | '__/ _` | __/ _ \
 |  __/| | |_| | (_| | | | | | | |  | | | (_| | | | (_| | ||  __/
 |_|   |_|\__,_|\__, |_|_| |_| |_|  |_|_|\__, |_|  \__,_|\__\___|
                |___/                    |___/

 Data-driven migration engine for MADS C++ plugins, used by `mads plugin --update`.

 A protocol jump (e.g. P7 -> P8) is described by a JSON file in share/plugin_migrations/
 (see share/plugin_migrations/P6-P7.json). This engine detects a plugin's current protocol
 from its CMakeLists.txt, chains the migration steps up to the target, applies the CMake tag
 bump(s) and the source-code transforms, backs up the originals, prints a report + a manual
 follow-up checklist, and (Tier B) compiles the migrated plugin so the compiler's pure-virtual
 diagnostics catch anything the rewriter missed.

 Source signature rewriting is done structurally, not by pattern-matching the parameter text:
   * Tier A (SpanRewriter, active): find `name(` and balance parentheses to locate the exact
     argument-list span, so internal reformatting / newlines / comments are irrelevant.
   * Tier C (TreeSitterRewriter, stubbed): reserved seam for a tree-sitter-cpp CST backend,
     selected per-transform when `ts_query` is set and MADS_ENABLE_TREE_SITTER is built in.
 Both live behind the SignatureRewriter interface so the engine never changes when Tier C lands.

 Author(s): Paolo Bosetti
*/
#ifndef PLUGIN_MIGRATE_HPP
#define PLUGIN_MIGRATE_HPP

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <rang.hpp>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace Mads {
namespace PluginMigrate {

namespace fs = std::filesystem;
using json = nlohmann::json;

/* ── options & result types ───────────────────────────────────────────────── */

struct Options {
  bool dry_run = false;   // compute & report, write nothing
  bool check = true;      // Tier B: compile the migrated plugin afterwards
  int from_override = -1; // skip protocol auto-detection when >= 0
  int to_override = -1;   // migrate up to this protocol instead of the manifest default
};

// One applied edit, for the human-readable report. `line == 0` means "whole-file / unknown".
struct Change {
  std::size_t line = 0;
  std::string detail;
};

/* ── small file / text helpers ────────────────────────────────────────────── */

inline bool read_file(const fs::path &p, std::string &out) {
  std::ifstream in(p, std::ios::binary);
  if (!in)
    return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

inline bool write_file(const fs::path &p, const std::string &content) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out)
    return false;
  out << content;
  return true;
}

inline std::size_t line_of_offset(const std::string &s, std::size_t off) {
  off = std::min(off, s.size());
  return static_cast<std::size_t>(std::count(s.begin(), s.begin() + off, '\n')) + 1;
}

inline bool is_ident_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Run a shell command, capturing combined stdout+stderr. Returns the exit code.
inline int run_command(const std::string &cmd, std::string &output) {
#if defined(_WIN32)
  FILE *pipe = _popen((cmd + " 2>&1").c_str(), "r");
#else
  FILE *pipe = popen((cmd + " 2>&1").c_str(), "r");
#endif
  if (!pipe)
    return -1;
  char buf[4096];
  while (std::fgets(buf, sizeof(buf), pipe) != nullptr)
    output += buf;
#if defined(_WIN32)
  return _pclose(pipe);
#else
  return pclose(pipe);
#endif
}

/* ── C++-aware span scan (shared by matcher and paren balancing) ──────────────

   A minimal lexer that classifies the source into code / line-comment /
   block-comment / string / char so that '(' ')' and identifiers inside comments
   or literals are never matched. This is the "code-aware-lite" core (Tier A).   */

enum class LexState { Code, Line, Block, Str, Chr };

// Given src[open] == '(', return the index of the matching ')', or npos.
inline std::size_t find_matching_paren(const std::string &s, std::size_t open) {
  int depth = 0;
  LexState st = LexState::Code;
  const std::size_t n = s.size();
  for (std::size_t i = open; i < n;) {
    char c = s[i];
    switch (st) {
    case LexState::Line:
      if (c == '\n')
        st = LexState::Code;
      i++;
      continue;
    case LexState::Block:
      if (c == '*' && i + 1 < n && s[i + 1] == '/') {
        st = LexState::Code;
        i += 2;
      } else
        i++;
      continue;
    case LexState::Str:
      if (c == '\\') {
        i += 2;
        continue;
      }
      if (c == '"')
        st = LexState::Code;
      i++;
      continue;
    case LexState::Chr:
      if (c == '\\') {
        i += 2;
        continue;
      }
      if (c == '\'')
        st = LexState::Code;
      i++;
      continue;
    case LexState::Code:
      break;
    }
    if (c == '/' && i + 1 < n && s[i + 1] == '/') {
      st = LexState::Line;
      i += 2;
      continue;
    }
    if (c == '/' && i + 1 < n && s[i + 1] == '*') {
      st = LexState::Block;
      i += 2;
      continue;
    }
    if (c == '"') {
      st = LexState::Str;
      i++;
      continue;
    }
    if (c == '\'') {
      st = LexState::Chr;
      i++;
      continue;
    }
    if (c == '(') {
      depth++;
      i++;
      continue;
    }
    if (c == ')') {
      if (--depth == 0)
        return i;
      i++;
      continue;
    }
    i++;
  }
  return std::string::npos;
}

/* ── source transform model ───────────────────────────────────────────────── */

// A whole-method signature rewrite (`kind: "method"`): replace the argument list
// of a declaration of `name` with `params` (which includes the parentheses).
struct MethodTransform {
  std::string name;              // method identifier, e.g. "load_data"
  std::string params;            // replacement arg list incl. "()"
  std::string require_qualifier; // e.g. "override"; empty => not required
  std::string ts_query;          // reserved for Tier C; empty => use Tier A
  std::string description;
};

/* ── matcher abstraction (the Tier A / Tier C seam) ───────────────────────── */

// The engine depends only on this interface, so a tree-sitter backend can be
// dropped in later without touching chaining, reporting, or the CLI.
class SignatureRewriter {
public:
  virtual ~SignatureRewriter() = default;
  // Rewrite every declaration of t.name in `src` in place; append one Change per
  // edit. Returns the number of edits applied.
  virtual int rewrite_method(std::string &src, const MethodTransform &t,
                             std::vector<Change> &out) = 0;
};

// Tier A: anchor on the method name, balance parentheses to find the argument
// list, and swap it. Immune to whitespace/newlines/comments inside the list.
class SpanRewriter : public SignatureRewriter {
public:
  int rewrite_method(std::string &src, const MethodTransform &t,
                     std::vector<Change> &out) override {
    struct Span {
      std::size_t open, close;
    };
    std::vector<Span> spans;

    LexState st = LexState::Code;
    char last_code = '\0'; // last significant code char before the cursor
    const std::string &name = t.name;
    const std::size_t n = src.size();

    for (std::size_t i = 0; i < n;) {
      char c = src[i];
      switch (st) {
      case LexState::Line:
        if (c == '\n')
          st = LexState::Code;
        i++;
        continue;
      case LexState::Block:
        if (c == '*' && i + 1 < n && src[i + 1] == '/') {
          st = LexState::Code;
          i += 2;
        } else
          i++;
        continue;
      case LexState::Str:
        if (c == '\\') {
          i += 2;
          continue;
        }
        if (c == '"')
          st = LexState::Code;
        i++;
        continue;
      case LexState::Chr:
        if (c == '\\') {
          i += 2;
          continue;
        }
        if (c == '\'')
          st = LexState::Code;
        i++;
        continue;
      case LexState::Code:
        break;
      }

      if (c == '/' && i + 1 < n && src[i + 1] == '/') {
        st = LexState::Line;
        i += 2;
        continue;
      }
      if (c == '/' && i + 1 < n && src[i + 1] == '*') {
        st = LexState::Block;
        i += 2;
        continue;
      }
      if (c == '"') {
        st = LexState::Str;
        last_code = c;
        i++;
        continue;
      }
      if (c == '\'') {
        st = LexState::Chr;
        last_code = c;
        i++;
        continue;
      }
      if (std::isspace(static_cast<unsigned char>(c))) {
        i++;
        continue;
      }

      // Candidate identifier match with word boundaries.
      if (is_ident_char(c) && !name.empty() && c == name[0] &&
          (i == 0 || !is_ident_char(src[i - 1])) &&
          src.compare(i, name.size(), name) == 0) {
        std::size_t after = i + name.size();
        bool end_boundary = (after >= n) || !is_ident_char(src[after]);
        std::size_t j = after;
        while (j < n && std::isspace(static_cast<unsigned char>(src[j])))
          j++;
        if (end_boundary && j < n && src[j] == '(') {
          // Declaration vs. call: a call is member/arrow/scope-qualified, i.e.
          // preceded by '.', '->' (ends in '>'), or '::' (ends in ':').
          bool is_call =
              (last_code == '.' || last_code == '>' || last_code == ':');
          if (!is_call) {
            std::size_t close = find_matching_paren(src, j);
            if (close != std::string::npos) {
              bool ok = true;
              if (!t.require_qualifier.empty()) {
                std::size_t k = close + 1;
                while (k < n && std::isspace(static_cast<unsigned char>(src[k])))
                  k++;
                std::size_t qn = t.require_qualifier.size();
                ok = src.compare(k, qn, t.require_qualifier) == 0 &&
                     (k + qn >= n || !is_ident_char(src[k + qn]));
              }
              if (ok)
                spans.push_back({j, close});
            }
          }
        }
        last_code = src[after - 1];
        i = after;
        continue;
      }

      last_code = c;
      i++;
    }

    // Apply from last to first so earlier offsets stay valid.
    int count = 0;
    for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
      std::size_t line = line_of_offset(src, it->open);
      src.replace(it->open, it->close - it->open + 1, t.params);
      out.push_back({line, t.description});
      count++;
    }
    return count;
  }
};

#if defined(MADS_ENABLE_TREE_SITTER)
// Tier C: true CST rewriting via tree-sitter-cpp. Not built by default.
// TODO(Tier C): parse `src` with tree-sitter-cpp, run t.ts_query, and replace the
// byte range of the matched `parameter_list` node with t.params.
class TreeSitterRewriter : public SignatureRewriter {
public:
  int rewrite_method(std::string &, const MethodTransform &,
                     std::vector<Change> &) override {
    throw std::runtime_error("TreeSitterRewriter not implemented yet");
  }
};
#else
// Stub kept compiled so the seam stays visible and make_rewriter() can dispatch
// to it once tree-sitter-cpp is vendored in.
class TreeSitterRewriter : public SignatureRewriter {
public:
  int rewrite_method(std::string &, const MethodTransform &t,
                     std::vector<Change> &) override {
    throw std::runtime_error(
        "tree-sitter matcher (Tier C) not built: rebuild MADS with "
        "-DMADS_ENABLE_TREE_SITTER=ON. Transform for method '" +
        t.name + "' requested it via a non-null ts_query.");
  }
};
#endif

// Pick the matcher for a transform: Tier C when it carries a tree-sitter query
// and the backend is compiled in, otherwise the dependency-free Tier A scanner.
inline std::unique_ptr<SignatureRewriter>
make_rewriter(const MethodTransform &t) {
#if defined(MADS_ENABLE_TREE_SITTER)
  if (!t.ts_query.empty())
    return std::make_unique<TreeSitterRewriter>();
#else
  (void)t;
#endif
  return std::make_unique<SpanRewriter>();
}

/* ── literal / regex source transforms ────────────────────────────────────── */

inline int apply_literal(std::string &src, const std::string &find,
                         const std::string &replace, const std::string &desc,
                         std::vector<Change> &out) {
  int count = 0;
  std::size_t pos = 0;
  while ((pos = src.find(find, pos)) != std::string::npos) {
    out.push_back({line_of_offset(src, pos), desc});
    src.replace(pos, find.size(), replace);
    pos += replace.size();
    count++;
  }
  return count;
}

inline int apply_regex(std::string &src, const std::string &pattern,
                       const std::string &replacement, const std::string &flags,
                       const std::string &desc, std::vector<Change> &out) {
  auto opts = std::regex::ECMAScript;
  if (flags.find('i') != std::string::npos)
    opts |= std::regex::icase;
  std::regex re(pattern, opts);
  auto first = std::sregex_iterator(src.begin(), src.end(), re);
  int count = static_cast<int>(std::distance(first, std::sregex_iterator()));
  if (count > 0) {
    src = std::regex_replace(src, re, replacement);
    out.push_back({0, desc});
  }
  return count;
}

/* ── CMake transforms ─────────────────────────────────────────────────────── */

// Parse the protocol number from the plugin FetchContent block (the `-P<N>`
// suffix of GIT_TAG). Returns -1 if not found.
inline int detect_protocol(const std::string &cmake) {
  std::regex re(
      R"(FetchContent_Populate\s*\(\s*plugin\b[\s\S]*?GIT_TAG\s+v[0-9.]+-P([0-9]+))");
  std::smatch m;
  if (std::regex_search(cmake, m, re))
    return std::stoi(m[1].str());
  return -1;
}

// Rewrite the GIT_TAG of a named FetchContent block (`plugin`, `pugg`, ...).
//
// A regex alone is fragile here: a `#` comment containing the token GIT_TAG
// before the real one would be matched (lazy `[\s\S]*?GIT_TAG` is comment-blind),
// and building a `std::regex_replace` replacement as `"$1" + new_tag` breaks when
// new_tag starts with a digit (e.g. pugg "1.2.0" -> "$11.2.0" reads as $11). So we
// only use a regex to locate the block header, then tokenize the block with a tiny
// CMake-aware scanner (honouring `#` line comments, quoted args, and nested parens)
// to find the GIT_TAG value token, and replace exactly that span by position. This
// tolerates arbitrary whitespace/line-splitting and comments.
inline int bump_git_tag(std::string &cmake, const std::string &block,
                        const std::string &new_tag, std::vector<Change> &out) {
  std::regex header("FetchContent_(?:Populate|Declare)\\s*\\(\\s*" + block +
                    "\\b");
  std::smatch m;
  if (!std::regex_search(cmake, m, header))
    return 0;

  const std::size_t n = cmake.size();
  std::size_t i = m.position(0) + m.length(0); // just past the block name
  int depth = 1;                               // inside the block's '('
  bool saw_tag = false;
  std::size_t val_begin = std::string::npos, val_end = std::string::npos;

  while (i < n && depth > 0) {
    const char c = cmake[i];
    if (c == '#') { // CMake line comment: skip to end of line
      while (i < n && cmake[i] != '\n')
        ++i;
      continue;
    }
    if (c == '"') { // quoted argument: its inner content may be the value
      const std::size_t q_begin = ++i;
      while (i < n && cmake[i] != '"') {
        if (cmake[i] == '\\' && i + 1 < n)
          ++i;
        ++i;
      }
      const std::size_t q_end = i;
      if (i < n)
        ++i; // consume closing quote
      if (saw_tag) {
        val_begin = q_begin;
        val_end = q_end;
        break;
      }
      continue;
    }
    if (c == '(') { ++depth; ++i; continue; }
    if (c == ')') { --depth; ++i; continue; }
    if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }

    // A bare (unquoted) token: runs until whitespace or a delimiter.
    const std::size_t tok_begin = i;
    while (i < n && !std::isspace(static_cast<unsigned char>(cmake[i])) &&
           cmake[i] != '(' && cmake[i] != ')' && cmake[i] != '#' &&
           cmake[i] != '"')
      ++i;
    if (saw_tag) {
      val_begin = tok_begin;
      val_end = i;
      break;
    }
    if (cmake.compare(tok_begin, i - tok_begin, "GIT_TAG") == 0)
      saw_tag = true;
  }

  if (val_begin == std::string::npos)
    return 0;
  if (cmake.compare(val_begin, val_end - val_begin, new_tag) == 0)
    return 0;
  const std::size_t line = line_of_offset(cmake, val_begin);
  cmake.replace(val_begin, val_end - val_begin, new_tag);
  out.push_back({line, block + " GIT_TAG -> " + new_tag});
  return 1;
}

/* ── migration definition loading & chaining ──────────────────────────────── */

struct Migration {
  int from = -1;
  int to = -1;
  std::string lang = "cpp";
  json doc;
  fs::path file;
};

// Load every *.json in `dir` that targets C++ (lang == "cpp"), keyed by `from`.
inline std::map<int, Migration> load_migrations(const fs::path &dir) {
  std::map<int, Migration> steps;
  if (!fs::is_directory(dir))
    return steps;
  for (const auto &entry : fs::directory_iterator(dir)) {
    if (entry.path().extension() != ".json")
      continue;
    std::string text;
    if (!read_file(entry.path(), text))
      continue;
    json doc;
    try {
      doc = json::parse(text);
    } catch (const std::exception &) {
      continue; // skip malformed files
    }
    Migration m;
    m.file = entry.path();
    m.from = doc.value("from", -1);
    m.to = doc.value("to", -1);
    m.lang = doc.value("lang", "cpp");
    m.doc = std::move(doc);
    if (m.lang == "cpp" && m.from >= 0 && m.to == m.from + 1)
      steps[m.from] = std::move(m);
  }
  return steps;
}

/* ── per-file / overall report ────────────────────────────────────────────── */

struct FileReport {
  fs::path path;
  std::string original;
  std::string updated;
  std::vector<Change> changes;
  bool dirty() const { return updated != original; }
};

// Apply the `source` transforms of one migration step to one file buffer.
inline void apply_source_transforms(const json &step, FileReport &fr) {
  if (!step.contains("source"))
    return;
  for (const auto &t : step["source"]) {
    const std::string kind = t.value("kind", "");
    const std::string desc = t.value("description", kind);
    if (kind == "method") {
      MethodTransform mt;
      mt.name = t.value("name", "");
      mt.params = t.value("params", "");
      mt.require_qualifier = t.value("require_qualifier", "");
      mt.ts_query = t.contains("ts_query") && !t["ts_query"].is_null()
                        ? t["ts_query"].get<std::string>()
                        : "";
      mt.description = desc;
      auto rw = make_rewriter(mt);
      rw->rewrite_method(fr.updated, mt, fr.changes);
    } else if (kind == "literal") {
      apply_literal(fr.updated, t.value("find", ""), t.value("replace", ""), desc,
                    fr.changes);
    } else if (kind == "regex") {
      apply_regex(fr.updated, t.value("pattern", ""), t.value("replacement", ""),
                  t.value("flags", ""), desc, fr.changes);
    }
  }
}

// Apply the `cmake` transforms of one migration step to the CMake buffer.
inline void apply_cmake_transforms(const json &step, FileReport &cmake) {
  if (!step.contains("cmake"))
    return;
  const json &c = step["cmake"];
  if (c.contains("plugin_git_tag"))
    bump_git_tag(cmake.updated, "plugin", c["plugin_git_tag"].get<std::string>(),
                 cmake.changes);
  if (c.contains("pugg_git_tag"))
    bump_git_tag(cmake.updated, "pugg", c["pugg_git_tag"].get<std::string>(),
                 cmake.changes);
  if (c.contains("replacements")) {
    for (const auto &r : c["replacements"]) {
      apply_regex(cmake.updated, r.value("regex", ""), r.value("replacement", ""),
                  r.value("flags", ""), r.value("description", "cmake edit"),
                  cmake.changes);
    }
  }
}

/* ── engine entry point ───────────────────────────────────────────────────── */

// Returns: 0 = migrated (or already current), 1 = usage/structural error,
//          3 = migrated but the Tier B build check failed.
inline int run(const fs::path &project_dir, const fs::path &migrations_dir,
               const fs::path &deps_manifest, const Options &opts) {
  using namespace rang;

  fs::path cmake_path = project_dir / "CMakeLists.txt";
  FileReport cmake;
  cmake.path = cmake_path;
  if (!read_file(cmake_path, cmake.original)) {
    std::cerr << fg::red << "Error: no CMakeLists.txt in " << project_dir
              << fg::reset << std::endl;
    return 1;
  }
  cmake.updated = cmake.original;

  int cur = opts.from_override >= 0 ? opts.from_override
                                    : detect_protocol(cmake.original);
  if (cur < 0) {
    std::cerr << fg::red
              << "Error: cannot detect the plugin protocol (no `GIT_TAG "
                 "v*-P<N>` in the plugin FetchContent block). Use --from to "
                 "override."
              << fg::reset << std::endl;
    return 1;
  }

  auto steps = load_migrations(migrations_dir);

  int target = opts.to_override;
  if (target < 0) {
    // Default target = manifest protocol, else the highest known migration step.
    std::string manifest_text;
    if (read_file(deps_manifest, manifest_text)) {
      try {
        target = json::parse(manifest_text).value("plugin_protocol", -1);
      } catch (const std::exception &) {
      }
    }
    if (target < 0 && !steps.empty())
      target = steps.rbegin()->second.to;
  }
  if (target < 0) {
    std::cerr << fg::red << "Error: cannot determine a target protocol."
              << fg::reset << std::endl;
    return 1;
  }

  std::cout << style::bold << "Plugin migration: " << project_dir.filename().string()
            << style::reset << std::endl;
  std::cout << "  Detected protocol: " << fg::yellow << "P" << cur << fg::reset
            << "   Target: " << fg::green << "P" << target << fg::reset
            << std::endl;

  if (cur >= target) {
    std::cout << fg::green << "  Already at protocol P" << cur
              << " (>= target). Nothing to do." << fg::reset << std::endl;
    return 0;
  }

  // Verify the full chain cur -> target exists before touching anything.
  for (int v = cur; v < target; ++v) {
    if (steps.find(v) == steps.end()) {
      std::cerr << fg::red << "Error: no migration step for P" << v << " -> P"
                << (v + 1) << " in " << migrations_dir << fg::reset << std::endl;
      return 1;
    }
  }

  // Collect the plugin's source files.
  std::vector<FileReport> sources;
  fs::path src_dir = project_dir / "src";
  if (fs::is_directory(src_dir)) {
    for (const auto &e : fs::directory_iterator(src_dir)) {
      auto ext = e.path().extension().string();
      if (ext == ".cpp" || ext == ".cxx" || ext == ".cc" || ext == ".hpp" ||
          ext == ".h") {
        FileReport fr;
        fr.path = e.path();
        if (read_file(fr.path, fr.original)) {
          fr.updated = fr.original;
          sources.push_back(std::move(fr));
        }
      }
    }
  }

  // Apply each step in order, gathering follow-up notes.
  std::vector<std::string> notes;
  for (int v = cur; v < target; ++v) {
    const json &step = steps[v].doc;
    std::cout << "  Applying step " << style::bold << "P" << v << " -> P"
              << (v + 1) << style::reset << " (" << steps[v].file.filename().string()
              << ")" << std::endl;
    apply_cmake_transforms(step, cmake);
    for (auto &fr : sources)
      apply_source_transforms(step, fr);
    if (step.contains("notes"))
      for (const auto &nnote : step["notes"])
        notes.push_back(nnote.get<std::string>());
  }

  // Report and (unless dry-run) write, backing up originals first.
  auto report_file = [&](FileReport &fr) {
    if (!fr.dirty())
      return;
    std::cout << "  " << style::bold << fs::relative(fr.path, project_dir).string()
              << style::reset << ":" << std::endl;
    for (const auto &ch : fr.changes) {
      std::cout << "    " << fg::green << "✓" << fg::reset << " " << ch.detail;
      if (ch.line > 0)
        std::cout << fg::gray << " (line " << ch.line << ")" << fg::reset;
      std::cout << std::endl;
    }
    if (!opts.dry_run) {
      write_file(fr.path.string() + ".bak", fr.original);
      write_file(fr.path, fr.updated);
    }
  };

  int edited_files = 0;
  report_file(cmake);
  if (cmake.dirty())
    edited_files++;
  for (auto &fr : sources) {
    report_file(fr);
    if (fr.dirty())
      edited_files++;
  }

  if (edited_files == 0) {
    std::cout << fg::yellow
              << "  No matching code found to change (already migrated?)."
              << fg::reset << std::endl;
  } else if (opts.dry_run) {
    std::cout << std::endl
              << fg::yellow << style::bold << "Dry run: " << style::reset
              << fg::yellow << "no files written (" << edited_files
              << " would change)." << fg::reset << std::endl;
  } else {
    std::cout << std::endl
              << fg::green << "Migrated " << edited_files
              << " file(s); originals saved as *.bak." << fg::reset << std::endl;
  }

  if (!notes.empty()) {
    std::cout << std::endl << style::bold << "MANUAL FOLLOW-UP:" << style::reset
              << std::endl;
    for (const auto &nnote : notes)
      std::cout << "  " << fg::yellow << "•" << fg::reset << " " << nnote
                << std::endl;
  }

  // Tier B: compile the migrated plugin; the base classes are pure-virtual, so a
  // botched override cannot compile. Surface any `error:` lines as follow-ups.
  if (opts.check && !opts.dry_run && edited_files > 0) {
    std::cout << std::endl
              << style::bold << "Verifying (cmake build)…" << style::reset
              << std::endl;
    fs::path build_dir = project_dir / "build";
    std::string out;
    std::string cfg = "cmake -S \"" + project_dir.string() + "\" -B \"" +
                      build_dir.string() + "\"";
    std::string bld = "cmake --build \"" + build_dir.string() + "\"";
    int rc = run_command(cfg, out);
    if (rc == 0)
      rc = run_command(bld, out);
    if (rc == 0) {
      std::cout << "  " << fg::green << "✓ Plugin compiles cleanly against P"
                << target << "." << fg::reset << std::endl;
    } else {
      std::cout << "  " << fg::red
                << "✗ Build failed — the rewrite likely needs manual "
                   "fixes. Compiler errors:"
                << fg::reset << std::endl;
      std::istringstream iss(out);
      std::string ln;
      int shown = 0;
      while (std::getline(iss, ln) && shown < 40) {
        std::string low = ln;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (low.find("error") != std::string::npos) {
          std::cout << "    " << ln << std::endl;
          shown++;
        }
      }
      std::cout << fg::gray << "  (full log under " << build_dir.string() << ")"
                << fg::reset << std::endl;
      return 3;
    }
  }

  return 0;
}

} // namespace PluginMigrate
} // namespace Mads

#endif // PLUGIN_MIGRATE_HPP
