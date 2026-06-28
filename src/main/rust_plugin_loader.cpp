/*
  ____           _     ____  _             _         _                 _
 |  _ \ _   _ __| |_  |  _ \| |_   _  __ _(_)_ __   | | ___   __ _  __| | ___ _ __
 | |_) | | | / _` __| | |_) | | | | |/ _` | | '_ \  | |/ _ \ / _` |/ _` |/ _ \ '__|
 |  _ <| |_| \__ \ |_  |  __/| | |_| | (_| | | | | | | | (_) | (_| | (_| |  __/ |
 |_| \_\\__,_|___/\__| |_|   |_|\__,_|\__, |_|_| |_| |_|\___/ \__,_|\__,_|\___|_|
                                       |___/

 Dedicated plugin loader for Rust plugins.  Mirrors plugin_loader.cpp but uses
 the mads_rust_plugin_t C ABI (dlopen + dlsym) instead of pugg.

 Compiled three times:
   RUST_PLUGIN_LOADER_SOURCE  -> mads-rsource
   RUST_PLUGIN_LOADER_FILTER  -> mads-rfilter
   RUST_PLUGIN_LOADER_SINK    -> mads-rsink

 Author(s): Paolo Bosetti
*/
#include "../agent_app.hpp"
#include "../exec_path.hpp"
#include "../mads.hpp"
#include "mads_rust_plugin.h"
#include <chrono>
#include <cxxopts.hpp>
#include <dlfcn.h>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <regex>

#if defined(RUST_PLUGIN_LOADER_SOURCE)
#define PLUGIN_KIND      "source"
#define PLUGIN_NAME      "RustSource"
#define PLUGIN_DEFAULT   "publish.plugin"
#define AGENT_NAME_DEFAULT "rpublish"
#elif defined(RUST_PLUGIN_LOADER_FILTER)
#define PLUGIN_KIND      "filter"
#define PLUGIN_NAME      "RustFilter"
#define PLUGIN_DEFAULT   "bridge.plugin"
#define AGENT_NAME_DEFAULT "rbridge"
#elif defined(RUST_PLUGIN_LOADER_SINK)
#define PLUGIN_KIND      "sink"
#define PLUGIN_NAME      "RustSink"
#define PLUGIN_DEFAULT   "feedback.plugin"
#define AGENT_NAME_DEFAULT "rfeedback"
#else
#error "Define RUST_PLUGIN_LOADER_SOURCE, RUST_PLUGIN_LOADER_FILTER, or RUST_PLUGIN_LOADER_SINK"
#endif

using namespace std;
using namespace cxxopts;
using namespace Mads;
using json = nlohmann::json;
namespace fs = std::filesystem;

/* ── helpers ─────────────────────────────────────────────────────────────── */

json str_to_num(const string &s) {
  string str = s;
  auto ltrim = [](string &s) {
    s.erase(s.begin(),
            find_if(s.begin(), s.end(), [](unsigned char c) { return !isspace(c); }));
  };
  auto rtrim = [](string &s) {
    s.erase(find_if(s.rbegin(), s.rend(),
                    [](unsigned char c) { return !isspace(c); }).base(),
            s.end());
  };
  ltrim(str); rtrim(str);

  if (str == "true"  || str == "TRUE")  return true;
  if (str == "false" || str == "FALSE") return false;
  try { size_t p; long long v = stoll(str, &p); if (p == str.size()) return v; }
  catch (...) {}
  try { size_t p; double v = stod(str, &p);    if (p == str.size()) return v; }
  catch (...) {}
  return str;
}

/* Thin RAII wrapper: dlopen handle + vtable pointer + per-instance handle. */
struct RustPlugin {
  void            *dl_handle = nullptr;
  const mads_rust_plugin_t *fns = nullptr;
  void            *self   = nullptr;

  RustPlugin() = default;
  ~RustPlugin() {
    if (self && fns) fns->destroy(self);
    if (dl_handle)   dlclose(dl_handle);
  }
  RustPlugin(const RustPlugin &) = delete;
  RustPlugin &operator=(const RustPlugin &) = delete;

  bool load(const string &path, string &error_out) {
    dl_handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!dl_handle) { error_out = dlerror(); return false; }

