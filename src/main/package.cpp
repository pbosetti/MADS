/*
  ___           _        _ _
 |_ _|_ __  ___| |_ __ _| | |
  | || '_ \/ __| __/ _` | | |
  | || | | \__ \ || (_| | | |
 |___|_| |_|___/\__\__,_|_|_|

 Install available packages from binaries.
 This mads subcommand fetches a JSON list of available packages from
 GitHub, then either presents a list of the packages, or installs a
 specified package from the latest release on GitHub.
*/

#include "../exec_path.hpp"
#include "../https_client.hpp"
#include "../mads.hpp"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cxxopts.hpp>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <rang.hpp>
#include <string>
#include <vector>

using namespace std;
using namespace cxxopts;
using namespace rang;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace Mads {

static const string PACKAGE_LIST_URL =
    "https://raw.githubusercontent.com/MADS-NET/.github/main/profile/packages.json";
static const string GITHUB_API_HOST = "api.github.com";
static constexpr int CACHE_SCHEMA_VERSION = 3;
static constexpr chrono::hours CACHE_MAX_AGE(6);
static bool GitHubTrafficLimited = false;

struct PackageEntry {
  string name;
  string uri;
  string type;
};

struct Platform {
  string os;
  string arch;
};

static bool string_ends_with(const string &value, const string &suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
             0;
}

static string lowercased(string value) {
  transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(tolower(c)); });
  return value;
}

static Platform current_platform() {
  Platform platform;

#ifdef _WIN32
  platform.os = "windows";
#elif defined(__APPLE__)
  platform.os = "darwin";
#else
  platform.os = "linux";
#endif

#if defined(_M_X64) || defined(_M_AMD64)
  platform.arch = platform.os == "windows" ? "amd64" : "x86_64";
#elif defined(__x86_64__)
  platform.arch = platform.os == "windows" ? "amd64" : "x86_64";
#elif defined(_M_ARM64)
  platform.arch = "arm64";
#elif defined(__aarch64__) && defined(__APPLE__)
  platform.arch = "arm64";
#elif defined(__aarch64__)
  platform.arch = "aarch64";
#elif defined(__arm__)
  platform.arch = "arm";
#else
  platform.arch = "unknown";
#endif

  return platform;
}

static string platform_requirement_name(const Platform &platform) {
  if (platform.os == "windows")
    return "Windows";
  if (platform.os == "darwin")
    return "macOS";
  if (platform.os == "linux")
    return "Linux";

  return platform.os;
}

static bool parse_https_url(const string &url, string &hostname, string &path) {
  static const string HTTPS_PREFIX = "https://";

  if (url.rfind(HTTPS_PREFIX, 0) != 0)
    return false;

  size_t host_start = HTTPS_PREFIX.size();
  size_t path_start = url.find('/', host_start);
  if (path_start == string::npos) {
    hostname = url.substr(host_start);
    path = "/";
  } else {
    hostname = url.substr(host_start, path_start - host_start);
    path = url.substr(path_start);
  }

  return !hostname.empty() && !path.empty();
}

static bool parse_github_repo_uri(const string &uri, string &owner,
                                  string &repo) {
  string hostname;
  string path;
  if (!parse_https_url(uri, hostname, path))
    return false;
  if (hostname != "github.com" && hostname != "www.github.com")
    return false;

  if (!path.empty() && path.front() == '/')
    path.erase(path.begin());
  if (!path.empty() && path.back() == '/')
    path.pop_back();

  size_t separator = path.find('/');
  if (separator == string::npos)
    return false;

  owner = path.substr(0, separator);
  size_t repo_end = path.find('/', separator + 1);
  repo = path.substr(separator + 1, repo_end - separator - 1);
  if (string_ends_with(repo, ".git"))
    repo.resize(repo.size() - 4);

  return !owner.empty() && !repo.empty();
}

static HttpsClient::Response get_https(const string &hostname,
                                       const string &path) {
  HttpsClient client;
  client.set_hostname(hostname);
  client.set_path(path);
  client.set_user_agent("MADS-Packager/" LIB_VERSION);
  return client.get();
}

static void note_http_response(const HttpsClient::Response &response) {
  if (response.status_code == 403)
    GitHubTrafficLimited = true;
}

static void print_github_limit_warning() {
  if (!GitHubTrafficLimited)
    return;

  cerr << fg::yellow
       << "Warning: GitHub may be limiting traffic; try again later."
       << fg::reset << endl;
}

static string http_error_message(const HttpsClient::Response &response);

static json get_json(const string &hostname, const string &path) {
  HttpsClient::Response response = get_https(hostname, path);
  note_http_response(response);
  if (response.status_code < 200 || response.status_code >= 300) {
    throw runtime_error("HTTP " + to_string(response.status_code) + " " +
                        response.status_message);
  }

  return json::parse(response.body);
}

