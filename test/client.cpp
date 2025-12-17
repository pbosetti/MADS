#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <zmqpp/zmqpp.hpp>
#include <zmqpp/curve.hpp>
#include <csignal>
#include "curve.hpp"


using namespace std;

#define CURVE_AUTH

bool Running = true;

int main(int argc, char *argv[]) {
  string my_key = "client";
  if (argc > 1) {
    my_key = argv[1];
  }

  // initialize the 0MQ context
  zmqpp::context context;
  ::signal(SIGTERM, [](int signum) {
    cout << "Received signal " << signum << ", exiting." << endl;
    Running = false;
  });

  // create and connect a client socket
  zmqpp::socket publisher(context, zmqpp::socket_type::pub);

#ifdef CURVE_AUTH
  vector<string> allowed_ips = {"127.0.0.1"};
  auth authenticator(context);
  mads::setup_auth(authenticator, allowed_ips, mads::auth_verbose::on);
  mads::setup_curve_client(publisher, ".", my_key, "server");
#endif
  
  publisher.connect("tcp://127.0.0.1:9000");

  // Send a single message from server to publisher
  size_t i = 0;
  zmqpp::message request;
  while (Running) {
    request << "test"
            << "Hello " + to_string(i++);
    publisher.send(request);
    this_thread::sleep_for(chrono::milliseconds(100));
  }

  publisher.disconnect("tcp://127.0.0.1:9000");
  publisher.close();

  return 0;
}