// Unit tests for the mads.ini key schema in src/ini/ini_schema.hpp: the
// embedded schema, the key lookup, the node type inference and the lint.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ini_document.hpp"
#include "ini_schema.hpp"

using Mads::Ini::Diagnostic;
using Mads::Ini::Document;
using Mads::Ini::KeySpec;
using Mads::Ini::Schema;
using Mads::Ini::ValueType;

namespace fs = std::filesystem;

namespace {

// Parses a mads.ini text and returns the section named `name`.
const Mads::Ini::Section &section_of(const Document &doc,
                                     const std::string &name) {
  const auto *section = doc.find(name);
  REQUIRE(section != nullptr);
  return *section;
}

// Lints a small mads.ini text with the built-in schema.
std::vector<Diagnostic> lint_text(const std::string &text) {
  const Document doc = Document::parse(text);
  return Schema::builtin().lint(doc);
}

// The diagnostics with the given severity, key and message fragment.
bool has_diagnostic(const std::vector<Diagnostic> &diags,
                    Diagnostic::Severity severity, const std::string &key,
                    const std::string &fragment = "") {
  return std::any_of(diags.begin(), diags.end(), [&](const Diagnostic &d) {
    return d.severity == severity && d.key == key &&
           d.message.find(fragment) != std::string::npos;
  });
}

bool has_key_named(const std::vector<Diagnostic> &diags,
                   const std::string &key) {
  return std::any_of(diags.begin(), diags.end(),
                     [&](const Diagnostic &d) { return d.key == key; });
}

std::string read_file(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

} // namespace

TEST_CASE("ini_schema: builtin schema loads the expected types",
          "[ini_schema]") {
  const Schema &schema = Schema::builtin();
  for (const char *id : {"agents", "broker", "source", "filter", "sink",
                         "logger", "custom"}) {
    INFO("type " << id);
    CHECK(schema.type(id) != nullptr);
  }
  CHECK(schema.type("no_such_type") == nullptr);
  CHECK(schema.type("agents")->document);
  CHECK_FALSE(schema.type("filter")->document);
  CHECK(schema.type("custom")->custom_keys);
  CHECK_FALSE(schema.type("logger")->custom_keys);
}

TEST_CASE("ini_schema: every type has a label and a category",
          "[ini_schema]") {
  for (const auto &type : Schema::builtin().types()) {
    INFO("type " << type.id);
    CHECK_FALSE(type.id.empty());
    CHECK_FALSE(type.label.empty());
    CHECK_FALSE(type.category.empty());
  }
}

TEST_CASE("ini_schema: every key has a parsed, non-Any type",
          "[ini_schema]") {
  const Schema &schema = Schema::builtin();
  auto check_keys = [](const std::vector<KeySpec> &keys,
                       const std::string &where) {
    for (const auto &spec : keys) {
      INFO(where << "." << spec.name);
      CHECK_FALSE(spec.name.empty());
      CHECK(spec.type != ValueType::Any);
      CHECK_FALSE(spec.doc.empty());
    }
  };
  check_keys(schema.common_keys(), "common");
  for (const auto &type : schema.types()) {
    check_keys(type.keys, type.id);
  }
}

TEST_CASE("ini_schema: key lookup falls back to the common keys",
          "[ini_schema]") {
  const Schema &schema = Schema::builtin();
  REQUIRE(schema.key("filter", "attachment") != nullptr);
  CHECK(schema.key("filter", "attachment")->widget == "file");
  REQUIRE(schema.key("filter", "pub_topic") != nullptr);
  CHECK(schema.key("filter", "pub_topic")->widget == "topic");
  REQUIRE(schema.key("logger", "sub_topic") != nullptr);
  CHECK(schema.key("logger", "sub_topic")->type == ValueType::StringArray);

  // Document types do not inherit the common keys.
  CHECK(schema.key("agents", "pub_topic") == nullptr);
  CHECK(schema.key("broker", "sub_topic") == nullptr);
  CHECK(schema.key("filter", "no_such_key") == nullptr);
  CHECK(schema.key("no_such_type", "pub_topic") == nullptr);
}

TEST_CASE("ini_schema: inherits flags of fleet-wide keys", "[ini_schema]") {
  const Schema &schema = Schema::builtin();
  REQUIRE(schema.key("source", "wire_format") != nullptr);
  CHECK(schema.key("source", "wire_format")->inherits);
  CHECK(schema.key("source", "wire_format")->choices ==
        std::vector<std::string>{"json", "msgpack"});
  CHECK_FALSE(schema.key("source", "pub_topic")->inherits);
  // [agents] carries the fleet-wide copies so they can be edited there.
  REQUIRE(schema.key("agents", "wire_format") != nullptr);
  CHECK(schema.key("agents", "wire_format")->inherits);
}

TEST_CASE("ini_schema: infer_type rule 1, explicit metadata", "[ini_schema]") {
  const Document doc = Document::parse("[foo]\n#@ type=sink\n"
                                       "pub_topic = \"x\"\n");
  CHECK(Schema::builtin().infer_type(section_of(doc, "foo")) == "sink");
}

TEST_CASE("ini_schema: infer_type rule 2, document sections",
          "[ini_schema]") {
  const Document doc =
      Document::parse("[agents]\ntimecode_fps = 25\n\n[broker]\nauth_verbose = false\n");
  CHECK(Schema::builtin().infer_type(section_of(doc, "agents")) == "agents");
  CHECK(Schema::builtin().infer_type(section_of(doc, "broker")) == "broker");
}

TEST_CASE("ini_schema: infer_type rule 3, type named after the section",
          "[ini_schema]") {
  const Document doc = Document::parse("[logger]\nmax_length = 10\n");
  CHECK(Schema::builtin().infer_type(section_of(doc, "logger")) == "logger");
}

TEST_CASE("ini_schema: infer_type rule 4, plugin loaders by topics",
          "[ini_schema]") {
  const Document doc = Document::parse(
      "[both]\nattachment = \"a.plugin\"\npub_topic = \"p\"\n"
      "sub_topic = [\"s\"]\n\n"
      "[pub_only]\nattachment = \"a.plugin\"\npub_topic = \"p\"\n\n"
      "[sub_only]\nattachment = \"a.plugin\"\nsub_topic = [\"s\"]\n");
  CHECK(Schema::builtin().infer_type(section_of(doc, "both")) == "filter");
  CHECK(Schema::builtin().infer_type(section_of(doc, "pub_only")) == "source");
  CHECK(Schema::builtin().infer_type(section_of(doc, "sub_only")) == "sink");
}

TEST_CASE("ini_schema: infer_type rule 5, custom agents", "[ini_schema]") {
  const Document doc = Document::parse(
      "[no_attachment]\npub_topic = \"p\"\n\n"
      "[neither]\nperiod = 10\n\n"
      "[attachment_only]\nattachment = \"a.plugin\"\n");
  CHECK(Schema::builtin().infer_type(section_of(doc, "no_attachment")) ==
        "custom");
  CHECK(Schema::builtin().infer_type(section_of(doc, "neither")) == "custom");
  CHECK(Schema::builtin().infer_type(section_of(doc, "attachment_only")) ==
        "custom");
}

TEST_CASE("ini_schema: lint warns about unknown keys on closed types",
          "[ini_schema]") {
  const auto diags = lint_text("[logger]\nmongo_uri = \"mongodb://x\"\n"
                               "not_a_key = 1\n");
  CHECK(has_diagnostic(diags, Diagnostic::Severity::Warning, "not_a_key",
                       "unknown key"));
  CHECK_FALSE(has_key_named(diags, "mongo_uri"));
}

TEST_CASE("ini_schema: lint accepts any key on a plugin loader",
          "[ini_schema]") {
  const auto diags = lint_text(
      "[myfilter]\nattachment = \"a.plugin\"\npub_topic = \"p\"\n"
      "sub_topic = [\"x\"]\nmy_own_key = 1\n");
  CHECK_FALSE(has_key_named(diags, "my_own_key"));
}

TEST_CASE("ini_schema: lint checks value types", "[ini_schema]") {
  const auto diags = lint_text(
      "[src]\nattachment = \"a.plugin\"\npub_topic = \"p\"\nperiod = \"x\"\n");
  CHECK(has_diagnostic(diags, Diagnostic::Severity::Warning, "period",
                       "expected integer"));

  // The built-in schema has no float keys, so use a small one: an integer is
  // accepted where a float is expected, a string is not.
  const Schema schema = Schema::parse(
      "[[types]]\nid = \"x\"\nlabel = \"X\"\ncategory = \"C\"\n"
      "[[types.keys]]\nname = \"tol\"\ntype = \"float\"\n");
  const Document ok_doc = Document::parse("[x]\ntol = 1\n");
  CHECK_FALSE(has_key_named(schema.lint(ok_doc), "tol"));
  const Document bad_doc = Document::parse("[x]\ntol = \"1\"\n");
  CHECK(has_diagnostic(schema.lint(bad_doc), Diagnostic::Severity::Warning,
                       "tol", "expected float"));
}

TEST_CASE("ini_schema: lint checks enum choices", "[ini_schema]") {
  const auto diags = lint_text("[feedback]\nwire_format = \"xml\"\n");
  CHECK(has_diagnostic(diags, Diagnostic::Severity::Warning, "wire_format",
                       "not one of"));
  const auto good = lint_text("[feedback]\nwire_format = \"msgpack\"\n");
  CHECK_FALSE(has_key_named(good, "wire_format"));
}

TEST_CASE("ini_schema: lint flags invalid wildcards in sub_topic",
          "[ini_schema]") {
  const auto diags = lint_text(
      "[pub]\npub_topic = \"a/b\"\n\n[sub]\nsub_topic = [\"a/#/b\"]\n");
  CHECK(has_diagnostic(diags, Diagnostic::Severity::Warning, "sub_topic",
                       "invalid wildcard"));
  const auto valid = lint_text(
      "[pub]\npub_topic = \"a/b\"\n\n[sub]\nsub_topic = [\"a/#\"]\n");
  CHECK_FALSE(has_diagnostic(valid, Diagnostic::Severity::Warning,
                             "sub_topic", "invalid wildcard"));
}

TEST_CASE("ini_schema: a bare string sub_topic is a one-element list",
          "[ini_schema]") {
  // Agent::init() accepts both forms, so neither is a type mismatch.
  const auto ok = lint_text(
      "[pub]\npub_topic = \"a/b\"\n\n[sub]\nsub_topic = \"a/b\"\n");
  CHECK_FALSE(has_diagnostic(ok, Diagnostic::Severity::Warning, "sub_topic"));
  const auto bad = lint_text(
      "[pub]\npub_topic = \"a/b\"\n\n[sub]\nsub_topic = \"a/#/b\"\n");
  CHECK(has_diagnostic(bad, Diagnostic::Severity::Warning, "sub_topic",
                       "invalid wildcard"));
}

TEST_CASE("ini_schema: lint reports dangling topics as Info",
          "[ini_schema]") {
  const auto diags = lint_text("[s]\nsub_topic = [\"nothing/here\"]\n");
  CHECK(has_diagnostic(diags, Diagnostic::Severity::Info, "sub_topic",
                       "nothing/here"));
  CHECK_FALSE(std::any_of(diags.begin(), diags.end(), [](const Diagnostic &d) {
    return d.severity == Diagnostic::Severity::Error;
  }));
}

TEST_CASE("ini_schema: lint skips disabled entries", "[ini_schema]") {
  const auto diags = lint_text("[logger]\n# not_a_key = 1\n");
  CHECK_FALSE(has_key_named(diags, "not_a_key"));
}

TEST_CASE("ini_schema: lint of the repo mads.ini has no errors",
          "[ini_schema]") {
  const fs::path path = fs::path(MADS_PROJECT_SOURCE_DIR) / "mads.ini";
  const Document doc = Document::parse(read_file(path));
  const auto diags = Schema::builtin().lint(doc);

  std::size_t warnings = 0;
  std::size_t infos = 0;
  for (const auto &d : diags) {
    const bool is_error = d.severity == Diagnostic::Severity::Error;
    if (is_error) {
      INFO("[" << d.section << "] " << d.key << ": " << d.message);
    }
    CHECK_FALSE(is_error);
    if (d.severity == Diagnostic::Severity::Warning) {
      ++warnings;
      std::cout << "WARN  [" << d.section << "] " << d.key << ": " << d.message
                << "\n";
    } else if (d.severity == Diagnostic::Severity::Info) {
      ++infos;
      std::cout << "INFO  [" << d.section << "] " << d.key << ": " << d.message
                << "\n";
    }
  }
  std::cout << "mads.ini lint: " << warnings << " warning(s), " << infos
            << " info(s)\n";
}

TEST_CASE("ini_schema: malformed schema text throws", "[ini_schema]") {
  // Not TOML at all.
  CHECK_THROWS_AS(Schema::parse("[[types\n"), std::runtime_error);
  // No types.
  CHECK_THROWS_AS(Schema::parse("# empty\n"), std::runtime_error);
  // A type without a label.
  CHECK_THROWS_AS(Schema::parse("[[types]]\nid = \"x\"\ncategory = \"c\"\n"),
                  std::runtime_error);
  // A key without a name.
  CHECK_THROWS_AS(Schema::parse("[[types]]\nid = \"x\"\nlabel = \"l\"\n"
                                "category = \"c\"\n[[types.keys]]\n"
                                "type = \"string\"\n"),
                  std::runtime_error);
  // Unknown role.
  CHECK_THROWS_AS(Schema::parse("[[types]]\nid = \"x\"\nlabel = \"l\"\n"
                                "category = \"c\"\nrole = \"bogus\"\n"),
                  std::runtime_error);
  // Duplicate type ids.
  CHECK_THROWS_AS(Schema::parse("[[types]]\nid = \"x\"\nlabel = \"l\"\n"
                                "category = \"c\"\n[[types]]\nid = \"x\"\n"
                                "label = \"l\"\ncategory = \"c\"\n"),
                  std::runtime_error);
}

TEST_CASE("ini_schema: minimal valid schema parses", "[ini_schema]") {
  const Schema schema = Schema::parse(
      "[[common]]\nname = \"pub_topic\"\ntype = \"string\"\n\n"
      "[[types]]\nid = \"x\"\nlabel = \"X\"\ncategory = \"C\"\n"
      "[[types.keys]]\nname = \"k\"\ntype = \"integer\"\n");
  REQUIRE(schema.type("x") != nullptr);
  CHECK(schema.key("x", "k")->type == ValueType::Integer);
  CHECK(schema.key("x", "pub_topic") != nullptr);
}
