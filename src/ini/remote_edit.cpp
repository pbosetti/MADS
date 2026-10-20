/*
 ____                      _         _____    _ _ _
|  _ \ ___ _ __ ___   ___ | |_ ___  | ____|__| (_) |_
| |_) / _ \ '_ ` _ \ / _ \| __/ _ \ |  _| / _` | | __|
|  _ <  __/ | | | | | (_) | ||  __/ | |__| (_| | | |_
|_| \_\___|_| |_| |_|\___/ \__\___| |_____\__,_|_|\__|

Remote editing of the broker's settings file: see remote_edit.hpp for the
protocol and the security model.

Author(s): Paolo Bosetti
*/
#include "remote_edit.hpp"

#include "mads.hpp"
#include "detail/broker_ipc.hpp"

#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>
#include <zmq.hpp>
#include <zmq_addon.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <memory>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

namespace Mads::Ini::Remote {

namespace {

namespace fs = std::filesystem;

// Room for the largest file plus the framing of a request or reply. Used as
// ZMQ_MAXMSGSIZE so that libzmq drops a connection announcing more, instead of
// buffering it: a client cannot make the broker allocate gigabytes.
constexpr std::size_t kMaxMessageSize = kMaxFileSize + 64 * 1024;
// A well-formed request has at most 4 frames; anything longer is refused
// before it is even interpreted.
constexpr std::size_t kMaxRequestFrames = 8;

bool read_file(const fs::path &path, std::string &out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return !in.bad();
}

std::vector<std::string> error_reply(const std::string &message) {
  return {LIB_VERSION, "error", message};
}

std::string utc_timestamp(std::time_t t, const char *format) {
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  const std::size_t n = std::strftime(buf, sizeof(buf), format, &tm);
  return std::string(buf, n);
}

// A backup of `base` is named `<base>.<YYYYmmdd-HHMMSS>.bak`, or
// `<base>.<YYYYmmdd-HHMMSS>-<n>.bak` when that name is taken (several puts in
// one second). Anything else next to the file (FileSource's plain `.bak`, the
// user's own copies) is left alone.
struct Backup {
  std::string stamp; // YYYYmmdd-HHMMSS, sorts chronologically
  unsigned counter = 0;
  fs::path path;
};

std::vector<Backup> list_backups(const fs::path &base) {
  static const std::regex pattern(R"(^(\d{8}-\d{6})(?:-(\d+))?\.bak$)");
  std::vector<Backup> found;
  const std::string prefix = base.filename().string() + ".";
  std::error_code ec;
  for (fs::directory_iterator it(base.parent_path().empty()
                                     ? fs::path(".")
                                     : base.parent_path(),
                                 ec),
       end;
       !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix))
      continue;
    std::smatch m;
    const std::string rest = name.substr(prefix.size());
    if (!std::regex_match(rest, m, pattern))
      continue;
    Backup b;
    b.stamp = m[1].str();
    if (m[2].matched)
      b.counter = static_cast<unsigned>(std::stoul(m[2].str()));
    b.path = it->path();
    found.push_back(std::move(b));
  }
  std::sort(found.begin(), found.end(), [](const Backup &a, const Backup &b) {
    return std::tie(a.stamp, a.counter) < std::tie(b.stamp, b.counter);
  });
  return found;
}

// Copies `path` to a fresh backup name; returns an empty string on success,
// else the reason.
std::string make_backup(const fs::path &path) {
  const std::string stamp = utc_timestamp(std::time(nullptr), "%Y%m%d-%H%M%S");
  std::error_code ec;
  for (unsigned n = 0; n < 1000; ++n) {
    fs::path bak = path;
    bak += "." + stamp + (n ? "-" + std::to_string(n) : "") + ".bak";
    if (fs::exists(bak, ec))
      continue;
    fs::copy_file(path, bak, fs::copy_options::none, ec);
    return ec ? "cannot write backup " + bak.string() + ": " + ec.message()
              : std::string();
  }
  return "too many backups with the same timestamp";
}

void prune_backups(const fs::path &path, std::size_t keep) {
  auto backups = list_backups(path);
  if (backups.size() <= keep)
    return;
  std::error_code ec;
  for (std::size_t i = 0; i + keep < backups.size(); ++i)
    fs::remove(backups[i].path, ec); // best effort
}

} // namespace

// ------------------------------------------------------------- EditService