static json get_json_from_url(const string &url) {
  string hostname;
  string path;
  if (!parse_https_url(url, hostname, path))
    throw runtime_error("Unsupported URL: " + url);

  return get_json(hostname, path);
}

static json get_json_from_file(const fs::path &path) {
  ifstream input(path);
  if (!input)
    throw runtime_error("Cannot open JSON file: " + path.string());

  return json::parse(input);
}

static json get_package_list_json(const string &package_list_source) {
  if (package_list_source.empty())
    return get_json_from_url(PACKAGE_LIST_URL);
  if (package_list_source.rfind("https://", 0) == 0)
    return get_json_from_url(package_list_source);
  if (package_list_source.find("://") != string::npos)
    throw runtime_error("Unsupported URL: " + package_list_source);

  return get_json_from_file(package_list_source);
}

static bool get_optional_json_from_url(const string &url, json &value) {
  string hostname;
  string path;
  if (!parse_https_url(url, hostname, path))
    throw runtime_error("Unsupported URL: " + url);

  HttpsClient::Response response = get_https(hostname, path);
  note_http_response(response);
  if (response.status_code == 404)
    return false;
  if (response.status_code < 200 || response.status_code >= 300)
    throw runtime_error(http_error_message(response));

  value = json::parse(response.body);
  return true;
}

static string json_string_value(const json &object, const string &name,
                                const string &fallback = "") {
  return object.contains(name) && object[name].is_string()
             ? object[name].get<string>()
             : fallback;
}

static string cache_component(const string &value) {
  string component;
  for (unsigned char c : value) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.')
      component.push_back(static_cast<char>(c));
    else
      component.push_back('_');
  }

  return component.empty() ? "package" : component;
}

static bool cache_directory(fs::path &directory) {
  const char *home = getenv("HOME");
#ifdef _WIN32
  if (home == nullptr || string(home).empty())
    home = getenv("USERPROFILE");
  string windows_home;
  if (home == nullptr || string(home).empty()) {
    const char *home_drive = getenv("HOMEDRIVE");
    const char *home_path = getenv("HOMEPATH");
    if (home_drive != nullptr && home_path != nullptr) {
      windows_home = string(home_drive) + string(home_path);
      home = windows_home.c_str();
    }
  }
#endif
  if (home == nullptr || string(home).empty())
    return false;

  directory = fs::path(home) / ".mads" / "packages";
  return true;
}

static bool cache_path(const string &filename, fs::path &path) {
  fs::path directory;
  if (!cache_directory(directory))
    return false;

  path = directory / filename;
  return true;
}

static bool cache_file_is_fresh(const fs::path &path) {
  error_code ec;
  if (!fs::exists(path, ec) || ec)
    return false;

  fs::file_time_type updated_at = fs::last_write_time(path, ec);
  if (ec)
    return false;

  return fs::file_time_type::clock::now() - updated_at <= CACHE_MAX_AGE;
}

static bool load_cache_file(const string &filename, const string &kind,
                            bool no_cache, json &cached) {
  if (no_cache)
    return false;

  fs::path path;
  if (!cache_path(filename, path) || !cache_file_is_fresh(path))
    return false;

  try {
    ifstream input(path);
    if (!input)
      return false;

    input >> cached;
    return cached.value("schema", 0) == CACHE_SCHEMA_VERSION &&
           cached.value("kind", "") == kind;
  } catch (const exception &) {
    return false;
  }
}

static void save_cache_file(const string &filename, const string &kind,
                            const json &data) {
  if (GitHubTrafficLimited)
    return;

  fs::path path;
  if (!cache_path(filename, path))
    return;

  try {
    fs::create_directories(path.parent_path());

    json cached = data;
    cached["schema"] = CACHE_SCHEMA_VERSION;
    cached["kind"] = kind;

    ofstream output(path);
    if (output)
      output << cached.dump(2) << endl;
  } catch (const exception &) {
  }
}

static string trim_copy(string value) {
  auto is_space = [](unsigned char c) { return isspace(c) != 0; };
  value.erase(value.begin(), find_if_not(value.begin(), value.end(), is_space));
  value.erase(find_if_not(value.rbegin(), value.rend(), is_space).base(),
              value.end());
  return value;
}

static string shell_quote(const string &value) {
#ifdef _WIN32
  string quoted = "\"";
  for (char c : value) {
    if (c == '"')
      quoted += "\\\"";
    else
      quoted += c;
  }
  quoted += "\"";
  return quoted;
#else
  string quoted = "'";
  for (char c : value) {
    if (c == '\'')
      quoted += "'\\''";
    else
      quoted += c;
  }
  quoted += "'";
  return quoted;
#endif
}

