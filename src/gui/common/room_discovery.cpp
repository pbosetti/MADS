#include "room_discovery.hpp"

#include "mads.hpp"

#include <cctype>
#include <chrono>
#include <exception>
#include <initializer_list>
#include <optional>
#include <string_view>

namespace MadsGui {

namespace {

std::string lower_copy(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char ch : value) {
    result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return result;
}

std::optional<int> find_port(const std::map<std::string, uint16_t> &ports,
                             std::initializer_list<std::string_view> names) {
  for (const std::string_view name : names) {
    const auto exact = ports.find(std::string(name));
    if (exact != ports.end()) {
      return static_cast<int>(exact->second);
    }
  }

  for (const auto &[key, port] : ports) {
    const std::string normalized_key = lower_copy(key);
    for (const std::string_view name : names) {
      if (normalized_key == lower_copy(name)) {
        return static_cast<int>(port);
      }
    }
  }

  return std::nullopt;
}

} // namespace

DiscoveredRoom make_discovered_room(const Mads::ServiceDiscovery::ServiceInfo &service) {
  DiscoveredRoom room;
  room.name = service.room.empty() ? "(unnamed)" : service.room;
  room.host = service.ip;
  room.encrypted = service.encrypted;
  room.hostname = service.hostname;
  room.version = service.version;
  room.ports = service.ports;

  if (const auto port = find_port(service.ports, {"frontend", "pub", "publish", "publisher"})) {
    room.pub_port = *port;
  }
  if (const auto port = find_port(service.ports, {"backend", "sub", "subscribe", "subscriber"})) {
    room.sub_port = *port;
  }
  if (const auto port = find_port(service.ports, {"edit"})) {
    room.edit_port = *port;
  }

  return room;
}

RoomDiscovery::~RoomDiscovery() {
  stop();
}

void RoomDiscovery::start() {
  if (_thread.joinable()) {
    return;
  }
  _stop_requested.store(false);
  _thread = std::thread(&RoomDiscovery::discovery_loop, this);
}

void RoomDiscovery::stop() {
  _stop_requested.store(true);
  if (_thread.joinable()) {
    _thread.join();
  }
}

std::vector<DiscoveredRoom> RoomDiscovery::snapshot_rooms() const {
  std::scoped_lock lock(_mutex);
  return _rooms;
}

std::string RoomDiscovery::last_error() const {
  std::scoped_lock lock(_mutex);
  return _last_error;
}

void RoomDiscovery::discovery_loop() {
  try {
    Mads::ServiceDiscovery discovery(MADS_SERVICE_PORT);
    while (!_stop_requested.load()) {
      try {
        std::vector<DiscoveredRoom> rooms;
        const auto services = discovery.list_rooms(std::chrono::milliseconds{1000});
        rooms.reserve(services.size());
        for (const auto &[name, service] : services) {
          DiscoveredRoom room = make_discovered_room(service);
          if (room.name.empty() || room.name == "(unnamed)") {
            room.name = name.empty() ? "(unnamed)" : name;
          }
          rooms.push_back(std::move(room));
        }

        {
          std::scoped_lock lock(_mutex);
          _rooms = std::move(rooms);
          _last_error.clear();
        }
      } catch (const std::exception &error) {
        set_error(error.what());
        sleep_in_slices(std::chrono::milliseconds{500});
      }
    }
  } catch (const std::exception &error) {
    set_error(error.what());
  }
}

void RoomDiscovery::sleep_in_slices(std::chrono::milliseconds duration) const {
  constexpr auto kSlice = std::chrono::milliseconds{50};
  auto slept = std::chrono::milliseconds{0};
  while (!_stop_requested.load() && slept < duration) {
    std::this_thread::sleep_for(kSlice);
    slept += kSlice;
  }
}

void RoomDiscovery::set_error(std::string message) {
  std::scoped_lock lock(_mutex);
  _last_error = std::move(message);
}

} // namespace MadsGui
