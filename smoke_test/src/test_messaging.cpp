/*
  MADS Smoke Test: C++ Messaging Tests
  Tests blocking/non-blocking receive, queue sizes, and timeouts.
  
  Usage: test_messaging --test-case <case> --settings-uri <uri>
  Cases: nonblocking, blocking, lkv, queue, timeout
*/
#include <agent.hpp>
#include <mads.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <csignal>
#include <cstring>

using namespace std;
using namespace Mads;
using json = nlohmann::json;

static int test_nonblocking(const string &settings_uri) {
  // Non-blocking receive should return NONE when no message is available
  Agent agent("smoke_cpp", settings_uri);
  agent.set_settings_timeout(5000);
  agent.init();
  agent.connect();

  auto mt = agent.receive(true); // dont_block = true
  if (mt != message_type::none) {
    cerr << "FAIL: non-blocking receive should return none, got " 
         << static_cast<int>(mt) << endl;
    agent.disconnect();
    return 1;
  }
  cout << "PASS: non-blocking receive returns none when no messages" << endl;
  agent.disconnect();
  return 0;
}

static int test_blocking(const string &settings_uri) {
  // Start a publisher thread, then receive with blocking
  Agent pub_agent("smoke_cpp", settings_uri);
  pub_agent.set_settings_timeout(5000);
  pub_agent.init();
  pub_agent.connect();

  // Publish a message after a delay (long enough for subscriber to connect,
  // retrieve settings from broker, and establish ZMQ subscription)
  thread publisher([&]() {
    this_thread::sleep_for(chrono::milliseconds(2000));
    json msg = {{"test", "blocking"}, {"value", 42}};
    pub_agent.publish(msg);
  });

  // Create a subscriber to receive it
  Agent sub_agent("feedback", settings_uri);
  sub_agent.set_settings_timeout(5000);
  sub_agent.init();
  sub_agent.set_receive_timeout(5000);
  sub_agent.connect();

  auto mt = sub_agent.receive(false); // blocking
  publisher.join();

  if (mt == message_type::json) {
    auto [topic, payload] = sub_agent.last_message();
    cout << "PASS: blocking receive got message on topic '" << topic << "'" << endl;
    sub_agent.disconnect();
    pub_agent.disconnect();
    return 0;
  }
  cerr << "FAIL: blocking receive did not get json message, got " 
       << static_cast<int>(mt) << endl;
  sub_agent.disconnect();
  pub_agent.disconnect();
  return 1;
}

static int test_lkv(const string &settings_uri) {
  // With queue_size=1 (LKV mode), only the latest message should survive
  Agent pub_agent("smoke_cpp", settings_uri);
  pub_agent.set_settings_timeout(5000);
  pub_agent.init();
  pub_agent.connect();

  Agent sub_agent("feedback", settings_uri);
  sub_agent.set_settings_timeout(5000);
  sub_agent.init();
  sub_agent.set_high_watermark(1); // LKV mode
  sub_agent.set_receive_timeout(3000);
  sub_agent.connect();

  // Give subscription time to establish (ZMQ "slow joiner" problem;
  // needs more time on Windows where socket operations have higher latency)
  this_thread::sleep_for(chrono::milliseconds(1000));

  // Publish 10 messages rapidly
  for (int i = 0; i < 10; i++) {
    json msg = {{"seq", i}, {"test", "lkv"}};
    pub_agent.publish(msg);
  }

  // Delay for message delivery through the broker
  this_thread::sleep_for(chrono::milliseconds(1000));

  // Receive — in LKV mode we should get a message (likely the last one)
  auto mt = sub_agent.receive(false);
  if (mt == message_type::json) {
    auto [topic, payload] = sub_agent.last_message();
    auto j = json::parse(payload);
    int seq = j["seq"];
    cout << "PASS: LKV mode received message seq=" << seq << " (expected near 9)" << endl;
    sub_agent.disconnect();
    pub_agent.disconnect();
    return 0;
  }
  cerr << "FAIL: LKV mode did not receive a message" << endl;
  sub_agent.disconnect();
  pub_agent.disconnect();
  return 1;
}

