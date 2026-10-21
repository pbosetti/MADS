#include "prefs.hpp"

#include "detail/config_dir.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <utility>
#include <stdexcept>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace MadsIni {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr const char *kPrefsFileName = "mads-ini.json";

// Atomic write: a temp file next to the target (0600 from the start on POSIX),
// then rename over it.
bool write_private_file(const fs::path &file, const std::string &content, std::string *error) {
  auto fail = [&](const std::string &why) {
    if (error) {
      *error = why;
    }
    return false;
  };
  fs::path tmp = file;
  tmp += ".tmp";
  std::error_code ec;
  fs::remove(tmp, ec);
#ifndef _WIN32
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) {
    return fail("cannot create " + tmp.string() + ": " + std::strerror(errno));
  }
  const char *p = content.data();
  std::size_t left = content.size();
  while (left > 0) {
    const ssize_t n = ::write(fd, p, left);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      ::close(fd);
      fs::remove(tmp, ec);
      return fail("cannot write " + tmp.string());
    }
    p += n;
    left -= static_cast<std::size_t>(n);
  }
  ::fsync(fd);
  if (::close(fd) != 0) {
    fs::remove(tmp, ec);
    return fail("cannot write " + tmp.string());
  }
#else
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
    if (!out) {
      fs::remove(tmp, ec);
      return fail("cannot write " + tmp.string());
    }
  }
#endif
  fs::rename(tmp, file, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return fail("cannot replace " + file.string());
  }
  return true;
}

} // namespace

PrefsStore::PrefsStore() {
  try {
    _file = Mads::detail::config_dir() / kPrefsFileName;
  } catch (const std::exception &error) {
    raise(std::string("Preferences are not saved: ") + error.what());
    return;
  }
  load();
}

PrefsStore::PrefsStore(fs::path file) : _file(std::move(file)) {
  load();
}

void PrefsStore::load() {
  std::error_code ec;
  if (!fs::exists(_file, ec)) {
    return;
  }
  Prefs prefs;
  try {
    std::ifstream in(_file, std::ios::binary);
    if (!in) {
      throw std::runtime_error("cannot read the file");
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const json root = json::parse(text);
    if (!root.is_object()) {
      throw std::runtime_error("not a JSON object");
    }
    prefs.manual_address = root.value("manual_address", std::string());
    prefs.use_keys = root.value("use_keys", false);
    prefs.client_key_file = root.value("client_key_file", std::string());
    prefs.broker_key_file = root.value("broker_key_file", std::string());
    prefs.local_port = root.value("local_port", 0);
    if (prefs.local_port < 0 || prefs.local_port > 65535) {
      prefs.local_port = 0;
    }
    if (root.contains("recent_files") && root["recent_files"].is_array()) {
      for (const auto &item : root["recent_files"]) {
        if (item.is_string() && prefs.recent_files.size() < Prefs::kMaxRecent) {
          prefs.recent_files.push_back(item.get<std::string>());
        }
      }
    }
    if (root.contains("remembered_secrets") && root["remembered_secrets"].is_object()) {
      for (const auto &[endpoint, value] : root["remembered_secrets"].items()) {
        if (value.is_string()) {
          prefs.remembered_secrets[endpoint] = value.get<std::string>();
        }
      }
    }
  } catch (const std::exception &error) {
    raise("Ignoring preferences in " + _file.string() + ": " + error.what());
    return; // defaults
  }
  data = std::move(prefs);
}

bool PrefsStore::save() {
  if (_file.empty()) {
    return false;
  }
  json root;
  root["manual_address"] = data.manual_address;
  root["use_keys"] = data.use_keys;
  root["client_key_file"] = data.client_key_file;
  root["broker_key_file"] = data.broker_key_file;
  root["local_port"] = data.local_port;
  root["recent_files"] = data.recent_files;
  root["remembered_secrets"] = data.remembered_secrets;
  std::string error;
  if (!write_private_file(_file, root.dump(2) + "\n", &error)) {
    if (!_write_failure_reported) {
      _write_failure_reported = true;
      raise("Cannot save preferences: " + error);
    }
    return false;
  }
  return true;
}

void PrefsStore::add_recent(const fs::path &path) {
  const std::string entry = path.string();
  auto &recent = data.recent_files;
  recent.erase(std::remove(recent.begin(), recent.end(), entry), recent.end());
  recent.insert(recent.begin(), entry);
  if (recent.size() > Prefs::kMaxRecent) {
    recent.resize(Prefs::kMaxRecent);
  }
  save();
}

void PrefsStore::clear_recent() {
  data.recent_files.clear();
  save();
}

std::string PrefsStore::take_notice() {
  return std::exchange(_notice, std::string());
}

void PrefsStore::raise(const std::string &message) {
  if (_notice.empty()) {
    _notice = message;
  }
}

} // namespace MadsIni
