#include "service_discovery.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Mads {

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t invalid_socket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t invalid_socket = -1;
#endif

constexpr auto ROOM_ADVERTISEMENT_CHECK_TIMEOUT =
    ServiceDiscovery::DEFAULT_ADVERTISE_INTERVAL +
    std::chrono::milliseconds{100};

std::mutex &advertised_rooms_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::set<std::pair<uint16_t, std::string>> &advertised_rooms() {
  static std::set<std::pair<uint16_t, std::string>> rooms;
  return rooms;
}

std::string format_room_name(const std::string &room) {
  return room.empty() ? std::string("<empty>") : "`" + room + "`";
}

bool reserve_advertised_room(uint16_t discovery_port, const std::string &room) {
  std::scoped_lock lock(advertised_rooms_mutex());
  return advertised_rooms().emplace(discovery_port, room).second;
}

void release_advertised_room(uint16_t discovery_port, const std::string &room) {
  std::scoped_lock lock(advertised_rooms_mutex());
  advertised_rooms().erase({discovery_port, room});
}

void ensure_socket_runtime() {
#ifdef _WIN32
  static const int initialized = []() {
    WSADATA data{};
    const int rc = WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
      throw std::runtime_error("WSAStartup failed with error " +
                               std::to_string(rc));
    }
    return rc;
  }();
  UNUSED(initialized);
#endif
}

void close_socket(socket_t socket_fd) {
  if (socket_fd == invalid_socket) {
    return;
  }
#ifdef _WIN32
  closesocket(socket_fd);
#else
  close(socket_fd);
#endif
}

bool socket_would_block() {
#ifdef _WIN32
  const int error = WSAGetLastError();
  return error == WSAEWOULDBLOCK || error == WSAETIMEDOUT;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

std::string last_socket_error(const std::string &message) {
#ifdef _WIN32
  const DWORD error = WSAGetLastError();
  char *buffer = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                      FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD size = FormatMessageA(
      flags, nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
  std::string details = size > 0 && buffer != nullptr
                            ? std::string(buffer, size)
                            : "error " + std::to_string(error);
  if (buffer != nullptr) {
    LocalFree(buffer);
  }
  while (!details.empty() &&
         (details.back() == '\n' || details.back() == '\r' ||
          details.back() == ' ')) {
    details.pop_back();
  }
  return message + ": " + details;
#else
  return message + ": " + std::strerror(errno);
#endif
}

socket_t create_udp_socket() {
  ensure_socket_runtime();
  socket_t socket_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_fd == invalid_socket) {
    throw std::runtime_error(last_socket_error("Unable to create UDP socket"));
  }
  return socket_fd;
}

void enable_socket_broadcast(socket_t socket_fd) {
  int enabled = 1;
  if (setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST,
                 reinterpret_cast<const char *>(&enabled),
                 sizeof(enabled)) < 0) {
    throw std::runtime_error(
        last_socket_error("Unable to enable socket broadcast"));
  }
}

void enable_socket_reuse(socket_t socket_fd) {
  int enabled = 1;
  if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char *>(&enabled),
                 sizeof(enabled)) < 0) {
    throw std::runtime_error(
        last_socket_error("Unable to enable socket reuse"));
  }
}

void enable_socket_port_reuse(socket_t socket_fd) {
#if (defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) ||      \
     defined(__OpenBSD__)) &&                                                 \
    defined(SO_REUSEPORT)
  int enabled = 1;
  if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEPORT,
                 reinterpret_cast<const char *>(&enabled),
                 sizeof(enabled)) < 0) {
    throw std::runtime_error(
        last_socket_error("Unable to enable socket port reuse"));
  }
#else
  UNUSED(socket_fd);
#endif
}