    using RegisterFn = const mads_rust_plugin_t *(*)();
    auto reg = reinterpret_cast<RegisterFn>(
        dlsym(dl_handle, "mads_rust_plugin_register"));
    if (!reg) {
      error_out = string("symbol mads_rust_plugin_register not found: ") + dlerror();
      return false;
    }
    fns = reg();
    if (!fns) { error_out = "mads_rust_plugin_register returned NULL"; return false; }
    if (fns->version != MADS_RUST_PLUGIN_VERSION) {
      error_out = "ABI version mismatch: plugin=" + to_string(fns->version) +
                  " loader=" + to_string(MADS_RUST_PLUGIN_VERSION);
      return false;
    }
    if (string(fns->kind) != PLUGIN_KIND) {
      error_out = string("plugin kind '") + fns->kind + "' does not match loader kind '"
                  PLUGIN_KIND "'";
      return false;
    }
    self = fns->create();
    if (!self) { error_out = "plugin create() returned NULL"; return false; }
    return true;
  }

  /* convenience wrappers */
  void set_params(const json &j) {
    string s = j.dump();
    fns->set_params(self, s.c_str(), s.size());
  }
  json get_info() {
    const char *raw = fns->get_info(self);
    if (!raw || !*raw) return json::object();
    try { return json::parse(raw); } catch (...) { return json::object(); }
  }
  string error() const {
    const char *e = fns->last_error(self);
    return (e && *e) ? string(e) : string("unknown error");
  }
  json output_json() const {
    const char *p = fns->output_json(self);
    size_t      n = fns->output_json_len(self);
    if (!p || !n) return json::object();
    try { return json::parse(string_view(p, n)); } catch (...) { return json::object(); }
  }
  vector<unsigned char> output_blob() const {
    const uint8_t *p = fns->output_blob(self);
    size_t         n = fns->output_blob_len(self);
    if (!p || !n) return {};
    return vector<unsigned char>(p, p + n);
  }
  chrono::milliseconds next_duration() const {
    long ms = fns->next_loop_ms(self);
    return ms < 0 ? chrono::milliseconds(0) : chrono::milliseconds(ms);
  }

#if defined(RUST_PLUGIN_LOADER_SOURCE)
  string blob_format() const {
    if (!fns->blob_format) return "";
    const char *f = fns->blob_format(self);
    return (f && *f) ? string(f) : string("");
  }
  return_type get_output() {
    return static_cast<return_type>(fns->get_output(self));
  }
#endif

#if defined(RUST_PLUGIN_LOADER_FILTER) || defined(RUST_PLUGIN_LOADER_SINK)
  return_type load_data(const json &in, const string &topic,
                        const vector<unsigned char> *blob = nullptr) {
    string js = in.dump();
    const uint8_t *blob_ptr = blob && !blob->empty()
                              ? reinterpret_cast<const uint8_t *>(blob->data())
                              : nullptr;
    size_t blob_len = blob ? blob->size() : 0;
    return static_cast<return_type>(
        fns->load_data(self, js.c_str(), js.size(), topic.c_str(),
                       blob_ptr, blob_len));
  }
#endif

#if defined(RUST_PLUGIN_LOADER_FILTER)
  return_type process() {
    return static_cast<return_type>(fns->process(self));
  }
#endif
};

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
  string plugin_file = PLUGIN_DEFAULT;
  string agent_name  = AGENT_NAME_DEFAULT;
  size_t count = 0, count_err = 0;
  size_t delay = 0;
  bool   silent = false;

  AgentApp agent(argv[0], SETTINGS_URI);
  // clang-format off
  agent.options()
    ("plugin",   "Rust plugin (.so/.dylib) to load", value<string>())
    ("n,name",   "Agent name (default: plugin stem)", value<string>())
    ("i,agent-id","Agent ID added to JSON frames",   value<string>())
    ("d,delay",  "Initial delay in ms",               value<size_t>())
    ("o,option", "Extra plugin options key=value (repeatable)",
                                                      value<vector<string>>())
    ("silent",   "Suppress status line");
#if defined(RUST_PLUGIN_LOADER_SOURCE) || defined(RUST_PLUGIN_LOADER_FILTER)
  agent.options()
    ("p,period", "Sampling period in ms (default 100)", value<size_t>());
