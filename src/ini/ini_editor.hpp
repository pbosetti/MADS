/*
 ___       _   _____    _ _ _
|_ _|_ __ (_) | ____|__| (_) |_ ___  _ __
 | || '_ \| | |  _| / _` | | __/ _ \| '__|
 | || | | | | | |__| (_| | | || (_) | |
|___|_| |_|_| |_____\__,_|_|\__\___/|_|

GUI-free state and operations of the mads-ini editor: the open document and
where it came from, undo/redo, the topology derived from it, node and link
editing. The ImGui front end (src/gui/ini) only draws this state and calls
these operations, so everything here is unit-testable.

Editing protocol for the front end: mutate doc() (directly or through the
helpers below, which do it for you), then call commit() once per completed
user action (not per frame, not per keystroke of an unfinished text field):
commit() records the undo step and recomputes the derived topology.

Author(s): Paolo Bosetti
*/
#pragma once

#include "ini_document.hpp"
#include "ini_schema.hpp"
#include "topology_graph.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Mads::Ini {

class Editor {
public:
  explicit Editor(const Schema &schema = Schema::builtin());

  // ----------------------------------------------------------- documents

  /// Replaces the document with `text` (e.g. the rendered template), with no
  /// source: the next save needs save_as(). Clears undo history.
  void new_document(const std::string &text);
  /// Loads `path` through a FileSource. On failure returns false, sets
  /// `error` and leaves the current document untouched.
  bool open(const std::filesystem::path &path, std::string *error = nullptr);
  /// Loads any Source (e.g. a Remote::RemoteSource) and makes it the current
  /// one. On failure returns false, sets `error` and leaves the current
  /// document untouched -- except that Remote::AuthError is rethrown, so the
  /// caller can ask for a password and retry with a new source (the old one is
  /// destroyed with the exception). `source` is consumed either way.
  bool open_source(std::unique_ptr<Source> source,
                   std::string *error = nullptr);
  /// Reloads from the current source, discarding unsaved changes. Like
  /// open_source(), rethrows Remote::AuthError.
  bool reload(std::string *error = nullptr);
  /// Saves to the current source (base etag = last loaded/saved one).
  /// Conflict when the file changed on disk meanwhile.
  SaveResult save();
  /// Saves to `path` (no conflict check) and makes that LOCAL file the
  /// current source -- also for a document that was opened remotely.
  SaveResult save_as(const std::filesystem::path &path);
  /// Like save() but ignoring the etag (overwrite after a conflict).
  SaveResult save_overwrite();

  bool has_source() const { return _source != nullptr; }
  /// Etag of the content as last loaded or saved (what changed_on_disk()
  /// compares the source against); empty without a source.
  const std::string &etag() const { return _etag; }
  /// The current source (nullptr without one), e.g. to hand a
  /// Remote::RemoteSource a new password-derived secret.
  Source *source() { return _source.get(); }
  /// The source's path, or "untitled".
  std::string location() const;
  /// Unsaved changes since the last load/save.
  bool dirty() const;
  /// Polls the source: true when its content differs from what was last
  /// loaded or saved (someone else edited the file). Cheap enough to call
  /// about once a second; returns false when there is no source or it is
  /// unreadable.
  bool changed_on_disk();

  // --------------------------------------------------------------- model

  Document &doc() { return _doc; }
  const Document &doc() const { return _doc; }
  const Schema &schema() const { return _schema; }

  /// Records an undo step labelled `what` if the document changed since the
  /// previous commit, then recomputes the derived state below.
  void commit(const std::string &what);
  bool can_undo() const;
  bool can_redo() const;
  /// Labels of the steps undo()/redo() would revert/reapply ("" if none).
  std::string undo_label() const;
  std::string redo_label() const;
  void undo();
  void redo();

  // ------------------------------------------------- derived (on commit)

  /// Node sections: every section but [agents]/[broker], in file order.
  const std::vector<std::string> &nodes() const { return _nodes; }
  /// Type id of a section (Schema::infer_type), cached per commit.
  std::string type_of(const std::string &section) const;
  /// Role of a node section: which pins it shows and where auto_layout()
  /// puts it. The schema type's role if that is not "any"; else the
  /// `role=` of its `#@` meta if it is source, filter or sink; else
  /// automatic_role(). "" for a section that is not a node. Cached per commit.
  std::string role_of(const std::string &section) const;
  /// The role from the section's active keys alone: pub_topic only is
  /// "source", sub_topic only is "sink", both or neither is "filter".
  std::string automatic_role(const std::string &section) const;
  /// True when role_of() comes from automatic_role() (no explicit role).
  bool role_is_inferred(const std::string &section) const;
  /// Sets the explicit role of a node whose schema type role is "any":
  /// source, filter or sink, or "" to remove it (back to automatic).
  /// Returns false, changing nothing, for other sections and other values.
  /// Commits "Set role of <section>" when the meta changed.
  bool set_role(const std::string &section, const std::string &role);
  const std::map<std::string, AgentTopicInfo> &topics() const {
    return _topics;
  }
  /// All edges, catch-all ones included (see TopologyEdge::catch_all).
  const std::vector<TopologyEdge> &edges() const { return _edges; }
  const DanglingTopics &dangling() const { return _dangling; }
  /// Parse diagnostics, validate() and lint() of the current document (not
  /// of the text as loaded: fixed lines stop being reported).
  const std::vector<Diagnostic> &diagnostics() const { return _diagnostics; }

