// Unit tests for the comment-preserving mads.ini model in
// src/ini/ini_document.hpp: round trips of the real settings files, the
// attachment of comments to sections/keys, editing helpers and FileSource.
#include <catch2/catch_test_macros.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ini_document.hpp"

using Mads::Ini::Diagnostic;
using Mads::Ini::Document;
using Mads::Ini::FileSource;
using Mads::Ini::SaveResult;

namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Every comment line of `text`, whitespace-collapsed (serialization may
// normalize the spacing of commented-out `# key = value  # note` lines).
std::vector<std::string> comment_lines(const std::string &text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] != '#') {
      continue;
    }
    std::string collapsed;
    bool space = false;
    for (char c : line.substr(first)) {
      if (c == ' ' || c == '\t' || c == '\r') {
        space = true;
        continue;
      }
      if (space && !collapsed.empty()) {
        collapsed += ' ';
      }
      space = false;
      collapsed += c;
    }
    out.push_back(collapsed);
  }
  return out;
}

// Inline comments (`value # note`) on active lines, whitespace-collapsed.
std::vector<std::string> inline_comments(const std::string &text) {
  std::vector<std::string> out;
  for (const auto &section : Document::parse(text).sections()) {
    for (const auto &entry : section.entries) {
      if (!entry.inline_comment.empty()) {
        out.push_back(entry.inline_comment);
      }
    }
  }
  return out;
}

void check_round_trip(const fs::path &path) {
  INFO("file: " << path.string());
  const std::string original = read_file(path);
  REQUIRE_FALSE(original.empty());

  std::vector<Diagnostic> diagnostics;
  const Document doc = Document::parse(original, &diagnostics);
  CHECK(diagnostics.empty());
  const std::string once = doc.serialize();

  // Same data.
  REQUIRE(toml::parse(original) == toml::parse(once));
  // No comment lost, in the same order.
  CHECK(comment_lines(original) == comment_lines(once));
  CHECK(inline_comments(original) == inline_comments(once));
  // Normalization is idempotent.
  CHECK(Document::parse(once).serialize() == once);
  CHECK(doc.validate().empty());
}

} // namespace

TEST_CASE("ini_document: round trip of the repo mads.ini", "[ini_document]") {
  check_round_trip(fs::path(MADS_PROJECT_SOURCE_DIR) / "mads.ini");
}

TEST_CASE("ini_document: round trip of the shipped template",
          "[ini_document]") {
  check_round_trip(fs::path(MADS_PROJECT_SOURCE_DIR) / "share" / "templates" /
                   "mads.ini");
}

TEST_CASE("ini_document: comments attach to sections and keys",
          "[ini_document]") {
  const Document doc = Document::parse(
      read_file(fs::path(MADS_PROJECT_SOURCE_DIR) / "mads.ini"));

  const auto *agents = doc.find("agents");
  REQUIRE(agents);
  CHECK(agents->comment == "Settings shared by all agents");
  // The ASCII-art banner above [agents] is its preamble.
  CHECK(std::any_of(agents->preamble.begin(), agents->preamble.end(),
                    [](const std::string &l) {
                      return l.find("\\____\\___/|_|") != std::string::npos;
                    }));
  // Documented knobs are disabled entries, with their prose as leading
  // comments.
  const auto *transport = agents->find_disabled("transport");
  REQUIRE(transport);
  CHECK(transport->raw_value == "\"auto\"");
  CHECK_FALSE(transport->leading_comments.empty());
  CHECK(transport->leading_comments.front().find("Transport to the broker") !=
        std::string::npos);
  // Prose that merely contains a colon is not a disabled entry.
  CHECK_FALSE(agents->find_disabled("clock_source:"));
  REQUIRE(agents->find_disabled("clock_source"));

  const auto *broker = doc.find("broker");
  REQUIRE(broker);
  CHECK(broker->comment.empty());
  REQUIRE(broker->find_disabled("io_threads"));
  CHECK(broker->find_disabled("io_threads")->raw_value == "1");
  REQUIRE(broker->find("auth_verbose"));
  CHECK(broker->find("auth_verbose")->raw_value == "false");

  // Disabled and active entries for the same key coexist.
  const auto *logger = doc.find("logger");
  REQUIRE(logger);
  REQUIRE(logger->find("sub_topic"));
  CHECK(logger->find("sub_topic")->raw_value == "[\"\"]");
  REQUIRE(logger->find_disabled("sub_topic"));

  const auto *feedback = doc.find("feedback");
  REQUIRE(feedback);
  CHECK(feedback->comment == "Subscribes to ALL topics");

  const auto *rerun = doc.find("fmu_rerun");
  REQUIRE(rerun);
  REQUIRE(rerun->find("sub_topic"));
  CHECK(rerun->find("sub_topic")->inline_comment ==
        "Topics to subscribe to (array of strings)");
  REQUIRE(rerun->find("keypaths"));
  CHECK(rerun->find("keypaths")->leading_comments ==
        std::vector<std::string>{"# List of keypaths for time series"});
  // A disabled entry keeps its own inline comment.
  REQUIRE(rerun->find_disabled("parallelize"));
  CHECK(rerun->find_disabled("parallelize")->inline_comment ==
        "Runs FFT and ACF computations in parallel");

  // The "Plugins" banner belongs to the section that follows it.
  const auto *restway = doc.find("restway");
  REQUIRE(restway);
  CHECK(std::any_of(restway->preamble.begin(), restway->preamble.end(),
                    [](const std::string &l) {
                      return l.find("|  __/| | |_| | (_| | | | | \\__ \\") !=
                             std::string::npos;
                    }));

  // Unquoted spacing is normalized, the value kept.
  const auto *serial = doc.find("serial_reader");
  REQUIRE(serial);
  CHECK(serial->find("port")->raw_value == "\"/dev/ttyACM0\"");
  CHECK(doc.serialize().find("port = \"/dev/ttyACM0\"") != std::string::npos);
}