template<typename InterfaceAddressT>
void bind_socket_to_interface(socket_t socket_fd,
                              const InterfaceAddressT &iface) {
#ifdef _WIN32
  if (iface.index == 0U) {
    return;
  }
  DWORD index = htonl(static_cast<DWORD>(iface.index));
  if (setsockopt(socket_fd, IPPROTO_IP, IP_UNICAST_IF,
                 reinterpret_cast<const char *>(&index), sizeof(index)) < 0) {
    throw std::runtime_error(
        last_socket_error("Unable to bind UDP sender socket to interface"));
  }
#elif defined(__APPLE__) && defined(IP_BOUND_IF)
  if (iface.index == 0U) {
    return;
  }
  const unsigned int index = iface.index;
  if (setsockopt(socket_fd, IPPROTO_IP, IP_BOUND_IF,
                 reinterpret_cast<const char *>(&index), sizeof(index)) < 0) {
    throw std::runtime_error(
        last_socket_error("Unable to bind UDP sender socket to interface"));
  }
#elif defined(SO_BINDTODEVICE)
  if (iface.name.empty()) {
    return;
  }
  if (setsockopt(socket_fd, SOL_SOCKET, SO_BINDTODEVICE, iface.name.c_str(),
                 static_cast<socklen_t>(iface.name.size() + 1)) < 0) {
    if (errno != EPERM && errno != EACCES) {
      throw std::runtime_error(
          last_socket_error("Unable to bind UDP sender socket to interface"));
    }
  }
#else
  UNUSED(socket_fd);
  UNUSED(iface);
#endif
}

sockaddr_in make_ipv4_address(const std::string &ip, uint16_t port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
    throw std::runtime_error("Invalid IPv4 address: " + ip);
  }
  return address;
}

template<typename InterfaceAddressT>
socket_t create_bound_sender_socket(const InterfaceAddressT &iface) {
  socket_t socket_fd = create_udp_socket();
  try {
    enable_socket_broadcast(socket_fd);
    bind_socket_to_interface(socket_fd, iface);
    auto address = make_ipv4_address(iface.ip, 0);
    if (bind(socket_fd, reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) < 0) {
      throw std::runtime_error(
          last_socket_error("Unable to bind UDP sender socket"));
    }
    return socket_fd;
  } catch (...) {
    close_socket(socket_fd);
    throw;
  }
}

void send_payload_to(socket_t socket_fd, const std::string &target,
                     uint16_t port, const std::string &payload) {
  const auto address = make_ipv4_address(target, port);
  const auto size = static_cast<int>(payload.size());
  const int sent =
      sendto(socket_fd, payload.data(), size, 0,
             reinterpret_cast<const sockaddr *>(&address), sizeof(address));
  if (sent != size) {
    throw std::runtime_error(last_socket_error("Unable to send UDP broadcast"));
  }
}

json ports_to_json(const std::map<std::string, uint16_t> &ports) {
  json ports_json = json::object();
  for (const auto &[service, port] : ports) {
    ports_json[service] = port;
  }
  return ports_json;
}

std::map<std::string, uint16_t> parse_ports(const json &ports_json) {
  if (!ports_json.is_object()) {
    throw std::runtime_error(
        "Service advertisement `ports` must be a JSON object");
  }

  std::map<std::string, uint16_t> ports;
  for (const auto &[service, value] : ports_json.items()) {
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
      throw std::runtime_error("Service advertisement port for `" + service +
                               "` must be an integer");
    }
    const auto port = value.get<long long>();
    if (port < 0 || port > std::numeric_limits<uint16_t>::max()) {
      throw std::runtime_error("Service advertisement port for `" + service +
                               "` is out of range");
    }
    ports[service] = static_cast<uint16_t>(port);
  }
  return ports;
}

std::string local_hostname() {
  ensure_socket_runtime();
  std::array<char, 256> hostname{};
  if (gethostname(hostname.data(), static_cast<int>(hostname.size())) != 0) {
    return "unknown";
  }
  hostname.back() = '\0';
  return hostname.data()[0] != '\0' ? hostname.data() : "unknown";
}

void populate_service_hostname(ServiceDiscovery::ServiceInfo &service) {
  if (service.hostname.empty()) {
    service.hostname = local_hostname();
  }
}

ServiceDiscovery::ServiceInfo
service_with_hostname(ServiceDiscovery::ServiceInfo service) {
  populate_service_hostname(service);
  return service;
}

std::string serialize_service(const ServiceDiscovery::ServiceInfo &service) {
  return service.to_json().dump();
}

