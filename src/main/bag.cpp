/*
  ____
 | __ )  __ _  __ _
 |  _ \ / _` |/ _` |
 | |_) | (_| | (_| |
 |____/ \__,_|\__, |
              |___/

mads-bag: inspection/export tool for bag files written by mads-record
(src/bag.hpp). Subcommands:

  mads bag info -f <bag>                    O(1) summary (footer/index
                                             permitting; falls back to a
                                             linear scan and reports
                                             truncated() otherwise).
  mads bag export -f <bag> --format jsonl   One JSON object per record.

Every part is exported as base64 (parts are raw, possibly-binary wire
bytes -- a header, a compressed payload, or blob bytes -- so this is the
only encoding that's always valid regardless of content; see
src/bag.hpp's format notes). `--format mcap` is intentionally not
implemented here: it is explicitly deferred/optional in NEW_FEATURES.md's
P3 section.

Author(s): Paolo Bosetti
*/
#include "../bag.hpp"
#include "../mads.hpp"
#include <cxxopts.hpp>
#include <rang.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace std;
using namespace rang;
using json = nlohmann::json;

namespace {

// Standard base64 (RFC 4648), self-contained: parts are raw wire bytes
// (a header, a compressed payload, or blob bytes), so this is the only
// encoding that is always valid JSON/UTF-8 output regardless of content.
string base64_encode(const string &in) {
  static const char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= in.size()) {
    uint32_t n = (static_cast<uint8_t>(in[i]) << 16) |
                (static_cast<uint8_t>(in[i + 1]) << 8) |
                static_cast<uint8_t>(in[i + 2]);
    out.push_back(table[(n >> 18) & 0x3F]);
    out.push_back(table[(n >> 12) & 0x3F]);
    out.push_back(table[(n >> 6) & 0x3F]);
    out.push_back(table[n & 0x3F]);
    i += 3;
  }
  size_t rem = in.size() - i;
  if (rem == 1) {
    uint32_t n = static_cast<uint8_t>(in[i]) << 16;
    out.push_back(table[(n >> 18) & 0x3F]);
    out.push_back(table[(n >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    uint32_t n =
        (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8);
    out.push_back(table[(n >> 18) & 0x3F]);
    out.push_back(table[(n >> 12) & 0x3F]);
    out.push_back(table[(n >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

string iso_time(int64_t ns) {
  auto tp = chrono::system_clock::time_point(
      chrono::duration_cast<chrono::system_clock::duration>(
          chrono::nanoseconds(ns)));
  return Mads::get_ISODate_time(tp);
}

void print_usage(ostream &out) {
  out << "Usage: mads bag <subcommand> [options]" << endl;
  out << endl;
  out << "Subcommands:" << endl;
  out << "  info    Print a summary of a bag file" << endl;
  out << "  export  Export a bag file (--format jsonl)" << endl;
  out << endl;
  out << "Run 'mads bag <subcommand> --help' for subcommand options."
      << endl;
}

// ---------------------------------------------------------------------------
// mads bag info
// ---------------------------------------------------------------------------

int run_info(int argc, char *argv[]) {
  cxxopts::Options options("mads bag info", "Summarize a bag file, version " +
                                                Mads::version());
  // clang-format off
  options.add_options()
    ("f,file", "Bag file to inspect (required)", cxxopts::value<string>())
    ("t,topics", "Include a per-topic message count (requires a full scan)")
    ("j,json", "Output as JSON")
    ("h,help", "Print usage");
  // clang-format on
  auto parsed = options.parse(argc, argv);
  if (parsed.count("help")) {
    cout << options.help() << endl;
    return 0;
  }
  if (parsed.count("file") == 0) {
    cerr << fg::red << "Error: --file is required" << fg::reset << endl;
    cerr << options.help() << endl;
    return EXIT_FAILURE;
  }
  string path = parsed["file"].as<string>();
  bool want_topics = parsed.count("topics") != 0;
  bool as_json = parsed.count("json") != 0;

  unique_ptr<Mads::BagReader> reader;
  try {
    reader = make_unique<Mads::BagReader>(path);
  } catch (const Mads::BagError &e) {
    cerr << fg::red << "Error opening bag file: " << e.what() << fg::reset
         << endl;
    return EXIT_FAILURE;
  }

  map<string, size_t> topic_counts;
  if (want_topics) {
    reader->rewind();
    while (auto rec = reader->next())
      ++topic_counts[rec->topic];
  }

  if (as_json) {
    json j;
    j["path"] = path;
    j["has_index"] = reader->has_index();
    j["truncated"] = reader->truncated();
    j["crc_enabled"] = reader->crc_enabled();
    j["record_count"] = reader->record_count();
    if (auto ts = reader->first_timestamp()) {
      j["first_timestamp_ns"] = *ts;
      j["first_timestamp_iso"] = iso_time(*ts);
    }
    if (auto ts = reader->last_timestamp()) {
      j["last_timestamp_ns"] = *ts;
      j["last_timestamp_iso"] = iso_time(*ts);
    }
    if (reader->first_timestamp() && reader->last_timestamp()) {
      j["duration_s"] =
          static_cast<double>(*reader->last_timestamp() -
                              *reader->first_timestamp()) /
          1e9;
    }
    if (want_topics) {
      json jt = json::object();
      for (auto const &[topic, count] : topic_counts)
        jt[topic] = count;
      j["topics"] = jt;
    }
    cout << j.dump(2) << endl;
    return 0;
  }

  cout << style::bold << "Bag file: " << fg::green << path << fg::reset
       << style::reset << endl;
  cout << "  Index:          " << style::bold
       << (reader->has_index() ? "present (O(1) info/seek)" : "missing "
                                                              "(recovered "
                                                              "via linear "
                                                              "scan)")
       << style::reset << endl;
  if (reader->truncated()) {
    cout << "  " << fg::yellow << "Truncated:      yes (trailing data was "
                                  "not fully recovered)"
         << fg::reset << endl;
  }
  cout << "  CRC32:          " << style::bold
       << (reader->crc_enabled() ? "enabled" : "disabled") << style::reset
       << endl;
  cout << "  Records:        " << style::bold << reader->record_count()
       << style::reset << endl;
  if (auto ts = reader->first_timestamp()) {
    cout << "  First record:   " << style::bold << iso_time(*ts)
         << style::reset << " (" << *ts << " ns)" << endl;
  }
  if (auto ts = reader->last_timestamp()) {
    cout << "  Last record:    " << style::bold << iso_time(*ts)
         << style::reset << " (" << *ts << " ns)" << endl;
  }
  if (reader->first_timestamp() && reader->last_timestamp()) {
    double duration_s = static_cast<double>(*reader->last_timestamp() -
                                            *reader->first_timestamp()) /
                        1e9;
    cout << "  Duration:       " << style::bold << duration_s << " s"
         << style::reset << endl;
  }
  if (want_topics) {
    cout << "  Topics:" << endl;
    for (auto const &[topic, count] : topic_counts) {
      cout << "    " << style::bold << count << style::reset << "  " << topic
           << endl;
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// mads bag export
// ---------------------------------------------------------------------------

int run_export(int argc, char *argv[]) {
  cxxopts::Options options("mads bag export",
                           "Export a bag file, version " + Mads::version());
  // clang-format off
  options.add_options()
    ("f,file", "Bag file to export (required)", cxxopts::value<string>())
    ("format", "Export format: jsonl (default; mcap is not implemented, "
               "see NEW_FEATURES.md)", cxxopts::value<string>()->default_value("jsonl"))
    ("o,output", "Output file (default: stdout)", cxxopts::value<string>())
    ("h,help", "Print usage");
  // clang-format on
  auto parsed = options.parse(argc, argv);
  if (parsed.count("help")) {
    cout << options.help() << endl;
    return 0;
  }
  if (parsed.count("file") == 0) {
    cerr << fg::red << "Error: --file is required" << fg::reset << endl;
    cerr << options.help() << endl;
    return EXIT_FAILURE;
  }
  string path = parsed["file"].as<string>();
  string format = parsed["format"].as<string>();
  if (format != "jsonl") {
    cerr << fg::red << "Error: unsupported --format '" << format
         << "' (only 'jsonl' is implemented; 'mcap' is deferred, see "
            "NEW_FEATURES.md)"
         << fg::reset << endl;
    return EXIT_FAILURE;
  }

  unique_ptr<Mads::BagReader> reader;
  try {
    reader = make_unique<Mads::BagReader>(path);
  } catch (const Mads::BagError &e) {
    cerr << fg::red << "Error opening bag file: " << e.what() << fg::reset
         << endl;
    return EXIT_FAILURE;
  }
  if (reader->truncated()) {
    cerr << fg::yellow << "Warning: bag file is truncated; exporting the "
                          "recovered prefix ("
         << reader->record_count() << " records)" << fg::reset << endl;
  }

  ofstream out_file;
  ostream *out = &cout;
  if (parsed.count("output")) {
    out_file.open(parsed["output"].as<string>(), ios::out | ios::trunc);
    if (!out_file) {
      cerr << fg::red << "Error: cannot open output file "
           << parsed["output"].as<string>() << fg::reset << endl;
      return EXIT_FAILURE;
    }
    out = &out_file;
  }

  size_t index = 0;
  reader->rewind();
  while (auto rec = reader->next()) {
    json j;
    j["index"] = index++;
    j["timestamp_ns"] = rec->timestamp_ns;
    j["timestamp_iso"] = iso_time(rec->timestamp_ns);
    j["topic"] = rec->topic;
    json parts_b64 = json::array();
    for (auto const &part : rec->parts)
      parts_b64.push_back(base64_encode(part));
    j["parts_base64"] = parts_b64;
    *out << j.dump() << "\n";
  }
  if (out_file.is_open())
    out_file.close();
  return 0;
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc < 2) {
    print_usage(cerr);
    return EXIT_FAILURE;
  }
  string subcommand = argv[1];
  if (subcommand == "-h" || subcommand == "--help") {
    print_usage(cout);
    return 0;
  }
  if (subcommand == "-v" || subcommand == "--version") {
    cout << LIB_VERSION << endl;
    return 0;
  }
  if (subcommand == "info")
    return run_info(argc - 1, argv + 1);
  if (subcommand == "export")
    return run_export(argc - 1, argv + 1);

  cerr << fg::red << "Error: unknown subcommand '" << subcommand << "'"
       << fg::reset << endl;
  print_usage(cerr);
  return EXIT_FAILURE;
}
