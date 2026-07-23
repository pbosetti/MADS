#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "main/plugin_migrate.hpp"

namespace fs = std::filesystem;
using namespace Mads::PluginMigrate;

// ---------------------------------------------------------------------------
// Test-local helpers: every test operates on copies under
// std::filesystem::temp_directory_path(); the repo fixture at
// tests/fixtures/plugin_p6/ is only ever read, never mutated.
// ---------------------------------------------------------------------------
namespace {

fs::path unique_temp_path(const std::string &tag) {
  static std::atomic<int> counter{0};
  return fs::temp_directory_path() /
         ("mads_ptm_" + tag + "_" + std::to_string(::getpid()) + "_" +
          std::to_string(counter++));
}

// RAII: removes the temp path (file or directory tree) on scope exit.
struct ScopedPath {
  fs::path p;
  explicit ScopedPath(fs::path p_) : p(std::move(p_)) {}
  ~ScopedPath() {
    std::error_code ec;
    fs::remove_all(p, ec);
  }
};

fs::path fixture_dir() { return fs::path(MADS_TEST_FIXTURES_DIR) / "plugin_p6"; }

fs::path migrations_dir() {
  return fs::path(MADS_PROJECT_SOURCE_DIR) / "share" / "plugin_migrations";
}

fs::path deps_manifest() {
  return fs::path(MADS_PROJECT_SOURCE_DIR) / "share" / "plugin_deps.json";
}

// Copies the fixture plugin project into a fresh temp directory and returns
// its path. Never touches the repo fixture.
fs::path copy_fixture(const std::string &tag) {
  fs::path dst = unique_temp_path(tag);
  fs::copy(fixture_dir(), dst, fs::copy_options::recursive);
  return dst;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. Pure helpers
// ---------------------------------------------------------------------------

TEST_CASE("is_ident_char classifies identifier characters",
          "[plugin_migrate][helpers]") {
  REQUIRE(is_ident_char('a'));
  REQUIRE(is_ident_char('Z'));
  REQUIRE(is_ident_char('9'));
  REQUIRE(is_ident_char('_'));
  REQUIRE_FALSE(is_ident_char(' '));
  REQUIRE_FALSE(is_ident_char('('));
  REQUIRE_FALSE(is_ident_char('.'));
}

TEST_CASE("line_of_offset counts 1-based lines", "[plugin_migrate][helpers]") {
  std::string s = "aaa\nbbb\nccc\n";
  REQUIRE(line_of_offset(s, 0) == 1);
  REQUIRE(line_of_offset(s, 3) == 1);  // still before the first '\n'
  REQUIRE(line_of_offset(s, 4) == 2);  // just after the first '\n'
  REQUIRE(line_of_offset(s, 8) == 3);
  // Offset beyond the string is clamped to size(), i.e. counts every '\n'.
  REQUIRE(line_of_offset(s, 1000) == 4);
}

TEST_CASE("read_file / write_file round-trip", "[plugin_migrate][helpers]") {
  ScopedPath dir(unique_temp_path("rw"));
  fs::create_directories(dir.p);
  fs::path f = dir.p / "roundtrip.txt";
  std::string content = "line1\nline2\nwith \"quotes\" and (parens)\n";

  REQUIRE(write_file(f, content));
  std::string out;
  REQUIRE(read_file(f, out));
  REQUIRE(out == content);
}

TEST_CASE("read_file fails on a missing file", "[plugin_migrate][helpers]") {
  std::string out = "unchanged";
  REQUIRE_FALSE(read_file(fs::temp_directory_path() / "mads_ptm_does_not_exist.txt", out));
}

TEST_CASE("write_file fails when the parent directory does not exist",
          "[plugin_migrate][helpers]") {
  fs::path bogus = unique_temp_path("nowrite") / "sub" / "file.txt";
  REQUIRE_FALSE(write_file(bogus, "content"));
}

TEST_CASE("run_command captures output and a zero exit code",
          "[plugin_migrate][helpers]") {
  std::string out;
  int rc = run_command("echo hi", out);
  REQUIRE(rc == 0);
  REQUIRE(out.find("hi") != std::string::npos);
}

TEST_CASE("run_command captures stderr too (2>&1 redirection)",
          "[plugin_migrate][helpers]") {
  std::string out;
  int rc = run_command("sh -c \"echo err-marker 1>&2\"", out);
  REQUIRE(rc == 0);
  REQUIRE(out.find("err-marker") != std::string::npos);
}

TEST_CASE("run_command returns a non-zero exit code on failure",
          "[plugin_migrate][helpers]") {
  std::string out;
  int rc = run_command("false", out);
  REQUIRE(rc != 0);
}

TEST_CASE("find_matching_paren balances simple and nested parens",
          "[plugin_migrate][helpers]") {
  std::string s1 = "foo()";
  REQUIRE(find_matching_paren(s1, 3) == 4);

  std::string s2 = "foo(a, (b, c), d)";
  REQUIRE(find_matching_paren(s2, 3) == 16);

  std::string s3 = "f((((x))))";
  REQUIRE(find_matching_paren(s3, 1) == 9);
}

TEST_CASE("find_matching_paren ignores parens inside string/char literals",
          "[plugin_migrate][helpers]") {
  // The '(' right after the opening paren, inside a string literal, must not
  // affect depth tracking; nor must the escaped quote inside it.
  std::string s = "f(\"a (b\\\" (c\" , 'x' , y)";
  std::size_t open = 1;
  std::size_t close = find_matching_paren(s, open);
  REQUIRE(close == s.size() - 1);
}

TEST_CASE("find_matching_paren ignores parens inside comments",
          "[plugin_migrate][helpers]") {
  std::string line_comment = "f(a // ( still open\n, b)";
  REQUIRE(find_matching_paren(line_comment, 1) == line_comment.size() - 1);

  std::string block_comment = "f(a /* ( nested ( parens */, b)";
  REQUIRE(find_matching_paren(block_comment, 1) == block_comment.size() - 1);
}

TEST_CASE("find_matching_paren returns npos on unbalanced input",
          "[plugin_migrate][helpers]") {
  std::string s = "f(a, (b, c)";
  REQUIRE(find_matching_paren(s, 1) == std::string::npos);
}

// ---------------------------------------------------------------------------
// 2. detect_protocol / bump_git_tag
// ---------------------------------------------------------------------------

TEST_CASE("detect_protocol parses the -P<N> suffix from GIT_TAG",
          "[plugin_migrate][protocol]") {
  std::string cmake = R"cmake(
FetchContent_Populate(plugin
  GIT_REPOSITORY https://github.com/pbosetti/mads_plugin.git
  GIT_TAG        v2.3-P6
)
)cmake";
  REQUIRE(detect_protocol(cmake) == 6);
}

TEST_CASE("detect_protocol returns -1 sentinel when absent",
          "[plugin_migrate][protocol]") {
  REQUIRE(detect_protocol("project(x)\n") == -1);
  // No protocol suffix on the tag.
  std::string cmake = R"cmake(
FetchContent_Populate(plugin
  GIT_TAG v2.3
)
)cmake";
  REQUIRE(detect_protocol(cmake) == -1);
}