bool is_loopback_ipv4(const std::string &ip) {
  in_addr address{};
  if (inet_pton(AF_INET, ip.c_str(), &address) != 1) {
    return false;
  }

  const uint32_t host_address = ntohl(address.s_addr);
  return (host_address & 0xFF000000u) == 0x7F000000u;
}

uint32_t prefix_to_netmask(unsigned int prefix) {
  if (prefix == 0U) {
    return 0U;
  }
  if (prefix >= 32U) {
    return 0xFFFFFFFFu;
  }
  return 0xFFFFFFFFu << (32U - prefix);
}

#ifdef _WIN32
std::string inet_ntop_string(const IN_ADDR &address) {
  std::array<char, INET_ADDRSTRLEN> buffer{};
  if (inet_ntop(AF_INET, &address, buffer.data(),
                static_cast<DWORD>(buffer.size())) == nullptr) {
    throw std::runtime_error(
        last_socket_error("Unable to format IPv4 address"));
  }
  return buffer.data();
}
#else
std::string inet_ntop_string(const in_addr &address) {
  std::array<char, INET_ADDRSTRLEN> buffer{};
  if (inet_ntop(AF_INET, &address, buffer.data(), buffer.size()) == nullptr) {
    throw std::runtime_error(
        last_socket_error("Unable to format IPv4 address"));
  }
  return buffer.data();
}
#endif

bool is_local_ipv4_address(const std::string &ip) {
  if (is_loopback_ipv4(ip)) {
    return true;
  }

#ifdef _WIN32
  ensure_socket_runtime();
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                GAA_FLAG_SKIP_DNS_SERVER;
  ULONG family = AF_INET;
  ULONG buffer_size = 16 * 1024;
  std::vector<unsigned char> buffer(buffer_size);
  auto *addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());

  ULONG result =
      GetAdaptersAddresses(family, flags, nullptr, addresses, &buffer_size);
  if (result == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(buffer_size);
    addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
    result =
        GetAdaptersAddresses(family, flags, nullptr, addresses, &buffer_size);
  }
  if (result != NO_ERROR) {
    return false;
  }

  for (auto *adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp) {
      continue;
    }
    for (auto *unicast = adapter->FirstUnicastAddress; unicast != nullptr;
         unicast = unicast->Next) {
      if (unicast->Address.lpSockaddr == nullptr ||
          unicast->Address.lpSockaddr->sa_family != AF_INET) {
        continue;
      }
      auto *sockaddr_ptr =
          reinterpret_cast<sockaddr_in *>(unicast->Address.lpSockaddr);
      if (ip == inet_ntop_string(sockaddr_ptr->sin_addr)) {
        return true;
      }
    }
  }
#else
  ifaddrs *raw_interfaces = nullptr;
  if (getifaddrs(&raw_interfaces) != 0) {
    return false;
  }
  const auto free_interfaces = [](ifaddrs *interfaces) {
    if (interfaces != nullptr) {
      freeifaddrs(interfaces);
    }
  };
  std::unique_ptr<ifaddrs, decltype(free_interfaces)> interfaces_guard(
      raw_interfaces, free_interfaces);

  for (ifaddrs *entry = raw_interfaces; entry != nullptr;
       entry = entry->ifa_next) {
    if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) {
      continue;
    }

    auto *ip_addr = reinterpret_cast<sockaddr_in *>(entry->ifa_addr);
    if (ip == inet_ntop_string(ip_addr->sin_addr)) {
      return true;
    }
  }
#endif

  return false;
}

} // namespace

json ServiceDiscovery::ServiceInfo::to_json() const {
  auto result = json{{"ip", ip},
              {"ports", ports_to_json(ports)},
              {"room", room},
              {"encrypted", encrypted},
              {"prefer_loopback_for_local_services",
               prefer_loopback_for_local_services}};
  if (!hostname.empty()) {
    result["hostname"] = hostname;
  }
  return result;
}

