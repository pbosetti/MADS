#include "remote_client.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <stdexcept>
#include <utility>

namespace MadsIni {

namespace Remote = Mads::Ini::Remote;

namespace {

std::string trim(const std::string &text) {
  const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
  const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  return first < last ? std::string(first, last) : std::string();
}

// Adds ":port" to a "host" or "[v6]" lacking one.
std::string with_default_port(const std::string &host) {
  const auto colons = std::count(host.begin(), host.end(), ':');
  if (!host.empty() && host.front() == '[') {
    return host.find("]:") != std::string::npos ? host : host + ":" + std::to_string(Remote::kDefaultPort);
  }
  if (colons == 0) {
    return host + ":" + std::to_string(Remote::kDefaultPort);
  }
  if (colons == 1) {
    return host;
  }
  return "[" + host + "]:" + std::to_string(Remote::kDefaultPort); // bare IPv6
}

std::optional<Remote::ClientKeys> keys_of(const RemoteTarget &target) {
  if (!target.use_keys || is_ipc_endpoint(target.endpoint)) {
    return std::nullopt;
  }
  if (target.client_key_file.empty() || target.broker_key_file.empty()) {
    throw std::runtime_error("select the editor's secret key (.key) and the broker's public key (.pub)");
  }
  return Remote::load_client_key_files(target.client_key_file, target.broker_key_file);
}

} // namespace

bool is_ipc_endpoint(const std::string &endpoint) {
  return endpoint.rfind("ipc://", 0) == 0;
}

std::string normalize_address(const std::string &text) {
  const std::string value = trim(text);
  if (value.empty()) {
    return {};
  }
  if (value.rfind("ipc://", 0) == 0) {
    return value;
  }
  if (value.rfind("tcp://", 0) == 0) {
    const std::string host = value.substr(6);
    return host.empty() ? std::string() : "tcp://" + with_default_port(host);
  }
  return "tcp://" + with_default_port(value);
}

ConnectOutcome connect_remote(const RemoteTarget &target, std::optional<Remote::ClientSecret> remembered,
                              std::optional<std::string> password) {
  ConnectOutcome outcome;
  try {
    auto source = std::make_unique<Remote::RemoteSource>(target.endpoint, keys_of(target), std::move(password));
    if (remembered) {
      source->set_secret(std::move(*remembered));
    }
    source->load();
    outcome.secret = source->secret();
    outcome.source = std::move(source);
  } catch (const Remote::AuthError &error) {
    outcome.auth_required = true;
    outcome.error = error.what();
  } catch (const std::exception &error) {
    outcome.error = error.what();
  }
  return outcome;
}

// ----------------------------------------------------------- RemotePoller

RemotePoller::~RemotePoller() {
  stop();
}

void RemotePoller::start(RemoteTarget target, std::optional<Remote::ClientSecret> secret,
                         std::chrono::milliseconds interval, std::chrono::milliseconds timeout) {
  stop();
  _stop.store(false);
  {
    std::scoped_lock lock(_mutex);
    _sample.reset();
    ++_generation;
  }
  _thread = std::thread(&RemotePoller::loop, this, std::move(target), std::move(secret), interval, timeout);
}

void RemotePoller::stop() {
  _stop.store(true);
  if (_thread.joinable()) {
    _thread.join();
  }
}

void RemotePoller::invalidate() {
  std::scoped_lock lock(_mutex);
  _sample.reset();
  ++_generation;
}

std::optional<std::string> RemotePoller::sample() const {
  std::scoped_lock lock(_mutex);
  return _sample;
}

void RemotePoller::loop(RemoteTarget target, std::optional<Remote::ClientSecret> secret,
                        std::chrono::milliseconds interval, std::chrono::milliseconds timeout) {
  std::unique_ptr<Remote::RemoteSource> source;
  try {
    source = std::make_unique<Remote::RemoteSource>(target.endpoint, keys_of(target), std::nullopt, timeout);
    if (secret) {
      source->set_secret(std::move(*secret));
    }
  } catch (const std::exception &) {
    return; // bad keys and the like: the open already vetted them
  }
  constexpr auto kSlice = std::chrono::milliseconds{50};
  while (!_stop.load()) {
    for (auto slept = std::chrono::milliseconds{0}; slept < interval && !_stop.load(); slept += kSlice) {
      std::this_thread::sleep_for(kSlice);
    }
    if (_stop.load()) {
      break;
    }
    unsigned generation = 0;
    {
      std::scoped_lock lock(_mutex);
      generation = _generation;
    }
    try {
      std::string etag = source->current_etag();
      std::scoped_lock lock(_mutex);
      if (generation == _generation) {
        _sample = std::move(etag);
      }
    } catch (const std::exception &) {
      // Unreachable broker or auth trouble: no sample, try again later.
    }
  }
}

} // namespace MadsIni