EditService::EditService(fs::path settings_path, std::size_t max_backups)
    : _path(std::move(settings_path)),
      // At least one: a put must never be able to destroy the only copy.
      _max_backups(std::max<std::size_t>(max_backups, 1)) {}

std::vector<std::string>
EditService::handle(const std::vector<std::string> &request,
                    const std::string &user) {
  const std::string who =
      user.empty() ? std::string("unknown") : user.substr(0, 8) + "…";
  auto note = [&](const std::string &line) {
    if (!log)
      return;
    try {
      log(line);
    } catch (...) {
    }
  };
  // Records an error in the log and builds the reply.
  auto fail = [&](const std::string &command, const std::string &message) {
    note(command + " by " + who + ": error, " + message);
    return error_reply(message);
  };

  std::string command = "request";
  try {
    if (request.size() < 2)
      return fail(command, "malformed request");
    try {
      if (!Mads::check_version(request[0]))
        return fail(command, "incompatible version " + request[0] +
                                 " (server is " + LIB_VERSION + ")");
    } catch (const std::exception &err) {
      return fail(command, err.what());
    }
    command = request[1];

    if (command == "get") {
      if (request.size() != 2)
        return fail(command, "malformed get");
      std::string text;
      if (!read_file(_path, text))
        return fail(command, "cannot read the settings file");
      return {LIB_VERSION, "ok", content_etag(text), text};
    }

    if (command == "info") {
      if (request.size() != 2)
        return fail(command, "malformed info");
      std::string text;
      std::error_code ec;
      if (!read_file(_path, text))
        return fail(command, "cannot read the settings file");
      // file_clock has no portable conversion to system_clock before C++20
      // library support is universal, so shift by the difference of "now".
      const auto ftime = fs::last_write_time(_path, ec);
      std::string mtime;
      if (!ec) {
        const auto shifted =
            std::chrono::system_clock::now() +
            std::chrono::duration_cast<std::chrono::system_clock::duration>(
                ftime - fs::file_time_type::clock::now());
        mtime = utc_timestamp(std::chrono::system_clock::to_time_t(shifted),
                              "%Y-%m-%dT%H:%M:%SZ");
      }
      nlohmann::json j;
      j["path"] = _path.string();
      j["size"] = text.size();
      j["mtime"] = mtime;
      j["etag"] = content_etag(text);
      j["version"] = LIB_VERSION;
      return {LIB_VERSION, "ok", j.dump()};
    }

    if (command == "put") {
      if (request.size() != 4)
        return fail(command, "malformed put");
      const std::string &base_etag = request[2];
      const std::string &text = request[3];
      if (text.size() > kMaxFileSize)
        return fail(command, "file too large (" + std::to_string(text.size()) +
                                 " bytes, limit " +
                                 std::to_string(kMaxFileSize) + ")");
      try {
        (void)toml::parse(text);
      } catch (const toml::parse_error &err) {
        return fail(command,
                    "not valid TOML: " + std::string(err.description()) +
                        " (line " + std::to_string(err.source().begin.line) +
                        ")");
      }

      std::error_code ec;
      const bool exists = fs::exists(_path, ec);
      if (exists && !base_etag.empty()) {
        std::string current;
        if (!read_file(_path, current))
          return fail(command, "cannot read the settings file");
        if (content_etag(current) != base_etag) {
          note("put by " + who + ": conflict");
          return {LIB_VERSION, "conflict", content_etag(current), current};
        }
      }

      fs::path tmp = _path;
      tmp += ".tmp";
      {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
          fs::remove(tmp, ec);
          return fail(command, "cannot write the temporary file");
        }
      }
      if (exists) {
        // The new file keeps the permissions of the one it replaces.
        const auto status = fs::status(_path, ec);
        if (!ec)
          fs::permissions(tmp, status.permissions(), ec);
        const std::string backup_error = make_backup(_path);
        if (!backup_error.empty()) {
          fs::remove(tmp, ec);
          return fail(command, backup_error);
        }
        prune_backups(_path, _max_backups);
      }
      fs::rename(tmp, _path, ec);
      if (ec && exists) {
        // Some platforms refuse to rename over an existing file.
        fs::remove(_path, ec);
        fs::rename(tmp, _path, ec);
      }
      if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return fail(command, "cannot replace the settings file: " +
                                 ec.message());
      }
      note("put by " + who + ": ok, " + std::to_string(text.size()) +
           " bytes");
      return {LIB_VERSION, "ok", content_etag(text)};
    }

    return fail(command, "unknown command");
  } catch (const std::exception &err) {
    return fail(command, err.what());
  } catch (...) {
    return error_reply("internal error");
  }
}

