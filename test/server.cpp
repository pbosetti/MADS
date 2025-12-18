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
  Mads::CurveAuth curve_auth(context);
  curve_auth.allowed_ips.push_back("127.0.0.1");
  curve_auth.setup_auth(Mads::auth_verbose::on);
  curve_auth.fetch_public_keys(".");
  curve_auth.setup_curve_server(subscriber, "server");
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