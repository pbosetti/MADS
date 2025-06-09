#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <fstream>
#include <iostream>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/opencv.hpp>
#include <cxxopts.hpp>
#include <rang.hpp>
#include <chrono>

using namespace std;
using namespace std::chrono;
using namespace cv;
using namespace cxxopts;
using namespace rang;
using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_array;
using bsoncxx::builder::basic::make_document;

int main(int argc, char *argv[]) {
  mongocxx::instance inst{};
  mongocxx::client conn{mongocxx::uri{}};
  mongocxx::database db = conn["mads_test"];
  size_t i = 0, n = 0;
  mongocxx::uri uri{};
  string collection;

  Options options(argv[0]);
  options.add_options()
    ("collection", "Collection name", value<string>())
    ("d", "Database URI (default localhost)", value<string>());
  options.parse_positional({"collection"});
  options.positional_help("<collection>");
  auto options_parsed = options.parse(argc, argv);

  if (options_parsed.count("d")) {
    string db_uri = options_parsed["d"].as<string>();
    uri = mongocxx::uri{db_uri};
  }
  if (options_parsed.count("collection") == 0) {
    cout << options.help() << endl << "No collection specified" << endl;
    exit(1);
  }
  collection = options_parsed["collection"].as<string>();

  auto images = db[collection];

  bsoncxx::document::view_or_value filter =
      make_document(kvp("data", make_document(kvp("$exists", true))));
  n = images.count_documents(filter);
  cout << fg::green << "There are " << n << " blobs in database. " 
       << style::bold << "Type 's' to save, 'q' to quit" << fg::reset
       << style::reset << endl;
  auto cursor = images.find(filter);
  for (auto &&doc : cursor) {
    auto format = doc["message"]["format"].get_string().value;
    if (format != "jpg") {
      cout << fg::yellow << "Skipping non-jpeg image of type " 
           << format << fg::reset << endl;
      continue;
    }
    long tp = doc["message"]["timestamp"].get_date().value.count() / 1000;
    cout << "[" << i + 1 << "/" << n << "] timecode "
         << doc["message"]["timecode"].get_double().value << ", "
         << "timestamp " << put_time(localtime(&tp), "%FT%T") << ", "
         << doc["message"]["format"].get_string().value << endl;
    auto img = doc["data"].get_binary();
    cv::Mat raw_img = cv::Mat(1, img.size, CV_8UC1, (char *)img.bytes);
    cv::Mat decoded_img = cv::imdecode(raw_img, cv::IMREAD_UNCHANGED);
    if (decoded_img.empty()) {
      cout << fg::red << "Error while decoding image" << fg::reset << endl;
      continue;
    }
    cv::imshow("Image", decoded_img);
    char k = cv::waitKey(0);
    if (k == 'q') {
      break;
    } else if (k == 's') {
      string filename = "image_" + to_string(i) + ".png";
      cout << style::italic << "Saving image to " << filename << style::reset 
           << endl;
      ofstream fout;
      fout.open(filename, ios::binary | ios::out);
      fout.write((const char *)img.bytes, img.size);
      fout.close();
    }
    i++;
  }
  destroyAllWindows();
  return 0;
}