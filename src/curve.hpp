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
using namespace zmqpp;
namespace fs = std::filesystem;

namespace mads {

enum class auth_verbose { off, on };

/**
 * Set up a ZAP authentication engine for the given context.
 * This will allow or deny incoming connections based on the given
 * list of allowed IP addresses.
 * 
 * \param authenticator the zmqpp::auth object to configure
 * \param ips a vector of allowed IP addresses
 * \param verbose whether to enable verbose logging
 */
void setup_auth(auth &authenticator, vector<string> const &ips,
                auth_verbose verbose = auth_verbose::off) {

  // Get some indication of what the authenticator is deciding
  authenticator.set_verbose(verbose == auth_verbose::on);
  authenticator.configure_domain("*");

  // Whitelist our address; any other address will be rejected
  for (const auto &ip : ips) {
    authenticator.allow(ip);
  }
}

/**
 * Set up CURVE security for a server socket.
 * This will read the server's keypair from files and configure
 * the authenticator to accept clients with the given public keys.
 * 
 * \param authenticator the zmqpp::auth object to configure
 * \param socket the zmqpp::socket to secure
 * \param key_dir the directory where the key files are stored
 * \param name the base name of the server key files (.key and .pub)
 * \param client_keys a vector of client public key base names (.pub files)
 */
void setup_curve_server(auth &authenticator, socket &socket,
                        fs::path const &key_dir, string const &key_name,
                        vector<string> &client_keys) {
  // We read the server certificates from files generated previously
  curve::keypair server_keypair;
  string name = "";
  try {
    name = (key_dir / (key_name + ".pub")).string();
    ifstream server_pub_file(name);
    if (!server_pub_file.is_open()) {
      throw runtime_error(string("cannot open server public key file ") + name);
    }
    getline(server_pub_file, server_keypair.public_key);
    server_pub_file.close();
    
    name = (key_dir / (key_name + ".key")).string();
    ifstream server_key_file(name);
    if (!server_key_file.is_open()) {
      throw runtime_error(string("cannot open server secret key file ") + name);
    }
    getline(server_key_file, server_keypair.secret_key);
    server_key_file.close();
  } catch (const std::exception &e) {
    throw runtime_error(e.what());
  }
  for (const auto &client_key_name : client_keys) {
    name = (key_dir / (client_key_name + ".pub")).string();
    ifstream client_pub_file(name);
    if (!client_pub_file.is_open()) {
      throw runtime_error("cannot open client public key file " + name);
    }
    string client_key;
    getline(client_pub_file, client_key);
    client_pub_file.close();
    authenticator.configure_curve(client_key);
  }
  socket.set(socket_option::identity, "IDENT");
  int as_server = 1;
  socket.set(socket_option::curve_server, as_server);
  socket.set(socket_option::curve_secret_key, server_keypair.secret_key);
}

/**
 * Set up CURVE security for a client socket.
 * This will read the client's keypair from files and configure
 * the socket to use the server's public key.
 * 
 * \param socket the zmqpp::socket to secure
 * \param key_dir the directory where the key files are stored
 * \param client_name the base name of the client key files (.key and .pub)
 * \param server_name the base name of the server public key file (.pub)
 */
void setup_curve_client(socket &socket, fs::path const &key_dir,
                        string const &client_name, string const &server_name) {
  // We load the certificates from files generated previously
  curve::keypair client_keypair;
  string server_public_key;
  try {
    string name = (key_dir / (client_name + ".pub")).string();
    ifstream client_pub_file(name);
    if (!client_pub_file.is_open()) {
      throw runtime_error(string("cannot open client public key file ") + name);
    }
    getline(client_pub_file, client_keypair.public_key);
    client_pub_file.close();

    name = (key_dir / (server_name + ".pub")).string();
    ifstream server_pub_file(name);
    if (!server_pub_file.is_open()) {
      throw runtime_error(string("cannot open server public key file ") + name);
    }
    getline(server_pub_file, server_public_key);
    server_pub_file.close();

    name = (key_dir / (client_name + ".key")).string();
    ifstream client_key_file(name);
    if (!client_key_file.is_open()) {
      throw runtime_error(string("cannot open client private key file ") + name);
    }
    getline(client_key_file, client_keypair.secret_key);
    client_key_file.close();
  } catch (const std::exception &e) {
    throw runtime_error(e.what());
  }

  socket.set(socket_option::curve_public_key, client_keypair.public_key);
  socket.set(socket_option::curve_secret_key, client_keypair.secret_key);
  socket.set(socket_option::curve_server_key, server_public_key);
}

} // namespace mads