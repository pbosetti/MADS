# Migrating MADS from zmqpp to cppzmq — assessment and plan

## Context

MADS uses **libzmq 4.3.5 + zmqpp** for all messaging. zmqpp has had no release since
2021 and `vendors/CMakeLists.txt` pins it to a moving `GIT_TAG master` — an
unmaintained, unpinned dependency in the middle of the framework's transport layer.
It is also the direct cause of two workarounds already carried in the tree:

- `tests/test_main.cpp:1-26` — every test executable calls `TerminateProcess()` on
  Windows because zmqpp's process-lifetime static (`zmqpp::actor::actor_pipe_ctx_`)
  aborts during DLL detach once `CurveAuth` has been used. This costs Windows a normal
  exit path and coverage instrumentation.
- `src/main/broker.cpp:679-681` — "there is a bug in zmqpp lib and PAUSE and RESUME
  commands have inverted meanings".

**cppzmq** is the ZeroMQ organisation's own C++ binding: header-only, actively
maintained (v4.11.0, 2024), and a much thinner layer over libzmq. The goal is to drop
zmqpp entirely, rewrite MADS's call sites in native cppzmq idioms, and ship a
**deprecated compatibility shim** so downstream `Mads::Agent` subclasses keep compiling.

The **wire protocol does not change**. Both libraries are wrappers over the same
libzmq; frame layouts, CURVE/ZAP handshakes and broker interop are unaffected.

---

## 1. Impact assessment

### 1.1 Surface area (measured)

Only **10 non-test files** name a ZMQ type; everything else goes through `Mads::Agent`.
~180 `zmqpp::` references total.

| File | Weight | What it uses |
|---|---|---|
| `src/agent.cpp` / `src/agent.hpp` | **heavy** (~50 sites) | context, pub/sub/req sockets, multipart build+parse, 6 socket options, 2 drain threads |
| `src/curve.hpp` | **heavy** | `zmqpp::auth` (ZAP), `zmqpp::curve::keypair`, 5 CURVE socket options |
| `src/main/broker.cpp` | medium | XSUB/XPUB + `proxy_steerable`, REP settings worker, STATISTICS parsing |
| `src/broker_probe.cpp` | light | one REQ round-trip |
| `src/main/mads.cpp` | light | `generate_keypair`, one context, one exception catch |
| `src/worker.hpp`, `src/dealer.hpp`, `src/bridge.hpp` | light | PUSH/PULL, single-part |
| `src/doctor_checks.cpp` | none | **already uses raw `<zmq.h>`** (`zmq_z85_decode`) — precedent exists |
| 7 test files | medium | fake brokers, raw PUB injection, CURVE end-to-end |

**Not used anywhere** (so not a migration concern): `zmqpp::poller`, `reactor`,
`z85`, `frame`, `signal`, socket monitoring, context options, and `zmqpp::proxy()`
(header included at `broker.cpp:44` but never called).

### 1.2 Mapping — what is mechanical, what is not

| zmqpp | cppzmq | Difficulty |
|---|---|---|
| `zmqpp::context` / `.terminate()` | `zmq::context_t` / `.close()` | trivial |
| `zmqpp::socket(ctx, socket_type::X)` | `zmq::socket_t(ctx, zmq::socket_type::X)` | trivial |
| `socket.set(socket_option::rcvtimeo, n)` | `socket.set(zmq::sockopt::rcvtimeo, n)` | trivial, and **compile-time type-checked** |
| `socket.subscribe(t)` | `socket.set(zmq::sockopt::subscribe, t)` | trivial |
| `socket_option::identity` | `zmq::sockopt::routing_id` | trivial |
| `zmqpp::message` `<<` `parts()` `get(i)` `raw_data(i)` `size(i)` `add_raw` `copy()` | `zmq::multipart_t` `addstr` `size()` `at(i).to_string()` `at(i).data()` `at(i).size()` `addmem` `clone()` | mechanical, ~40 sites |
| `msg.get<uint64_t>(i)` (`broker.cpp:698`) | `memcpy` from `at(i).data()` | small |
| `zmqpp::curve::generate_keypair()` | `zmq_curve_keypair()` — **libzmq C API**, cppzmq has no wrapper (Phase 0) | trivial |
| z85 encode/decode | `zmq_z85_encode/decode` — **libzmq C API**, as `doctor_checks.cpp` already does | trivial |
| `zmqpp::proxy_steerable(f,b,ctrl)` | `zmq::proxy_steerable(f,b,capture,ctrl)` (`ZMQ_HAS_PROXY_STEERABLE`) | small — **note the extra `capture` argument** |
| `zmqpp::zmq_internal_exception` | `zmq::error_t` | trivial (2 sites) |
| **`zmqpp::auth`** | **no equivalent** | **the one real piece of new work** |