TEST_CASE("detect_protocol requires FetchContent_Populate specifically",
          "[plugin_migrate][protocol]") {
  // A Declare-only block (no Populate) does not match: this documents the
  // engine's actual (narrower) detection scope rather than assuming it.
  std::string cmake = R"cmake(
FetchContent_Declare(plugin
  GIT_TAG v2.3-P6
)
)cmake";
  REQUIRE(detect_protocol(cmake) == -1);
}

TEST_CASE("bump_git_tag replaces a bare GIT_TAG token", "[plugin_migrate][protocol]") {
  std::string cmake = R"cmake(
FetchContent_Declare(pugg
  GIT_REPOSITORY https://github.com/pbosetti/pugg.git
  GIT_TAG        1.0.0
  GIT_SHALLOW    TRUE
)
)cmake";
  std::vector<Change> changes;
  int n = bump_git_tag(cmake, "pugg", "1.1.0", changes);
  REQUIRE(n == 1);
  REQUIRE(changes.size() == 1);
  REQUIRE(cmake.find("1.1.0") != std::string::npos);
  REQUIRE(cmake.find("GIT_TAG        1.0.0") == std::string::npos);
}

TEST_CASE("bump_git_tag replaces a quoted GIT_TAG value", "[plugin_migrate][protocol]") {
  std::string cmake = R"cmake(
FetchContent_Declare(plugin
  GIT_TAG "v2.3-P6"
)
)cmake";
  std::vector<Change> changes;
  int n = bump_git_tag(cmake, "plugin", "v2.3-P7", changes);
  REQUIRE(n == 1);
  REQUIRE(cmake.find("\"v2.3-P7\"") != std::string::npos);
}

