/*
    ____ _   _ ______     _______               _   _
  / ___| | | |  _ \ \   / / ____|   __ _ _   _| |_| |__
 | |   | | | | |_) \ \ / /|  _|    / _` | | | | __| '_ \
 | |___| |_| |  _ < \ V / | |___  | (_| | |_| | |_| | | |
  \____|\___/|_| \_\ \_/  |_____|  \__,_|\__,_|\__|_| |_|

ZMQ sockets: ZAP and CURVE helper functions
Copyright (C) 2025 Paolo Bosetti
*/

#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#endif
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <set>
#include <zmq.hpp>

#include "zap_auth.hpp"

using namespace std;
namespace fs = std::filesystem;

namespace Mads {

enum class auth_verbose { off, on };

class CurveAuth {
public:
  /**
   * @brief  Constructs a CurveAuth object for managing CURVE authentication.
   *
   * @param context The zmq::context_t to use for authentication.
   **/
  CurveAuth(zmq::context_t & context) : _authenticator(context) {}
  ~CurveAuth() = default;

  /**
   * @brief Sets up ZAP authentication with the specified verbosity.
   *
   * Also setups the allowed IP addresses for authentication, reading from
   * the allowed_ips member variable.
   *
   * @param verbose Whether to enable verbose logging.
  */
  void setup_auth(auth_verbose verbose = auth_verbose::off) {
    // Get some indication of what the authenticator is deciding
    _authenticator.set_verbose(verbose == auth_verbose::on);
    _authenticator.configure_domain("*");

    // Whitelist our address; any other address will be rejected
    for (const auto &ip : allowed_ips) {
      _authenticator.allow(ip);
    }
    // Binds inproc://zeromq.zap.01 before returning, so no socket configured
    // after this point can complete a handshake unauthenticated.
    _authenticator.start();
  }

  /**
   * @brief Fetches client public keys from the specified directory.
   *
   * Scans the given directory for files with .pub extension and stores
   * the client key names (without extension) in the _client_keys member variable.
   *
   * @param key_dir The directory to scan for client public keys.
   * @throws runtime_error if no client public keys are found.
  */
  void fetch_public_keys(fs::path const &key_dir) {
    _key_dir = key_dir;
    _client_keys.clear();
    if (!fs::exists(key_dir) || !fs::is_directory(key_dir)) {
      throw runtime_error("Key directory does not exist: " + key_dir.string());
    }
    for (const auto &entry : fs::directory_iterator(key_dir)) {
      if (entry.path().extension() == ".pub") {
        _client_keys.push_back(entry.path().stem().string());
      }
    }
    if (_client_keys.empty()) {
      throw runtime_error("No client public keys found in " + key_dir.string());
    }
  }

  /**
   * @brief Reads the server keypair `<key_name>.pub` / `<key_name>.key` (first
   * line of each) from the key directory.
   *
   * Used by setup_curve_server() and by sockets that need the same CURVE
   * identity without joining the bus allowlist (e.g. the remote-edit service).
   *
   * @param key_name The base name of the server key files.
   * @throws runtime_error if the key directory is unset or a file cannot be
   * opened.
  */
  CurveKeypair load_server_keypair(string const &key_name) const {
    if (_key_dir.empty()) {
      throw runtime_error("Key directory not set. Call fetch_public_keys() first.");
    }
    CurveKeypair server_keypair;
    string name = (_key_dir / (key_name + ".pub")).string();
    ifstream server_pub_file(name);
    if (!server_pub_file.is_open()) {
      throw runtime_error(string("cannot open server public key file ") + name + " from " + _key_dir.string());
    }
    getline(server_pub_file, server_keypair.public_key);
    name = (_key_dir / (key_name + ".key")).string();
    ifstream server_key_file(name);
    if (!server_key_file.is_open()) {
      throw runtime_error(string("cannot open server secret key file ") + name + " from " + _key_dir.string());
    }
    getline(server_key_file, server_keypair.secret_key);
    return server_keypair;
  }

  /**
   * @brief The Z85 public keys the bus accepts: the first line of every
   * `*.pub` found by fetch_public_keys().
   *
   * @throws runtime_error if a key file cannot be opened.
  */
  set<string> client_public_keys() const {
    set<string> keys;
    for (const auto &client_key_name : _client_keys) {
      string name = (_key_dir / (client_key_name + ".pub")).string();
      ifstream f(name);
      if (!f.is_open()) {
        throw runtime_error("cannot open client public key file " + name);
      }
      string key;
      getline(f, key);
      keys.insert(key);
    }
    return keys;
  }

  /**
   * @brief Gives a ZAP domain its own exclusive set of allowed client keys
   * (pass-through to ZapAuth::configure_domain_curve()). Safe after
   * setup_auth(); must be called before binding a socket that uses the domain.
   *
   * @param domain The ZAP domain (ZMQ_ZAP_DOMAIN of the server socket).
   * @param z85_keys Allowed client public keys, Z85-encoded.
  */
  void configure_domain_curve(string const &domain, set<string> const &z85_keys) {
    _authenticator.configure_domain_curve(domain, z85_keys);
  }