### 1.3 Complexity estimate

| Work item | Est. |
|---|---|
| `Mads::ZapAuth` (new RFC-27 ZAP handler) + rewrite `curve.hpp` | 2–3 d |
| `agent.hpp` / `agent.cpp` rewrite | 1.5 d |
| `broker.cpp` (proxy_steerable + control protocol + statistics) | 1 d |
| `broker_probe.cpp`, `worker.hpp`, `dealer.hpp`, `bridge.hpp`, `mads.cpp` | 0.5 d |
| 7 test files + new ZAP/shim tests | 1 d |
| Build system (`vendors/`, `CMakeLists.txt`, install rules) | 0.5 d — **net deletion** |
| Deprecated `zmqpp` compat shim + its test | 1 d |
| CI shake-out: Windows/MSVC, macOS universal, Linux arm64, smoke tests | 1–2 d |
| **Total** | **~8–11 working days**, ~2 weeks calendar with review |

### 1.4 Risks

| # | Risk | Severity | Status / mitigation |
|---|---|---|---|
| R1 | **Hand-written ZAP handler.** A bug means either open access or total lockout — security-critical. | **High** | **De-risked in Phase 0**: a ~60-line RFC 27 prototype accepted an authorised CURVE client and rejected an unauthorised one end-to-end. `tests/test_curve.cpp` (15+ cases) is the acceptance gate. |
| R2 | **`multipart_t::recv()` semantics.** `Agent::receive_raw()` and both drain threads reuse one long-lived message object. If `recv()` does not clear first, parts accumulate silently. | **High** | **Resolved in Phase 0**: `recv()` clears before filling; verified across receives of 2/3/4 parts into one reused object. |
| R3 | **`proxy_steerable` behaviour change.** cppzmq passes straight through to libzmq, so the PAUSE/RESUME inversion workaround is probably wrong afterwards; the extra `capture` argument must be a null `socket_ref`. | Medium | Manual broker test of `p`/`r`/`i`/`q` keys; flip the handlers back and record it in CHANGES.md as a user-visible fix. |
| R4 | **`ZMQ_HAS_PROXY_STEERABLE` + STATISTICS** availability against pinned libzmq v4.3.5. | Medium | **Resolved in Phase 0**: the macro is defined with libzmq 4.3.5. |
| R5 | **Wire-parser regressions** in the blob path (`raw_data(3)`/`size(3)` → `at(3)`). | Medium | `tests/test_agent_wire.cpp` covers malformed/extended frames and `dropped_messages()` — do not touch it, just make it pass. |
| R6 | **Windows/MSVC header ordering** (`winsock2.h` before ZMQ headers) — `curve.hpp:13-21` and `broker_probe.cpp:6-7` already fight this. | Medium | cppzmq is header-only and better behaved, but re-verify on the Windows CI job early, not at the end. |
| R7 | **ABI break.** `MadsCore` is a `SHARED` library and `zmqpp::context/socket` are protected data members — changing their type changes the class layout. | Medium | Downstream must recompile regardless of source compatibility. Requires a version bump + CHANGES.md note. |
| R8 | Silent socket-option drift (e.g. `identity`→`routing_id`, the HWM getter — the only `get()` in the repo). | Low | Compile-time typing in `zmq::sockopt` catches most of this class of bug. |

### 1.5 Backward compatibility

**Unaffected — zero work:**
- **Wire protocol / broker interop.** Frame layouts, topics, CURVE handshake all
  libzmq-side. A cppzmq-based agent talks to a zmqpp-based broker and vice versa.
- **Plugins** (`.plugin` pugg modules). `share/templates/CMakeLists.txt` links only
  `pugg` + `nlohmann_json`; no template or migration references ZMQ. The plugin
  protocol (v8) does not change.
- **C API** (`src/agent_c.h`) and the **Rust** crate (`rust/`, pure C ABI over dlopen).
- **`bag` format** — `bag.hpp` already stores frames as `topic + vector<string> parts`,
  explicitly "no sockets, no Agent dependency, no ZMQ".
