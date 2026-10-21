/*
 ___       _   ____ _     ___
|_ _|_ __ (_)  / ___| |   |_ _|
 | || '_ \| | | |   | |    | |
 | || | | | | | |___| |___ | |
|___|_| |_|_|  \____|_____|___|

Implementation of the `mads ini` command line (see ini_cli.hpp).

Author(s): Paolo Bosetti
*/
#include "ini_cli.hpp"

#include "ini_document.hpp"
#include "ini_schema.hpp"
#include "remote_edit.hpp"
#include "../exec_path.hpp"
#include "../mads.hpp"

#include <cxxopts.hpp>
#include <inja/inja.hpp>
#include <nlohmann/json.hpp>
#include <rang.hpp>
#include <sodium.h>

#include <cstdio>
#include <exception>
#include <optional>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace Mads::Ini {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
using rang::fg;

// Splits "section.key" on its first '.'; keys may contain further dots.
bool split_section_key(std::string_view text, std::string &section,
                       std::string &key) {
  auto dot = text.find('.');
  if (dot == std::string_view::npos || dot == 0 || dot + 1 == text.size()) {
    return false;
  }
  section = std::string(text.substr(0, dot));
  key = std::string(text.substr(dot + 1));
  return true;
}

std::string trim(std::string_view text) {
  auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return "";
  }
  auto last = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(first, last - first + 1));
}

// A TOML basic string holding `text` verbatim.
std::string quote_toml_string(std::string_view text) {
  std::string out = "\"";
  for (unsigned char c : text) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20 || c == 0x7f) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "\\u%04X", static_cast<unsigned>(c));
        out += buf;
      } else {
        out += static_cast<char>(c);
      }
    }
  }
  out += '"';
  return out;
}

// Removes a trailing CR and LF (a line read with getline keeps the CR of CRLF).
void strip_eol(std::string &text) {
  while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
    text.pop_back();
  }
}

// Wipes a secret string on scope exit (also on error paths).
class SecretGuard {
public:
  explicit SecretGuard(std::string &secret) : _secret(secret) {}
  ~SecretGuard() {
    if (!_secret.empty()) {
      sodium_memzero(_secret.data(), _secret.size());
    }
  }
  SecretGuard(const SecretGuard &) = delete;
  SecretGuard &operator=(const SecretGuard &) = delete;

private:
  std::string &_secret;
};

bool stdin_is_terminal() {
#ifdef _WIN32
  return _isatty(_fileno(stdin)) != 0;
#else
  return isatty(STDIN_FILENO) != 0;
#endif
}

// Turns terminal echo off for the lifetime of the object; the previous mode is
// restored on destruction, also when reading throws.
class EchoOff {
public:
  EchoOff() {
#ifdef _WIN32
    _handle = GetStdHandle(STD_INPUT_HANDLE);
    if (GetConsoleMode(_handle, &_mode) != 0) {
      _changed = SetConsoleMode(_handle, _mode & ~ENABLE_ECHO_INPUT) != 0;
    }
#else
    if (tcgetattr(STDIN_FILENO, &_saved) == 0) {
      termios quiet = _saved;
      quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
      _changed = tcsetattr(STDIN_FILENO, TCSANOW, &quiet) == 0;
    }
#endif
  }
  ~EchoOff() {
    if (_changed) {
#ifdef _WIN32
      SetConsoleMode(_handle, _mode);
#else
      tcsetattr(STDIN_FILENO, TCSANOW, &_saved);
#endif
    }
  }
  EchoOff(const EchoOff &) = delete;
  EchoOff &operator=(const EchoOff &) = delete;

private:
  bool _changed = false;
#ifdef _WIN32
  HANDLE _handle = nullptr;
  DWORD _mode = 0;
#else
  termios _saved{};
#endif
};

constexpr std::size_t kPasswordReserve = 1024;

