/*
 ___       _   ____                                        _
|_ _|_ __ (_) |  _ \  ___   ___ _   _ _ __ ___   ___ _ __ | |_
 | || '_ \| | | | | |/ _ \ / __| | | | '_ ` _ \ / _ \ '_ \| __|
 | || | | | | | |_| | (_) | (__| |_| | | | | | |  __/ | | | |_
|___|_| |_|_| |____/ \___/ \___|\__,_|_| |_| |_|\___|_| |_|\__|

Comment-preserving model of a mads.ini (TOML) settings file, used by
`mads ini` and the mads-ini editor. toml++ drops comments, so the file is read
by a line-oriented parser that keeps every comment attached to the section or
key it documents; toml++ is used only to validate single values and the whole
serialized text.

Layout of a section, as parsed and serialized:

  # ---- banner ----            <- Section::preamble (verbatim lines)
  [name]                        <- header
  #@ type=filter pos=420,180    <- Section::meta (editor-only, hidden)
  # What this agent does        <- Section::comment (one line per '\n')
  # Leading note on period      <- Entry::leading_comments (verbatim lines)
  period = 100 # ms             <- Entry: key, raw_value, inline_comment
  # dont_block = false          <- Entry with disabled = true
  # trailing note               <- Section::trailing (verbatim lines)
  [name.sub]                    <- Section::raw_children (verbatim block)

Serialization normalizes layout only: `key = value # comment` spacing, at most
one consecutive blank line, exactly one blank line before each section (none
before the first one, unless root keys precede it), no blank lines at the end
of a section, LF line endings, a single trailing newline. Values are written
back verbatim from raw_value.

Author(s): Paolo Bosetti
*/
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Mads::Ini {

/// A problem found while parsing or linting a document.
struct Diagnostic {
  enum class Severity { Info, Warning, Error };
  Severity severity = Severity::Error;
  int line = 0;         ///< 1-based source line, 0 when not applicable
  std::string section;  ///< section name, empty when not applicable
  std::string key;      ///< key name, empty when not applicable
  std::string message;
};

/// One `key = value` line (possibly a multi-line value), active or
/// commented out.
struct Entry {
  /// Key text as written (bare, quoted or dotted keys are kept verbatim).
  std::string key;
  /// Verbatim TOML value text, without the inline comment and surrounding
  /// whitespace. Spans several lines for multi-line arrays/strings (inner
  /// comments of a multi-line array stay inside raw_value).
  std::string raw_value;
  /// Text after the '#' that follows the value on its (last) line, trimmed
  /// of surrounding whitespace. Empty when there is none. Serialized as
  /// `key = value # inline_comment`.
  std::string inline_comment;
  /// Comment and blank lines directly above the entry, verbatim (blank lines
  /// are empty strings, comments keep their leading '#').
  std::vector<std::string> leading_comments;
  /// True for a commented-out knob: a line `#key = value` or `# key = value`
  /// (at most one space after '#') whose value parses as a single-line TOML
  /// value. Serialized as `# key = value`.
  bool disabled = false;
};

/// Editor-only metadata, stored as a `#@ k=v k=v` line right under the
/// section header. Agents never see it: it is a TOML comment.
struct NodeMeta {
  std::string type;                ///< node type id (see ini_schema.hpp)
  std::optional<float> x, y;       ///< canvas position (written as integers)
  std::map<std::string, std::string> extra; ///< unknown k=v, kept verbatim
  bool empty() const { return type.empty() && !x && !y && extra.empty(); }
};

/// One `[name]` table.
struct Section {
  std::string name;
  std::vector<std::string> preamble;
  /// The contiguous comment lines right under the header (after the `#@`
  /// line, if any), stripped of '#' and of one following space, joined with
  /// '\n'. A blank line, an entry or a disabled entry ends the block.
  std::string comment;
  NodeMeta meta;
  std::vector<Entry> entries;
  std::vector<std::string> trailing;
  /// Sub-table blocks `[name.x]` / `[[name.x]]` that follow this section,
  /// each kept verbatim (header line included, '\n'-joined).
  std::vector<std::string> raw_children;

  /// The active entry for `key`, or nullptr.
  Entry *find(std::string_view key);
  const Entry *find(std::string_view key) const;
  /// The disabled entry for `key`, or nullptr.
  Entry *find_disabled(std::string_view key);
  const Entry *find_disabled(std::string_view key) const;
};

class Document {
public:
  /// Parses `text`. Never throws: problems are appended to `diagnostics`
  /// (when given) and the offending lines are kept verbatim, so that
  /// serialize() never loses content. Accepts LF and CRLF input.
  static Document parse(std::string_view text,
                        std::vector<Diagnostic> *diagnostics = nullptr);

  /// Normalized text (see file header). Top-level keys that precede the
  /// first header (root table) are emitted first.
  std::string serialize() const;

  /// Serializes and re-parses with toml++. Returns the parse error (with
  /// line) if the result is not valid TOML, an empty vector otherwise.
  std::vector<Diagnostic> validate() const;

  Section &root() { return _root; }
  const Section &root() const { return _root; }
  std::vector<Section> &sections() { return _sections; }
  const std::vector<Section> &sections() const { return _sections; }
  Section *find(std::string_view name);
  const Section *find(std::string_view name) const;

  /// Appends a new empty section; returns nullptr if the name is taken or
  /// is not a valid bare key.
  Section *add_section(const std::string &name);
  bool remove_section(std::string_view name);
  /// Renames a section (and the headers of its raw_children). Fails if
  /// `new_name` is taken or not a bare key.
  bool rename_section(std::string_view old_name, const std::string &new_name);

  /// The active raw value of `section.key`, or nullopt.
  std::optional<std::string> raw_value(std::string_view section,
                                       std::string_view key) const;
  /// The active raw value of `section.key`, else of `agents.key`.
  std::optional<std::string> effective_raw_value(std::string_view section,
                                                 std::string_view key) const;
  /// Sets `section.key` to `raw` (trimmed). Returns false, changing nothing,
  /// if the section does not exist or `raw` is not a valid TOML value.
  /// Updates the active entry; else re-enables a disabled one; else appends a
  /// new entry at the end of the section.
  bool set_raw_value(std::string_view section, std::string_view key,
                     std::string_view raw);
  /// Turns an active entry into a disabled one or vice versa. Enabling a key
  /// that also has an active entry disables that one first (swap). Returns
  /// false if there is nothing to toggle.
  bool set_enabled(std::string_view section, std::string_view key,
                   bool enabled);
  /// Removes the active (and any disabled) entry of `key`.
  bool remove_key(std::string_view section, std::string_view key);

private:
  Section _root; // name is empty; no header is emitted
  std::vector<Section> _sections;
};

/// True if `raw` parses as exactly one TOML value.
bool is_valid_value(std::string_view raw);
/// True if `key` is a TOML bare key (A-Za-z0-9_-, non-empty).
bool is_bare_key(std::string_view key);

/// What a Source returns on load.
struct Loaded {
  std::string text;
  std::string etag; ///< opaque content version, used to detect conflicts
};

struct SaveResult {
  enum class Status { Ok, Conflict, Error };
  Status status = Status::Error;
  std::string etag;    ///< new etag on Ok
  std::string message; ///< human-readable reason on Conflict/Error
};

/// Where a document is loaded from and saved to. FileSource now; a remote
/// (broker-side) source later implements the same interface.
class Source {
public:
  virtual ~Source() = default;
  /// Throws std::runtime_error when the content cannot be read.
  virtual Loaded load() = 0;
  /// Writes `text` if the stored content still matches `base_etag` (an
  /// empty base_etag skips the check). Never throws.
  virtual SaveResult save(const std::string &text,
                          const std::string &base_etag) = 0;
  /// Human-readable location, e.g. the file path.
  virtual std::string describe() const = 0;
};

/// A local file. etag = hex FNV-1a 64 of the content. save() refuses text
/// that is not valid TOML, writes a temp file in the same directory and
/// renames it over the target, keeping the previous content as `<file>.bak`.
class FileSource : public Source {
public:
  explicit FileSource(std::filesystem::path path);
  Loaded load() override;
  SaveResult save(const std::string &text,
                  const std::string &base_etag) override;
  std::string describe() const override;
  const std::filesystem::path &path() const { return _path; }

private:
  std::filesystem::path _path;
};

/// The etag FileSource computes for `text`.
std::string content_etag(std::string_view text);

} // namespace Mads::Ini
