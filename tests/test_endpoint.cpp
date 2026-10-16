// Unit tests for the local-transport contract helpers: endpoint parsing
// (src/detail/endpoint.hpp), the `transport` reply schema
// (src/detail/transport.hpp), the ipc path convention
// (src/detail/runtime_dir.hpp) and the wire header's shm flag.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "detail/endpoint.hpp"
#include "detail/host_token.hpp"
#include "detail/runtime_dir.hpp"
#include "detail/transport.hpp"
#include "detail/wire_format.hpp"

using namespace Mads::detail;

TEST_CASE("parse_endpoint splits tcp endpoints", "[endpoint]") {
  auto e = parse_endpoint("tcp://localhost:9090");
  REQUIRE(e);
  CHECK(e->is_tcp());
  CHECK(e->host == "localhost");
  CHECK(e->port_number() == 9090);
  CHECK(e->to_string() == "tcp://localhost:9090");
  CHECK(e->with_host("10.0.0.2").to_string() == "tcp://10.0.0.2:9090");

  auto w = parse_endpoint("tcp://*:9092");
  REQUIRE(w);
  CHECK(w->host == "*");
  CHECK(w->port_number() == 9092);

  auto v6 = parse_endpoint("tcp://[::1]:9091");
  REQUIRE(v6);
  CHECK(v6->host == "[::1]");
  CHECK(v6->port_number() == 9091);

  auto eph = parse_endpoint("tcp://*:*");
  REQUIRE(eph);
  CHECK_FALSE(eph->port_number());
}

TEST_CASE("parse_endpoint handles ipc and inproc", "[endpoint]") {
  auto e = parse_endpoint("ipc:///tmp/mads-501/broker-9090-front.ipc");
  REQUIRE(e);
  CHECK(e->is_ipc());
  CHECK(e->path == "/tmp/mads-501/broker-9090-front.ipc");
  CHECK_FALSE(e->port_number());
  CHECK(e->with_host("x").to_string() == e->to_string());

  auto w = parse_endpoint(R"(ipc://C:\Users\miro\AppData\Local\mads\run\b.ipc)");
  REQUIRE(w);
  CHECK(w->path == R"(C:\Users\miro\AppData\Local\mads\run\b.ipc)");

  auto i = parse_endpoint("inproc://mads-broker-frontend");
  REQUIRE(i);
  CHECK(i->is_inproc());
}

TEST_CASE("parse_endpoint rejects malformed input", "[endpoint]") {
  CHECK_FALSE(parse_endpoint("mads.ini"));
  CHECK_FALSE(parse_endpoint("none"));
  CHECK_FALSE(parse_endpoint("tcp://localhost"));
  CHECK_FALSE(parse_endpoint("tcp://:9090"));
  CHECK_FALSE(parse_endpoint("tcp://host:99999"));
  CHECK_FALSE(parse_endpoint("ipc://"));
  CHECK_FALSE(parse_endpoint("udp://host:1"));
  CHECK_THROWS(parse_endpoint_or_throw("garbage"));
}

TEST_CASE("is_zmq_uri tells endpoints from settings files", "[endpoint]") {
  CHECK(is_zmq_uri("tcp://localhost:9092"));
  CHECK(is_zmq_uri("ipc:///tmp/x.ipc"));
  CHECK_FALSE(is_zmq_uri("mads.ini"));
  CHECK_FALSE(is_zmq_uri("/etc/tcp://odd.ini"));
  CHECK_FALSE(is_zmq_uri("none"));
}

TEST_CASE("transport reply round-trips and tolerates legacy brokers",
          "[transport]") {
  TransportInfo t;
  t.host_token = host_token();
  t.frontend_ipc = "ipc:///tmp/f.ipc";
  t.backend_ipc = "ipc:///tmp/b.ipc";
  t.curve = true;
  nlohmann::json j = t;
  auto back = parse_transport_reply(j.dump());
  REQUIRE(back);
  CHECK(back->host_token == t.host_token);
  CHECK(back->frontend_ipc == t.frontend_ipc);
  CHECK(back->settings_ipc.empty());
  CHECK(back->curve);
  CHECK(back->offers_ipc());

  CHECK_FALSE(parse_transport_reply(""));
  CHECK_FALSE(parse_transport_reply("not json"));
  CHECK(parse_transport_mode("ipc") == TransportMode::Ipc);
  CHECK_FALSE(parse_transport_mode("udp"));
}

TEST_CASE("host token is stable and non-trivial", "[transport]") {
  auto t = host_token();
  CHECK(t == host_token());
  CHECK(t.find('/') != std::string::npos);
  CHECK(t.size() > 2);
}

TEST_CASE("default ipc endpoints live in the runtime dir", "[transport]") {
  auto dir = runtime_dir();
  auto ep = default_ipc_endpoint(dir, 9090, "front");
  auto e = parse_endpoint(ep);
  REQUIRE(e);
  CHECK(e->is_ipc());
  CHECK(e->path.size() <= IPC_PATH_MAX);
  CHECK_THROWS(default_ipc_endpoint(dir / std::string(120, 'x'), 9090, "x"));
}

TEST_CASE("wire header carries the shm flag", "[wire]") {
  auto h = make_wire_header(Mads::WireFormat::MsgPack, Comp::None,
                            static_cast<uint8_t>(WIRE_FLAG_SHM));
  WireHeader out;
  REQUIRE(parse_wire_header(h, out));
  CHECK(out.has_shm);
  CHECK_FALSE(out.has_blob);

  auto b = make_wire_header(Mads::WireFormat::MsgPack, Comp::None, true);
  REQUIRE(parse_wire_header(b, out));
  CHECK(out.has_blob);
  CHECK_FALSE(out.has_shm);
}