#ifdef _WIN32
static string powershell_quote(const string &value) {
  string quoted = "'";
  for (char c : value) {
    if (c == '\'')
      quoted += "''";
    else
      quoted += c;
  }
  quoted += "'";
  return quoted;
}
#endif

static bool run_command(const string &command) {
  return system(command.c_str()) == 0;
}

static fs::path make_temp_directory(const string &package_name) {
  fs::path base = fs::temp_directory_path();
  string stem = "mads-packager-" + cache_component(package_name) + "-";
  auto ticks = chrono::steady_clock::now().time_since_epoch().count();

  for (size_t i = 0; i < 100; ++i) {
    fs::path path = base / (stem + to_string(ticks) + "-" + to_string(i));
    error_code ec;
    if (fs::create_directory(path, ec))
      return path;
  }

  throw runtime_error("Could not create temporary install directory");
}

static void ensure_install_prefix_writable(const fs::path &prefix) {
  error_code ec;
  if (!fs::exists(prefix, ec) || ec) {
    throw runtime_error("Cannot install package: MADS prefix does not exist: " +
                        prefix.string());
  }

  if (!fs::is_directory(prefix, ec) || ec) {
    throw runtime_error("Cannot install package: MADS prefix is not a "
                        "directory: " +
                        prefix.string());
  }

  string probe_name =
      ".mads-package-write-test-" +
      to_string(chrono::steady_clock::now().time_since_epoch().count());
  fs::path probe_path = prefix / probe_name;
  {
    ofstream probe(probe_path, ios::binary);
    if (!probe) {
      throw runtime_error("Cannot install package: MADS prefix is not writable "
                          "by the current user: " +
                          prefix.string() +
                          ". Re-run with appropriate permissions or install "
                          "MADS in a user-writable prefix.");
    }

    probe << "test";
    if (!probe) {
      fs::remove(probe_path, ec);
      throw runtime_error("Cannot install package: failed to write inside "
                          "MADS prefix: " +
                          prefix.string());
    }
  }

  fs::remove(probe_path, ec);
  if (ec) {
    throw runtime_error("Cannot install package: MADS prefix is writable, but "
                        "temporary permission probe could not be removed: " +
                        probe_path.string() + " (" + ec.message() + ")");
  }
}

static void check_download_status(const string &status_code, bool command_ok) {
  if (status_code == "403") {
    GitHubTrafficLimited = true;
    throw runtime_error("HTTP 403 forbidden while downloading package asset");
  }
  if (status_code.size() >= 3 && status_code[0] == '2')
    return;
  if (!status_code.empty())
    throw runtime_error("HTTP " + status_code + " while downloading package asset");
  if (!command_ok)
    throw runtime_error("curl failed while downloading package asset");
  throw runtime_error("Could not determine package asset download status");
}

static bool curl_available() {
#ifdef _WIN32
  return run_command("where curl.exe >nul 2>&1");
#else
  return run_command("which curl >/dev/null 2>&1");
#endif
}

static void download_file_native(const string &url, const fs::path &output_path,
                                 const fs::path &status_path) {
  string hostname, path;
  if (!parse_https_url(url, hostname, path))
    throw runtime_error("Unsupported URL for native download: " + url);

  HttpsClient client;
  client.set_hostname(hostname);
  client.set_path(path);
  client.set_user_agent("MADS-Packager/" LIB_VERSION);
  HttpsClient::Response resp = client.get();

  {
    ofstream sf(status_path);
    if (sf) sf << resp.status_code;
  }

  if (resp.status_code >= 200 && resp.status_code < 300) {
    ofstream of(output_path, ios::binary);
    if (!of)
      throw runtime_error("Cannot write to " + output_path.string());
    of.write(resp.body.data(), static_cast<streamsize>(resp.body.size()));
  }
}

static void download_file(const string &url, const fs::path &output_path,
                          const fs::path &status_path) {
  if (curl_available()) {
#ifdef _WIN32
    string curl = "curl.exe";
#else
    string curl = "curl";
#endif
    string command = curl + " -L -sS -o " + shell_quote(output_path.string()) +
                     " -w " + shell_quote("%{http_code}") + " " +
                     shell_quote(url) + " > " + shell_quote(status_path.string());
    bool command_ok = run_command(command);
    ifstream status_file(status_path);
    string status_code;
    if (status_file)
      getline(status_file, status_code);
    check_download_status(trim_copy(status_code), command_ok);
    return;
  }

  // curl not available: use native HttpsClient
  download_file_native(url, output_path, status_path);
  ifstream status_file(status_path);
  string status_code;
  if (status_file)
    getline(status_file, status_code);
  check_download_status(trim_copy(status_code), true);
}