// Prompts on stderr and reads one line from the terminal without echo.
std::string read_typed_line(const char *prompt) {
  std::cerr << prompt << std::flush;
  std::string text;
  // Room for any sensible password up front: getline then never reallocates,
  // so no stray copy is left in freed memory (only this buffer is wiped).
  text.reserve(kPasswordReserve);
  {
    EchoOff echo;
    std::getline(std::cin, text);
  }
  // Echo was off, so the newline the user typed is not on screen.
  std::cerr << std::endl;
  strip_eol(text);
  return text;
}

fs::path settings_path(const cxxopts::ParseResult &parsed) {
  if (parsed.count("file")) {
    return fs::path(parsed["file"].as<std::string>());
  }
  return default_ini_path();
}

// Legacy template output: -o, -i, -p and the template parameters.
int template_command(const cxxopts::ParseResult &parsed, json &data,
                     const std::string &etc_dir) {
  std::string template_dir = Mads::exec_dir("../share/templates/");
  std::string output = "";
  if (parsed.count("output")) {
    if (parsed.count("install")) {
      std::cerr << fg::red << "Cannot install and write to file at the same time"
                << fg::reset << std::endl;
      return -1;
    }
    output = parsed["output"].as<std::string>();
  }
  if (parsed.count("broker")) {
    data["broker"] = parsed["broker"].as<std::string>();
  }
  if (parsed.count("frontend")) {
    data["port_frontend"] = parsed["frontend"].as<int>();
  }
  if (parsed.count("backend")) {
    data["port_backend"] = parsed["backend"].as<int>();
  }
  if (parsed.count("settings")) {
    data["port_settings"] = parsed["settings"].as<int>();
  }
  if (parsed.count("fps")) {
    data["fps"] = parsed["fps"].as<int>();
  }
  if (parsed.count("mongo")) {
    data["mongo_uri"] = parsed["mongo"].as<std::string>();
  }
  try {
    if (output != "") {
      fs::path out_path{output};
      if (out_path.is_relative()) {
        out_path = fs::absolute(out_path);
      }
      auto path = out_path.parent_path();
      auto file = out_path.filename();
      auto tmp = fs::temp_directory_path();
      if (!fs::exists(path)) {
        std::cerr << fg::red << "Path " << path << " does not exist"
                  << fg::reset << std::endl;
        return -1;
      }
      inja::Environment env{template_dir + "/", tmp.string() + "/"};
      env.write("mads.ini", data, file.string());
      try {
        fs::copy(tmp / file, path / file);
        fs::remove(tmp / file);
      } catch (const fs::filesystem_error &e) {
        fs::remove(tmp / file);
        std::cerr << e.what() << std::endl;
        std::cerr << fg::red << "Cannot write or overwrite " << path / file
                  << fg::reset << std::endl;
        return -1;
      }
      std::cout << fg::green << "INI file written to " << output << fg::reset
                << std::endl;
      return 0;
    }
#ifndef _WIN32
    if (parsed.count("install")) {
      if (!fs::exists(etc_dir)) {
        try {
          fs::create_directory(etc_dir);
        } catch (const fs::filesystem_error &e) {
          std::cerr << fg::red << "Cannot create directory " << etc_dir
                    << " (need sudo?)" << fg::reset << std::endl;
          return -1;
        }
      }
      auto etc = fs::absolute(etc_dir);
      auto tmp = fs::temp_directory_path();
      inja::Environment env{template_dir + "/", tmp.string() + "/"};
      env.write("mads.ini", data, "mads.ini");
      try {
        fs::copy(tmp / "mads.ini", etc / "mads.ini");
        fs::remove(tmp / "mads.ini");
      } catch (const fs::filesystem_error &e) {
        fs::remove(tmp / "mads.ini");
        std::cerr << fg::red << "Cannot write or overwrite " << etc / "mads.ini"
                  << fg::reset << std::endl;
        return -1;
      }
      std::cout << fg::green << "INI file installed to " << etc_dir
                << "/mads.ini" << fg::reset << std::endl;
      return 0;
    }
#endif
    std::cout << render_template(data) << std::endl;
    return 0;
  } catch (const std::exception &e) {
    // Missing or unreadable template (e.g. run from the build tree).
    std::cerr << fg::red << "Cannot render the INI template: " << e.what()
              << fg::reset << std::endl;
    return 1;
  }
}

