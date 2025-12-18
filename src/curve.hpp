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
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <zmqpp/zmqpp.hpp>
#include <zmqpp/curve.hpp>

using namespace std;
namespace fs = std::filesystem;

namespace Mads {

enum class auth_verbose { off, on };

class CurveAuth {
public:
  /**
   * @brief  Constructs a CurveAuth object for managing CURVE authentication.
   * 
   * @param context The zmqpp::context to use for authentication.
   **/
  CurveAuth(zmqpp::context & context) : _authenticator(context) {}
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
   * @brief Sets up CURVE security for a server socket.
   * 
   * Reads the server's keypair from files and configures
   * the authenticator to accept clients with the given public keys.
   * 
   * @param socket The zmqpp::socket to secure.
   * @param key_name The base name of the server key files (.key and .pub).
   * @throws runtime_error if key files cannot be opened or read.
  */
  void setup_curve_server(zmqpp::socket &socket, string const &key_name) {
    if (_key_dir.empty()) {
      throw runtime_error("Key directory not set. Call fetch_public_keys() first.");
    }
    // We read the server certificates from files generated previously
    zmqpp::curve::keypair server_keypair;
    string name = "";
    try {
      name = (_key_dir / (key_name + ".pub")).string();
      ifstream server_pub_file(name);
      if (!server_pub_file.is_open()) {
        throw runtime_error(string("cannot open server public key file ") + name + " from " + _key_dir.string());
      }
      getline(server_pub_file, server_keypair.public_key);
      server_pub_file.close();

      name = (_key_dir / (key_name + ".key")).string();
      ifstream server_key_file(name);
      if (!server_key_file.is_open()) {
        throw runtime_error(string("cannot open server secret key file ") + name + " from " + _key_dir.string());
      }
      getline(server_key_file, server_keypair.secret_key);
      server_key_file.close();
    } catch (const std::exception &e) {
      throw runtime_error(e.what());
    }
    for (const auto &client_key_name : _client_keys) {
      name = (_key_dir / (client_key_name + ".pub")).string();
      ifstream client_pub_file(name);
      if (!client_pub_file.is_open()) {
        throw runtime_error("cannot open client public key file " + name + " from " + _key_dir.string());
      }
      string client_key;
      getline(client_pub_file, client_key);
      client_pub_file.close();
      _authenticator.configure_curve(client_key);
    }
    socket.set(zmqpp::socket_option::identity, "IDENT");
    int as_server = 1;
    socket.set(zmqpp::socket_option::curve_server, as_server);
    socket.set(zmqpp::socket_option::curve_secret_key, server_keypair.secret_key);
  }

  /** 
   * @brief Sets up CURVE security for a client socket.
   * 
   * Reads the client's keypair from files and configures
   * the socket to use the server's public key.
   * 
   * @param socket The zmqpp::socket to secure.
   * @param client_name The base name of the client key files (.key and .pub).
   * @param server_name The base name of the server public key file (.pub).
   * @throws runtime_error if key files cannot be opened or read.
  */
  void setup_curve_client(zmqpp::socket &socket, string const &client_name, string const &server_name) {
    // We load the certificates from files generated previously
    zmqpp::curve::keypair client_keypair;
    string server_public_key;
    try {
      string name = (_key_dir / (client_name + ".pub")).string();
      ifstream client_pub_file(name);
      if (!client_pub_file.is_open()) {
        throw runtime_error(string("cannot open client public key file ") + name + " from " + _key_dir.string());
      }
      getline(client_pub_file, client_keypair.public_key);
      client_pub_file.close();

      name = (_key_dir / (server_name + ".pub")).string();
      ifstream server_pub_file(name);
      if (!server_pub_file.is_open()) {
        throw runtime_error(string("cannot open server public key file ") + name + " from " + _key_dir.string());
      }
      getline(server_pub_file, server_public_key);
      server_pub_file.close();

      name = (_key_dir / (client_name + ".key")).string();
      ifstream client_key_file(name);
      if (!client_key_file.is_open()) {
        throw runtime_error(string("cannot open client private key file ") + name + " from " + _key_dir.string());
      }
      getline(client_key_file, client_keypair.secret_key);
      client_key_file.close();
    } catch (const std::exception &e) {
      throw runtime_error(e.what());
    }

    socket.set(zmqpp::socket_option::curve_public_key, client_keypair.public_key);
    socket.set(zmqpp::socket_option::curve_secret_key, client_keypair.secret_key);
    socket.set(zmqpp::socket_option::curve_server_key, server_public_key);
  }

  void set_key_dir(fs::path const &key_dir) {
    _key_dir = key_dir;
    if (!fs::exists(_key_dir) || !fs::is_directory(_key_dir)) {
      throw runtime_error("Key directory does not exist: " + _key_dir.string());
    }
  }

  // zmqpp::auth &get_authenticator() { return _authenticator; }
  
  vector<string> allowed_ips{};
private:
  zmqpp::auth _authenticator;
  vector<string> _client_keys{};
  fs::path _key_dir;
};



} // namespace mads