- **`AgentApp` / `AgentAppT` consumers — verified unchanged.** `src/agent_app.hpp` and
  `src/agent_app.cpp` contain **zero** ZMQ references; the class is templated on the
  agent type and its whole public API is JSON/callback/settings based. Any downstream
  app built on `AgentApp` recompiles as-is. This is the recommended migration target
  to point downstream users at.

**Source-breaking without a shim** (all of it inside `class Agent`'s `protected:`
block, plus one public accessor):
1. `_context`, `_publisher`, `_subscriber` change type — breaks subclasses that build
   their own sockets from `_context` (exactly what in-repo `Worker`/`Dealer` do).
2. `bool receive_raw(zmqpp::message&, bool)` signature.
3. `SharedLatest<zmqpp::message_t> _latest_message`.
4. Public `curve_auth()` → `CurveAuth::setup_curve_client(zmqpp::socket&)`.
5. Downstream code relying on `agent.hpp` transitively including `<zmqpp/zmqpp.hpp>`.

**Shim design (per the chosen approach).** `src/zmqpp_compat.hpp` — installed, *not*
included by any MADS header, opt-in with one `#include`. It defines a minimal `zmqpp`
namespace whose types **derive from / alias the cppzmq types**, so `_publisher` and
friends still bind to `zmqpp::socket&` parameters in downstream code:

```cpp
// src/zmqpp_compat.hpp — DEPRECATED, will be removed in the release after next.
namespace zmqpp {
  using socket_type = zmq::socket_type;
  struct context : zmq::context_t { using zmq::context_t::context_t; void terminate() { close(); } };
  struct message : zmq::multipart_t { /* <<, >>, parts(), get(i), get<T>(i), raw_data(i), size(i), add_raw(), copy() */ };
  struct socket  : zmq::socket_t   { /* set/get(option,…), send/receive(message&,bool), subscribe() */ };
  namespace socket_option { /* the 10 options MADS actually exposed */ }
}
```
Roughly 200 LOC, header-only, covering only the ~10 options / ~11 socket methods /
~9 message methods the repo ever used. MADS's own code never includes it; one test
(`tests/test_zmqpp_compat.cpp`) does a PUB/SUB round-trip through it so it cannot rot.

`Mads::CurveAuth`'s `setup_curve_*` methods keep their names and gain
`zmq::socket_t&` parameters — since the shim's `zmqpp::socket` derives from
`zmq::socket_t`, existing downstream calls still bind.

### 1.6 Advantages over the status quo

1. **Maintained and pinnable.** cppzmq v4.11.0 (2024), same org as libzmq, pinned to a
   tag — replacing an unreleased-since-2021 library tracked at `master`.
2. **Header-only.** Deletes the entire zmqpp workaround block in `vendors/CMakeLists.txt`
   (dummy target for `generate_export_header`, `CMP0079`, `ZEROMQ_INCLUDE_DIR` override,
   MSVC `/FIwinsock2.h`, and the header install rules), one static library from every
   link line, and one dependency from the installed SDK tree.
3. **Fixes the Windows teardown abort.** With MADS owning the ZAP handler thread and
   shutting it down deterministically, `tests/test_main.cpp` can return normally on
   Windows — restoring the standard exit path and unblocking Windows coverage.