#endif
#if defined(RUST_PLUGIN_LOADER_FILTER) || defined(RUST_PLUGIN_LOADER_SINK)
  agent.options()
    ("b,dont-block", "Non-blocking receive");
#endif
  // clang-format on
  agent.add_common_options();
  agent.raw_options().parse_positional({"plugin"});
  agent.raw_options().positional_help("<plugin.so>");

  auto opts = agent.parse_options(argc, argv);
  if (int rc = AgentApp::handle_standard_exit_options<AgentApp>(
          opts, agent.raw_options(), argv); rc >= 0)
    return rc;

  if (opts.count("plugin")) {
    plugin_file = opts["plugin"].as<string>();
    agent_name  = fs::path(plugin_file).stem().string();
  }
  if (opts.count("name"))  agent_name = opts["name"].as<string>();
  if (opts.count("delay")) delay       = opts["delay"].as<size_t>();
  if (opts.count("silent")) silent     = true;

  agent.set_agent_name(agent_name);
  try {
    agent.init(opts);
  } catch (const AgentError &e) {
    cerr << fg::red << "Error initialising agent: " << e.what() << fg::reset << endl;
    return EXIT_FAILURE;
  } catch (const exception &e) {
    cerr << fg::red << "Runtime error: " << e.what() << fg::reset << endl;
    return EXIT_FAILURE;
  }

#if defined(RUST_PLUGIN_LOADER_SOURCE)
  agent.enable_threaded_remote_control();
#else
  agent.enable_remote_control();
#endif

  json settings = agent.settings_json();
  settings["agent_name"] = agent_name;
  if (opts.count("agent-id"))
    settings["agent_id"] = opts["agent-id"].as<string>();
  settings["prefix"] = Mads::prefix();
  if (settings["receive_timeout"].is_number())
    agent.set_receive_timeout(settings["receive_timeout"].get<int>());
  if (!settings["high_watermark"].is_null())
    agent.set_high_watermark(settings.value("high_watermark", 1000));

  if (opts.count("option")) {
    auto re = regex(R"((.+?)=(.*))");
    smatch match;
    for (auto &v : opts["option"].as<vector<string>>()) {
      if (regex_match(v, match, re))
        settings[match[1].str()] = str_to_num(match[2].str());
    }
  }

#if defined(RUST_PLUGIN_LOADER_SOURCE) || defined(RUST_PLUGIN_LOADER_FILTER)
  chrono::milliseconds period{0};
  cerr << "  Sampling period:  " << style::bold;
  if (opts.count("p")) {
    period = chrono::milliseconds(opts["p"].as<size_t>());
    cerr << fg::yellow << period.count() << " ms (from -p)" << fg::reset;
  } else if (!settings["period"].is_null()) {
    period = chrono::milliseconds(settings["period"].get<size_t>());
    cerr << period.count() << " ms (from settings)";
  } else {
    cerr << fg::red << "free run" << fg::reset << " (default)";
  }
  cerr << style::reset << endl;
#endif

  agent.info(cerr);
  agent.connect();

#if defined(RUST_PLUGIN_LOADER_FILTER) || defined(RUST_PLUGIN_LOADER_SINK)
  bool dont_block = settings.value("dont_block", false);
  if (opts.count("dont-block")) dont_block = true;
#endif

  /* ── resolve plugin path ─────────────────────────────────────────────── */
  if (opts.count("plugin")) {
    if (!fs::exists(plugin_file)) {
      cerr << style::italic << "  Searching installed plugins in " << style::reset;
#ifdef _WIN32
      cerr << Mads::exec_dir("../bin/") << endl;
      plugin_file = Mads::exec_dir("../bin/" + plugin_file);
#else
      cerr << Mads::exec_dir("../lib/") << endl;
      plugin_file = Mads::exec_dir("../lib/" + plugin_file);
#endif
    }
    if (!fs::exists(plugin_file)) {
      cerr << fg::red << "Error: cannot find plugin file " << plugin_file
           << fg::reset << endl;
      return EXIT_FAILURE;
    }
  } else if (!agent.attachment_path().empty()) {
    plugin_file = agent.attachment_path().string();
  }

  /* ── load plugin ─────────────────────────────────────────────────────── */
  RustPlugin plugin;
  {
    string load_err;
    if (!plugin.load(plugin_file, load_err)) {
      cerr << fg::red << "Error loading Rust plugin '" << plugin_file
           << "': " << load_err << fg::reset << endl;
      return EXIT_FAILURE;
    }
  }
  plugin.set_params(settings);

  cerr << style::bold << "Rust plugin settings:" << style::reset << endl
       << "  Plugin:   " << style::bold << plugin_file
       << " (name=" << plugin.fns->name
       << ", kind=" << plugin.fns->kind
       << ", abi=" << plugin.fns->version << ")" << style::reset << endl;

  json info_obj = plugin.get_info();
  for (auto &[k, v] : info_obj.items()) {
    string val = v.is_string() ? v.get<string>() : v.dump();
    cerr << "  " << left << setw(18) << k << style::bold << val
         << style::reset << endl;
  }