ServiceDiscovery::ServiceInfo
ServiceDiscovery::ServiceInfo::from_json(const json &payload) {
  if (!payload.is_object()) {
    throw std::runtime_error("Service advertisement must be a JSON object");
  }
  if (!payload.contains("ip") || !payload["ip"].is_string()) {
    throw std::runtime_error(
        "Service advertisement is missing string field `ip`");
  }
  if (!payload.contains("ports")) {
    throw std::runtime_error("Service advertisement is missing field `ports`");
  }
  if (!payload.contains("room") || !payload["room"].is_string()) {
    throw std::runtime_error(
        "Service advertisement is missing string field `room`");
  }

  ServiceInfo service;
  service.ip = payload["ip"].get<std::string>();
  service.ports = parse_ports(payload["ports"]);
  service.room = payload["room"].get<std::string>();
  service.hostname = payload.value("hostname", "");
  service.encrypted = payload.value("encrypted", false);
  service.prefer_loopback_for_local_services =
      payload.value("prefer_loopback_for_local_services", true);
  return service;
}

ServiceDiscovery::ServiceDiscovery(uint16_t discovery_port)
    : _discovery_port(discovery_port) {}

ServiceDiscovery::~ServiceDiscovery() { stop_advertising(); }

uint16_t ServiceDiscovery::discovery_port() const noexcept {
  return _discovery_port;
}

bool ServiceDiscovery::is_advertising() const noexcept {
  std::scoped_lock lock(_mutex);
  return _advertising;
}

void ServiceDiscovery::advertise_once(const ServiceInfo &service) const {
  const auto interfaces = list_broadcast_interfaces();
  if (interfaces.empty()) {
    throw std::runtime_error("No broadcast-capable IPv4 interfaces found");
  }

  const auto advertised_service = service_with_hostname(service);
  std::optional<std::string> last_error;
  std::size_t sent_count = 0;
  for (const auto &iface : interfaces) {
    ServiceInfo iface_service = advertised_service;
    iface_service.ip = iface.ip;
    try {
      socket_t socket_fd = create_bound_sender_socket(iface);
      try {
        send_payload_to(socket_fd, iface.broadcast, _discovery_port,
                        serialize_service(iface_service));
      } catch (...) {
        close_socket(socket_fd);
        throw;
      }
      close_socket(socket_fd);
      ++sent_count;
    } catch (const std::exception &e) {
      last_error = e.what();
    }
  }

  if (sent_count == 0) {
    throw std::runtime_error(last_error.has_value()
                                 ? *last_error
                                 : "Unable to broadcast service advertisement");
  }
}

void ServiceDiscovery::start_advertising(ServiceInfo &service,
                                         std::chrono::milliseconds interval) {
  if (interval <= std::chrono::milliseconds::zero()) {
    throw std::runtime_error("Advertising interval must be positive");
  }

  populate_service_hostname(service);
  const auto room = service.room;
  {
    std::scoped_lock lock(_mutex);
    if (_advertising && _service.room == room) {
      throw std::runtime_error("Room " + format_room_name(room) +
                               " is already advertised");
    }
  }

  stop_advertising();
  if (const auto existing_service =
          try_discover(room, ROOM_ADVERTISEMENT_CHECK_TIMEOUT, true)) {
    throw std::runtime_error("Room " + format_room_name(room) +
                             " is already advertised by " +
                             existing_service->ip);
  }
  if (!reserve_advertised_room(_discovery_port, room)) {
    throw std::runtime_error("Room " + format_room_name(room) +
                             " is already advertised");
  }

  ServiceInfo advertised_service;
  {
    std::scoped_lock lock(_mutex);
    _service = service;
    _interval = interval;
    _stop_requested = false;
    _advertising = true;
    _room_reserved = true;
    advertised_service = _service;
  }
  try {
    advertise_once(advertised_service);
  } catch (...) {
    std::scoped_lock lock(_mutex);
    _advertising = false;
    _stop_requested = false;
    _room_reserved = false;
    release_advertised_room(_discovery_port, room);
    throw;
  }
  try {
    _advertising_thread = std::thread([this]() { advertising_loop(); });
  } catch (...) {
    std::scoped_lock lock(_mutex);
    _advertising = false;
    _stop_requested = false;
    _room_reserved = false;
    release_advertised_room(_discovery_port, room);
    throw;
  }
}