int check_command(const fs::path &path) {
  Loaded loaded;
  try {
    loaded = FileSource(path).load();
  } catch (const std::exception &e) {
    std::cerr << fg::red << e.what() << fg::reset << std::endl;
    return 2;
  }
  std::vector<Diagnostic> diags;
  Document doc = Document::parse(loaded.text, &diags);
  for (auto &d : doc.validate()) {
    diags.push_back(d);
  }
  for (auto &d : Schema::builtin().lint(doc)) {
    diags.push_back(d);
  }
  int errors = 0;
  int warnings = 0;
  for (const auto &d : diags) {
    std::string where = path.string();
    if (d.line > 0) {
      where += ":" + std::to_string(d.line);
    }
    std::cout << where << ": ";
    switch (d.severity) {
    case Diagnostic::Severity::Error:
      ++errors;
      std::cout << fg::red << "error" << fg::reset;
      break;
    case Diagnostic::Severity::Warning:
      ++warnings;
      std::cout << fg::yellow << "warning" << fg::reset;
      break;
    case Diagnostic::Severity::Info:
      std::cout << "info";
      break;
    }
    std::cout << ": ";
    if (!d.section.empty()) {
      std::cout << "[" << d.section << "] ";
    }
    if (!d.key.empty()) {
      std::cout << d.key << ": ";
    }
    std::cout << d.message << std::endl;
  }
  if (errors == 0 && warnings == 0) {
    std::cout << fg::green << "OK" << fg::reset << ": " << path.string()
              << std::endl;
  } else {
    std::cout << errors << " error(s), " << warnings << " warning(s) in "
              << path.string() << std::endl;
  }
  if (errors > 0) {
    return 2;
  }
  return warnings > 0 ? 1 : 0;
}

int get_command(const std::string &spec, const fs::path &path) {
  std::string section;
  std::string key;
  if (!split_section_key(spec, section, key)) {
    std::cerr << fg::red << "Expected SECTION.KEY, got '" << spec << "'"
              << fg::reset << std::endl;
    return 1;
  }
  Document doc;
  try {
    doc = Document::parse(FileSource(path).load().text);
  } catch (const std::exception &e) {
    std::cerr << fg::red << e.what() << fg::reset << std::endl;
    return 1;
  }
  auto value = doc.raw_value(section, key);
  if (!value) {
    std::cerr << fg::red << "No active key " << section << "." << key
              << " in " << path.string() << fg::reset << std::endl;
    return 1;
  }
  std::cout << *value << std::endl;
  return 0;
}

struct Assignment {
  std::string section;
  std::string key;
  std::string raw;
};

