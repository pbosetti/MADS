/*
  __  __       _                _             _       
 |  \/  | __ _| | _____   _ __ | |_   _  __ _(_)_ __  
 | |\/| |/ _` | |/ / _ \ | '_ \| | | | |/ _` | | '_ \ 
 | |  | | (_| |   <  __/ | |_) | | |_| | (_| | | | | |
 |_|  |_|\__,_|_|\_\___| | .__/|_|\__,_|\__, |_|_| |_|
                         |_|            |___/         
Plugin maker: creates stub files for developing a new MADS plugin
*/
#include <iostream>
#include <algorithm>
#include <inja/inja.hpp>
#include <cxxopts.hpp>
#include <filesystem>
#include <rang.hpp>
#include "../exec_path.hpp"
#include "../mads.hpp"

using namespace std;
using namespace inja;
using json = nlohmann::json;
using namespace cxxopts;
using namespace rang;

static const vector<string> types = {"source", "sink", "filter"};

/*
  _   _ _   _ _ _ _            __                  _   _                 
 | | | | |_(_) (_) |_ _   _   / _|_   _ _ __   ___| |_(_) ___  _ __  ___ 
 | | | | __| | | | __| | | | | |_| | | | '_ \ / __| __| |/ _ \| '_ \/ __|
 | |_| | |_| | | | |_| |_| | |  _| |_| | | | | (__| |_| | (_) | | | \__ \
  \___/ \__|_|_|_|\__|\__, | |_|  \__,_|_| |_|\___|\__|_|\___/|_| |_|___/
                      |___/                                              
*/
static string lowercase(string str) {
  transform(str.begin(), str.end(), str.begin(), [](unsigned char c){ 
    return std::tolower(c); 
  });
  return str;
}
static string uppercase(string str) {
  transform(str.begin(), str.end(), str.begin(), [](unsigned char c){ 
    return std::toupper(c); 
  });
  return str;
}

static string ucfirst(string str) {
  str[0] = toupper(str[0]);
  return str;
}



/*
  __  __       _       
 |  \/  | __ _(_)_ __  
 | |\/| |/ _` | | '_ \ 
 | |  | | (_| | | | | |
 |_|  |_|\__,_|_|_| |_|
                       
*/

int main(int argc, char **argv) {
  json data;
  string str;
  string dir = "plugins/";
  auto exec_path = Mads::exec_path();
  auto template_dir = Mads::exec_dir("../share/templates/");
  bool overwrite = false;
  string cli{argv[0]}; // Name of the executable, used
  for (int i = 1; i < argc; i++) {
    cli += " ";
    cli += argv[i];
  }

  Options options(argv[0]);

  options.add_options()
    ("n,name", "Name of the plugin", value<string>())
    ("t,type", "Type of the plugin", value<string>())
    ("d,dir", "Directory of the plugin", value<string>())
    ("i,install-dir", "Directory where to install the plugin", value<string>())
    ("o,overwrite", "Overwrite existing files")
    ("v,version", "Print version")
    ("h,help", "Print usage");
  options.parse_positional({"name"});
  options.positional_help("name");

  auto options_parsed = options.parse(argc, argv);

  if (options_parsed.count("version") > 0) {
    cout << Mads::version() << endl;
    exit(0);
  }

  if (options_parsed.count("help") > 0) {
    std::cout << options.help() << endl;
    exit(0);
  }

  data["name"] = "example";
  data["type"] = "source";

  if (options_parsed.count("name") == 0) {
    cerr << fg::red << "No plugin name provided" << fg::reset << endl;
    exit(1);
  } else {
    data["name"] = lowercase(options_parsed["name"].as<string>());
    if (options_parsed.count("name") > 1) 
      cerr << fg::yellow << "Warning: multiple names provided, using " 
           << data["name"] << fg::reset << endl;
  }
  if (options_parsed.count("type") > 0) {
    data["type"] = lowercase(options_parsed["type"].as<string>());
    if (options_parsed.count("type") > 1) 
      cerr << fg::yellow << "Warning: multiple types provided, using " 
           << data["type"] << fg::reset << endl;
  }

  if (find(types.begin(), types.end(), string(data["type"])) == types.end()) {
    cerr << fg::red << "Invalid plugin type: " << data["type"] << fg::reset 
         << endl;
    exit(1);
  }

  if (options_parsed.count("dir") > 0) {
    dir = options_parsed["dir"].as<string>() + "/";
  }

  if (options_parsed.count("install-dir") > 0) {
    data["install_dir"] = options_parsed["install-dir"].as<string>();
  } else {
    data["install_dir"] = "/usr/local";
  }

  if (options_parsed.count("overwrite") > 0) {
    overwrite = true;
  }

  data["class_name"] = ucfirst(data["name"]) + "Plugin";
  data["parent_header"] = string(data["type"]) + ".hpp";
  data["source_template"] = string(data["type"]) + ".cpp";
  data["type"] = ucfirst(data["type"]);
  data["parent"] = ucfirst(data["type"]);
  data["driver_name"] = uppercase(data["type"]);
  data["source_file"] = string(data["name"]) + ".cpp";
  data["mads_version"] = Mads::version();
  data["cli"] = cli;
  data["today"] = Mads::get_ISODate_time(chrono::system_clock::now());
  data["cwd"] = filesystem::current_path().string();
  char hostname[HOST_NAME_MAX] = "unknown";
  if (gethostname(hostname, HOST_NAME_MAX)) {
    data["hostname"] = "unknown";
  } else {
    data["hostname"] = hostname;
  }


  filesystem::create_directory(dir);
  filesystem::create_directory(dir + "src/");
  Environment env_cmake{template_dir + "/", dir};
  Environment env_src{template_dir + "/", dir + "src/"};

  string cmake_file = dir + "CMakeLists.txt";
  string source_file = dir + "src/" + string(data["source_file"]);
  
  cout << "Creating plugin " << style::bold << data["name"] << style::reset 
       <<" of type " << style::bold << data["type"] << " in " 
       << style::bold << dir << style::reset << endl;
       
  cout << "==> " << style::bold << cmake_file << style::reset << ": ";
  if (!overwrite && filesystem::exists(cmake_file)) {
    cout << fg::red << " already exists, skipped; use -o to overwrite" 
    << fg::reset << endl;
  } else {
    env_cmake.write("CMakeLists.txt", data, "CMakeLists.txt");
    cout << fg::green << "created" << fg::reset << endl;
  }

  cout << "==> " << style::bold << source_file << style::reset << ": ";
  if (!overwrite && filesystem::exists(source_file)) {
    cerr << fg::red << " already exists, skipped; use -o to overwrite" 
    << fg::reset << endl;
  } else {
    env_src.write(data["source_template"], data, data["source_file"]);
    cout << fg::green << "created" << fg::reset << endl;
  }

  cout << "To build: "  << style::bold << "cd " << dir 
       << " && cmake -Bbuild && cmake --build build" << style::reset << endl;

  return 0;
}