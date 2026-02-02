#include "https_client.hpp"
#include <stdexcept>

namespace Mads {

HttpsClient::HttpsClient()
    : _hostname(""), _path(""), _user_agent("") {}

HttpsClient::~HttpsClient() = default;

void HttpsClient::set_hostname(const std::string& hostname) {
    _hostname = hostname;
}

void HttpsClient::set_path(const std::string& path) {
    _path = path;
}

void HttpsClient::set_user_agent(const std::string& user_agent) {
    _user_agent = user_agent;
}

void HttpsClient::add_query_pair(const std::string& k, const std::string &v) {
  _query[k] = v;
}

std::string HttpsClient::query_string() {
  std::string s = "?";
  for (const auto &[k, v]: _query) {
    s += k + "=" + v;
  }
  return s;
}

HttpsClient::Response HttpsClient::get() {
    if (_hostname.empty()) {
        throw std::runtime_error("HttpsClient: hostname not set");
    }
    if (_path.empty()) {
        throw std::runtime_error("HttpsClient: path not set");
    }

#ifdef _WIN32
    return _get_windows();
#elif __APPLE__
    return _get_macos();
#else
    return _get_linux();
#endif
}

} // namespace Mads