static void extract_zip_file(const fs::path &zip_path, const fs::path &prefix,
                             bool force) {
  fs::create_directories(prefix);

  // Extract into a temporary subdirectory first, so we can strip any
  // single top-level directory the archive may contain.
  fs::path extract_dir = zip_path.parent_path() / "extract";
  fs::create_directories(extract_dir);

#ifdef _WIN32
  string ps_command = "Expand-Archive -LiteralPath " +
                      powershell_quote(zip_path.string()) +
                      " -DestinationPath " + powershell_quote(extract_dir.string()) +
                      " -Force";
  string command = "powershell -NoProfile -ExecutionPolicy Bypass -Command " +
                   shell_quote(ps_command);
#else
  string command = string("unzip -oq ") +
                   shell_quote(zip_path.string()) + " -d " +
                   shell_quote(extract_dir.string());
#endif

  if (!run_command(command)) {
    error_code ec;
    fs::remove_all(extract_dir, ec);
    throw runtime_error("Failed to extract package archive");
  }

  // If the archive contains exactly one top-level directory, treat its
  // contents as the package root (strip one level of nesting).
  fs::path source_dir = extract_dir;
  {
    vector<fs::path> top_entries;
    for (auto &entry : fs::directory_iterator(extract_dir))
      top_entries.push_back(entry.path());
    if (top_entries.size() == 1 && fs::is_directory(top_entries[0]))
      source_dir = top_entries[0];
  }

  // Recursively merge source_dir into prefix, copying each file individually
  // so that existing directories are merged rather than skipped or replaced.
  error_code ec;
  for (auto &entry : fs::recursive_directory_iterator(source_dir)) {
    fs::path rel = fs::relative(entry.path(), source_dir, ec);
    if (ec)
      continue;
    fs::path dest = prefix / rel;
    if (fs::is_directory(entry.path())) {
      fs::create_directories(dest, ec);
      continue;
    }
    if (fs::exists(dest, ec) && !force)
      continue;
    fs::copy_file(entry.path(), dest,
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
      fs::remove_all(extract_dir, ec);
      throw runtime_error("Failed to install " + rel.string() + ": " +
                          ec.message());
    }
  }

  fs::remove_all(extract_dir, ec);
}

static string release_timestamp(const json &release) {
  string published_at = json_string_value(release, "published_at");
  if (!published_at.empty())
    return published_at;

  return json_string_value(release, "created_at");
}

static string http_error_message(const HttpsClient::Response &response) {
  string message = "HTTP " + to_string(response.status_code);
  if (!response.status_message.empty())
    message += " " + response.status_message;
  return message;
}

static json response_json(const HttpsClient::Response &response) {
  note_http_response(response);
  if (response.status_code < 200 || response.status_code >= 300)
    throw runtime_error(http_error_message(response));

  return json::parse(response.body);
}

static json get_latest_release(const string &owner, const string &repo,
                               bool &is_prerelease_fallback) {
  is_prerelease_fallback = false;

  string releases_path = "/repos/" + owner + "/" + repo + "/releases";
  HttpsClient::Response latest_response =
      get_https(GITHUB_API_HOST, releases_path + "/latest");
  if (latest_response.status_code != 404)
    return response_json(latest_response);

  json releases = get_json(GITHUB_API_HOST, releases_path);
  if (!releases.is_array())
    throw runtime_error("Release list response is not an array");

  json latest_prerelease;
  string latest_timestamp;
  bool found_prerelease = false;
  for (const json &release : releases) {
    if (!release.is_object() || !release.value("prerelease", false) ||
        release.value("draft", false))
      continue;

    string timestamp = release_timestamp(release);
    if (!found_prerelease || timestamp > latest_timestamp) {
      latest_prerelease = release;
      latest_timestamp = timestamp;
      found_prerelease = true;
    }
  }

  if (!found_prerelease)
    throw runtime_error(http_error_message(latest_response));

  is_prerelease_fallback = true;
  return latest_prerelease;
}

static vector<PackageEntry> read_package_entries(const json &package_list) {
  if (!package_list.contains("packages") || !package_list["packages"].is_object())
    throw runtime_error("Package list does not contain a packages object");

  vector<PackageEntry> entries;
  for (auto it = package_list["packages"].begin();
       it != package_list["packages"].end(); ++it) {
    if (!it.value().is_object())
      throw runtime_error("Package entry " + it.key() + " is not an object");

    string package_uri = json_string_value(it.value(), "URI");
    if (package_uri.empty())
      package_uri = json_string_value(it.value(), "uri");
    if (package_uri.empty())
      throw runtime_error("Package entry " + it.key() + " has no URI");

    entries.push_back(
        {it.key(), package_uri, json_string_value(it.value(), "type")});
  }

  return entries;
}

static bool is_zip_asset(const json &asset) {
  string asset_name = lowercased(json_string_value(asset, "name"));
  string content_type = lowercased(json_string_value(asset, "content_type"));

  return string_ends_with(asset_name, ".zip") ||
         content_type == "application/zip" ||
         content_type == "application/x-zip-compressed";
}

