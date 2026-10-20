// Unit tests for the `mads ini` command line in src/ini/ini_cli.hpp: exit
// codes of --check, --get and --set on temporary files, comment preservation
// on save, and the GUI dispatch (FILE / bare) through a fake launch_gui.
// The template-rendering paths need an installed prefix and are not tested.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "ini_cli.hpp"
#include "ini_document.hpp"

using Mads::Ini::Document;
using Mads::Ini::run_cli;

namespace fs = std::filesystem;

namespace {

// Builds the argv of a run_cli call: argv[0] is the command name.
class Args {
public:
  Args(std::initializer_list<std::string> args) : _store{"ini"} {
    _store.insert(_store.end(), args.begin(), args.end());
    for (auto &s : _store) {
      _ptrs.push_back(s.data());
    }
    _ptrs.push_back(nullptr);
  }
  int argc() const { return static_cast<int>(_store.size()); }
  char **argv() { return _ptrs.data(); }

private:
  std::vector<std::string> _store;
  std::vector<char *> _ptrs;
};

// A unique temporary mads.ini, removed (with its .bak) on destruction.
class TempIni {
public:
  explicit TempIni(const std::string &text) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::mt19937 rng{std::random_device{}()};
    _path = fs::temp_directory_path() /
            ("mads_ini_cli_" + std::to_string(stamp) + "_" +
             std::to_string(rng() % 100000) + ".ini");
    std::ofstream out(_path, std::ios::binary);
    out << text;
  }
  ~TempIni() {
    std::error_code ec;
    fs::remove(_path, ec);
    fs::remove(fs::path(_path.string() + ".bak"), ec);
  }
  TempIni(const TempIni &) = delete;
  TempIni &operator=(const TempIni &) = delete;

  const fs::path &path() const { return _path; }
  std::string str() const {
    std::ifstream in(_path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }

private:
  fs::path _path;
};

// Silences std::cout / std::cerr for the lifetime of the object.
class Quiet {
public:
  Quiet() : _out(std::cout.rdbuf()), _err(std::cerr.rdbuf()) {
    std::cout.rdbuf(_sink.rdbuf());
    std::cerr.rdbuf(_sink.rdbuf());
  }
  ~Quiet() {
    std::cout.rdbuf(_out);
    std::cerr.rdbuf(_err);
  }

private:
  std::ostringstream _sink;
  std::streambuf *_out;
  std::streambuf *_err;
};

// Captures what run_cli prints to std::cout.
class CaptureOut {
public:
  CaptureOut() : _old(std::cout.rdbuf(_sink.rdbuf())) {}
  ~CaptureOut() { std::cout.rdbuf(_old); }
  std::string str() const { return _sink.str(); }

private:
  std::ostringstream _sink;
  std::streambuf *_old;
};

// Every line starting with '#' (after blanks), in order.
std::vector<std::string> comment_lines(const std::string &text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    auto first = line.find_first_not_of(" \t");
    if (first != std::string::npos && line[first] == '#') {
      out.push_back(line.substr(first));
    }
  }
  return out;
}

const char *kClean = R"(# Header comment for the database
[logger]
# Database URI, one line
mongo_uri = "mongodb://old:1" # old note
# plain note, no key
)";

} // namespace

TEST_CASE("mads ini --check exit codes", "[ini][cli]") {
  Quiet quiet;
  SECTION("clean file returns 0") {
    TempIni ini(kClean);
    Args args{"--check", ini.path().string()};
    CHECK(run_cli(args.argc(), args.argv()) == 0);
  }
  SECTION("unknown key in [logger] returns 1") {
    TempIni ini(std::string(kClean) + "bogus_key = 1\n");
    Args args{"--check", ini.path().string()};
    CHECK(run_cli(args.argc(), args.argv()) == 1);
  }
  SECTION("invalid TOML returns 2") {
    TempIni ini("[logger\nmongo_uri = 1\n");
    Args args{"--check", ini.path().string()};
    CHECK(run_cli(args.argc(), args.argv()) == 2);
  }
  SECTION("unreadable file returns 2") {
    Args args{"--check", "/nonexistent/dir/mads.ini"};
    CHECK(run_cli(args.argc(), args.argv()) == 2);
  }
}

TEST_CASE("mads ini --get prints raw values", "[ini][cli]") {
  Quiet quiet;
  TempIni ini(kClean);
  SECTION("existing key") {
    CaptureOut out;
    Args args{"--get", "logger.mongo_uri", ini.path().string()};
    CHECK(run_cli(args.argc(), args.argv()) == 0);
    CHECK(out.str() == "\"mongodb://old:1\"\n");
  }
  SECTION("missing key returns 1") {
    Args args{"--get", "logger.nope", ini.path().string()};
    CHECK(run_cli(args.argc(), args.argv()) == 1);
  }
  SECTION("missing section returns 1") {
    Args args{"--get", "nosuch.key", ini.path().string()};
    CHECK(run_cli(args.argc(), args.argv()) == 1);
  }
  SECTION("key without a dot is an error") {
    Args args{"--get", "logger", ini.path().string()};
    CHECK(run_cli(args.argc(), args.argv()) == 1);
  }
}