void ServiceDiscovery::stop_advertising() {
  std::thread thread_to_join;
  std::optional<std::string> room_to_release;
  {
    std::scoped_lock lock(_mutex);
    if (!_advertising_thread.joinable()) {
      _advertising = false;
      _stop_requested = false;
      if (_room_reserved) {
        room_to_release = _service.room;
        _room_reserved = false;
      }
    } else {
      _stop_requested = true;
      _advertising = false;
      thread_to_join = std::move(_advertising_thread);
    }
  }
  if (!thread_to_join.joinable()) {
    if (room_to_release.has_value()) {
      release_advertised_room(_discovery_port, *room_to_release);
    }
    return;
  }
  _cv.notify_all();
  thread_to_join.join();
  {
    std::scoped_lock lock(_mutex);
    _stop_requested = false;
    if (_room_reserved) {
      room_to_release = _service.room;
      _room_reserved = false;
    }
  }
  if (room_to_release.has_value()) {
    release_advertised_room(_discovery_port, *room_to_release);
  }
}

std::optional<ServiceDiscovery::ServiceInfo>
ServiceDiscovery::try_discover(const std::string &room,
                               std::chrono::milliseconds timeout,
                               bool exact_room) const {
  socket_t socket_fd = create_udp_socket();
  try {
    enable_socket_reuse(socket_fd);
    enable_socket_port_reuse(socket_fd);

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(_discovery_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_fd, reinterpret_cast<const sockaddr *>(&local),
             sizeof(local)) < 0) {
      throw std::runtime_error(
          last_socket_error("Unable to bind discovery socket"));
    }

    const auto deadline = timeout <= std::chrono::milliseconds::zero()
                              ? std::chrono::steady_clock::time_point::max()
                              : std::chrono::steady_clock::now() + timeout;

    while (true) {
      fd_set read_fds;
      FD_ZERO(&read_fds);
      FD_SET(socket_fd, &read_fds);

      timeval tv{};
      timeval *tv_ptr = nullptr;
      if (deadline != std::chrono::steady_clock::time_point::max()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          close_socket(socket_fd);
          return std::nullopt;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::microseconds>(deadline -
                                                                  now);
        tv.tv_sec = static_cast<long>(remaining.count() / 1000000);
        tv.tv_usec = static_cast<long>(remaining.count() % 1000000);
        tv_ptr = &tv;
      }

      const int ready = select(
#ifdef _WIN32
          0,
#else
          socket_fd + 1,
#endif
          &read_fds, nullptr, nullptr, tv_ptr);

      if (ready == 0) {
        close_socket(socket_fd);
        return std::nullopt;
      }
      if (ready < 0) {
        throw std::runtime_error(
            last_socket_error("Unable to wait for discovery message"));
      }

      std::array<char, 4096> buffer{};
      sockaddr_in remote{};
#ifdef _WIN32
      int remote_size = sizeof(remote);
#else
      socklen_t remote_size = sizeof(remote);
#endif
      const int received = recvfrom(
          socket_fd, buffer.data(), static_cast<int>(buffer.size() - 1), 0,
          reinterpret_cast<sockaddr *>(&remote), &remote_size);
      if (received < 0) {
        if (socket_would_block()) {
          continue;
        }
        throw std::runtime_error(
            last_socket_error("Unable to receive discovery message"));
      }

      buffer[static_cast<std::size_t>(received)] = '\0';
      ServiceInfo discovered_service;
      try {
        discovered_service = ServiceInfo::from_json(json::parse(buffer.data()));
      } catch (...) {
        continue;
      }
      const auto remote_ip = inet_ntop_string(remote.sin_addr);
      if (discovered_service.ip != remote_ip) {
        continue;
      }
      discovered_service.ip =
          discovered_service.prefer_loopback_for_local_services &&
                  is_local_ipv4_address(remote_ip)
              ? "127.0.0.1"
              : remote_ip;
      const bool room_matches =
          exact_room ? discovered_service.room == room
                     : room.empty() || discovered_service.room == room;
      if (room_matches) {
        close_socket(socket_fd);
        return discovered_service;
      }
    }
  } catch (...) {
    close_socket(socket_fd);
    throw;
  }
}