static bool is_compatible_zip_asset(const json &asset,
                                    const Platform &platform) {
  if (!is_zip_asset(asset))
    return false;

  string asset_name = lowercased(json_string_value(asset, "name"));
  string suffix = "-" + platform.os + "-" + platform.arch + ".zip";
  if (string_ends_with(asset_name, suffix))
    return true;

  return platform.os == "darwin" &&
         string_ends_with(asset_name, "-darwin-universal.zip");
}

static bool is_exact_platform_zip_asset(const json &asset,
                                        const Platform &platform) {
  if (!is_zip_asset(asset))
    return false;

  string asset_name = lowercased(json_string_value(asset, "name"));
  return string_ends_with(asset_name,
                          "-" + platform.os + "-" + platform.arch + ".zip");
}

static bool is_universal_macos_zip_asset(const json &asset) {
  if (!is_zip_asset(asset))
    return false;

  return string_ends_with(lowercased(json_string_value(asset, "name")),
                          "-darwin-universal.zip");
}

static size_t package_result_name_width(const json &packages) {
  size_t width = 0;
  if (!packages.is_array())
    return width;

  for (const json &package : packages)
    width = max(width, json_string_value(package, "name").size());
  return width;
}

static json fetch_package_result(const PackageEntry &entry) {
  json package;
  package["name"] = entry.name;
  package["uri"] = entry.uri;
  package["type"] = entry.type;

  string owner;
  string repo;
  if (!parse_github_repo_uri(entry.uri, owner, repo)) {
    package["error"] =
        "Cannot inspect releases: not a GitHub repository URI";
    return package;
  }

  try {
    bool is_prerelease_fallback = false;
    package["release"] = get_latest_release(owner, repo, is_prerelease_fallback);
    package["prerelease_fallback"] = is_prerelease_fallback;
  } catch (const exception &e) {
    package["error"] = e.what();
  }

  return package;
}

static json fetch_repository_metadata(const string &owner, const string &repo) {
  return get_json(GITHUB_API_HOST, "/repos/" + owner + "/" + repo);
}

static bool fetch_mads_package_file(const string &owner, const string &repo,
                                    const string &branch,
                                    json &mads_package) {
  string url = "https://raw.githubusercontent.com/" + owner + "/" + repo +
               "/" + branch + "/mads_package.json";
  return get_optional_json_from_url(url, mads_package);
}

static json fetch_list_packages_data(const string &package_list_source) {
  json package_list = get_package_list_json(package_list_source);
  vector<PackageEntry> entries = read_package_entries(package_list);

  json data;
  data["package_list"] = package_list;
  data["packages"] = json::array();
  for (const PackageEntry &entry : entries)
    data["packages"].push_back(fetch_package_result(entry));

  return data;
}

static json load_or_fetch_list_packages_data(bool no_cache,
                                             const string &package_list_source) {
  bool use_cache = package_list_source.empty();
  json data;
  if (use_cache &&
      load_cache_file("list_packages.json", "list_packages", no_cache, data))
    return data;

  data = fetch_list_packages_data(package_list_source);
  if (use_cache)
    save_cache_file("list_packages.json", "list_packages", data);
  return data;
}

static string package_info_cache_filename(const string &package_name) {
  return "info_" + cache_component(package_name) + ".json";
}

static json fetch_package_info_data(const string &package_name,
                                    const string &package_list_source) {
  json package_list = get_package_list_json(package_list_source);
  vector<PackageEntry> entries = read_package_entries(package_list);

  for (const PackageEntry &entry : entries) {
    if (entry.name != package_name)
      continue;

    json data;
    data["package_list"] = package_list;
    data["package"] = fetch_package_result(entry);

    string owner;
    string repo;
    if (!parse_github_repo_uri(entry.uri, owner, repo)) {
      data["repository_error"] = "Package URI is not a GitHub repository URI";
      data["mads_package_found"] = false;
      return data;
    }

    json repository = fetch_repository_metadata(owner, repo);
    data["repository"] = repository;

    string default_branch = json_string_value(repository, "default_branch");
    if (default_branch.empty())
      default_branch = "main";

    json mads_package;
    data["mads_package_found"] =
        fetch_mads_package_file(owner, repo, default_branch, mads_package);
    if (data["mads_package_found"].get<bool>())
      data["mads_package"] = mads_package;
    return data;
  }

  throw runtime_error("Unknown package: " + package_name);
}

static json load_or_fetch_package_info_data(const string &package_name,
                                            bool no_cache,
                                            const string &package_list_source,
                                            bool save_result = true) {
  bool use_cache = package_list_source.empty();
  string filename = package_info_cache_filename(package_name);
  json data;
  if (use_cache && load_cache_file(filename, "package_info", no_cache, data))
    return data;

  data = fetch_package_info_data(package_name, package_list_source);
  if (use_cache && save_result)
    save_cache_file(filename, "package_info", data);
  return data;
}