int set_command(const std::vector<std::string> &items,
                const std::string *comment, const fs::path &path) {
  std::vector<Assignment> assignments;
  for (const auto &item : items) {
    auto eq = item.find('=');
    if (eq == std::string::npos) {
      std::cerr << fg::red << "Expected SECTION.KEY=VALUE, got '" << item
                << "'" << fg::reset << std::endl;
      return 1;
    }
    Assignment a;
    if (!split_section_key(std::string_view(item).substr(0, eq), a.section,
                           a.key)) {
      std::cerr << fg::red << "Expected SECTION.KEY=VALUE, got '" << item
                << "'" << fg::reset << std::endl;
      return 1;
    }
    std::string value = trim(std::string_view(item).substr(eq + 1));
    a.raw = is_valid_value(value) ? value : quote_toml_string(value);
    assignments.push_back(a);
  }

  Loaded loaded;
  Document doc;
  FileSource source(path);
  try {
    loaded = source.load();
    doc = Document::parse(loaded.text);
  } catch (const std::exception &e) {
    std::cerr << fg::red << e.what() << fg::reset << std::endl;
    return 1;
  }
  for (const auto &a : assignments) {
    if (doc.find(a.section) == nullptr && doc.add_section(a.section) == nullptr) {
      std::cerr << fg::red << "Invalid section name '" << a.section << "'"
                << fg::reset << std::endl;
      return 1;
    }
    if (!doc.set_raw_value(a.section, a.key, a.raw)) {
      std::cerr << fg::red << "Cannot set " << a.section << "." << a.key
                << " to " << a.raw << fg::reset << std::endl;
      return 1;
    }
    if (comment) {
      std::string text = *comment;
      for (auto &c : text) {
        if (c == '\n' || c == '\r') {
          c = ' ';
        }
      }
      if (Entry *entry = doc.find(a.section)->find(a.key)) {
        entry->inline_comment = text;
      }
    }
  }

  SaveResult result = source.save(doc.serialize(), loaded.etag);
  if (result.status != SaveResult::Status::Ok) {
    std::cerr << fg::red
              << (result.status == SaveResult::Status::Conflict
                      ? "Not saved, the file changed meanwhile: "
                      : "Cannot save: ")
              << result.message << fg::reset << std::endl;
    return 1;
  }
  for (const auto &a : assignments) {
    std::cout << fg::green << "set " << a.section << "." << a.key << " = "
              << a.raw << fg::reset << std::endl;
  }
  return 0;
}

// Derives and writes the verifier of `settings` from `password`, which is
// wiped before returning. Shared by the terminal and the stdin paths.
int store_password(const fs::path &settings, std::string &password,
                   std::ostream &out, std::ostream &err) {
  SecretGuard guard(password);
  if (password.empty()) {
    err << fg::red << "The editing password cannot be empty, nothing written"
        << fg::reset << std::endl;
    return 1;
  }
  if (password.size() < 8) {
    err << fg::yellow
        << "Warning: the password is shorter than 8 characters" << fg::reset
        << std::endl;
  }
  const fs::path file = Remote::password_file_for(settings);
  std::error_code ec;
  const bool existed = fs::exists(file, ec);
  err << "Deriving key..." << std::endl;
  try {
    Remote::PasswordVerifier verifier = Remote::PasswordVerifier::create(password);
    std::string error;
    if (!Remote::write_password_file(file, verifier, &error)) {
      err << fg::red << "Cannot write " << file.string() << ": " << error
          << fg::reset << std::endl;
      return 1;
    }
  } catch (const std::exception &e) {
    err << fg::red << "Cannot create the editing password: " << e.what()
        << fg::reset << std::endl;
    return 1;
  }
  out << fg::green
      << (existed ? "Editing password replaced, written to "
                  : "Editing password written to ")
      << file.string() << fg::reset << std::endl;
  out << "Restart the broker to apply it (see mads-broker(1))." << std::endl;
  return 0;
}

int set_password_command(const fs::path &settings) {
  if (!stdin_is_terminal()) {
    return set_password(settings, std::cin, std::cout, std::cerr);
  }
  std::string first = read_typed_line("New editing password: ");
  SecretGuard first_guard(first);
  std::string second = read_typed_line("Repeat it: ");
  SecretGuard second_guard(second);
  if (first != second) {
    std::cerr << fg::red << "The passwords do not match, nothing written"
              << fg::reset << std::endl;
    return 1;
  }
  return store_password(settings, first, std::cout, std::cerr);
}