static int test_queue(const string &settings_uri) {
  // With default queue (1000), multiple messages should be queued
  Agent pub_agent("smoke_cpp", settings_uri);
  pub_agent.set_settings_timeout(5000);
  pub_agent.init();
  pub_agent.connect();

  Agent sub_agent("feedback", settings_uri);
  sub_agent.set_settings_timeout(5000);
  sub_agent.init();
  sub_agent.set_high_watermark(1000);
  sub_agent.set_receive_timeout(3000);
  sub_agent.connect();

  // Give subscription time to establish (generous for cross-platform reliability)
  this_thread::sleep_for(chrono::milliseconds(1000));

  // Publish 5 messages
  for (int i = 0; i < 5; i++) {
    json msg = {{"seq", i}, {"test", "queue"}};
    pub_agent.publish(msg);
  }

  // Delay for message delivery through the broker
  this_thread::sleep_for(chrono::milliseconds(1000));

  // Receive multiple messages
  int count = 0;
  for (int i = 0; i < 5; i++) {
    auto mt = sub_agent.receive(true);
    if (mt == message_type::json) {
      count++;
    }
  }

  if (count >= 2) {
    cout << "PASS: queue mode received " << count << "/5 messages" << endl;
    sub_agent.disconnect();
    pub_agent.disconnect();
    return 0;
  }
  cerr << "FAIL: queue mode only received " << count << "/5 messages" << endl;
  sub_agent.disconnect();
  pub_agent.disconnect();
  return 1;
}

static int test_timeout(const string &settings_uri) {
  // Receive with a short timeout should return within that time
  // Use smoke_cpp (queue_size=1000) to avoid LKV mode which ignores socket timeout
  Agent agent("smoke_cpp", settings_uri);
  agent.set_settings_timeout(5000);
  agent.init();
  agent.set_receive_timeout(500); // 500ms timeout
  agent.connect();

  auto start = chrono::steady_clock::now();
  auto mt = agent.receive(false); // blocking, but should timeout
  auto elapsed = chrono::duration_cast<chrono::milliseconds>(
    chrono::steady_clock::now() - start
  ).count();

  agent.disconnect();

  if (mt == message_type::none && elapsed >= 400 && elapsed < 3000) {
    cout << "PASS: receive timed out after " << elapsed << "ms" << endl;
    return 0;
  }
  cerr << "FAIL: timeout test — mt=" << static_cast<int>(mt) 
       << " elapsed=" << elapsed << "ms" << endl;
  return 1;
}

int main(int argc, char *argv[]) {
  string test_case;
  string settings_uri = "tcp://localhost:19092";

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--test-case") == 0 && i + 1 < argc) {
      test_case = argv[++i];
    } else if (strcmp(argv[i], "--settings-uri") == 0 && i + 1 < argc) {
      settings_uri = argv[++i];
    }
  }

  if (test_case.empty()) {
    cerr << "Usage: " << argv[0] 
         << " --test-case <nonblocking|blocking|lkv|queue|timeout>"
         << " [--settings-uri <uri>]" << endl;
    return 1;
  }

  try {
    if (test_case == "nonblocking") return test_nonblocking(settings_uri);
    if (test_case == "blocking")    return test_blocking(settings_uri);
    if (test_case == "lkv")         return test_lkv(settings_uri);
    if (test_case == "queue")       return test_queue(settings_uri);
    if (test_case == "timeout")     return test_timeout(settings_uri);
    cerr << "Unknown test case: " << test_case << endl;
    return 1;
  } catch (const exception &e) {
    cerr << "EXCEPTION in test '" << test_case << "': " << e.what() << endl;
    return 1;
  }
}
