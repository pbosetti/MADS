/*
  _____              _         _   _
 |__  /__ _ _ __    / \  _   _| |_| |__
   / // _` | '_ \  / _ \| | | | __| '_ \
  / /| (_| | |_) |/ ___ \ |_| | |_| | | |
 /____\__,_| .__//_/   \_\__,_|\__|_| |_|
           |_|

ZeroMQ Authentication Protocol (ZAP, RFC 27) handler
Copyright (C) 2026 Paolo Bosetti
*/

#include "zap_auth.hpp"

#include <iostream>
#include <vector>

#include <zmq.h>
#include <zmq_addon.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

using namespace std;

namespace Mads {

namespace {

bool is_ip_literal(const string &address) {
  unsigned char buf[16];
  return inet_pton(AF_INET, address.c_str(), buf) == 1 ||
         inet_pton(AF_INET6, address.c_str(), buf) == 1;
}

bool all_digits(const string &s, size_t from, size_t to) {
  if (from >= to)
    return false;
  for (size_t i = from; i < to; ++i)
    if (s[i] < '0' || s[i] > '9')
      return false;
  return true;
}

// ":<n>:<n>:<n>" (Linux SO_PEERCRED: uid, gid, pid) or ":<n>:[<n>]:" (BSD
// LOCAL_PEERCRED: uid, first group if any, no pid), as appended by libzmq.
bool is_peercred_suffix(const string &s) {
  if (s.empty() || s[0] != ':')
    return false;
  const size_t a = s.find(':', 1);
  if (a == string::npos || !all_digits(s, 1, a))
    return false;
  const size_t b = s.find(':', a + 1);
  if (b == string::npos)
    return false;
  const bool gid_ok = b == a + 1 || all_digits(s, a + 1, b);
  if (!gid_ok)
    return false;
  if (b + 1 == s.size()) // BSD form
    return true;
  return all_digits(s, b + 1, s.size()) && b > a + 1; // Linux form
}

// RFC 27 request frames, in order.
constexpr size_t ZAP_VERSION = 0;
constexpr size_t ZAP_REQUEST_ID = 1;
constexpr size_t ZAP_DOMAIN = 2;
constexpr size_t ZAP_ADDRESS = 3;
// Frame 4 is the peer identity, which MADS does not use.
constexpr size_t ZAP_MECHANISM = 5;
constexpr size_t ZAP_CREDENTIALS = 6;
// version..mechanism are mandatory; credentials are mechanism-specific.
constexpr size_t ZAP_MIN_FRAMES = 6;

// How long the worker blocks in recv() before re-checking the stop flag.
constexpr int ZAP_POLL_MS = 100;

} // namespace

void ZapAuth::start() {
  if (_running.load())
    return;
  // Bind on the caller's thread so that start() returning is a hard guarantee
  // that the endpoint is live: libzmq lets connections through unauthenticated
  // while no handler is bound, so a lazily-bound socket would be a security
  // hole for anything connecting in the meantime.
  _socket = make_unique<zmq::socket_t>(_context, zmq::socket_type::rep);
  try {
    _socket->set(zmq::sockopt::linger, 0);
    _socket->set(zmq::sockopt::rcvtimeo, ZAP_POLL_MS);
    _socket->bind(endpoint);
  } catch (...) {
    // Leave the object cleanly un-started (typically: another handler already
    // owns the ZAP endpoint in this context) so a caller that handles the
    // failure can retry start() later.
    _socket.reset();
    throw;
  }
  _stop.store(false);
  _running.store(true);
  _thread = thread(&ZapAuth::_serve, this);
}

void ZapAuth::stop() {
  if (!_running.load())
    return;
  _stop.store(true);
  if (_thread.joinable())
    _thread.join();
  _running.store(false);
  // Closed only after the worker has joined: the socket belongs to that thread
  // for its whole lifetime.
  if (_socket) {
    try {
      _socket->close();
    } catch (...) {
    }
    _socket.reset();
  }
}

void ZapAuth::configure_domain(const string &domain) {
  lock_guard<mutex> lock(_config_mutex);
  _domain = domain;
}

void ZapAuth::allow(const string &address) {
  lock_guard<mutex> lock(_config_mutex);
  _allowed_addresses.insert(address);
}

void ZapAuth::configure_curve(const string &z85_public_key) {
  lock_guard<mutex> lock(_config_mutex);
  _allowed_curve_keys.insert(z85_public_key);
}

bool ZapAuth::_authorise(const string &domain, const string &address,
                         const string &mechanism, const string &credentials,
                         string &reason) const {
  lock_guard<mutex> lock(_config_mutex);

  if (_domain != "*" && !_domain.empty() && domain != _domain) {
    reason = "Unknown domain";
    return false;
  }
  // An empty whitelist means "no address restriction"; a non-empty one is
  // exclusive, which is what MADS relies on (see CurveAuth::setup_auth()).
  // It restricts network peers only: an ipc:// peer is on this host by
  // construction, and the owner-only runtime directory holding the socket
  // files is its access control. CURVE keys are still checked below.
  // Anything not recognisably ipc is a network peer, listed either verbatim
  // or (for an IP) without brackets and scope id.
  if (!_allowed_addresses.empty()) {
    string host;
    const ZapPeerKind kind = classify_zap_address(address, &host);
    const bool listed =
        _allowed_addresses.count(address) > 0 ||
        (kind == ZapPeerKind::Ip && _allowed_addresses.count(host) > 0);
    if (kind != ZapPeerKind::Ipc && !listed) {
      reason = "Address not allowed";
      return false;
    }
  }

  if (mechanism == "CURVE") {
    if (_allowed_curve_keys.empty() || _allowed_curve_keys.count("*") > 0)
      return true;
    // The credentials frame carries the raw 32-byte key; the allowlist holds
    // the Z85 text read from the .pub files.
    const string encoded = z85_encode(credentials);
    if (encoded.empty() || _allowed_curve_keys.count(encoded) == 0) {
      reason = "Unknown client key";
      return false;
    }
    return true;
  }
  if (mechanism == "NULL")
    return true;

  // PLAIN and GSSAPI are never configured by MADS: refuse rather than let an
  // unconfigured mechanism through.
  reason = "Unsupported mechanism " + mechanism;
  return false;
}

void ZapAuth::_serve() {
  while (!_stop.load()) {
    zmq::multipart_t request;
    try {
      if (!request.recv(*_socket))
        continue; // rcvtimeo expiry: re-check the stop flag
    } catch (const zmq::error_t &e) {
      // e.num() rather than zmq_errno(): the latter can have been overwritten
      // by the time we look at it.
      if (e.num() == ETERM)
        break; // context terminated under us
      continue;
    }
    if (request.size() < ZAP_MIN_FRAMES) {
      if (_verbose.load())
        cout << "ZAP: malformed request with " << request.size() << " frames"
             << endl;
      continue;
    }

    const string version = request.at(ZAP_VERSION).to_string();
    const string request_id = request.at(ZAP_REQUEST_ID).to_string();
    const string domain = request.at(ZAP_DOMAIN).to_string();
    const string address = request.at(ZAP_ADDRESS).to_string();
    const string mechanism = request.at(ZAP_MECHANISM).to_string();
    string credentials;
    if (request.size() > ZAP_CREDENTIALS) {
      const auto &frame = request.at(ZAP_CREDENTIALS);
      credentials.assign(static_cast<const char *>(frame.data()), frame.size());
    }

    string reason;
    bool ok = version == "1.0";
    if (!ok)
      reason = "Unsupported ZAP version " + version;
    else
      ok = _authorise(domain, address, mechanism, credentials, reason);

    if (ok)
      _granted++;
    else
      _denied++;
    if (_verbose.load()) {
      cout << "ZAP: " << (ok ? "granted" : "DENIED ") << " " << mechanism
           << " from " << address;
      if (!ok)
        cout << " (" << reason << ")";
      cout << endl;
    }

    // Reply frames per RFC 27: version, request id, status code, status text,
    // user id, metadata.
    zmq::multipart_t reply;
    reply.addstr("1.0");
    reply.addstr(request_id);
    reply.addstr(ok ? "200" : "400");
    reply.addstr(ok ? "OK" : reason);
    reply.addstr("");
    reply.addstr("");
    try {
      reply.send(*_socket);
    } catch (const zmq::error_t &e) {
      if (e.num() == ETERM)
        break;
    }
  }
}

ZapPeerKind classify_zap_address(const string &address, string *host) {
  if (address.empty())
    return ZapPeerKind::Ipc; // getnameinfo() failed on an AF_UNIX peer
  const string localhost = "localhost";
  if (address == localhost ||
      (address.rfind(localhost, 0) == 0 &&
       is_peercred_suffix(address.substr(localhost.size()))) ||
      is_peercred_suffix(address))
    return ZapPeerKind::Ipc;

  string bare = address;
  if (bare.size() > 2 && bare.front() == '[' && bare.back() == ']')
    bare = bare.substr(1, bare.size() - 2);
  // An IPv6 scope id ("fe80::1%en0"), which inet_pton() rejects.
  if (auto pct = bare.find('%');
      pct != string::npos && bare.find(':') != string::npos)
    bare.erase(pct);
  if (!is_ip_literal(bare))
    return ZapPeerKind::Other;
  if (host)
    *host = std::move(bare);
  return ZapPeerKind::Ip;
}

CurveKeypair generate_keypair() {
  // 40 Z85 characters plus the NUL that zmq_curve_keypair() writes.
  char public_key[41] = {0};
  char secret_key[41] = {0};
  if (zmq_curve_keypair(public_key, secret_key) != 0)
    throw zmq::error_t();
  return {string(public_key, 40), string(secret_key, 40)};
}

string z85_encode(const string &data) {
  if (data.empty() || data.size() % 4 != 0)
    return {};
  // Z85 expands 4 bytes into 5 characters, plus the terminating NUL.
  vector<char> out(data.size() * 5 / 4 + 1, '\0');
  if (zmq_z85_encode(out.data(),
                     reinterpret_cast<const uint8_t *>(data.data()),
                     data.size()) == nullptr)
    return {};
  return string(out.data(), data.size() * 5 / 4);
}

} // namespace Mads