static void print_package_result(const json &package, const Platform &platform,
                                 size_t name_width) {
  cout << fg::cyan << style::bold << left << setw(name_width)
       << json_string_value(package, "name") << style::reset << fg::reset
       << "  " << json_string_value(package, "uri") << endl;
  string package_type = json_string_value(package, "type");
  if (!package_type.empty())
    cout << "  type: " << package_type << endl;

  string error = json_string_value(package, "error");
  if (!error.empty()) {
    cout << "  " << fg::yellow << "Cannot inspect latest release: " << error
         << fg::reset << endl
         << endl;
    return;
  }

  if (!package.contains("release") || !package["release"].is_object()) {
    cout << "  " << fg::yellow << "No release information available"
         << fg::reset << endl
         << endl;
    return;
  }

  const json &release = package["release"];
  cout << "  ";
  if (release.value("prerelease", false))
    cout << fg::yellow << "latest pre-release" << fg::reset;
  else
    cout << "latest release";
  cout << ": " << style::bold << release.value("tag_name", "unknown")
       << style::reset;
  string release_url = json_string_value(release, "html_url");
  if (!release_url.empty())
    cout << "\n  URL: " << release_url;
  cout << endl;

  if (!release.contains("assets") || !release["assets"].is_array()) {
    cout << "  " << fg::yellow << "No assets in latest release" << fg::reset
         << endl
         << endl;
    return;
  }

  size_t zip_count = 0;
  for (const json &asset : release["assets"]) {
    if (!asset.is_object() || !is_compatible_zip_asset(asset, platform))
      continue;

    zip_count++;
    cout << "  " << fg::green << json_string_value(asset, "name", "zip")
         << fg::reset;
    if (asset.contains("size") && asset["size"].is_number_unsigned())
      cout << style::italic << " (" << asset["size"].get<size_t>() / 1024
           << " KiB)" << style::reset;
    cout << endl
         << "    " << json_string_value(asset, "browser_download_url") << endl;
  }

  if (zip_count == 0)
    cout << "  " << fg::yellow
         << "No compatible ZIP installers found in latest release" << fg::reset
         << endl;
  cout << endl;
}

static void print_list_packages_data(const json &data, bool verbose) {
  if (!data.contains("package_list") || !data["package_list"].is_object())
    throw runtime_error("Cached list result has no package_list object");
  if (!data.contains("packages") || !data["packages"].is_array())
    throw runtime_error("Cached list result has no packages array");

  Platform platform = current_platform();
  size_t name_width = package_result_name_width(data["packages"]);

  cout << style::bold << "MADS installable packages" << style::reset << endl;
  string description = json_string_value(data["package_list"], "description");
  if (!description.empty() && description != "MADS installable packages")
    cout << style::italic << description << style::reset << endl;
  cout << "Compatible ZIPs for " << style::bold << platform.os << "-"
       << platform.arch << style::reset;
  if (platform.os == "darwin")
    cout << " (including universal)";
  cout << endl;
  cout << endl;

  if (verbose) {
    for (const json &package : data["packages"])
      print_package_result(package, platform, name_width);
  } else {
    for (const json &package : data["packages"]) {
      cout << fg::cyan << style::bold << left << setw(name_width)
           << json_string_value(package, "name") << style::reset << fg::reset
           << "  " << json_string_value(package, "uri") << endl;
      string error = json_string_value(package, "error");
      if (!error.empty()) {
        cout << "  " << fg::yellow << "Cannot inspect latest release: " << error
             << fg::reset << endl;
      }
    }
    cout << style::italic << "Run 'mads list --verbose' to see release details"
         << style::reset << endl
         << endl;
  }
  cout << right;
}

static void append_json_strings(vector<string> &values, const json &value) {
  if (value.is_string()) {
    values.push_back(value.get<string>());
    return;
  }

  if (!value.is_array())
    return;

  for (const json &item : value) {
    if (item.is_string())
      values.push_back(item.get<string>());
  }
}

static void append_requirement_values(const json &mads_package,
                                      const string &section_name,
                                      const vector<string> &field_names,
                                      vector<string> &values) {
  if (!mads_package.contains("requirements") ||
      !mads_package["requirements"].is_object())
    return;

  const json &requirements = mads_package["requirements"];
  if (!requirements.contains(section_name) ||
      !requirements[section_name].is_object())
    return;

  const json &section = requirements[section_name];
  for (const string &field_name : field_names) {
    if (section.contains(field_name))
      append_json_strings(values, section[field_name]);
  }
}