int remove_password_command(const fs::path &settings) {
  const fs::path file = Remote::password_file_for(settings);
  std::error_code ec;
  const bool removed = fs::remove(file, ec);
  if (ec) {
    std::cerr << fg::red << "Cannot remove " << file.string() << ": "
              << ec.message() << fg::reset << std::endl;
    return 1;
  }
  if (!removed) {
    std::cout << fg::yellow << "no editing password file at " << file.string()
              << fg::reset << std::endl;
    return 0;
  }
  std::cout << fg::green << "Editing password removed: " << file.string()
            << fg::reset << std::endl;
  std::cout << "Restart the broker to apply it (see mads-broker(1))."
            << std::endl;
  return 0;
}

} // namespace

int set_password(const fs::path &settings, std::istream &in, std::ostream &out,
                 std::ostream &err) {
  std::string password;
  password.reserve(kPasswordReserve); // see read_typed_line()
  std::getline(in, password);
  strip_eol(password);
  return store_password(settings, password, out, err);
}

json default_template_data() {
  json data;
  data["broker"] = "localhost";
  data["port_frontend"] = "9090";
  data["port_backend"] = "9091";
  data["port_settings"] = "9092";
  data["mongo_uri"] = "mongodb://localhost:27017";
  return data;
}

std::string render_template(const json &data) {
  std::string template_dir = Mads::exec_dir("../share/templates/");
  inja::Environment env{template_dir + "/", "."};
  return env.render_file("mads.ini", data);
}

fs::path default_ini_path() {
  return fs::path(Mads::exec_dir("../etc/")) / "mads.ini";
}

