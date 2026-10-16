// Unit tests for Mads::classify_zap_address() (src/zap_auth.hpp), which
// decides which ZAP peers the ip whitelist applies to: only the address
// forms libzmq reports for ipc:// peers skip it.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "zap_auth.hpp"

using Mads::classify_zap_address;
using Mads::ZapPeerKind;

TEST_CASE("classify_zap_address recognises exactly libzmq's ipc forms",
          "[zap]") {
  // macOS/Windows/musl: getnameinfo() fails on AF_UNIX.
  CHECK(classify_zap_address("") == ZapPeerKind::Ipc);
  // glibc "localhost" + SO_PEERCRED ":uid:gid:pid"; bare if getsockopt fails.
  CHECK(classify_zap_address("localhost:1000:1000:4242") == ZapPeerKind::Ipc);
  CHECK(classify_zap_address("localhost") == ZapPeerKind::Ipc);
  // BSD LOCAL_PEERCRED ":uid:gid:" (gid may be missing).
  CHECK(classify_zap_address(":501:20:") == ZapPeerKind::Ipc);
  CHECK(classify_zap_address(":501::") == ZapPeerKind::Ipc);

  // Look-alikes are not ipc.
  CHECK(classify_zap_address("localhost:1000:1000") == ZapPeerKind::Other);
  CHECK(classify_zap_address("localhost:a:b:c") == ZapPeerKind::Other);
  CHECK(classify_zap_address("localhost.evil.com") == ZapPeerKind::Other);
  CHECK(classify_zap_address("somehost") == ZapPeerKind::Other);
  CHECK(classify_zap_address(":::") == ZapPeerKind::Other);
}

TEST_CASE("classify_zap_address strips IPv6 scope ids and brackets",
          "[zap]") {
  std::string host;
  CHECK(classify_zap_address("10.0.0.7", &host) == ZapPeerKind::Ip);
  CHECK(host == "10.0.0.7");
  // A link-local tcp peer: inet_pton() alone rejects the scope id, which
  // used to let such peers bypass the whitelist.
  CHECK(classify_zap_address("fe80::1%en0", &host) == ZapPeerKind::Ip);
  CHECK(host == "fe80::1");
  CHECK(classify_zap_address("[fe80::1%4]", &host) == ZapPeerKind::Ip);
  CHECK(host == "fe80::1");
  CHECK(classify_zap_address("[::1]", &host) == ZapPeerKind::Ip);
  CHECK(host == "::1");
  // A '%' outside an IPv6 literal is not a scope id.
  CHECK(classify_zap_address("10.0.0.7%en0") == ZapPeerKind::Other);
}