TEST_CASE("bump_git_tag is comment-aware: a fake GIT_TAG in a # comment is skipped",
          "[plugin_migrate][protocol]") {
  std::string cmake = R"cmake(
FetchContent_Populate(plugin
  # legacy pin, superseded below: GIT_TAG v1.0-P5 (deprecated)
  GIT_REPOSITORY https://github.com/pbosetti/mads_plugin.git
  GIT_TAG        v2.3-P6
)
)cmake";
  std::vector<Change> changes;
  int n = bump_git_tag(cmake, "plugin", "v2.3-P7", changes);
  REQUIRE(n == 1);
  REQUIRE(cmake.find("v1.0-P5") != std::string::npos);   // comment untouched
  REQUIRE(cmake.find("GIT_TAG        v2.3-P7") != std::string::npos);
  REQUIRE(cmake.find("GIT_TAG        v2.3-P6") == std::string::npos);
}

TEST_CASE("bump_git_tag is a no-op when the block is absent",
          "[plugin_migrate][protocol]") {
  std::string cmake = "project(x)\n";
  std::string original = cmake;
  std::vector<Change> changes;
  int n = bump_git_tag(cmake, "plugin", "v2.3-P7", changes);
  REQUIRE(n == 0);
  REQUIRE(changes.empty());
  REQUIRE(cmake == original);
}

TEST_CASE("bump_git_tag is idempotent when already at the target tag",
          "[plugin_migrate][protocol]") {
  std::string cmake = R"cmake(
FetchContent_Declare(plugin
  GIT_TAG v2.3-P7
)
)cmake";
  std::string original = cmake;
  std::vector<Change> changes;
  int n = bump_git_tag(cmake, "plugin", "v2.3-P7", changes);
  REQUIRE(n == 0);
  REQUIRE(changes.empty());
  REQUIRE(cmake == original);
}

