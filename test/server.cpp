#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <csignal>
#include <zmqpp/zmqpp.hpp>
#include <zmqpp/curve.hpp>
#include "curve.hpp"

using namespace std;

#define CURVE_AUTH

bool Running = true;

int main(int argc, char *argv[]) {
  // initialize the 0MQ context
  zmqpp::context context;
  ::signal(SIGTERM, [](int signum) {
    cout << "Received signal " << signum << ", exiting." << endl;
    Running = false;
  });

zmqpp::socket subscriber(context, zmqpp::socket_type::sub);

#ifdef CURVE_AUTH
  vector<string> allowed_ips = {"127.0.0.1"};
  vector<string> client_keys = {"client", "server", "mads"};
  auth authenticator(context);
  mads::setup_auth(authenticator, allowed_ips, mads::auth_verbose::on);
  mads::setup_curve_server(authenticator, subscriber, ".", "server", client_keys);
#endif

  subscriber.bind("tcp://*:9000");
  subscriber.subscribe("test");
  subscriber.set(zmqpp::socket_option::receive_timeout, 100);

  zmqpp::message response;
  string msg_content;
  string topic;

  while (Running) {
    if (subscriber.receive(response)) {
      response >> topic >> msg_content;
      cout << "Topic: " << topic << endl;
      cout << "Message Content: " << msg_content << endl;
    }
  }

  subscriber.unbind("tcp://*:9000");
  subscriber.close();

  return 0;
}