TEST_CASE("ini_document: tricky TOML is preserved", "[ini_document]") {
  const std::string text = R"([a]
url = "http://host/#frag" # a '#' inside a string
lit = 'C:\path#1'
multi = [
  "x", # first
  "y",
]
text = """
a # not a comment
b"""
"dotted.key" = 1
inline = { k = 1, s = "#" }

[a.child]
deep = true

[[a.list]]
n = 1
)";
  std::vector<Diagnostic> diagnostics;
  const Document doc = Document::parse(text, &diagnostics);
  CHECK(diagnostics.empty());
  const auto *a = doc.find("a");
  REQUIRE(a);
  CHECK(a->find("url")->raw_value == "\"http://host/#frag\"");
  CHECK(a->find("url")->inline_comment == "a '#' inside a string");
  CHECK(a->find("lit")->raw_value == "'C:\\path#1'");
  CHECK(a->find("lit")->inline_comment.empty());
  CHECK(a->find("multi")->raw_value == "[\n  \"x\", # first\n  \"y\",\n]");
  CHECK(a->find("text")->raw_value == "\"\"\"\na # not a comment\nb\"\"\"");
  CHECK(a->find("\"dotted.key\""));
  CHECK(a->find("inline")->raw_value == "{ k = 1, s = \"#\" }");
  CHECK(a->raw_children.size() == 2);
  CHECK(doc.sections().size() == 1);

  const std::string out = doc.serialize();
  CHECK(toml::parse(text) == toml::parse(out));
  CHECK(out.find("\"x\", # first") != std::string::npos);
  CHECK(Document::parse(out).serialize() == out);
}

TEST_CASE("ini_document: node metadata line", "[ini_document]") {
  const std::string text = "[clock]\n"
                           "#@ type=source pos=420,-180 color=red\n"
                           "# Publishes a tick\n"
                           "pub_topic = \"clock\"\n";
  Document doc = Document::parse(text);
  auto *clock = doc.find("clock");
  REQUIRE(clock);
  CHECK(clock->meta.type == "source");
  REQUIRE(clock->meta.x);
  CHECK(*clock->meta.x == 420.0F);
  CHECK(*clock->meta.y == -180.0F);
  CHECK(clock->meta.extra.at("color") == "red");
  CHECK(clock->comment == "Publishes a tick");

  clock->meta.x = 10.6F;
  clock->meta.y = 20.2F;
  clock->comment = "Line one\n\nLine three";
  const std::string out = doc.serialize();
  CHECK(out.find("#@ ") != std::string::npos);
  CHECK(out.find("pos=11,20") != std::string::npos);
  CHECK(out.find("type=source") != std::string::npos);
  CHECK(out.find("color=red") != std::string::npos);
  CHECK(out.find("# Line one\n#\n# Line three\n") != std::string::npos);
  CHECK(Document::parse(out).find("clock")->comment == "Line one\n\nLine three");

  // No meta, no line.
  clock->meta = {};
  CHECK(doc.serialize().find("#@") == std::string::npos);
}

TEST_CASE("ini_document: layout normalization", "[ini_document]") {
  const std::string text = "\n\n[a]\nx=1\n\n\n\ny   =    2     #   note\n"
                           "\n\n\n[b]\r\nz = 3\r\n";
  const Document doc = Document::parse(text);
  CHECK(doc.serialize() ==
        "[a]\nx = 1\n\ny = 2 # note\n\n[b]\nz = 3\n");
}

TEST_CASE("ini_document: root keys before the first header",
          "[ini_document]") {
  const std::string text = "title = \"x\"\n\n[a]\nk = 1\n";
  const Document doc = Document::parse(text);
  REQUIRE(doc.root().find("title"));
  CHECK(doc.serialize() == text);
}