ServiceDiscovery::ServiceInfo
ServiceDiscovery::discover(const std::string &room,
                           std::chrono::milliseconds timeout) const {
  const auto service = try_discover(room, timeout, false);
  if (!service.has_value()) {
    throw std::runtime_error("Timed out waiting for service advertisement");
  }
  return *service;
}

std::map<std::string, ServiceDiscovery::ServiceInfo>
ServiceDiscovery::list_rooms(std::chrono::milliseconds timeout) const {
  std::map<std::string, ServiceDiscovery::ServiceInfo> rooms;
  if (timeout <= std::chrono::milliseconds::zero()) {
    return rooms;
  }

  socket_t socket_fd = create_udp_socket();
  try {
    enable_socket_reuse(socket_fd);
    enable_socket_port_reuse(socket_fd);

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(_discovery_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_fd, reinterpret_cast<const sockaddr *>(&local),
             sizeof(local)) < 0) {
      throw std::runtime_error(
          last_socket_error("Unable to bind discovery socket"));
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        close_socket(socket_fd);
        return rooms;
      }

      fd_set read_fds;
      FD_ZERO(&read_fds);
      FD_SET(socket_fd, &read_fds);

      const auto remaining =
          std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
      timeval tv{};
      tv.tv_sec = static_cast<long>(remaining.count() / 1000000);
      tv.tv_usec = static_cast<long>(remaining.count() % 1000000);

      const int ready = select(
#ifdef _WIN32
          0,
#else
          socket_fd + 1,
#endif
          &read_fds, nullptr, nullptr, &tv);

      if (ready == 0) {
        close_socket(socket_fd);
        return rooms;
      }
      if (ready < 0) {
        throw std::runtime_error(
            last_socket_error("Unable to wait for discovery message"));
      }

      std::array<char, 4096> buffer{};
      sockaddr_in remote{};
#ifdef _WIN32
      int remote_size = sizeof(remote);
#else
      socklen_t remote_size = sizeof(remote);
#endif
      const int received = recvfrom(
          socket_fd, buffer.data(), static_cast<int>(buffer.size() - 1), 0,
          reinterpret_cast<sockaddr *>(&remote), &remote_size);
      if (received < 0) {
        if (socket_would_block()) {
          continue;
        }
        throw std::runtime_error(
            last_socket_error("Unable to receive discovery message"));
      }

      buffer[static_cast<std::size_t>(received)] = '\0';
      ServiceInfo discovered_service;
      try {
        discovered_service = ServiceInfo::from_json(json::parse(buffer.data()));
      } catch (...) {
        continue;
      }

      const auto remote_ip = inet_ntop_string(remote.sin_addr);
      if (discovered_service.ip != remote_ip) {
        continue;
      }
      discovered_service.ip =
          discovered_service.prefer_loopback_for_local_services &&
                  is_local_ipv4_address(remote_ip)
              ? "127.0.0.1"
              : remote_ip;

      rooms[discovered_service.room] = discovered_service;
    }
  } catch (...) {
    close_socket(socket_fd);
    throw;
  }
  return rooms;
}