TEST_CASE("mads ini --set updates keep every comment", "[ini][cli]") {
  Quiet quiet;
  TempIni ini(kClean);
  const auto before = comment_lines(ini.str());

  Args args{"--set", "logger.mongo_uri=\"mongodb://new:2\"", ini.path().string()};
  REQUIRE(run_cli(args.argc(), args.argv()) == 0);

  CHECK(comment_lines(ini.str()) == before);
  auto doc = Document::parse(ini.str());
  CHECK(doc.raw_value("logger", "mongo_uri") == "\"mongodb://new:2\"");
}

TEST_CASE("mads ini --set creates missing keys and sections", "[ini][cli]") {
  Quiet quiet;
  TempIni ini(kClean);
  Args args{"--set", "logger.mongo_db=42", "--set", "newsec.flag=true",
            ini.path().string()};
  REQUIRE(run_cli(args.argc(), args.argv()) == 0);

  auto doc = Document::parse(ini.str());
  CHECK(doc.raw_value("logger", "mongo_db") == "42");
  CHECK(doc.raw_value("newsec", "flag") == "true");
  CHECK(doc.find("newsec") != nullptr);
  CHECK(comment_lines(ini.str()) == comment_lines(kClean));
}

TEST_CASE("mads ini --set quotes plain words as strings", "[ini][cli]") {
  Quiet quiet;
  TempIni ini(kClean);
  Args args{"--set", "logger.mongo_db=hello world", "--set",
            "logger.mongo_uri=a\"b\\c", ini.path().string()};
  REQUIRE(run_cli(args.argc(), args.argv()) == 0);

  auto doc = Document::parse(ini.str());
  CHECK(doc.raw_value("logger", "mongo_db") == "\"hello world\"");
  CHECK(doc.raw_value("logger", "mongo_uri") == "\"a\\\"b\\\\c\"");
}

TEST_CASE("mads ini --set applies --comment", "[ini][cli]") {
  Quiet quiet;
  TempIni ini(kClean);
  Args args{"--set", "logger.mongo_db=7", "--comment", "tuned by hand",
            ini.path().string()};
  REQUIRE(run_cli(args.argc(), args.argv()) == 0);

  auto doc = Document::parse(ini.str());
  const auto *entry = doc.find("logger")->find("mongo_db");
  REQUIRE(entry != nullptr);
  CHECK(entry->inline_comment == "tuned by hand");
}

TEST_CASE("mads ini --set rejects bad specs without saving", "[ini][cli]") {
  Quiet quiet;
  TempIni ini(kClean);
  Args args{"--set", "logger.mongo_db=1", "--set", "no_equal_sign",
            ini.path().string()};
  CHECK(run_cli(args.argc(), args.argv()) == 1);
  CHECK(ini.str() == kClean);
}

TEST_CASE("mads ini dispatch to the GUI", "[ini][cli]") {
  Quiet quiet;
  SECTION("FILE alone passes the path to launch_gui") {
    std::optional<fs::path> seen;
    int calls = 0;
    auto launch = [&](std::optional<fs::path> p) {
      seen = p;
      ++calls;
      return 7;
    };
    Args args{"/tmp/some/mads.ini"};
    CHECK(run_cli(args.argc(), args.argv(), launch) == 7);
    CHECK(calls == 1);
    REQUIRE(seen.has_value());
    CHECK(*seen == fs::path("/tmp/some/mads.ini"));
  }
  SECTION("bare invocation passes the default path only if it exists") {
    std::optional<fs::path> seen;
    auto launch = [&](std::optional<fs::path> p) {
      seen = p;
      return 0;
    };
    Args args{};
    CHECK(run_cli(args.argc(), args.argv(), launch) == 0);
    if (fs::exists(Mads::Ini::default_ini_path())) {
      REQUIRE(seen.has_value());
      CHECK(*seen == Mads::Ini::default_ini_path());
    } else {
      CHECK_FALSE(seen.has_value());
    }
  }
  SECTION("FILE alone without a GUI is an error") {
    Args args{"/tmp/some/mads.ini"};
    CHECK(run_cli(args.argc(), args.argv()) != 0);
  }
  SECTION("--check combined with -o is an error") {
    TempIni ini(kClean);
    Args args{"--check", "-o", "/tmp/x.ini", ini.path().string()};
    CHECK(run_cli(args.argc(), args.argv()) != 0);
  }
}
