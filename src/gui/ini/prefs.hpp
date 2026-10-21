// Per-user preferences of the mads-ini editor, kept as JSON in
// Mads::detail::config_dir()/mads-ini.json (owner-only).
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace MadsIni {

struct Prefs {
  static constexpr std::size_t kMaxRecent = 10;

  /// Last manually typed address of a broker.
  std::string manual_address;
  /// Last choice of the "Use CURVE keys" box for manual tcp addresses.
  bool use_keys = false;
  std::string client_key_file; ///< editor's secret key (NAME.key)
  std::string broker_key_file; ///< broker's public key
  int local_port = 0;          ///< 0: the default edit port
  /// Local files only, most recent first.
  std::vector<std::string> recent_files;
  /// endpoint -> ClientSecret::to_string(). Password-equivalent for that one
  /// broker: the file is owner-only.
  std::map<std::string, std::string> remembered_secrets;
};

/// Loads the preferences at construction and saves them on request. Without a
/// usable config directory it runs in memory only.
class PrefsStore {
public:
  /// Reads the default location (Mads::detail::config_dir()).
  PrefsStore();
  /// Explicit file, for tests.
  explicit PrefsStore(std::filesystem::path file);

  Prefs data;

  /// Writes the file now (temp + rename, owner-only). Failures are reported
  /// once through take_notice(); returns false when not persisted.
  bool save();
  bool persistent() const { return !_file.empty(); }

  /// Moves `path` to the front of the recent files (trimmed to kMaxRecent) and saves.
  void add_recent(const std::filesystem::path &path);
  void clear_recent();

  /// A problem to show the user once (bad file, no config directory, write
  /// failure); empty when there is none. Clears it.
  std::string take_notice();

private:
  void load();
  void raise(const std::string &message);

  std::filesystem::path _file;
  std::string _notice;
  bool _write_failure_reported = false;
};

} // namespace MadsIni