4. **Fixes the PAUSE/RESUME inversion** in the interactive broker.
5. **Compile-time-checked socket options** (`zmq::sockopt::*` carries the option's type)
   instead of zmqpp's runtime-typed `socket_option` enum.
6. **Access to the full modern libzmq surface** zmqpp never exposed — notably
   `ZMQ_ONLY_FIRST_SUBSCRIBE`, directly relevant to the topic byte-prefix pitfalls
   documented in `CONTEXT.md` and worked around in `src/topic_match.hpp`; plus
   `zmq::poller_t` and socket monitoring for future `mads doctor`/`mads top` work.
7. **Leaner crypto path.** Today `Agent::setup_crypto()` spawns a zmqpp actor thread in
   *every* crypto-enabled agent, though only the broker needs a ZAP authenticator.
   Owning the handler lets it be created lazily and joined deterministically.

### 1.7 Recommendation

**Migrate.** The surface is narrow (10 files, well-bounded API subset), the existing
test suite covers exactly the risky parts (CURVE end-to-end, wire parsing, fake
brokers), the wire protocol is untouched, and the migration removes two workarounds
already documented as upstream defects. The single non-mechanical piece is the ZAP
handler; it is ~250 LOC against a stable, well-specified protocol (RFC 27), and a
working prototype already exists (Phase 0).

**Do not copy zmqpp's `auth.cpp`** — it is MPL-2.0 while MADS is Apache-2.0. Write the
ZAP handler fresh against RFC 27.

---

## 2. Implementation plan

Branch: `devel/cppzmq`. Each phase should build and pass tests on its own.

### Phase 0 — Spikes (de-risk before touching MADS code) — **DONE**
1. cppzmq vendored in `vendors/CMakeLists.txt` **alongside** zmqpp: pinned to v4.11.0,
   populated without configuring its project (its CMake insists on
   `find_package(ZeroMQ)`, which would not see the in-tree libzmq target), headers
   exposed through `MADS_CPPZMQ_INCLUDE_DIR` and installed next to the other vendor
   headers.
2. **Findings** (spikes in `scratchpad/spike.cpp`, `spike2.cpp`, against cppzmq 4.11.0
   + libzmq 4.3.5):
   - `zmq::multipart_t::recv()` **clears the object first** — reusing one message
     across receives of 2/3/4 parts reported exactly the sent count each time (**R2
     closed**).
   - `recv()` returns **`false`, no throw**, both on `ZMQ_RCVTIMEO` expiry and with
     `ZMQ_DONTWAIT` — the same bool contract as zmqpp's `receive(msg, dont_block)`.
   - `ZMQ_HAS_PROXY_STEERABLE` **is defined** with libzmq 4.3.5 (**R4 closed**).
   - **Plan correction:** cppzmq does **not** provide `zmq::curve_keypair()` or
     `zmq::z85_encode/decode()`. Use the libzmq C API (`zmq_curve_keypair`,
     `zmq_z85_encode`, `zmq_z85_decode`), which `src/doctor_checks.cpp` already does.
   - A ~60-line RFC 27 ZAP handler prototype (REP socket on
     `inproc://zeromq.zap.01`, owned thread, `ZMQ_RCVTIMEO` + atomic stop) accepted an
     authorised CURVE client and rejected an unauthorised one over real TCP (**R1
     substantially de-risked**).
3. **Baseline recorded:** 362/362 tests pass on this branch before any code change
   (serial run; a parallel `-j4` run has one pre-existing port-collision flake in
   "startup event is delivered to a metadata subscriber"). Serial `ctest` is the gate.

### Phase 1 — `Mads::ZapAuth` and `curve.hpp`
- New `src/zap_auth.hpp` + `src/zap_auth.cpp`: a REP socket bound to
  `inproc://zeromq.zap.01` on a given `zmq::context_t`, serviced by an owned
  `std::thread` with `ZMQ_RCVTIMEO` + an atomic stop flag, joined in the destructor.
  Implements RFC 27 request `[version, request_id, domain, address, identity,
  mechanism, credentials…]` → reply `[version, request_id, status, text, user_id,
  metadata]`.
  Public methods mirroring what `curve.hpp` uses today: `set_verbose(bool)`,
  `configure_domain(std::string)`, `allow(std::string ip)`,
  `configure_curve(std::string z85_client_key)`. Bind must happen before any socket
  using it starts handshaking — keep it in `CurveAuth::setup_auth()`, matching today's
  ordering in `broker.cpp` and `agent.cpp`.
  CURVE credentials arrive as a raw 32-byte key: compare via `zmq_z85_encode`.
- Rewrite `src/curve.hpp`: `zmqpp::auth _authenticator` → `Mads::ZapAuth`,
  `zmqpp::curve::keypair` → a local `struct Keypair {string public_key, secret_key;}`
  fed by `zmq_curve_keypair()`, socket setters → `zmq::sockopt::curve_server`
  /`curve_secretkey`/`curve_publickey`/`curve_serverkey`/`routing_id`.
  Keep every method name and the file-loading logic byte-for-byte identical.
- **Gate:** `tests/test_curve.cpp` and `tests/test_doctor_checks.cpp` pass with only
  their `zmqpp::curve::generate_keypair()` calls swapped for the new helper.

### Phase 2 — `Agent`
- `src/agent.hpp`: `#include <zmq.hpp>` + `<zmq_addon.hpp>`; `_context`→`zmq::context_t`,
  `_publisher`/`_subscriber`→`zmq::socket_t`, `SharedLatest<zmq::multipart_t>`,
  `receive_raw(zmq::multipart_t&, bool)`, `setup_curve_on(zmq::socket_t&)`.
- `src/agent.cpp`: drop `using namespace zmqpp` — it hides three unqualified
  `socket_option::` uses; qualify them.
  Rewrite the publish builders, the frame parser (incl. `raw_data(3)`/`size(3)` →
  `at(3).data()/size()`), the broker round-trip, `connect_pub`/`connect_sub`, and both
  drain threads.
  `catch (const zmqpp::zmq_internal_exception&)` → `catch (const zmq::error_t&)`.
- `src/worker.hpp`, `src/dealer.hpp`, `src/bridge.hpp` — mechanical.
- **Gate:** `test_agent_wire`, `test_agent_pubsub`, `test_agent_loop`,
  `test_agent_events`, `test_agent_settings`, `test_agent_broker`, `test_agent_app`,
  `test_lazy_payload`, `test_bag_roundtrip`, `test_echo_loopback` all pass untouched
  except for their own fake-broker socket code.

### Phase 3 — Broker, probe, CLI
- `src/main/broker.cpp`: `zmq::socket_t` XSUB/XPUB; `proxy()` helper →
  `zmq::proxy_steerable(frontend, backend, zmq::socket_ref{}, ctrl)`; STATISTICS
  parsing via `memcpy` from `at(i).data()`; **re-test PAUSE/RESUME and remove the
  inversion workaround if it no longer applies**; `context.terminate()` → `close()`.
- `src/broker_probe.cpp`, `src/main/mads.cpp` — mechanical.

### Phase 4 — Compatibility shim
- Add `src/zmqpp_compat.hpp` as described in §1.5 (installed by the existing
  non-recursive `file(GLOB ${SOURCE_DIR}/*.hpp)`).
- Add `tests/test_zmqpp_compat.cpp`: a PUB/SUB round-trip and a REQ/REP round-trip
  written entirely in the old `zmqpp::` idiom, compiled with deprecation warnings
  suppressed, proving downstream `Agent` subclasses still build.

### Phase 5 — Build system and cleanup
- `vendors/CMakeLists.txt`: delete the zmqpp `FetchContent_Declare`, its option block,
  the dummy-target workaround, `CMP0079` + `ZEROMQ_INCLUDE_DIR`, the header install
  rules and the MSVC `/FIwinsock2.h` block.
- `CMakeLists.txt`: drop the `zmqpp-static zmqpp` `link_first_available` calls and the
  derived include-dir plumbing; keep `libzmq` and `sodium`.
- `tests/test_main.cpp`: remove the `TerminateProcess()` workaround **once Windows CI
  confirms a clean exit** (do this last, in its own commit, so it can be reverted
  independently).
- Docs: `README.md`, `.github/copilot-instructions.md`, `CONTEXT.md`
  ("Messaging: `libzmq` + `zmqpp`"), and a `CHANGES.md` entry covering the ABI break,
  the shim, the deprecation window, and the PAUSE/RESUME fix.

---

## 3. Verification

1. **Unit/integration:** `cmake -Bbuild -DCMAKE_BUILD_TYPE=Debug -GNinja && cmake --build build -j6 && ctest --test-dir build --output-on-failure`.
   Suites whose *content* must not change (they are the regression gate):
   `test_agent_wire`, `test_curve`, `test_bag_roundtrip`, `test_lazy_payload`.
2. **CURVE end-to-end (R1):** beyond `test_curve.cpp`, run a real broker with
   `-k <keydir>` plus one authorised and one unauthorised agent; confirm the
   unauthorised one is rejected and `mads doctor` reports the key check green.
3. **Cross-version interop (backward compat):** run a **pre-migration** `mads-broker`
   against a **post-migration** `mads-source`/`mads-sink`, and the reverse. Payloads
   must flow in both directions — this is the proof the wire format is untouched.
4. **Broker control:** interactive `mads-broker`, exercise `p`/`r`/`i`/`q`; confirm
   pause actually pauses and STATISTICS numbers match pre-migration output.
5. **Plugins:** scaffold with `mads plugin`, build it, run under `mads-filter`
   against the migrated broker — no plugin rebuild flags should change.
6. **C/Rust:** `smoke_test/` (`test_messaging.cpp`, `test_cpp_agent.cpp`,
   `test_c_agent.c`) plus a `rust/mads-plugin-example` build.
7. **Shim:** build `tests/test_zmqpp_compat.cpp`; separately, compile a scratch
   `Agent` subclass that touches `_context`/`_publisher` through the shim.
8. **CI:** full matrix (macOS universal, Linux x86_64 + arm64, Windows MSVC) plus the
   coverage job — the Windows job is the one that validates removing the
   `TerminateProcess()` hack.