  // ---------------------------------------------------------- node ops
  // Each of these commits on success.

  /// Adds a section of node type `type_id` at canvas position (x, y), named
  /// after the type's first free alias, else `type_id` or, if taken,
  /// `type_id_2`, `type_id_3`, ... Writes the `#@`
  /// meta. Returns the new name, or "" for an unknown or document type.
  std::string add_node(const std::string &type_id, float x, float y);
  bool delete_node(const std::string &name);
  /// Copies a section (entries, comment, type) under a fresh name, offset
  /// by (40, 40) on the canvas. Returns the new name or "".
  std::string duplicate_node(const std::string &name);
  /// Renames a section. Subscriptions of other sections that matched the
  /// old *implicit* pub_topic (the section name) are not rewritten: a
  /// renamed section with no pub_topic key publishes under its new name.
  bool rename_node(const std::string &old_name, const std::string &new_name);
  /// Stores a node position in its meta. Commits only if `commit_now`
  /// (callers moving a node every frame commit once, at drag end).
  void set_position(const std::string &name, float x, float y,
                    bool commit_now = false);
  /// Places every node that has no position (or all of them if `all`):
  /// layered left-to-right by longest path from publishers to subscribers
  /// (ignoring catch-all edges), sources left, sinks right, nodes in a layer
  /// stacked top-down in file order, wrapping into extra columns past a few
  /// rows. A sink role (see role_of()) with no edges other than catch-all
  /// ones goes to the last layer, as catch-all-only subscribers do.
  /// Commits if anything moved.
  /// Loading a document (new_document, open, reload) already places the
  /// nodes that have no position, as part of the clean state: a freshly
  /// opened file is not dirty, and the positions reach the file with the
  /// next save.
  void auto_layout(bool all = false);

  // ---------------------------------------------------------- link ops

  enum class ConnectResult {
    Added,          ///< publisher's topic appended to subscriber's sub_topic
    AlreadyMatched, ///< subscriber already receives it (see `via`)
    Invalid         ///< unknown section, self-link, or no pub topic
  };
  /// Makes `to` receive what `from` publishes (its effective pub_topic,
  /// implicit included): appends that topic to `to`'s sub_topic, turning a
  /// bare-string sub_topic into an array and creating the key if needed.
  /// When some entry already matches, sets `via` to it and changes nothing.
  /// Commits on Added.
  ConnectResult connect(const std::string &from, const std::string &to,
                        std::string *via = nullptr);

  enum class DisconnectResult {
    Removed,    ///< the exact sub_topic entry was removed
    ViaPattern, ///< matched by a prefix/wildcard/catch-all entry: refused
    NotFound
  };
  /// Removes the edge from->to by deleting the subscriber's sub_topic
  /// entries that equal the publisher's topic exactly; refused when the
  /// edge also exists through a non-exact entry (the user must edit that
  /// pattern). Removing the last entry leaves `sub_topic = []`. Commits on
  /// Removed.
  DisconnectResult disconnect(const std::string &from, const std::string &to);

private:
  void recompute();
  void reset_history();
  // auto_layout() without the commit; true if any position changed.
  bool layout_nodes(bool all);
  // Common tail of new_document/open/reload: derived state, initial
  // layout, clean baseline, fresh history.
  void finish_load();

  const Schema &_schema;
  Document _doc;
  std::unique_ptr<Source> _source;
  std::string _etag;            // of the last load/save
  std::string _saved_text;      // serialize() at the last load/save
  struct Step {
    std::string text;
    std::string label;
  };
  std::vector<Step> _history;   // _history[_cursor] is the current text
  size_t _cursor = 0;
  std::vector<std::string> _nodes;
  std::map<std::string, std::string> _types;
  std::map<std::string, AgentTopicInfo> _topics;
  std::vector<TopologyEdge> _edges;
  DanglingTopics _dangling;
  std::vector<Diagnostic> _diagnostics;
  struct RoleInfo {
    std::string role;      // effective role
    std::string automatic; // automatic_role()
    bool inferred = false; // role == automatic
  };
  std::map<std::string, RoleInfo> _roles;
};

} // namespace Mads::Ini