// ------------------------------------------------------------------- serve

namespace {

// One bound listener: its ROUTER socket and, for ipc, the socket file this
// process created (removed on destruction, only if still ours).
struct BoundListener {
  std::unique_ptr<zmq::socket_t> socket;
  bool curve = false;
  Mads::detail::IpcSocketFile file;
};

void configure_listener(zmq::socket_t &socket, const Listener &listener) {
  socket.set(zmq::sockopt::linger, 0);
  socket.set(zmq::sockopt::sndtimeo, 1000);
  socket.set(zmq::sockopt::maxmsgsize, static_cast<int64_t>(kMaxMessageSize));
  if (listener.curve) {
    socket.set(zmq::sockopt::curve_server, 1);
    socket.set(zmq::sockopt::curve_secretkey, listener.curve->secret_key);
    // Must precede bind(): the domain selects the editor-only allowlist.
    socket.set(zmq::sockopt::zap_domain, kZapDomain);
  }
}

// Reads one pending multipart message, and answers it unless it is malformed
// (then it is dropped silently).
void serve_one(zmq::socket_t &socket, bool curve, EditService &service,
               const std::function<void(const std::string &)> &note) {
  std::vector<zmq::message_t> frames;
  bool more = true, oversized = false;
  while (more) {
    zmq::message_t frame;
    if (!socket.recv(frame, zmq::recv_flags::none))
      return;
    more = frame.more();
    if (frames.size() <= kMaxRequestFrames + 1)
      frames.push_back(std::move(frame));
    else
      oversized = true; // keep draining the message, bounded memory
  }
  if (frames.empty())
    return;

  // Envelope: routing id(s) up to and including the empty delimiter that a
  // REQ client adds.
  std::size_t delimiter = 0;
  while (delimiter < frames.size() && frames[delimiter].size() != 0)
    ++delimiter;
  if (delimiter == 0 || delimiter >= frames.size()) {
    note("request without a REQ envelope dropped");
    return;
  }

  std::string user;
  if (curve) {
    // ZAP's User-Id: the Z85 key of the client, set only for requests the
    // edit-domain allowlist granted. Read from the first content frame; if
    // absent the request is refused (fail closed) even though the handshake
    // succeeded.
    if (delimiter + 1 < frames.size()) {
      try {
        if (const char *uid = frames[delimiter + 1].gets("User-Id"))
          user = uid;
      } catch (const zmq::error_t &) {
      }
    }
  } else {
    user = "ipc"; // plaintext ipc: the owner-only socket directory is the gate
  }

  std::vector<std::string> reply;
  if (user.empty()) {
    note("request without an authenticated key refused");
    reply = error_reply("not authorized");
  } else if (oversized) {
    reply = error_reply("malformed request");
  } else {
    std::vector<std::string> request;
    for (std::size_t i = delimiter + 1; i < frames.size(); ++i)
      request.emplace_back(frames[i].data<char>(), frames[i].size());
    reply = service.handle(request, user);
  }

  zmq::multipart_t out;
  for (std::size_t i = 0; i <= delimiter; ++i)
    out.add(std::move(frames[i]));
  for (const auto &part : reply)
    out.addstr(part);
  out.send(socket);
}

} // namespace

