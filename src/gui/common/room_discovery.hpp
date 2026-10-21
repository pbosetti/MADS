// Background discovery of MADS rooms (brokers announcing themselves through
// Mads::ServiceDiscovery), shared by the GUI apps.
#pragma once

#include <service_discovery.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace MadsGui {

struct DiscoveredRoom {
  std::string name;
  std::string host;
  int pub_port = -1;
  int sub_port = -1;
  /// All the ports the broker announced, by name.
  std::map<std::string, uint16_t> ports;
  /// The announced "edit" port (settings editing), -1 when not advertised.
  int edit_port = -1;
  bool encrypted = false;
  std::string hostname;
  std::string version;
};

DiscoveredRoom make_discovered_room(const Mads::ServiceDiscovery::ServiceInfo &service);

/// Polls the network from a worker thread once started.
class RoomDiscovery {
public:
  RoomDiscovery() = default;
  ~RoomDiscovery();

  RoomDiscovery(const RoomDiscovery &) = delete;
  RoomDiscovery &operator=(const RoomDiscovery &) = delete;

  void start();
  void stop();

  std::vector<DiscoveredRoom> snapshot_rooms() const;
  std::string last_error() const;

private:
  void discovery_loop();
  void sleep_in_slices(std::chrono::milliseconds duration) const;
  void set_error(std::string message);

  std::thread _thread;
  mutable std::mutex _mutex;
  std::vector<DiscoveredRoom> _rooms;
  std::string _last_error;
  std::atomic<bool> _stop_requested{false};
};

} // namespace MadsGui
