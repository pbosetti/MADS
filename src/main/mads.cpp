

#include <iostream>
#include <inja/inja.hpp>
#include <cxxopts.hpp>
#include <filesystem>
#include <rang.hpp>
#include "../mads.hpp"
#include "../exec_path.hpp"

#define SYSTEMD_PATH "/etc/systemd/system"

using namespace std;
using namespace inja;
using json = nlohmann::json;
using namespace cxxopts;
using namespace rang;


// const vector<string> ext_commands = {
//   "broker", "logger",
//   "source", "filter", "sink",
//   "dealer", "worker",
//   "command", "logging",
//   "plugin",
// };


bool includes(vector<string> const &commands, string const &command) {
  return find(commands.begin(), commands.end(), command) != commands.end();
}

int make_ini(int argc, char **argv) {
  string template_dir = Mads::exec_dir("../share/templates/");
  string etc_dir = Mads::exec_dir("../etc/");
  string output = "";
  json data;
  data["broker"] = "localhost";
  data["port_frontend"] = "9090";
  data["port_backend"] = "9091";
  data["port_settings"] = "9092";
  data["mongo_uri"] = "mongodb://localhost:27017";
  Options options("mads ini", 
    "Create INI file template, version " + Mads::version());
  options.add_options()
    ("o,output", "Output file", value<string>())
    ("i,install", "Install INI file to " + etc_dir, value<bool>())
    ("b,broker", "broker hostname or IP", value<string>())
    ("F,frontend", "frontend port", value<int>())
    ("B,backend", "backend port", value<int>())
    ("s,settings", "settings port", value<int>())
    ("f,fps", "Frames per second", value<int>())
    ("m,mongo", "MongoDB URI", value<string>())
    ("h,help", "Print help");
  ParseResult options_parsed;
  try {
    options_parsed = options.parse(argc, argv);
  } catch (const std::exception &e) {
    cerr << fg::red << "Unknown CLI option" << fg::reset << '\n';
    cerr << options.help() << endl;
    return -1;
  }
  if (options_parsed.count("help")) {
    cout << options.help() << endl;
    return 0;
  }
  if (options_parsed.count("output")) {
    if (options_parsed.count("install")) {
      cerr << fg::red << "Cannot install and write to file at the same time"
           << fg::reset << endl;
      return -1;
    }
    output = options_parsed["output"].as<string>();
  }
  if (options_parsed.count("broker")) {
    data["broker"] = options_parsed["broker"].as<string>();
  }
  if (options_parsed.count("frontend")) {
    data["port_frontend"] = options_parsed["frontend"].as<int>();
  }
  if (options_parsed.count("backend")) {
    data["port_backend"] = options_parsed["backend"].as<int>();
  }
  if (options_parsed.count("settings")) {
    data["port_settings"] = options_parsed["settings"].as<int>();
  }
  if (options_parsed.count("fps")) {
    data["fps"] = options_parsed["fps"].as<int>();
  }
  if (options_parsed.count("mongo")) {
    data["mongo_uri"] = options_parsed["mongo"].as<string>();
  }
  if (output != "") {
    Environment env{template_dir + "/", "./"};
    env.write("mads.ini", data, output);
    cout << fg::green << "INI file written to " << output << fg::reset << endl;
    return 0;
  }
  if (options_parsed.count("install")) {
    if (!filesystem::exists(etc_dir)) {
      try {
        filesystem::create_directory(etc_dir);
      } catch (const filesystem::filesystem_error &e) {
        cerr << fg::red << "Cannot create directory " << etc_dir 
             << " (need sudo?)"<< fg::reset << endl;
        return -1;
      }      
    }
    Environment env{template_dir + "/", etc_dir + "/"};
    env.write("mads.ini", data, "mads.ini");
    cout << fg::green << "INI file installed to " << etc_dir << "mads.ini" 
         << fg::reset << endl;
    return 0;
  }
  Environment env{template_dir + "/", "."};
  cout << env.render_file("mads.ini", data) << endl;
  return 0;
}