TEST_CASE("bump_git_tag handles a new_tag starting with a digit",
          "[plugin_migrate][protocol]") {
  // Regression guard for the exact hazard documented in the header: naive
  // std::regex_replace with "$1" + new_tag breaks when new_tag starts with a
  // digit (e.g. "$11.2.0" is read as capture group 11).
  std::string cmake = R"cmake(
FetchContent_Declare(pugg
  GIT_TAG 1.1.0
)
)cmake";
  std::vector<Change> changes;
  int n = bump_git_tag(cmake, "pugg", "1.2.0", changes);
  REQUIRE(n == 1);
  REQUIRE(cmake.find("1.2.0") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 3. apply_literal / apply_regex
// ---------------------------------------------------------------------------

TEST_CASE("apply_literal replaces every occurrence", "[plugin_migrate][transforms]") {
  std::string src = "a(x); a(x); a(x);";
  std::vector<Change> changes;
  int n = apply_literal(src, "a(x)", "b(y)", "rename call", changes);
  REQUIRE(n == 3);
  REQUIRE(changes.size() == 3);
  REQUIRE(src == "b(y); b(y); b(y);");
}

TEST_CASE("apply_literal returns 0 and leaves the buffer untouched on no match",
          "[plugin_migrate][transforms]") {
  std::string src = "unrelated content";
  std::string original = src;
  std::vector<Change> changes;
  int n = apply_literal(src, "needle", "replacement", "desc", changes);
  REQUIRE(n == 0);
  REQUIRE(changes.empty());
  REQUIRE(src == original);
}

TEST_CASE("apply_regex replaces all matches but logs a single Change",
          "[plugin_migrate][transforms]") {
  std::string src = "INSTALL_SOURCE_DRIVER(Foo, json)\nINSTALL_SOURCE_DRIVER(Bar, json)\n";
  std::vector<Change> changes;
  int n = apply_regex(src, R"(INSTALL_SOURCE_DRIVER\s*\(\s*([A-Za-z_]\w*)\s*,[^)]*\))",
                      "MADS_REGISTER_PLUGINS($1)", "", "driver macro", changes);
  REQUIRE(n == 2);
  REQUIRE(changes.size() == 1); // one Change entry regardless of match count
  REQUIRE(src == "MADS_REGISTER_PLUGINS(Foo)\nMADS_REGISTER_PLUGINS(Bar)\n");
}

TEST_CASE("apply_regex returns 0 on no match and leaves the buffer untouched",
          "[plugin_migrate][transforms]") {
  std::string src = "nothing to see here";
  std::string original = src;
  std::vector<Change> changes;
  int n = apply_regex(src, "NOPE_(\\w+)", "$1", "", "desc", changes);
  REQUIRE(n == 0);
  REQUIRE(changes.empty());
  REQUIRE(src == original);
}

TEST_CASE("apply_regex honours the case-insensitive flag", "[plugin_migrate][transforms]") {
  std::string src = "Hello WORLD";
  std::vector<Change> changes;
  int n = apply_regex(src, "world", "there", "i", "desc", changes);
  REQUIRE(n == 1);
  REQUIRE(src == "Hello there");
}

// ---------------------------------------------------------------------------
// 4. load_migrations against the real share/plugin_migrations directory
// ---------------------------------------------------------------------------

TEST_CASE("load_migrations chains the real P6->P7 and P7->P8 steps",
          "[plugin_migrate][migrations]") {
  auto steps = load_migrations(migrations_dir());
  REQUIRE(steps.size() == 2);
  REQUIRE(steps.count(6) == 1);
  REQUIRE(steps.count(7) == 1);
  REQUIRE(steps.at(6).from == 6);
  REQUIRE(steps.at(6).to == 7);
  REQUIRE(steps.at(6).lang == "cpp");
  REQUIRE(steps.at(7).from == 7);
  REQUIRE(steps.at(7).to == 8);

  // Spot-check the method transforms present in the P6->P7 document.
  const json &p6p7 = steps.at(6).doc;
  REQUIRE(p6p7.contains("source"));
  std::vector<std::string> method_names;
  for (const auto &t : p6p7["source"])
    if (t.value("kind", "") == "method")
      method_names.push_back(t.value("name", ""));
  REQUIRE(method_names.size() == 4);
  REQUIRE(std::find(method_names.begin(), method_names.end(), "set_params") !=
          method_names.end());
  REQUIRE(std::find(method_names.begin(), method_names.end(), "load_data") !=
          method_names.end());
  REQUIRE(std::find(method_names.begin(), method_names.end(), "process") !=
          method_names.end());
  REQUIRE(std::find(method_names.begin(), method_names.end(), "get_output") !=
          method_names.end());

  REQUIRE(p6p7["cmake"]["plugin_git_tag"] == "v2.3-P7");
  REQUIRE(p6p7["cmake"]["pugg_git_tag"] == "1.1.0");

  const json &p7p8 = steps.at(7).doc;
  REQUIRE(p7p8["cmake"]["plugin_git_tag"] == "v2.4-P8");
  REQUIRE(p7p8["cmake"]["pugg_git_tag"] == "1.2.0");
}

TEST_CASE("load_migrations returns an empty map for a non-existent directory",
          "[plugin_migrate][migrations]") {
  auto steps = load_migrations(unique_temp_path("no_such_dir"));
  REQUIRE(steps.empty());
}

TEST_CASE("load_migrations skips malformed JSON and non-cpp/non-chaining entries",
          "[plugin_migrate][migrations]") {
  ScopedPath dir(unique_temp_path("migsyn"));
  fs::create_directories(dir.p);

  REQUIRE(write_file(dir.p / "ok.json",
                     R"({"from": 6, "to": 7, "lang": "cpp"})"));
  REQUIRE(write_file(dir.p / "broken.json", "{ this is not json"));
  REQUIRE(write_file(dir.p / "wrong_lang.json",
                     R"({"from": 1, "to": 2, "lang": "rust"})"));
  REQUIRE(write_file(dir.p / "non_chaining.json",
                     R"({"from": 2, "to": 9, "lang": "cpp"})"));
  REQUIRE(write_file(dir.p / "not_json.txt", "irrelevant"));

  auto steps = load_migrations(dir.p);
  REQUIRE(steps.size() == 1);
  REQUIRE(steps.count(6) == 1);
}

// ---------------------------------------------------------------------------
// 5. SpanRewriter / MethodTransform on realistic, tricky snippets
// ---------------------------------------------------------------------------

TEST_CASE("SpanRewriter rewrites a simple single-line signature",
          "[plugin_migrate][rewriter]") {
  std::string src = "  void set_params(void *params) override {\n    _p = params;\n  }\n";
  MethodTransform t;
  t.name = "set_params";
  t.params = "(const json &params)";
  t.require_qualifier = "override";
  t.description = "set_params now takes a json&";

  auto rw = make_rewriter(t);
  std::vector<Change> changes;
  int n = rw->rewrite_method(src, t, changes);
  REQUIRE(n == 1);
  REQUIRE(changes.size() == 1);
  REQUIRE(changes[0].line == 1);
  REQUIRE(src.find("set_params(const json &params) override") != std::string::npos);
}

TEST_CASE("SpanRewriter handles a signature split across multiple lines",
          "[plugin_migrate][rewriter]") {
  std::string src =
      "  bool load_data(\n"
      "      json const &input\n"
      "  ) override {\n"
      "    return true;\n"
      "  }\n";
  MethodTransform t;
  t.name = "load_data";
  t.params = "(json const &input, string topic = \"\", vector<unsigned char> const *blob = nullptr)";
  t.require_qualifier = "override";
  t.description = "load_data gains topic/blob";

  auto rw = make_rewriter(t);
  std::vector<Change> changes;
  int n = rw->rewrite_method(src, t, changes);
  REQUIRE(n == 1);
  REQUIRE(src.find("load_data(json const &input, string topic = \"\", "
                   "vector<unsigned char> const *blob = nullptr) override") !=
          std::string::npos);
  // The multi-line whitespace that used to separate '(' and the parameter is
  // gone: the whole span from '(' to ')' was replaced verbatim.
  REQUIRE(src.find("json const &input\n") == std::string::npos);
}

TEST_CASE("SpanRewriter ignores '(' inside a line comment right before the signature",
          "[plugin_migrate][rewriter]") {
  std::string src =
      "  // load_data(...) used a raw pointer pre-P7\n"
      "  bool load_data(json const &input) override { return true; }\n";
  MethodTransform t;
  t.name = "load_data";
  t.params = "(json const &input, string topic)";
  t.require_qualifier = "override";

  auto rw = make_rewriter(t);
  std::vector<Change> changes;
  int n = rw->rewrite_method(src, t, changes);
  REQUIRE(n == 1);
  REQUIRE(changes[0].line == 2);
  REQUIRE(src.find("// load_data(...) used a raw pointer pre-P7") != std::string::npos);
}

TEST_CASE("SpanRewriter ignores '(' inside a block comment and a string literal",
          "[plugin_migrate][rewriter]") {
  std::string src =
      "  /* process(...) takes no args (yet) */\n"
      "  std::string msg = \"about to call process(...) now\";\n"
      "  bool process() override { return true; }\n";
  MethodTransform t;
  t.name = "process";
  t.params = "(json &out, vector<unsigned char> *blob = nullptr)";
  t.require_qualifier = "override";

  auto rw = make_rewriter(t);
  std::vector<Change> changes;
  int n = rw->rewrite_method(src, t, changes);
  REQUIRE(n == 1);
  REQUIRE(src.find("process(json &out, vector<unsigned char> *blob = nullptr) override") !=
          std::string::npos);
  REQUIRE(src.find("\"about to call process(...) now\"") != std::string::npos);
}

TEST_CASE("SpanRewriter skips call sites (member/scope-qualified) and only "
          "rewrites declarations",
          "[plugin_migrate][rewriter]") {
  std::string src =
      "  instance.set_params(&params);\n"
      "  Ns::set_params(&params);\n"
      "  void set_params(void *params) override { }\n";
  MethodTransform t;
  t.name = "set_params";
  t.params = "(const json &params)";
  t.require_qualifier = "override";

  auto rw = make_rewriter(t);
  std::vector<Change> changes;
  int n = rw->rewrite_method(src, t, changes);
  REQUIRE(n == 1);
  REQUIRE(src.find("instance.set_params(&params)") != std::string::npos);
  REQUIRE(src.find("Ns::set_params(&params)") != std::string::npos);
  REQUIRE(src.find("void set_params(const json &params) override") != std::string::npos);
}

TEST_CASE("SpanRewriter requires the qualifier when one is configured",
          "[plugin_migrate][rewriter]") {
  std::string src = "  void set_params(void *params) { /* no override */ }\n";
  MethodTransform t;
  t.name = "set_params";
  t.params = "(const json &params)";
  t.require_qualifier = "override";

  auto rw = make_rewriter(t);
  std::vector<Change> changes;
  int n = rw->rewrite_method(src, t, changes);
  REQUIRE(n == 0);
  REQUIRE(changes.empty());
  REQUIRE(src.find("void set_params(void *params)") != std::string::npos);
}

TEST_CASE("SpanRewriter rewrites multiple declarations in one buffer, last-to-first",
          "[plugin_migrate][rewriter]") {
  std::string src =
      "  bool process() override { return true; }\n"
      "  // a second overload further down\n"
      "  bool process() override { return false; }\n";
  MethodTransform t;
  t.name = "process";
  t.params = "(json &out)";
  t.require_qualifier = "override";

  auto rw = make_rewriter(t);
  std::vector<Change> changes;
  int n = rw->rewrite_method(src, t, changes);
  REQUIRE(n == 2);
  REQUIRE(changes.size() == 2);
  std::size_t first = src.find("process(json &out) override");
  std::size_t second = src.find("process(json &out) override", first + 1);
  REQUIRE(first != std::string::npos);
  REQUIRE(second != std::string::npos);
}

TEST_CASE("SpanRewriter returns 0 when the method name does not occur",
          "[plugin_migrate][rewriter]") {
  std::string src = "  void unrelated() override { }\n";
  MethodTransform t;
  t.name = "set_params";
  t.params = "(const json &params)";
  t.require_qualifier = "override";

  auto rw = make_rewriter(t);
  std::vector<Change> changes;
  int n = rw->rewrite_method(src, t, changes);
  REQUIRE(n == 0);
  REQUIRE(src == "  void unrelated() override { }\n");
}

TEST_CASE("make_rewriter falls back to SpanRewriter when no ts_query is set",
          "[plugin_migrate][rewriter]") {
  MethodTransform t;
  t.name = "foo";
  auto rw = make_rewriter(t);
  REQUIRE(rw != nullptr);
  std::string src = "void foo() override {}\n";
  std::vector<Change> changes;
  // Just confirm the returned object is a working SpanRewriter (Tier A),
  // since MADS_ENABLE_TREE_SITTER is not built in this configuration.
  int n = rw->rewrite_method(src, t, changes);
  REQUIRE(n == 1);
}

// ---------------------------------------------------------------------------
// 6. run() end-to-end against a temp copy of tests/fixtures/plugin_p6
// ---------------------------------------------------------------------------

TEST_CASE("run() dry-run leaves every file untouched", "[plugin_migrate][run]") {
  fs::path project = copy_fixture("dryrun");
  ScopedPath cleanup(project);

  std::string cmake_before, src_before, helper_before;
  REQUIRE(read_file(project / "CMakeLists.txt", cmake_before));
  REQUIRE(read_file(project / "src" / "p6demo.cpp", src_before));
  REQUIRE(read_file(project / "src" / "p6demo_helper.cpp", helper_before));

  Options opts;
  opts.dry_run = true;
  opts.check = false;
  int rc = run(project, migrations_dir(), deps_manifest(), opts);
  REQUIRE(rc == 0);

  std::string cmake_after, src_after, helper_after;
  REQUIRE(read_file(project / "CMakeLists.txt", cmake_after));
  REQUIRE(read_file(project / "src" / "p6demo.cpp", src_after));
  REQUIRE(read_file(project / "src" / "p6demo_helper.cpp", helper_after));

  REQUIRE(cmake_after == cmake_before);
  REQUIRE(src_after == src_before);
  REQUIRE(helper_after == helper_before);
  REQUIRE_FALSE(fs::exists(project / "CMakeLists.txt.bak"));
  REQUIRE_FALSE(fs::exists(project / "src" / "p6demo.cpp.bak"));
}

TEST_CASE("run() migrates P6 -> P8 end to end with the compile check disabled",
          "[plugin_migrate][run]") {
  fs::path project = copy_fixture("full");
  ScopedPath cleanup(project);

  Options opts;
  opts.dry_run = false;
  opts.check = false; // never invoke cmake/network from the test suite
  int rc = run(project, migrations_dir(), deps_manifest(), opts);
  REQUIRE(rc == 0);

  std::string cmake_after;
  REQUIRE(read_file(project / "CMakeLists.txt", cmake_after));
  // Both migration steps' cmake transforms applied, in order: final tag is
  // the P7->P8 target, not the intermediate P6->P7 one.
  REQUIRE(cmake_after.find("v2.4-P8") != std::string::npos);
  REQUIRE(cmake_after.find("v2.3-P6") == std::string::npos);
  REQUIRE(cmake_after.find("1.2.0") != std::string::npos);
  // The nlohmann/json FetchContent block was modernized to the tarball form.
  REQUIRE(cmake_after.find("json.tar.xz") != std::string::npos);
  REQUIRE(cmake_after.find("GIT_TAG        v3.11.3") == std::string::npos);
  // ...and carried all the way to the P7->P8 json version. P6->P7 rewrites the
  // block to the tarball form at v3.11.3, so the P7->P8 rule matching the
  // legacy GIT_TAG form can never fire here; a second rule bumps the tarball
  // URL itself. Without it the migration silently stops at v3.11.3, leaving a
  // migrated plugin older than one `mads plugin` scaffolds from
  // share/plugin_deps.json.
  REQUIRE(cmake_after.find("v3.12.0") != std::string::npos);
  REQUIRE(cmake_after.find("v3.11.3") == std::string::npos);

  std::string src_after;
  REQUIRE(read_file(project / "src" / "p6demo.cpp", src_after));
  REQUIRE(src_after.find("set_params(const json &params) override") != std::string::npos);
  REQUIRE(src_after.find("_params.merge_patch(params)") != std::string::npos);
  REQUIRE(src_after.find("*(json *)params") == std::string::npos);
  REQUIRE(src_after.find("load_data(json const &input, string topic = \"\", "
                         "vector<unsigned char> const *blob = nullptr) override") !=
          std::string::npos);
  REQUIRE(src_after.find("process(json &out, vector<unsigned char> *blob = nullptr) override") !=
          std::string::npos);
  REQUIRE(src_after.find("get_output(json &out, vector<unsigned char> *blob = nullptr) override") !=
          std::string::npos);
  // P7->P8: the INSTALL_SOURCE_DRIVER macro is replaced.
  REQUIRE(src_after.find("MADS_REGISTER_PLUGINS(P6demoPlugin)") != std::string::npos);
  REQUIRE(src_after.find("INSTALL_SOURCE_DRIVER") == std::string::npos);
  // Untouched declarations/comments/strings survive.
  REQUIRE(src_after.find("about to call set_params(...)") != std::string::npos);
  REQUIRE(src_after.find("const char *kind() override") != std::string::npos);

  std::string helper_after;
  REQUIRE(read_file(project / "src" / "p6demo_helper.cpp", helper_after));
  REQUIRE(helper_after.find(".set_params(params)") != std::string::npos);
  REQUIRE(helper_after.find(".set_params(&params)") == std::string::npos);

  // Backups of the pre-migration originals were written.
  REQUIRE(fs::exists(project / "CMakeLists.txt.bak"));
  REQUIRE(fs::exists(project / "src" / "p6demo.cpp.bak"));
  REQUIRE(fs::exists(project / "src" / "p6demo_helper.cpp.bak"));
  std::string cmake_bak;
  REQUIRE(read_file(project / "CMakeLists.txt.bak", cmake_bak));
  REQUIRE(cmake_bak.find("v2.3-P6") != std::string::npos);
}

TEST_CASE("run() reports nothing-to-do when already at/above the target protocol",
          "[plugin_migrate][run]") {
  fs::path project = copy_fixture("attarget");
  ScopedPath cleanup(project);

  std::string cmake_before;
  REQUIRE(read_file(project / "CMakeLists.txt", cmake_before));

  Options opts;
  opts.check = false;
  opts.to_override = 6; // == the auto-detected current protocol
  int rc = run(project, migrations_dir(), deps_manifest(), opts);
  REQUIRE(rc == 0);

  std::string cmake_after;
  REQUIRE(read_file(project / "CMakeLists.txt", cmake_after));
  REQUIRE(cmake_after == cmake_before);
  REQUIRE_FALSE(fs::exists(project / "CMakeLists.txt.bak"));
}

TEST_CASE("run() fails with an error code when CMakeLists.txt is missing",
          "[plugin_migrate][run]") {
  ScopedPath project(unique_temp_path("nocmake"));
  fs::create_directories(project.p);

  Options opts;
  opts.check = false;
  int rc = run(project.p, migrations_dir(), deps_manifest(), opts);
  REQUIRE(rc == 1);
}

TEST_CASE("run() fails with an error code when the protocol cannot be auto-detected",
          "[plugin_migrate][run]") {
  ScopedPath project(unique_temp_path("noproto"));
  fs::create_directories(project.p);
  REQUIRE(write_file(project.p / "CMakeLists.txt", "project(noproto)\n"));

  Options opts;
  opts.check = false; // no --from override, and no GIT_TAG v*-P<N> to detect
  int rc = run(project.p, migrations_dir(), deps_manifest(), opts);
  REQUIRE(rc == 1);
}

TEST_CASE("run() honours --from/--to overrides and stops short of the full chain",
          "[plugin_migrate][run]") {
  fs::path project = copy_fixture("override");
  ScopedPath cleanup(project);

  // Strip the -P6 suffix so auto-detection would fail without --from.
  std::string cmake;
  REQUIRE(read_file(project / "CMakeLists.txt", cmake));
  std::string stripped = cmake;
  auto pos = stripped.find("v2.3-P6");
  REQUIRE(pos != std::string::npos);
  stripped.replace(pos, std::string("v2.3-P6").size(), "v2.3");
  REQUIRE(detect_protocol(stripped) == -1); // confirms auto-detection would fail
  REQUIRE(write_file(project / "CMakeLists.txt", stripped));

  Options opts;
  opts.check = false;
  opts.from_override = 6;
  opts.to_override = 7; // stop after the P6->P7 step only
  int rc = run(project, migrations_dir(), deps_manifest(), opts);
  REQUIRE(rc == 0);

  std::string cmake_after;
  REQUIRE(read_file(project / "CMakeLists.txt", cmake_after));
  REQUIRE(cmake_after.find("v2.3-P7") != std::string::npos);
  REQUIRE(cmake_after.find("v2.4-P8") == std::string::npos);

  std::string src_after;
  REQUIRE(read_file(project / "src" / "p6demo.cpp", src_after));
  // P6->P7 source transforms applied...
  REQUIRE(src_after.find("set_params(const json &params) override") != std::string::npos);
  // ...but P7->P8 did not run.
  REQUIRE(src_after.find("INSTALL_SOURCE_DRIVER") != std::string::npos);
  REQUIRE(src_after.find("MADS_REGISTER_PLUGINS") == std::string::npos);
}

TEST_CASE("run() fails when a migration step is missing from the chain",
          "[plugin_migrate][run]") {
  fs::path project = copy_fixture("gap");
  ScopedPath cleanup(project);

  std::string cmake_before;
  REQUIRE(read_file(project / "CMakeLists.txt", cmake_before));

  Options opts;
  opts.check = false;
  opts.from_override = 2; // no P2->P3 step exists in share/plugin_migrations
  int rc = run(project, migrations_dir(), deps_manifest(), opts);
  REQUIRE(rc == 1);

  // The chain is verified before any file is touched.
  std::string cmake_after;
  REQUIRE(read_file(project / "CMakeLists.txt", cmake_after));
  REQUIRE(cmake_after == cmake_before);
}

TEST_CASE("run() fails when no target protocol can be determined",
          "[plugin_migrate][run]") {
  fs::path project = copy_fixture("notarget");
  ScopedPath cleanup(project);

  ScopedPath empty_migrations(unique_temp_path("empty_migrations"));
  fs::create_directories(empty_migrations.p);
  fs::path missing_manifest = unique_temp_path("missing_manifest.json");

  Options opts;
  opts.check = false;
  int rc = run(project, empty_migrations.p, missing_manifest, opts);
  REQUIRE(rc == 1);
}

TEST_CASE("run() reports no matching code when the chain applies cleanly but nothing matches",
          "[plugin_migrate][run]") {
  ScopedPath project(unique_temp_path("nomatch"));
  fs::create_directories(project.p);
  // No FetchContent(plugin/pugg) blocks and no src/ directory: the P6->P7
  // step has nothing to touch, so the run should still succeed with 0 edits.
  REQUIRE(write_file(project.p / "CMakeLists.txt", "project(nomatch)\n"));

  Options opts;
  opts.check = false;
  opts.from_override = 6;
  opts.to_override = 7;
  int rc = run(project.p, migrations_dir(), deps_manifest(), opts);
  REQUIRE(rc == 0);

  std::string cmake_after;
  REQUIRE(read_file(project.p / "CMakeLists.txt", cmake_after));
  REQUIRE(cmake_after == "project(nomatch)\n");
  REQUIRE_FALSE(fs::exists(project.p / "CMakeLists.txt.bak"));
}
