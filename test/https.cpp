#include <https_client.hpp>
#include <iostream>
#include <mads.hpp>
#include <nlohmann/json.hpp>
#include <rang.hpp>
#include <regex>
#include <sstream>

using namespace std;
using namespace rang;
using json = nlohmann::json;

void describe_release(const json &release) {
  std::string plat, arch;
#ifdef _WIN32
  plat = "Windows-";
#elif defined(__APPLE__)
  plat = "Darwin-";
#else
  plat = "Linux-";
#endif

#ifdef __x86_64__
  arch = "x86_64";
#elif defined(__aarch64__)
  arch = "arm64";
#elif defined(__arm__)
  arch = "arm";
#elif defined(_M_X64)
  arch = "x86_64";
#elif defined(_M_ARM64)
  arch = "aarch64";
#else
  arch = "unknown";
#endif
  if (release["prerelease"])
    cout << fg::yellow << "This is a pre-release!" << fg::reset << endl;
  cout << release.value("name", "no name") << " (tag " << style::bold
       << release.value("tag_name", "no tag") << style::reset << ") has "
       << release["assets"].size() << " assets:" << endl;
  for (json const a : release["assets"]) {
    if (regex_match(a.value("name", ""), regex(".*" + plat + arch + ".*")) ||
        regex_match(a.value("name", ""), regex(".*" + plat + "universal.*")))
      cout << fg::green << "=> ";
    else
      cout << " - ";
    cout << a.value("browser_download_url", "unnamed") << ", "
         << a.value("size", 0) / 1024 << " kbytes" << fg::reset << endl;
  }
  cout << "Release URL: " << release.value("html_url", "unknown") << endl;
}

int main(int argc, const char **argv) {
  try {
    Mads::HttpsClient client;
    client.set_hostname("api.github.com");
    if (argc > 1)
      client.set_path("/repos/pbosetti/mads/releases");
    else
      client.set_path("/repos/pbosetti/mads/releases/latest");
    client.add_query_pair("per_page", "1");
    client.set_user_agent("MADS" LIB_VERSION);

    auto response = client.get();

    json releases = json::parse(response.body);
    if (releases.is_array())
      describe_release(releases[0]);
    else
      describe_release(releases);

  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << "\n";
  }

  return 0;
}