std::vector<ServiceDiscovery::InterfaceAddress>
ServiceDiscovery::list_broadcast_interfaces() const {
  std::vector<InterfaceAddress> interfaces;
  std::set<std::pair<std::string, std::string>> unique_interfaces;

#ifdef _WIN32
  ensure_socket_runtime();
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                GAA_FLAG_SKIP_DNS_SERVER;
  ULONG family = AF_INET;
  ULONG buffer_size = 16 * 1024;
  std::vector<unsigned char> buffer(buffer_size);
  auto *addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());

  ULONG result =
      GetAdaptersAddresses(family, flags, nullptr, addresses, &buffer_size);
  if (result == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(buffer_size);
    addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
    result =
        GetAdaptersAddresses(family, flags, nullptr, addresses, &buffer_size);
  }
  if (result != NO_ERROR) {
    throw std::runtime_error("GetAdaptersAddresses failed with error " +
                             std::to_string(result));
  }

  for (auto *adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp ||
        adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
      continue;
    }
    for (auto *unicast = adapter->FirstUnicastAddress; unicast != nullptr;
         unicast = unicast->Next) {
      if (unicast->Address.lpSockaddr == nullptr ||
          unicast->Address.lpSockaddr->sa_family != AF_INET) {
        continue;
      }

      auto *sockaddr_ptr =
          reinterpret_cast<sockaddr_in *>(unicast->Address.lpSockaddr);
      const auto prefix =
          static_cast<unsigned int>(unicast->OnLinkPrefixLength);
      if (prefix > 32U) {
        continue;
      }

      const uint32_t ip_host = ntohl(sockaddr_ptr->sin_addr.S_un.S_addr);
      const uint32_t mask_host = prefix_to_netmask(prefix);
      IN_ADDR broadcast_addr{};
      broadcast_addr.S_un.S_addr = htonl(ip_host | ~mask_host);

      const auto ip = inet_ntop_string(sockaddr_ptr->sin_addr);
      const auto broadcast = inet_ntop_string(broadcast_addr);
      if (unique_interfaces.emplace(ip, broadcast).second) {
        interfaces.push_back({adapter->AdapterName != nullptr
                                  ? std::string(adapter->AdapterName)
                                  : std::string(),
                              static_cast<unsigned int>(adapter->IfIndex), ip,
                              broadcast});
      }
    }
  }
#else
  ifaddrs *raw_interfaces = nullptr;
  if (getifaddrs(&raw_interfaces) != 0) {
    throw std::runtime_error(last_socket_error("getifaddrs failed"));
  }
  const auto free_interfaces = [](ifaddrs *interfaces) {
    if (interfaces != nullptr) {
      freeifaddrs(interfaces);
    }
  };
  std::unique_ptr<ifaddrs, decltype(free_interfaces)> interfaces_guard(
      raw_interfaces, free_interfaces);

  for (ifaddrs *entry = raw_interfaces; entry != nullptr;
       entry = entry->ifa_next) {
    if (entry->ifa_addr == nullptr || entry->ifa_broadaddr == nullptr) {
      continue;
    }
    if (entry->ifa_addr->sa_family != AF_INET ||
        entry->ifa_broadaddr->sa_family != AF_INET) {
      continue;
    }
    if ((entry->ifa_flags & IFF_UP) == 0 ||
        (entry->ifa_flags & IFF_BROADCAST) == 0 ||
        (entry->ifa_flags & IFF_LOOPBACK) != 0) {
      continue;
    }

    auto *ip_addr = reinterpret_cast<sockaddr_in *>(entry->ifa_addr);
    auto *broadcast_addr =
        reinterpret_cast<sockaddr_in *>(entry->ifa_broadaddr);
    const std::string name = entry->ifa_name != nullptr ? entry->ifa_name : "";
    const auto ip = inet_ntop_string(ip_addr->sin_addr);
    const auto broadcast = inet_ntop_string(broadcast_addr->sin_addr);
    if (unique_interfaces.emplace(ip, broadcast).second) {
      interfaces.push_back({name, if_nametoindex(name.c_str()), ip, broadcast});
    }
  }

#endif

  return interfaces;
}

void ServiceDiscovery::advertising_loop() {
  while (true) {
    ServiceInfo service;
    std::chrono::milliseconds interval{0};
    {
      std::scoped_lock lock(_mutex);
      if (_stop_requested || !Mads::running) {
        _advertising = false;
        break;
      }
      service = _service;
      interval = _interval;
    }

    try {
      advertise_once(service);
    } catch (const std::exception &e) {
      std::cerr << "ServiceDiscovery advertising failed: " << e.what()
                << std::endl;
    }

    std::unique_lock lock(_mutex);
    _cv.wait_for(lock, interval,
                 [this]() { return _stop_requested || !Mads::running; });
    if (_stop_requested || !Mads::running) {
      _advertising = false;
      break;
    }
  }
}

} // namespace Mads