static vector<string> requirement_values(const json &mads_package,
                                         const Platform &platform,
                                         const vector<string> &field_names) {
  vector<string> values;
  append_requirement_values(mads_package, "Common", field_names, values);
  append_requirement_values(mads_package, platform_requirement_name(platform),
                            field_names, values);
  return values;
}

static void print_bulleted_section(const string &title,
                                   const vector<string> &values,
                                   const string &empty_message) {
  cout << style::bold << title << style::reset << endl;
  if (values.empty()) {
    cout << "  " << style::italic << empty_message << style::reset << endl
         << endl;
    return;
  }

  for (const string &value : values)
    cout << "  - " << value << endl;
  cout << endl;
}

static void print_about_section(const json &data) {
  cout << style::bold << "About" << style::reset << endl;

  if (data.contains("repository") && data["repository"].is_object()) {
    const json &repository = data["repository"];
    string description = json_string_value(repository, "description");
    string html_url = json_string_value(repository, "html_url");
    string homepage = json_string_value(repository, "homepage");

    if (!description.empty())
      cout << "  " << description << endl;
    else
      cout << "  " << style::italic << "No repository description available"
           << style::reset << endl;

    if (!html_url.empty())
      cout << "  Repository: " << html_url << endl;
    if (!homepage.empty())
      cout << "  Homepage: " << homepage << endl;
  } else {
    string repository_error = json_string_value(data, "repository_error");
    if (!repository_error.empty())
      cout << "  " << fg::yellow << repository_error << fg::reset << endl;
    else
      cout << "  " << style::italic << "Repository metadata unavailable"
           << style::reset << endl;
  }

  cout << endl;
}

static void print_package_info_data(const json &data) {
  if (!data.contains("package") || !data["package"].is_object())
    throw runtime_error("Cached package info has no package object");

  Platform platform = current_platform();
  const json &package = data["package"];

  cout << fg::cyan << style::bold << json_string_value(package, "name")
       << style::reset << fg::reset << "  "
       << json_string_value(package, "uri") << endl;
  string package_type = json_string_value(package, "type");
  if (!package_type.empty())
    cout << "Type: " << package_type << endl;
  cout << endl;

  print_about_section(data);

  json mads_package = json::object();
  bool has_mads_package =
      data.value("mads_package_found", false) &&
      data.contains("mads_package") && data["mads_package"].is_object();
  if (has_mads_package)
    mads_package = data["mads_package"];
  else
    cout << fg::yellow << "No mads_package.json information found"
         << fg::reset << endl
         << endl;

  vector<string> notes =
      requirement_values(mads_package, platform, {"note", "notes"});
  vector<string> commands =
      requirement_values(mads_package, platform, {"commands", "command"});

  print_bulleted_section("Notes", notes, "No notes available");
  print_bulleted_section("Suggested setup commands", commands, "No setup commands needed");
  cout << right;
}

static json find_install_asset(const json &release, const Platform &platform) {
  if (!release.contains("assets") || !release["assets"].is_array())
    throw runtime_error("Release has no assets");

  json universal_asset;
  bool found_universal = false;
  for (const json &asset : release["assets"]) {
    if (!asset.is_object())
      continue;

    if (is_exact_platform_zip_asset(asset, platform))
      return asset;

    if (platform.os == "darwin" && is_universal_macos_zip_asset(asset) &&
        !found_universal) {
      universal_asset = asset;
      found_universal = true;
    }
  }

  if (found_universal)
    return universal_asset;

  throw runtime_error("No compatible ZIP installer found in latest release");
}

bool list_packages(bool no_cache, bool verbose,
                   const string &package_list_source) {
  GitHubTrafficLimited = false;

  try {
    json data = load_or_fetch_list_packages_data(no_cache, package_list_source);
    print_list_packages_data(data, verbose);
    return true;
  } catch (const json::parse_error &e) {
    cerr << fg::red << "Error parsing package data: " << e.what() << fg::reset
         << endl;
  } catch (const exception &e) {
    cerr << fg::red << "Error: " << e.what() << fg::reset << endl;
  }

  return false;
}

bool print_package_info(const string &package_name, bool no_cache,
                        const string &package_list_source) {
  GitHubTrafficLimited = false;

  try {
    json data =
        load_or_fetch_package_info_data(package_name, no_cache,
                                        package_list_source);
    print_package_info_data(data);
    return true;
  } catch (const exception &e) {
    cerr << fg::red << "Error: " << e.what() << fg::reset << endl;
  }

  return false;
}