int make_service(int argc, char **argv) {
  auto template_dir = Mads::exec_dir("../share/templates/");
  string this_exe = Mads::exec_path().stem().string();
  int start = 1;
  json data;
  string command_line = Mads::exec_dir() + "/";
  string args = "";
  if (argc < 3) {
    cerr << fg::red << "No enough arguments provided" << fg::reset << endl;
    return -1;
  }
  for (int i = 1; i < argc; i++) {
    args += argv[i];
    args += " ";
  }
  data["service_name"] = string(MADS_PREFIX) + argv[1];
  argv++;
  argc--;
  if (argc < 2) {
    cerr << fg::red << "No service name provided" << fg::reset << endl;
    return -1;
  }
  if (strncmp(argv[1], MADS_PREFIX, strlen(MADS_PREFIX)) == 0) {
    argv++;
    argc--;
    data["name"] = argv[0];
  } else if (this_exe == argv[1]) {
    argv += 2;
    argc -= 2;
    command_line += MADS_PREFIX;
    data["name"] = string(MADS_PREFIX) + argv[0];
  } else {
    argv++;
    argc--;
    data["name"] = string(MADS_PREFIX) + argv[0];
    command_line += MADS_PREFIX;
  }
  for (int i = 0; i < argc; i++) {
    command_line += argv[i];
    command_line += " ";
  }
  data["command"] = command_line;
  data["systemd_path"] = SYSTEMD_PATH;
  data["ini_file"] = Mads::exec_dir("../etc/mads.ini");
  data["this_exe"] = this_exe;
  data["args"] = args;
  Environment env{template_dir + "/", "."};
  if (getuid() == 0) {
    string dest = string(SYSTEMD_PATH) + "/" + data["service_name"].get<string>() + ".service";
    ofstream file(dest);
    file << env.render_file("service.tpl", data) << endl;
    file.close();
    cout << fg::green << "Service file written to " << dest << fg::reset 
         << endl;
    cout << "Start it with " << style::bold << "sudo systemctl start " 
         << data["service_name"].get<string>() << style::reset << endl;
    cout << "Enable it permanently with " << style::bold 
         << "sudo systemctl enable " << data["service_name"].get<string>() 
         << style::reset << endl;
  } else {
    cout << env.render_file("service.tpl", data) << endl;
  }
  return 0;
}



int main(int argc, char **argv) {
  string exec_dir = Mads::exec_dir();
  auto template_dir = Mads::exec_dir("../share/templates/");

  vector<string> ext_commands;
  for (auto const &item: filesystem::directory_iterator(exec_dir)) {
    string filename = item.path().stem().string();
    if (filename.substr(0, strlen(MADS_PREFIX)) == MADS_PREFIX) {
      ext_commands.push_back(filename.substr(strlen(MADS_PREFIX)));
    }
  }

  if (argc > 1) {
    if(includes(ext_commands, argv[1])) {
      string cmd = exec_dir + "/" + string(MADS_PREFIX) + argv[1];
      execv(cmd.c_str(), argv + 1);
    } else if (strncmp(argv[1], "ini", 3) == 0) {
      return make_ini(argc - 1, argv + 1);
    } 
#ifdef __linux__
    else if (strncmp(argv[1], "service", 7) == 0) {
      return make_service(argc - 1, argv + 1);
    }
#endif
  }

  Options options("mads", 
    "Mads command line interface version " + Mads::version());
  options.add_options()
    ("i,info", "Print information on MADS installation")
    ("v,version", "Print version")
    ("h,help", "Print help");
  ParseResult options_parsed;
  try {
    options_parsed = options.parse(argc, argv);
  } catch (const std::exception &e) {
    cerr << fg::red << "Unknown CLI option" << fg::reset << '\n';
    cerr << options.help() << endl;;
  }

  if (options_parsed.count("help")) {
    cout << options.help() << endl;
    return 0;
  }
  if (options_parsed.count("version")) {
    cout << Mads::version() << endl;
    return 0;
  }
  if (options_parsed.count("info")) {
    cout << "Mads version: " << style::bold << Mads::version() << style::reset
         << endl;
    cout << "Mads installation directory: " << style::bold << exec_dir
         << style::reset << endl;
    cout << "Mads template directory: " << style::bold << template_dir
         << style::reset << endl;
    cout << "Mads INI file: " << style::bold
         << Mads::exec_dir("../etc/mads.ini") << style::reset << endl;
    return 0;
  }
  cout << style::bold << "Available commands:" << style::reset << endl;
  for (auto const &cmd: ext_commands) {
    cout << setw(10) << cmd << style::italic << " (wraps " << MADS_PREFIX 
         << cmd << ")" << style::reset << endl;
  }
  cout << setw(10) << "ini" << style::italic << " (internal)"  
       << style::reset << endl;
  #ifdef __linux__
  cout << setw(10) << "service" << style::italic << " (internal)"  
       << style::reset << endl;
  #endif

  return 0;
}