int run_cli(int argc, char **argv, const LaunchGui &launch_gui) {
  json data = default_template_data();
  std::string etc_dir = Mads::exec_dir("../etc/");
  cxxopts::Options options("mads ini",
                           "Create INI file template, version " +
                               Mads::version());
  // clang-format off
  options.add_options()
    ("o,output", "Output file on a given path", cxxopts::value<std::string>())
#ifndef _WIN32
    ("i,install", "Install INI file to " + etc_dir, cxxopts::value<bool>())
#endif
    ("b,broker", "broker hostname or IP", cxxopts::value<std::string>())
    ("F,frontend", "frontend port", cxxopts::value<int>())
    ("B,backend", "backend port", cxxopts::value<int>())
    ("s,settings", "settings port", cxxopts::value<int>())
    ("f,fps", "Frames per second", cxxopts::value<int>())
    ("m,mongo", "MongoDB URI", cxxopts::value<std::string>())
    ("p,print", "Print the rendered template")
    ("check", "Parse and lint FILE: 0 clean, 1 warnings, 2 errors")
    ("get", "Print the raw value of SECTION.KEY", cxxopts::value<std::string>())
    ("set", "Set SECTION.KEY=VALUE (repeatable)", cxxopts::value<std::vector<std::string>>())
    ("comment", "Inline comment for the --set keys", cxxopts::value<std::string>())
    ("set-password", "Set the broker's editing password (FILE: settings file)")
    ("remove-password", "Remove the broker's editing password file")
    ("file", "Settings file (positional)", cxxopts::value<std::string>())
    ("h,help", "Print help");
  // clang-format on
  options.parse_positional({"file"});
  options.positional_help("[FILE]");
  cxxopts::ParseResult parsed;
  try {
    parsed = options.parse(argc, argv);
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    std::cerr << fg::red << "Unknown CLI option" << fg::reset << '\n';
    std::cerr << options.help() << std::endl;
    return -1;
  }
  if (parsed.count("help")) {
    std::cout << options.help() << std::endl;
    return 0;
  }

  bool template_opts = parsed.count("output") || parsed.count("broker") ||
                       parsed.count("frontend") || parsed.count("backend") ||
                       parsed.count("settings") || parsed.count("fps") ||
                       parsed.count("mongo") || parsed.count("print");
#ifndef _WIN32
  template_opts = template_opts || parsed.count("install");
#endif
  const int modes = static_cast<int>(parsed.count("check") > 0) +
                    static_cast<int>(parsed.count("get") > 0) +
                    static_cast<int>(parsed.count("set") > 0) +
                    static_cast<int>(parsed.count("set-password") > 0) +
                    static_cast<int>(parsed.count("remove-password") > 0);
  const bool has_file = parsed.count("file") > 0;

  if (parsed.count("comment") && !parsed.count("set")) {
    std::cerr << fg::red << "--comment requires --set" << fg::reset
              << std::endl;
    return 1;
  }
  if (modes > 1) {
    std::cerr << fg::red
              << "Use only one of --check, --get, --set, --set-password and "
                 "--remove-password"
              << fg::reset << std::endl;
    return 1;
  }
  if (modes > 0 && template_opts) {
    std::cerr << fg::red
              << "--check, --get, --set, --set-password and --remove-password "
                 "cannot be mixed with template options (-o, -i, -b, -F, -B, "
                 "-s, -f, -m, -p)"
              << fg::reset << std::endl;
    return 1;
  }
  if (template_opts && has_file) {
    std::cerr << fg::red << "FILE cannot be combined with template options"
              << fg::reset << std::endl;
    return 1;
  }

  if (template_opts) {
    return template_command(parsed, data, etc_dir);
  }
  // Every remaining mode works on an existing settings file: check it first,
  // so that e.g. --set-password does not ask for a password in vain. Run from
  // a build tree, the default <prefix>/etc/mads.ini usually does not exist.
  const bool file_mode = parsed.count("set-password") ||
                         parsed.count("remove-password") ||
                         parsed.count("check") || parsed.count("get") ||
                         parsed.count("set");
  if (file_mode) {
    const fs::path settings = settings_path(parsed);
    std::error_code ec;
    if (!fs::is_regular_file(settings, ec)) {
      std::cerr << fg::red << "No settings file at " << settings.string()
                << fg::reset << std::endl;
      if (!has_file) {
        std::cerr << "That is the installed default; pass the settings file "
                     "explicitly, e.g. `mads ini "
                  << (parsed.count("set-password") ? "--set-password" :
                      parsed.count("remove-password") ? "--remove-password" :
                      parsed.count("check") ? "--check" :
                      parsed.count("get") ? "--get KEY" : "--set KEY=VALUE")
                  << " path/to/mads.ini`" << std::endl;
      }
      return parsed.count("check") ? 2 : 1;
    }
  }
  if (parsed.count("set-password")) {
    return set_password_command(settings_path(parsed));
  }
  if (parsed.count("remove-password")) {
    return remove_password_command(settings_path(parsed));
  }
  if (parsed.count("check")) {
    return check_command(settings_path(parsed));
  }
  if (parsed.count("get")) {
    return get_command(parsed["get"].as<std::string>(), settings_path(parsed));
  }
  if (parsed.count("set")) {
    std::string comment;
    const std::string *comment_ptr = nullptr;
    if (parsed.count("comment")) {
      comment = parsed["comment"].as<std::string>();
      comment_ptr = &comment;
    }
    return set_command(parsed["set"].as<std::vector<std::string>>(),
                       comment_ptr, settings_path(parsed));
  }

  if (has_file) {
    if (!launch_gui) {
      std::cerr << fg::red
                << "no GUI in this build; use --check/--get/--set"
                << fg::reset << std::endl;
      return 1;
    }
    return launch_gui(fs::path(parsed["file"].as<std::string>()));
  }

  // Bare invocation.
  if (launch_gui) {
    fs::path path = default_ini_path();
    return launch_gui(fs::exists(path) ? std::optional<fs::path>(path)
                                       : std::nullopt);
  }
  try {
    std::cout << render_template(data) << std::endl;
  } catch (const std::exception &e) {
    std::cerr << fg::red << "Cannot render the INI template: " << e.what()
              << fg::reset << std::endl;
    return 1;
  }
  return 0;
}

} // namespace Mads::Ini
