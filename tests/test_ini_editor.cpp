// Unit tests for the GUI-free mads-ini editor state in src/ini/ini_editor.hpp:
// undo/redo, derived topology, node and link operations, saving.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "ini_editor.hpp"

using Mads::Ini::Editor;
using Mads::Ini::SaveResult;

namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_file(const fs::path &path, const std::string &text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

// A fresh scratch directory, removed on destruction.
struct TempDir {
  fs::path path;
  TempDir() {
    static int counter = 0;
    path = fs::temp_directory_path() /
           ("test_ini_editor_" + std::to_string(std::chrono::steady_clock::now()
                              .time_since_epoch()
                              .count()) +
            "_" + std::to_string(counter++));
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

const Mads::TopologyEdge *find_edge(const Editor &ed, const std::string &from,
                                    const std::string &to) {
  for (const auto &e : ed.edges()) {
    if (e.from == from && e.to == to) {
      return &e;
    }
  }
  return nullptr;
}

const char *kPipeline = R"([agents]
timecode_fps = 25

[src]
#@ type=custom pos=0,0
pub_topic = "raw"

[flt]
#@ type=custom pos=10,10
sub_topic = ["raw"]
pub_topic = "clean"

[snk]
#@ type=custom pos=20,20
sub_topic = ["clean"]

[all]
sub_topic = [""]
)";

} // namespace

TEST_CASE("new_document derives nodes and types", "[ini][editor]") {
  Editor ed;
  CHECK(ed.nodes().empty());
  CHECK_FALSE(ed.has_source());
  CHECK(ed.location() == "untitled");

  ed.new_document("[agents]\nx = 1\n\n[broker]\n\n[logger]\n\n[a]\n"
                  "#@ type=filter\n\n[b]\npub_topic = \"b\"\n");
  REQUIRE(ed.nodes() == std::vector<std::string>{"logger", "a", "b"});
  CHECK(ed.type_of("logger") == "logger");
  CHECK(ed.type_of("a") == "filter");
  CHECK(ed.type_of("agents") == "");
  CHECK_FALSE(ed.dirty());
  CHECK_FALSE(ed.can_undo());
}

TEST_CASE("invalid TOML leaves topology empty and reports a diagnostic",
          "[ini][editor]") {
  Editor ed;
  ed.new_document(kPipeline);
  REQUIRE_FALSE(ed.edges().empty());
  ed.doc().root().entries.push_back({"bad", "= = =", "", {}, false});
  ed.commit("break");
  CHECK(ed.edges().empty());
  CHECK(ed.topics().empty());
  CHECK_FALSE(ed.diagnostics().empty());
}

TEST_CASE("open() of the repo mads.ini yields its topology",
          "[ini][editor]") {
  Editor ed;
  std::string error;
  REQUIRE(ed.open(fs::path(MADS_PROJECT_SOURCE_DIR) / "mads.ini", &error));
  CHECK(ed.has_source());
  // Every node got a position on load, yet the document is clean.
  CHECK_FALSE(ed.dirty());
  CHECK_FALSE(ed.can_undo());
  for (const auto &name : ed.nodes()) {
    CHECK(ed.doc().find(name)->meta.x.has_value());
  }

  const auto *exact = find_edge(ed, "publish", "fmu_linear_axis");
  REQUIRE(exact);
  CHECK(exact->kind == Mads::SubMatch::Exact);
  CHECK(exact->topic == "fmu_control");

  const auto *implicit = find_edge(ed, "serial_reader", "running_avg");
  REQUIRE(implicit);
  CHECK(implicit->kind == Mads::SubMatch::Exact);
  CHECK(implicit->implicit_pub);

  CHECK(std::find(ed.nodes().begin(), ed.nodes().end(), "logger") !=
        ed.nodes().end());
  CHECK(std::find(ed.nodes().begin(), ed.nodes().end(), "agents") ==
        ed.nodes().end());
}

TEST_CASE("open() failure leaves the document untouched", "[ini][editor]") {
  Editor ed;
  ed.new_document(kPipeline);
  std::string error;
  CHECK_FALSE(ed.open("/definitely/not/there.ini", &error));
  CHECK_FALSE(error.empty());
  CHECK(ed.nodes().size() == 4);
  CHECK_FALSE(ed.has_source());
}

TEST_CASE("commit, undo, redo, labels and dirty", "[ini][editor]") {
  Editor ed;
  ed.new_document(kPipeline);
  const std::string original = ed.doc().serialize();

  ed.doc().set_raw_value("src", "pub_topic", "\"raw2\"");
  CHECK(ed.dirty());
  ed.commit("Change topic");
  CHECK(ed.can_undo());
  CHECK_FALSE(ed.can_redo());
  CHECK(ed.undo_label() == "Change topic");
  CHECK(ed.redo_label() == "");
  CHECK(ed.topics().at("src").pub_topic == "raw2");

  ed.doc().set_raw_value("src", "pub_topic", "\"raw3\"");
  ed.commit("Again");
  CHECK(ed.undo_label() == "Again");

  ed.undo();
  CHECK(ed.undo_label() == "Change topic");
  CHECK(ed.redo_label() == "Again");
  CHECK(ed.topics().at("src").pub_topic == "raw2");
  ed.undo();
  CHECK(ed.doc().serialize() == original);
  CHECK_FALSE(ed.dirty());
  CHECK_FALSE(ed.can_undo());
  CHECK(ed.can_redo());
  ed.undo(); // nothing left: harmless
  CHECK(ed.doc().serialize() == original);

  ed.redo();
  ed.redo();
  CHECK(ed.topics().at("src").pub_topic == "raw3");
  CHECK_FALSE(ed.can_redo());

  // A new step discards the redo branch.
  ed.undo();
  ed.doc().set_raw_value("src", "pub_topic", "\"other\"");
  ed.commit("Branch");
  CHECK_FALSE(ed.can_redo());
}

TEST_CASE("commit without changes records no step", "[ini][editor]") {
  Editor ed;
  ed.new_document(kPipeline);
  ed.commit("nothing");
  CHECK_FALSE(ed.can_undo());
  CHECK_FALSE(ed.dirty());
}

TEST_CASE("add_node names, meta and rejections", "[ini][editor]") {
  Editor ed;
  CHECK(ed.add_node("filter", 100, 50) == "filter");
  CHECK(ed.add_node("filter", 300, 50) == "filter_2");
  CHECK(ed.add_node("no_such_type", 0, 0) == "");
  CHECK(ed.add_node("agents", 0, 0) == "");
  CHECK(ed.add_node("broker", 0, 0) == "");
  REQUIRE(ed.nodes() == std::vector<std::string>{"filter", "filter_2"});

  const auto *s = ed.doc().find("filter_2");
  REQUIRE(s);
  CHECK(s->meta.type == "filter");
  CHECK(*s->meta.x == 300.0f);
  CHECK(*s->meta.y == 50.0f);
  CHECK(s->entries.empty()); // no pub_topic written
  CHECK(ed.type_of("filter") == "filter");
  CHECK(ed.undo_label() == "Add filter_2");
  CHECK(ed.dirty());
}

TEST_CASE("add_node names a node after the type's aliases first",
          "[ini][editor]") {
  Editor ed;
  CHECK(ed.add_node("federate", 0, 0) == "federate_a");
  CHECK(ed.add_node("federate", 0, 0) == "federate_b");
  CHECK(ed.add_node("federate", 0, 0) == "federate");
  CHECK(ed.add_node("federate", 0, 0) == "federate_2");
  CHECK(ed.type_of("federate_b") == "federate");
}

TEST_CASE("duplicate, rename and delete nodes", "[ini][editor]") {
  Editor ed;
  ed.new_document(std::string(kPipeline) + "\n[flt.extra]\nk = 1\n");
  // [flt.extra] belongs to flt.
  REQUIRE(ed.doc().find("flt")->raw_children.size() == 1);

  std::string copy = ed.duplicate_node("flt");
  CHECK(copy == "flt_2");
  const auto *c = ed.doc().find("flt_2");
  REQUIRE(c);
  CHECK(*c->meta.x == 50.0f);
  CHECK(*c->meta.y == 50.0f);
  CHECK(c->find("pub_topic")->raw_value == "\"clean\"");
  REQUIRE(c->raw_children.size() == 1);
  CHECK(c->raw_children[0].find("[flt_2.extra]") != std::string::npos);
  CHECK(ed.duplicate_node("nope") == "");
  CHECK(ed.duplicate_node("agents") == "");

  CHECK(ed.rename_node("flt_2", "second"));
  CHECK(ed.doc().find("second"));
  CHECK_FALSE(ed.doc().find("flt_2"));
  CHECK_FALSE(ed.rename_node("second", "src")); // taken
  CHECK_FALSE(ed.rename_node("second", "bad name"));
  CHECK_FALSE(ed.rename_node("nope", "x"));
  CHECK(ed.undo_label() == "Rename flt_2");

  // A renamed section without pub_topic publishes under its new name.
  ed.doc().remove_key("second", "pub_topic");
  ed.commit("drop pub");
  CHECK(ed.topics().at("second").pub_topic == "second");
  CHECK(ed.topics().at("second").pub_implicit);

  CHECK(ed.delete_node("second"));
  CHECK_FALSE(ed.doc().find("second"));
  CHECK_FALSE(ed.delete_node("second"));
  CHECK_FALSE(ed.delete_node("agents"));
  ed.undo();
  CHECK(ed.doc().find("second"));
}

TEST_CASE("set_position commits only on request", "[ini][editor]") {
  Editor ed;
  ed.new_document(kPipeline);
  ed.set_position("src", 111, 222);
  CHECK(ed.dirty());
  CHECK_FALSE(ed.can_undo());
  ed.set_position("src", 333, 444, true);
  CHECK(ed.can_undo());
  CHECK(*ed.doc().find("src")->meta.x == 333.0f);
  ed.set_position("nope", 1, 2, true); // ignored
  ed.undo();
  CHECK(*ed.doc().find("src")->meta.x == 0.0f);
}

TEST_CASE("auto_layout orders sources, filters and sinks left to right",
          "[ini][editor]") {
  Editor ed;
  ed.new_document("[src]\npub_topic = \"raw\"\n\n"
                  "[snk]\nsub_topic = [\"clean\"]\n\n"
                  "[flt]\nsub_topic = [\"raw\"]\npub_topic = \"clean\"\n\n"
                  "[all]\nsub_topic = [\"\"]\n");
  // Laid out on load, as part of the clean state.
  const auto x = [&](const char *n) { return *ed.doc().find(n)->meta.x; };
  const auto y = [&](const char *n) { return *ed.doc().find(n)->meta.y; };
  CHECK(x("src") == 0.0f);
  CHECK(x("flt") == 260.0f);
  CHECK(x("snk") == 520.0f);
  CHECK(x("all") > x("src")); // catch-all sink is not a source
  CHECK(y("src") == 0.0f);
  CHECK_FALSE(ed.dirty());
  CHECK_FALSE(ed.can_undo());

  // Everything placed and all=false: nothing moves, no new step.
  ed.set_position("src", 7, 7, true);
  const std::string before = ed.doc().serialize();
  ed.auto_layout(false);
  CHECK(ed.doc().serialize() == before);

  // all=true repositions.
  ed.auto_layout(true);
  CHECK(x("src") == 0.0f);
  CHECK(y("src") == 0.0f);
  CHECK(ed.undo_label() == "Auto layout");
}

TEST_CASE("auto_layout wraps tall layers into extra columns",
          "[ini][editor]") {
  std::string text;
  for (int i = 0; i < 8; ++i) {
    text += "[s" + std::to_string(i) + "]\npub_topic = \"t" +
            std::to_string(i) + "\"\n\n";
  }
  text += "[sink]\nsub_topic = [\"t0\"]\n";
  Editor ed;
  ed.new_document(text);
  const auto x = [&](const std::string &n) { return *ed.doc().find(n)->meta.x; };
  const auto y = [&](const std::string &n) { return *ed.doc().find(n)->meta.y; };
  // Layer 0 holds s0..s7: six rows, then a second column.
  CHECK(x("s5") == 0.0f);
  CHECK(y("s5") == 5 * 140.0f);
  CHECK(x("s6") == 260.0f);
  CHECK(y("s6") == 0.0f);
  // The next layer starts after both columns.
  CHECK(x("sink") == 520.0f);
}

TEST_CASE("auto_layout only places unplaced nodes and survives cycles",
          "[ini][editor]") {
  Editor ed;
  ed.new_document("[a]\n#@ pos=5,5\npub_topic = \"ta\"\nsub_topic = [\"tb\"]\n\n"
                  "[b]\npub_topic = \"tb\"\nsub_topic = [\"ta\"]\n");
  ed.auto_layout();
  CHECK(*ed.doc().find("a")->meta.x == 5.0f);
  REQUIRE(ed.doc().find("b")->meta.x);
  ed.auto_layout(true);
  CHECK(ed.doc().find("a")->meta.x.has_value());
}

TEST_CASE("connect adds, converts and creates sub_topic", "[ini][editor]") {
  Editor ed;
  ed.new_document("[src]\npub_topic = \"raw\"\n\n"
                  "[bare]\nsub_topic = \"other\"\n\n"
                  "[none]\npub_topic = \"n\"\n\n"
                  "[arr]\nsub_topic = [\"x\", \"y\"]\n\n"
                  "[off]\n# sub_topic = [\"z\"]\n");
  using R = Editor::ConnectResult;

  CHECK(ed.connect("src", "bare") == R::Added);
  CHECK(ed.doc().raw_value("bare", "sub_topic") == "[\"other\", \"raw\"]");
  CHECK(find_edge(ed, "src", "bare"));
  CHECK(ed.undo_label() == "Connect src -> bare");

  CHECK(ed.connect("src", "none") == R::Added);
  CHECK(ed.doc().raw_value("none", "sub_topic") == "[\"raw\"]");

  CHECK(ed.connect("src", "arr") == R::Added);
  CHECK(ed.doc().raw_value("arr", "sub_topic") ==
        "[\"x\", \"y\", \"raw\"]");

  CHECK(ed.connect("src", "off") == R::Added);
  CHECK(ed.doc().raw_value("off", "sub_topic") == "[\"raw\"]");
  CHECK(ed.doc().find("off")->find_disabled("sub_topic")); // kept

  // The edge exists now, so the same link is already matched.
  CHECK(ed.connect("src", "bare") == R::AlreadyMatched);
}

TEST_CASE("connect quotes topics that need escaping", "[ini][editor]") {
  Editor ed;
  ed.new_document("[src]\npub_topic = \"a\\\"b\\\\c\"\n\n[dst]\n");
  REQUIRE(ed.topics().at("src").pub_topic == "a\"b\\c");
  CHECK(ed.connect("src", "dst") == Editor::ConnectResult::Added);
  CHECK(ed.topics().at("dst").sub_topic ==
        std::vector<std::string>{"a\"b\\c"});
}

TEST_CASE("connect reports AlreadyMatched with its via entry",
          "[ini][editor]") {
  Editor ed;
  ed.new_document("[src]\npub_topic = \"sensors/imu/raw\"\n\n"
                  "[pre]\nsub_topic = [\"sensors\"]\n\n"
                  "[wild]\nsub_topic = [\"sensors/+/raw\"]\n\n"
                  "[all]\nsub_topic = [\"\"]\n\n"
                  "[self]\npub_topic = \"s\"\nsub_topic = [\"s\"]\n");
  using R = Editor::ConnectResult;
  std::string via = "unset";

  CHECK(ed.connect("src", "pre", &via) == R::AlreadyMatched);
  CHECK(via == "sensors");
  CHECK(ed.connect("src", "wild", &via) == R::AlreadyMatched);
  CHECK(via == "sensors/+/raw");
  via = "unset";
  CHECK(ed.connect("src", "all", &via) == R::AlreadyMatched);
  CHECK(via == "");
  CHECK_FALSE(ed.can_undo()); // nothing changed

  CHECK(ed.connect("src", "src") == R::Invalid);
  CHECK(ed.connect("self", "self") == R::Invalid);
  CHECK(ed.connect("nope", "src") == R::Invalid);
  CHECK(ed.connect("src", "nope") == R::Invalid);
  CHECK(ed.connect("src", "agents") == R::Invalid);
}

TEST_CASE("disconnect removes exact entries only", "[ini][editor]") {
  Editor ed;
  ed.new_document("[src]\npub_topic = \"raw\"\n\n"
                  "[two]\nsub_topic = [\"raw\", \"keep\"]\n\n"
                  "[one]\nsub_topic = \"raw\"\n\n"
                  "[pre]\nsub_topic = [\"ra\"]\n\n"
                  "[both]\nsub_topic = [\"raw\", \"ra\"]\n\n"
                  "[all]\nsub_topic = [\"\"]\n\n"
                  "[wild]\nsub_topic = [\"raw/#\"]\n\n"
                  "[idle]\npub_topic = \"idle\"\n");
  using R = Editor::DisconnectResult;

  CHECK(ed.disconnect("src", "two") == R::Removed);
  CHECK(ed.doc().raw_value("two", "sub_topic") == "[\"keep\"]");
  CHECK(ed.undo_label() == "Disconnect src -> two");
  CHECK_FALSE(find_edge(ed, "src", "two"));

  CHECK(ed.disconnect("src", "one") == R::Removed);
  CHECK(ed.doc().raw_value("one", "sub_topic") == "[]");

  CHECK(ed.disconnect("src", "pre") == R::ViaPattern);
  CHECK(ed.disconnect("src", "both") == R::ViaPattern);
  CHECK(ed.disconnect("src", "all") == R::ViaPattern);
  CHECK(ed.disconnect("src", "wild") == R::ViaPattern);
  CHECK(ed.doc().raw_value("both", "sub_topic") == "[\"raw\", \"ra\"]");

  CHECK(ed.disconnect("src", "idle") == R::NotFound);
  CHECK(ed.disconnect("src", "nope") == R::NotFound);
  CHECK(ed.disconnect("two", "src") == R::NotFound);
}

TEST_CASE("save, save_as, conflicts and changed_on_disk", "[ini][editor]") {
  TempDir dir;
  const fs::path file = dir.path / "a.ini";
  write_file(file, kPipeline);

  Editor ed;
  REQUIRE(ed.open(file));
  CHECK(ed.location() == file.string());
  CHECK_FALSE(ed.changed_on_disk());

  ed.doc().set_raw_value("src", "pub_topic", "\"changed\"");
  ed.commit("edit");
  CHECK(ed.dirty());
  auto r = ed.save();
  REQUIRE(r.status == SaveResult::Status::Ok);
  CHECK_FALSE(ed.dirty());
  CHECK_FALSE(ed.changed_on_disk());
  CHECK(read_file(file).find("\"changed\"") != std::string::npos);

  // Someone else edits the file.
  write_file(file, std::string(kPipeline) + "\n[extra]\n");
  CHECK(ed.changed_on_disk());
  ed.doc().set_raw_value("src", "pub_topic", "\"mine\"");
  ed.commit("edit 2");
  r = ed.save();
  CHECK(r.status == SaveResult::Status::Conflict);
  CHECK(ed.dirty());
  CHECK(read_file(file).find("[extra]") != std::string::npos);

  r = ed.save_overwrite();
  REQUIRE(r.status == SaveResult::Status::Ok);
  CHECK_FALSE(ed.dirty());
  CHECK_FALSE(ed.changed_on_disk());
  CHECK(read_file(file).find("\"mine\"") != std::string::npos);
  CHECK(ed.save().status == SaveResult::Status::Ok);

  // reload discards local changes and picks up the disk.
  write_file(file, "[only]\n");
  REQUIRE(ed.changed_on_disk());
  std::string error;
  REQUIRE(ed.reload(&error));
  CHECK(ed.nodes() == std::vector<std::string>{"only"});
  CHECK_FALSE(ed.can_undo());
  CHECK_FALSE(ed.dirty());
  CHECK_FALSE(ed.changed_on_disk());

  // The file disappears: unreadable counts as unchanged.
  fs::remove(file);
  CHECK_FALSE(ed.changed_on_disk());
}

TEST_CASE("save_as adopts the new source", "[ini][editor]") {
  TempDir dir;
  Editor ed;
  ed.new_document(kPipeline);
  CHECK(ed.save().status == SaveResult::Status::Error); // no source yet
  CHECK_FALSE(ed.reload());
  CHECK_FALSE(ed.changed_on_disk());

  ed.add_node("filter", 1, 2);
  const fs::path file = dir.path / "new.ini";
  auto r = ed.save_as(file);
  REQUIRE(r.status == SaveResult::Status::Ok);
  CHECK(ed.has_source());
  CHECK(ed.location() == file.string());
  CHECK_FALSE(ed.dirty());
  CHECK(read_file(file) == ed.doc().serialize());

  Editor other;
  REQUIRE(other.open(file));
  CHECK(other.nodes() == ed.nodes());

  ed.add_node("filter", 3, 4);
  CHECK(ed.dirty());
  CHECK(ed.save().status == SaveResult::Status::Ok);

  // save_as to an existing file does not check for conflicts.
  write_file(dir.path / "other.ini", "[x]\n");
  CHECK(ed.save_as(dir.path / "other.ini").status == SaveResult::Status::Ok);
}
