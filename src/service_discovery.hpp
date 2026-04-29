#ifndef SERVICE_DISCOVERY_HPP
#define SERVICE_DISCOVERY_HPP

#include "mads.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Mads {

using json = nlohmann::json;

class ServiceDiscovery {
public:
  struct ServiceInfo {
    std::string ip;
    std::map<std::string, uint16_t> ports;
    std::string room;
    std::string note;
    bool encrypted{false};
    bool prefer_loopback_for_local_services{true};

    json to_json() const;
    static ServiceInfo from_json(const json &payload);
  };

  static constexpr uint16_t DEFAULT_DISCOVERY_PORT = 39092;
  static constexpr std::chrono::milliseconds DEFAULT_ADVERTISE_INTERVAL{1000};

  explicit ServiceDiscovery(uint16_t discovery_port = DEFAULT_DISCOVERY_PORT);
  ~ServiceDiscovery();

  ServiceDiscovery(const ServiceDiscovery &) = delete;
  ServiceDiscovery &operator=(const ServiceDiscovery &) = delete;
  ServiceDiscovery(ServiceDiscovery &&) = delete;
  ServiceDiscovery &operator=(ServiceDiscovery &&) = delete;

  uint16_t discovery_port() const noexcept;
  bool is_advertising() const noexcept;

  void advertise_once(const ServiceInfo &service) const;
  void start_advertising(
      ServiceInfo service,
      std::chrono::milliseconds interval = DEFAULT_ADVERTISE_INTERVAL);
  void stop_advertising();

  ServiceInfo discover(const std::string &room = "",
                       std::chrono::milliseconds timeout =
                           std::chrono::milliseconds::zero()) const;

private:
  struct InterfaceAddress {
    std::string name;
    unsigned int index{0};
    std::string ip;
    std::string broadcast;
  };

  std::vector<InterfaceAddress> list_broadcast_interfaces() const;
  std::optional<ServiceInfo> try_discover(const std::string &room,
                                          std::chrono::milliseconds timeout,
                                          bool exact_room) const;
  void advertising_loop();

  uint16_t _discovery_port;
  mutable std::mutex _mutex;
  std::condition_variable _cv;
  std::thread _advertising_thread;
  ServiceInfo _service;
  std::chrono::milliseconds _interval{DEFAULT_ADVERTISE_INTERVAL};
  bool _advertising{false};
  bool _stop_requested{false};
  bool _room_reserved{false};
};

} // namespace Mads

#endif