#if defined(RUST_PLUGIN_LOADER_SOURCE)
  string out_format = plugin.blob_format();
  cerr << "  Blob format:      " << style::bold << out_format
       << style::reset << endl;
#endif

  if (delay > 0)
    this_thread::sleep_for(chrono::milliseconds(delay));

  agent.register_event(event_type::startup, settings, "modified_settings");
  cerr << fg::green << PLUGIN_NAME " plugin started" << fg::reset << endl;

  /* ══════════════════════════════════════════════════════════════════════
     Main loops — one per plugin kind
     ══════════════════════════════════════════════════════════════════════ */

#if defined(RUST_PLUGIN_LOADER_SOURCE)
  json out, err;
  return_type rt;
  vector<unsigned char> blob;
  agent.loop([&]() -> chrono::milliseconds {
    out.clear(); blob.clear(); err.clear();
    rt = plugin.get_output();
    switch (rt) {
    case return_type::warning:
      try { out["warning"]["get_output"] = plugin.error(); }
      catch (...) {
        cerr << fg::yellow << "Warning: " << plugin.error() << fg::reset << endl;
      }
      [[fallthrough]];
    case return_type::success:
      if (out.empty()) out = plugin.output_json();
      blob = plugin.output_blob();
      if (out.empty() && blob.empty())
        out["warning"]["get_output"] = "plugin returned no output";
      if (!blob.empty()) {
        if (!out.contains("format")) out["format"] = out_format;
        auto topic = out.value("topic", "");
        agent.publish(blob, std::move(out), topic);
      } else if (!out.empty()) {
        auto topic = out.value("topic", "");
        agent.publish(std::move(out), topic);
      }
      break;
    case return_type::retry:
      return 0ms;
    case return_type::error:
      err = {{"error", {{"get_output", plugin.error()}}}};
      agent.register_event(event_type::message, err);
      count_err++;
      break;
    case return_type::critical:
      count_err++;
      Mads::running = false;
      throw runtime_error("Critical error in get_output: " + plugin.error());
    }
    if (!silent)
      cerr << "\r\x1b[0KMessages: " << fg::green << ++count << fg::reset
           << " ok, " << fg::red << count_err << fg::reset << " err ";
    return plugin.next_duration();
  }, period);