TEST_CASE("ini_document: editing helpers", "[ini_document]") {
  Document doc = Document::parse("[agents]\ntimecode_fps = 25\n"
                                 "# compression = \"auto\"\n\n"
                                 "[clock]\npub_topic = \"clock\"\n"
                                 "# period = 100\n");

  SECTION("set_raw_value updates, re-enables or appends") {
    CHECK(doc.set_raw_value("clock", "pub_topic", " \"tick\" "));
    CHECK(doc.raw_value("clock", "pub_topic") == "\"tick\"");
    CHECK(doc.set_raw_value("clock", "period", "200"));
    CHECK_FALSE(doc.find("clock")->find_disabled("period"));
    CHECK(doc.raw_value("clock", "period") == "200");
    CHECK(doc.set_raw_value("clock", "new_key", "[1, 2]"));
    CHECK(doc.find("clock")->entries.back().key == "new_key");
    CHECK_FALSE(doc.set_raw_value("clock", "bad", "not toml"));
    CHECK_FALSE(doc.raw_value("clock", "bad"));
    CHECK_FALSE(doc.set_raw_value("nope", "k", "1"));
  }
  SECTION("set_enabled toggles and swaps") {
    CHECK(doc.set_enabled("clock", "pub_topic", false));
    CHECK_FALSE(doc.raw_value("clock", "pub_topic"));
    CHECK(doc.serialize().find("# pub_topic = \"clock\"") != std::string::npos);
    CHECK(doc.set_enabled("clock", "period", true));
    CHECK(doc.raw_value("clock", "period") == "100");
    CHECK_FALSE(doc.set_enabled("clock", "missing", true));
  }
  SECTION("effective values fall back to [agents]") {
    CHECK(doc.effective_raw_value("clock", "timecode_fps") == "25");
    CHECK_FALSE(doc.effective_raw_value("clock", "compression"));
    doc.set_raw_value("clock", "timecode_fps", "30");
    CHECK(doc.effective_raw_value("clock", "timecode_fps") == "30");
  }
  SECTION("sections: add, rename, remove") {
    REQUIRE(doc.add_section("filter_1"));
    CHECK_FALSE(doc.add_section("filter_1"));
    CHECK_FALSE(doc.add_section("bad name"));
    CHECK(doc.rename_section("filter_1", "filter_2"));
    CHECK_FALSE(doc.rename_section("filter_2", "clock"));
    CHECK(doc.find("filter_2"));
    CHECK(doc.remove_section("filter_2"));
    CHECK_FALSE(doc.find("filter_2"));
    CHECK(doc.remove_key("clock", "period"));
    CHECK(doc.serialize().find("period") == std::string::npos);
  }
  CHECK(doc.validate().empty());
  CHECK(toml::parse(doc.serialize()).size() == 2);
}

TEST_CASE("ini_document: rename carries raw children", "[ini_document]") {
  Document doc = Document::parse("[img]\nk = 1\n[img.watch_list]\nx = 2\n");
  REQUIRE(doc.rename_section("img", "camera"));
  const auto table = toml::parse(doc.serialize());
  CHECK(table["camera"]["watch_list"]["x"].value_or(0) == 2);
}

TEST_CASE("ini_document: invalid input keeps its lines", "[ini_document]") {
  const std::string text = "[a]\nk = 1\nthis is garbage\n";
  std::vector<Diagnostic> diagnostics;
  const Document doc = Document::parse(text, &diagnostics);
  REQUIRE_FALSE(diagnostics.empty());
  CHECK(diagnostics.front().line == 3);
  CHECK(doc.serialize().find("this is garbage") != std::string::npos);
  CHECK_FALSE(doc.validate().empty());
}

TEST_CASE("ini_document: FileSource save, backup and conflicts",
          "[ini_document]") {
  const fs::path dir = fs::temp_directory_path() / "mads_test_ini_document";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path file = dir / "mads.ini";
  {
    std::ofstream out(file, std::ios::binary);
    out << "[a]\nk = 1\n";
  }
  FileSource source(file);
  const auto loaded = source.load();
  CHECK(loaded.text == "[a]\nk = 1\n");
  CHECK(loaded.etag == Mads::Ini::content_etag(loaded.text));

  const SaveResult ok = source.save("[a]\nk = 2\n", loaded.etag);
  CHECK(ok.status == SaveResult::Status::Ok);
  CHECK(read_file(file) == "[a]\nk = 2\n");
  CHECK(read_file(dir / "mads.ini.bak") == "[a]\nk = 1\n");
  CHECK(ok.etag == Mads::Ini::content_etag("[a]\nk = 2\n"));

  // Stale base etag: refused, file untouched.
  const SaveResult stale = source.save("[a]\nk = 3\n", loaded.etag);
  CHECK(stale.status == SaveResult::Status::Conflict);
  CHECK(read_file(file) == "[a]\nk = 2\n");

  // Invalid TOML: refused.
  const SaveResult bad = source.save("[a\n", "");
  CHECK(bad.status == SaveResult::Status::Error);
  CHECK(read_file(file) == "[a]\nk = 2\n");

  // Empty base etag skips the check.
  CHECK(source.save("[a]\nk = 4\n", "").status == SaveResult::Status::Ok);
  fs::remove_all(dir);
}