void serve(zmq::context_t &context, const std::vector<Listener> &listeners,
           EditService &service, const std::atomic<bool> &running) {
  using Mads::detail::IpcSocketState;

  // Validate everything before binding anything.
  std::vector<std::optional<Mads::detail::Endpoint>> parsed;
  for (const auto &l : listeners) {
    auto e = Mads::detail::parse_endpoint(l.endpoint);
    if (!e)
      throw std::invalid_argument("invalid endpoint " + l.endpoint);
    if (!l.curve && !e->is_ipc())
      throw std::invalid_argument(
          "refusing a plaintext edit listener on " + l.endpoint +
          " (only ipc:// may go without CURVE)");
    if (e->is_ipc() && !Mads::detail::zmq_has_ipc())
      throw std::runtime_error("this libzmq build has no ipc:// transport: " +
                               l.endpoint);
    parsed.push_back(std::move(e));
  }

  // Destroyed in reverse order on return or throw: sockets close before the
  // files they created are unlinked.
  struct Guard {
    std::vector<BoundListener> bound;
    ~Guard() {
      for (auto it = bound.rbegin(); it != bound.rend(); ++it) {
        it->socket.reset();
        it->file.remove();
      }
    }
  } guard;

  for (std::size_t i = 0; i < listeners.size(); ++i) {
    const auto &l = listeners[i];
    BoundListener b;
    b.curve = l.curve.has_value();
    b.socket = std::make_unique<zmq::socket_t>(context, zmq::socket_type::router);
    configure_listener(*b.socket, l);
    if (parsed[i]->is_ipc()) {
      // libzmq unlinks the path before binding: never let that steal the
      // socket of a broker that is still listening on it.
      switch (Mads::detail::probe_ipc_socket(parsed[i]->path)) {
      case IpcSocketState::Live:
        throw std::runtime_error(parsed[i]->path +
                                 " is in use by another process");
      case IpcSocketState::Foreign:
        throw std::runtime_error(parsed[i]->path +
                                 " exists and is not a socket");
      default:
        break;
      }
      b.socket->bind(l.endpoint);
      b.file.claim(parsed[i]->path);
    } else {
      b.socket->bind(l.endpoint);
    }
    guard.bound.push_back(std::move(b));
  }

  auto note = [&](const std::string &line) {
    if (!service.log)
      return;
    try {
      service.log(line);
    } catch (...) {
    }
  };

  std::vector<zmq::pollitem_t> items;
  for (auto &b : guard.bound)
    items.push_back({b.socket->handle(), 0, ZMQ_POLLIN, 0});

  while (running.load()) {
    try {
      if (items.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
      zmq::poll(items, std::chrono::milliseconds(200));
      for (std::size_t i = 0; i < items.size(); ++i) {
        if (!(items[i].revents & ZMQ_POLLIN))
          continue;
        auto &b = guard.bound[i];
        // One message per readiness event is enough: poll fires again if
        // more are queued, and every socket gets its turn.
        try {
          serve_one(*b.socket, b.curve, service, note);
        } catch (const zmq::error_t &err) {
          if (err.num() == ETERM)
            return;
          note(std::string("edit service: ") + err.what());
        } catch (const std::exception &err) {
          note(std::string("edit service: ") + err.what());
        }
      }
    } catch (const zmq::error_t &err) {
      if (err.num() == ETERM)
        return;
      note(std::string("edit service: ") + err.what());
    }
  }
}

std::string local_endpoint(int port, const fs::path &ipc_dir) {
  if (!Mads::detail::zmq_has_ipc())
    throw std::runtime_error(
        "this libzmq build has no ipc:// transport; use the tcp endpoint");
  fs::path dir;
  if (ipc_dir.empty()) {
    dir = Mads::detail::runtime_dir();
  } else {
    dir = ipc_dir;
    Mads::detail::ensure_private_dir(dir, "ipc_dir");
  }
  return Mads::detail::default_ipc_endpoint(dir, static_cast<unsigned>(port),
                                            "edit");
}

// -------------------------------------------------------------- client keys

namespace {

std::string read_key_line(const fs::path &file) {
  std::ifstream in(file);
  if (!in)
    throw std::runtime_error("cannot open key file " + file.string());
  std::string line;
  std::getline(in, line);
  while (!line.empty() &&
         std::isspace(static_cast<unsigned char>(line.back())))
    line.pop_back();
  if (line.size() != 40)
    throw std::runtime_error("malformed key file " + file.string() +
                             " (expected 40 Z85 characters)");
  return line;
}

} // namespace

ClientKeys load_client_keys(const fs::path &dir, const std::string &client,
                            const std::string &server) {
  ClientKeys keys;
  keys.public_key = read_key_line(dir / (client + ".pub"));
  keys.secret_key = read_key_line(dir / (client + ".key"));
  keys.server_public_key = read_key_line(dir / (server + ".pub"));
  return keys;
}

// ------------------------------------------------------------ RemoteSource

RemoteSource::RemoteSource(std::string endpoint, std::optional<ClientKeys> keys,
                           std::chrono::milliseconds timeout)
    : _endpoint(std::move(endpoint)), _keys(std::move(keys)),
      _timeout(timeout), _context(nullptr) {
  // Explicit rules, no guessing: ipc is plaintext, everything else is CURVE.
  const auto e = Mads::detail::parse_endpoint(_endpoint);
  if (!e)
    throw std::invalid_argument("invalid endpoint " + _endpoint);
  if (e->is_ipc() && _keys)
    throw std::invalid_argument("an ipc:// endpoint takes no CURVE keys");
  if (!e->is_ipc() && !_keys)
    throw std::invalid_argument(
        "CURVE keys are required for " + _endpoint +
        " (plaintext is only possible on ipc://)");
  if (!e->is_ipc() && !e->is_tcp())
    throw std::invalid_argument("unsupported endpoint " + _endpoint);
  _context = new zmq::context_t(1);
}

RemoteSource::~RemoteSource() {
  delete _context;
}

std::vector<std::string>
RemoteSource::request(const std::vector<std::string> &frames) {
  const std::string timeout_message =
      "no reply from " + _endpoint + " within " +
      std::to_string(_timeout.count()) +
      (_keys ? " ms (broker down, remote_edit disabled, or this editor's key "
               "is not authorized)"
             : " ms (broker not running on this host, or remote_edit "
               "disabled)");
  try {
    zmq::socket_t socket(*_context, zmq::socket_type::req);
    const int timeout = static_cast<int>(_timeout.count());
    socket.set(zmq::sockopt::linger, 0);
    socket.set(zmq::sockopt::rcvtimeo, timeout);
    socket.set(zmq::sockopt::sndtimeo, timeout);
    socket.set(zmq::sockopt::maxmsgsize, static_cast<int64_t>(kMaxMessageSize));
    if (_keys) {
      socket.set(zmq::sockopt::curve_serverkey, _keys->server_public_key);
      socket.set(zmq::sockopt::curve_publickey, _keys->public_key);
      socket.set(zmq::sockopt::curve_secretkey, _keys->secret_key);
    }
    socket.connect(_endpoint);

    zmq::multipart_t out;
    out.addstr(LIB_VERSION);
    for (const auto &frame : frames)
      out.addstr(frame);
    if (!out.send(socket))
      throw std::runtime_error(timeout_message);

    zmq::multipart_t in;
    if (!in.recv(socket) || in.size() < 2)
      throw std::runtime_error(timeout_message);
    std::vector<std::string> reply;
    for (std::size_t i = 0; i < in.size(); ++i)
      reply.push_back(in[i].to_string());
    return reply;
  } catch (const zmq::error_t &err) {
    if (err.num() == EAGAIN)
      throw std::runtime_error(timeout_message);
    throw std::runtime_error("edit connection to " + _endpoint + ": " +
                             err.what());
  }
}

namespace {

// Throws the server's message for an "error" reply, or a protocol error.
void expect_ok(const std::vector<std::string> &reply, std::size_t frames) {
  if (reply[1] == "error")
    throw std::runtime_error(reply.size() > 2 ? reply[2] : "remote error");
  if (reply[1] != "ok" || reply.size() != frames)
    throw std::runtime_error("unexpected reply from the edit service");
}

} // namespace

Loaded RemoteSource::load() {
  const auto reply = request({"get"});
  expect_ok(reply, 4);
  return {reply[3], reply[2]};
}

SaveResult RemoteSource::save(const std::string &text,
                              const std::string &base_etag) {
  SaveResult result;
  try {
    const auto reply = request({"put", base_etag, text});
    if (reply[1] == "ok" && reply.size() == 3) {
      result.status = SaveResult::Status::Ok;
      result.etag = reply[2];
    } else if (reply[1] == "conflict" && reply.size() == 4) {
      result.status = SaveResult::Status::Conflict;
      result.etag = reply[2]; // the current etag; the text is not kept
      result.message = _endpoint + " changed since it was loaded";
    } else if (reply[1] == "error") {
      result.message = reply.size() > 2 ? reply[2] : "remote error";
    } else {
      result.message = "unexpected reply from the edit service";
    }
  } catch (const std::exception &err) {
    result.status = SaveResult::Status::Error;
    result.message = err.what();
  }
  return result;
}

std::string RemoteSource::describe() const {
  return _endpoint + " (remote)";
}

std::string RemoteSource::info() {
  const auto reply = request({"info"});
  expect_ok(reply, 3);
  return reply[2];
}

} // namespace Mads::Ini::Remote