#elif defined(RUST_PLUGIN_LOADER_FILTER)
  json in, out, err;
  return_type rt;
  message_type type;
  tuple<string, string, vector<unsigned char>> msg_blob;
  vector<unsigned char> blob;
  agent.loop([&]() -> chrono::milliseconds {
    type = message_type::none;
    in.clear(); out.clear(); err.clear(); blob.clear();
    try { type = agent.receive(dont_block); }
    catch (const AgentError &e) {
      cerr << fg::red << "Receive error: " << e.what() << fg::reset << endl;
    }
    if (agent.last_topic() == agent.pub_topic()) return 0ms;
    if (type == message_type::json) {
      if (agent.last_topic() == "control") return 0ms;
      try { in = get<1>(agent.last_json()); }
      catch (json::parse_error &e) {
        cerr << fg::red << "JSON parse error: " << e.what() << fg::reset << endl;
        return 0ms;
      }
      rt = plugin.load_data(in, agent.last_topic());
    } else if (type == message_type::blob) {
      msg_blob = agent.last_blob();
      in = json::parse(get<1>(msg_blob));
      rt = plugin.load_data(in, agent.last_topic(), &get<2>(msg_blob));
    } else {
      if (dont_block) goto process_output;
      return plugin.next_duration();
    }
    switch (rt) {
    case return_type::warning:
      err["warning"]["load_data"] = plugin.error();
      agent.register_event(event_type::message, err);
      [[fallthrough]];
    case return_type::success: break;
    case return_type::retry:   return 0ms;
    case return_type::error:
      err["error"]["load_data"] = plugin.error();
      agent.register_event(event_type::message, err);
      count_err++;
      goto status_line;
    case return_type::critical:
      err["error"]["load_data"] = plugin.error();
      agent.register_event(event_type::message, err);
      Mads::running = false;
      return 0ms;
    }
  process_output:
    rt = plugin.process();
    out = plugin.output_json();
    blob = plugin.output_blob();
    if (!err.empty()) out.merge_patch(err);
    switch (rt) {
    case return_type::warning:
      out["warning"]["process"] = plugin.error();
      [[fallthrough]];
    case return_type::success:
      if (out.empty()) out["warning"]["process"] = "plugin returned no output";
      break;
    case return_type::retry: return 0ms;
    case return_type::error:
      err["error"]["process"] = plugin.error();
      agent.register_event(event_type::message, err);
      count_err++;
      goto status_line;
    case return_type::critical:
      err["error"]["process"] = plugin.error();
      agent.register_event(event_type::message, err);
      Mads::running = false;
      return 0ms;
    }
    if (!blob.empty()) {
      if (!out.contains("format")) out["format"] = "raw";
      auto topic = out.value("topic", "");
      agent.publish(blob, std::move(out), topic);
    } else {
      auto topic = out.value("topic", "");
      agent.publish(std::move(out), topic);
    }
  status_line:
    if (!silent)
      cerr << "\r\x1b[0KMessages: " << fg::green << ++count << fg::reset
           << " ok, " << fg::red << count_err << fg::reset << " err ";
    return plugin.next_duration();
  }, period);

#elif defined(RUST_PLUGIN_LOADER_SINK)
  json in, err;
  return_type rt;
  message_type type;
  tuple<string, string, vector<unsigned char>> msg_blob;
  agent.loop([&]() -> chrono::milliseconds {
    type = message_type::none;
    in.clear(); err.clear();
    try { type = agent.receive(); }
    catch (const AgentError &e) {
      cerr << fg::red << "Receive error: " << e.what() << fg::reset << endl;
    }
    if (type == message_type::none) return 0ms;
    if (type == message_type::blob) {
      msg_blob = agent.last_blob();
      in  = json::parse(get<1>(msg_blob));
      rt  = plugin.load_data(in, agent.last_topic(), &get<2>(msg_blob));
    } else {
      if (agent.last_topic() == "control") return 0ms;
      try { in = get<1>(agent.last_json()); }
      catch (json::parse_error &e) {
        cerr << fg::red << "JSON parse error: " << e.what() << fg::reset << endl;
        return 0ms;
      }
      rt = plugin.load_data(in, agent.last_topic());
    }
    switch (rt) {
    case return_type::warning:
      cerr << fg::yellow << "Warning: " << plugin.error() << fg::reset << endl;
      err = {{"warning", {{"load_data", plugin.error()}}}};
      agent.register_event(event_type::message, err);
      [[fallthrough]];
    case return_type::success:
    case return_type::retry:
      break;
    case return_type::error:
      err = {{"error", {{"load_data", plugin.error()}}}};
      agent.register_event(event_type::message, err);
      count_err++;
      break;
    case return_type::critical:
      err = {{"error", {{"load_data", plugin.error()}}}};
      agent.register_event(event_type::message, err);
      Mads::running = false;
      return 0ms;
    }
    if (!silent)
      cerr << "\r\x1b[0KMessages: " << fg::green << ++count << fg::reset
           << " ok, " << fg::red << count_err << fg::reset << " err ";
    return plugin.next_duration();
  });
#endif

  cerr << fg::green << "\n" PLUGIN_NAME " plugin stopped" << fg::reset << endl;
  agent.register_event(event_type::shutdown);
  agent.disconnect();
  agent.restart_if_requested(argv);
  return 0;
}