  /**
   * @brief Sets up CURVE security for a server socket.
   *
   * Reads the server's keypair from files and configures
   * the authenticator to accept clients with the given public keys.
   *
   * @param socket The zmq::socket_t to secure.
   * @param key_name The base name of the server key files (.key and .pub).
   * @throws runtime_error if key files cannot be opened or read.
  */
  void setup_curve_server(zmq::socket_t &socket, string const &key_name) {
    CurveKeypair server_keypair = load_server_keypair(key_name);
    for (const auto &client_key_name : _client_keys) {
      string name = (_key_dir / (client_key_name + ".pub")).string();
      ifstream client_pub_file(name);
      if (!client_pub_file.is_open()) {
        throw runtime_error("cannot open client public key file " + name + " from " + _key_dir.string());
      }
      string client_key;
      getline(client_pub_file, client_key);
      client_pub_file.close();
      _authenticator.configure_curve(client_key);
    }
    socket.set(zmq::sockopt::routing_id, "IDENT");
    socket.set(zmq::sockopt::curve_server, 1);
    socket.set(zmq::sockopt::curve_secretkey, server_keypair.secret_key);
  }

  /**
   * @brief Sets up CURVE security for a client socket.
   *
   * Reads the client's keypair from files and configures
   * the socket to use the server's public key.
   *
   * @param socket The zmq::socket_t to secure.
   * @param client_name The base name of the client key files (.key and .pub).
   * @param server_name The base name of the server public key file (.pub).
   * @throws runtime_error if key files cannot be opened or read.
  */
  void setup_curve_client(zmq::socket_t &socket, string const &client_name, string const &server_name) {
    // We load the certificates from files generated previously
    try {
      string name = (_key_dir / (client_name + ".pub")).string();
      ifstream client_pub_file(name);
      if (!client_pub_file.is_open()) {
        throw runtime_error(string("cannot open client public key file ") + name + " from " + _key_dir.string());
      }
      getline(client_pub_file, _client_keypair.public_key);
      client_pub_file.close();

      name = (_key_dir / (server_name + ".pub")).string();
      ifstream server_pub_file(name);
      if (!server_pub_file.is_open()) {
        throw runtime_error(string("cannot open server public key file ") + name + " from " + _key_dir.string());
      }
      getline(server_pub_file, _server_public_key);
      server_pub_file.close();

      name = (_key_dir / (client_name + ".key")).string();
      ifstream client_key_file(name);
      if (!client_key_file.is_open()) {
        throw runtime_error(string("cannot open client private key file ") + name + " from " + _key_dir.string());
      }
      getline(client_key_file, _client_keypair.secret_key);
      client_key_file.close();
    } catch (const std::exception &e) {
      throw runtime_error(e.what());
    }

    setup_curve_client(socket);
  }

  /**
   * @brief Sets up CURVE security for a client socket using raw keys.
   *
   * Configures the socket to use the provided public and secret keys.
   *
   * @param socket The zmq::socket_t to secure.
   */
  void setup_curve_client(zmq::socket_t &socket) {
    if (_client_keypair.public_key.empty() || _client_keypair.secret_key.empty() || _server_public_key.empty()) {
      throw runtime_error("Client or server keys not set. Call setup_curve_client() or set them individually first.");
    }
    socket.set(zmq::sockopt::curve_publickey, _client_keypair.public_key);
    socket.set(zmq::sockopt::curve_secretkey, _client_keypair.secret_key);
    socket.set(zmq::sockopt::curve_serverkey, _server_public_key);
  }

  void set_key_dir(fs::path const &key_dir) {
    _key_dir = key_dir;
    if (!fs::exists(_key_dir) || !fs::is_directory(_key_dir)) {
      throw runtime_error("Key directory does not exist: " + _key_dir.string());
    }
  }

  void set_client_public_key(string const &key) {
    _client_keypair.public_key = key;
  }

  string client_public_key() const {
    return _client_keypair.public_key;
  }

  void set_client_secret_key(string const &key) {
    _client_keypair.secret_key = key;
  }

  string client_secret_key() const {
    return _client_keypair.secret_key;
  }

  void set_server_public_key(string const &key) {
    _server_public_key = key;
  }

  string server_public_key() const {
    return _server_public_key;
  }

  // ZapAuth &get_authenticator() { return _authenticator; }

  vector<string> allowed_ips{};
private:
  ZapAuth _authenticator;
  vector<string> _client_keys{};
  fs::path _key_dir;
  CurveKeypair _client_keypair = {"", ""};
  string _server_public_key = "";
};



} // namespace mads
