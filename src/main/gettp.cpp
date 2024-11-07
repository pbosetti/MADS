#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "../httplib.h"
#include <iostream>
#include <string>
#include <thread>
#define GH_URL "https://api.github.com:443"
#define GH_PATH "/repos/pbosetti/MADS/releases/latest"
// #define GH_URL "tcp://localhost:80"
// #define GH_PATH "/"

using namespace std;

int main() {
  // HTTPS
  httplib::Client cli(GH_URL);

  auto res = cli.Get( GH_PATH);
  cout << "Status:" << res->status << endl;
  cout << "Body: " << res->body << endl;

  return 0;
}