bool install_package(const string &package_name, bool force, bool no_cache,
                     const string &package_list_source) {
  GitHubTrafficLimited = false;

  fs::path temp_dir;
  try {
    json data = load_or_fetch_package_info_data(package_name, no_cache,
                                                package_list_source, false);
    if (!data.contains("package") || !data["package"].is_object())
      throw runtime_error("Package information is unavailable");

    const json &package = data["package"];
    string package_error = json_string_value(package, "error");
    if (!package_error.empty())
      throw runtime_error(package_error);

    if (!package.contains("release") || !package["release"].is_object())
      throw runtime_error("Package release information is unavailable");

    Platform platform = current_platform();
    json asset = find_install_asset(package["release"], platform);
    string asset_name = json_string_value(asset, "name", package_name + ".zip");
    string download_url = json_string_value(asset, "browser_download_url");
    if (download_url.empty())
      throw runtime_error("Selected ZIP asset has no download URL");

    fs::path prefix = fs::path(Mads::prefix());
    ensure_install_prefix_writable(prefix);

    temp_dir = make_temp_directory(package_name);
    fs::path asset_filename = fs::path(asset_name).filename();
    if (asset_filename.empty())
      asset_filename = package_name + ".zip";
    fs::path zip_path = temp_dir / asset_filename;
    fs::path status_path = temp_dir / "download.status";

    cout << "Installing " << style::bold << package_name << style::reset
         << endl;
    cout << "  Downloading " << fg::green << asset_name << fg::reset << endl;
    download_file(download_url, zip_path, status_path);

    cout << "  Extracting to " << style::bold << prefix.string()
         << style::reset << endl;
    extract_zip_file(zip_path, prefix, force);

    error_code cleanup_error;
    fs::remove_all(temp_dir, cleanup_error);

    cout << fg::green << "Installed " << package_name << " to "
         << prefix.string() << fg::reset << endl;
    return true;
  } catch (const exception &e) {
    if (!temp_dir.empty()) {
      error_code cleanup_error;
      fs::remove_all(temp_dir, cleanup_error);
    }
    cerr << fg::red << "Error: " << e.what() << fg::reset << endl;
  }

  return false;
}
} // namespace Mads

int main(int argc, char **argv) {
  Options options(argv[0]);
  // clang-format off
  options.add_options()
    ("l,list", "List available packages")
    ("v,verbose", "Show detailed information in list output")
    ("n,info", "Print information on a package installation", value<string>())
    ("i,install", "Install a package by name", value<string>())
    ("f,force", "Force overwrite of existing files")
    ("no-cache", "Fetch package data without reading cached results")
    ("h,help", "Print help");
  options.add_options("Undocumented")
    ("url", "Override package list URL", value<string>());
  // clang-format on

  ParseResult options_parsed;
  try {
    options_parsed = options.parse(argc, argv);
  } catch (const cxxopts::exceptions::exception &e) {
    cerr << fg::red << "Error parsing command line: " << e.what()
         << style::reset << endl;
    std::exit(EXIT_FAILURE);
  }

  if (options_parsed.count("list") + options_parsed.count("info") +
      options_parsed.count("install") > 1) {
    cerr << fg::red << "Error: only one of --list, --info, or --install can be used"
         << style::reset << endl;
    return -1;
  }

  if (options_parsed.arguments().empty()) {
    cerr << fg::red << "Error: no options provided" << style::reset << endl;
    cout << options.help({""}) << endl;
    return -1;
  }

  if (options_parsed.count("help")) {
    cout << options.help({""}) << endl;
    return 0;
  }

  if (options_parsed.count("force") && !options_parsed.count("install")) {
    cerr << fg::red << "Error: --force option only valid with --install"
         << style::reset << endl;
    return -1;
  }

  string package_list_source;
  if (options_parsed.count("url"))
    package_list_source = options_parsed["url"].as<string>();
  bool no_cache = options_parsed.count("no-cache") > 0 ||
                  !package_list_source.empty();

  if (options_parsed.count("list")) {
    bool verbose = options_parsed.count("verbose") > 0;
    bool listed = Mads::list_packages(no_cache, verbose, package_list_source);
    if (!listed) {
      cerr << fg::red << "Error listing packages" << style::reset << endl;
      Mads::print_github_limit_warning();
      return -1;
    }
    Mads::print_github_limit_warning();
    return 0;
  }

  if (options_parsed.count("info")) {
    string package_name = options_parsed["info"].as<string>();
    if (!Mads::print_package_info(package_name, no_cache,
                                  package_list_source)) {
      cerr << fg::red << "Error fetching package info: " << package_name
           << style::reset << endl;
      Mads::print_github_limit_warning();
      return -1;
    }
    Mads::print_github_limit_warning();
    return 0;
  }

  if (options_parsed.count("install")) {
    string package_name = options_parsed["install"].as<string>();
    if (!Mads::install_package(package_name, options_parsed.count("force") > 0,
                               no_cache, package_list_source)) {
      cerr << fg::red << "Error installing package: " << package_name
           << style::reset << endl;
      Mads::print_github_limit_warning();
      return -1;
    }
    Mads::print_github_limit_warning();
    return 0;
  }
}
