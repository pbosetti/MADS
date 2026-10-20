/*
 ___       _    ____ _     ___
|_ _|_ __ (_)  / ___| |   |_ _|
 | || '_ \| | | |   | |    | |
 | || | | | | | |___| |___ | |
|___|_| |_|_|  \____|_____|___|

`mads ini` command line, shared by the internal command of the `mads`
dispatcher (builds without GUI) and the mads-ini editor executable.

  mads ini                     GUI on <prefix>/etc/mads.ini, or a new document
                               from the template if that file does not exist;
                               without a GUI: print the template (legacy)
  mads ini FILE                GUI on FILE
  mads ini -p|--print          print the rendered template
  mads ini -o FILE | -i        write / install the rendered template
           [-b -F -B -s -f -m] template parameters (unchanged)
  mads ini --check [FILE]      parse + lint; exit 0 clean, 1 warnings, 2 errors
  mads ini --get S.K [FILE]    print the raw TOML value of key K in section S
  mads ini --set S.K=V [FILE]  set key K of section S (repeatable), keeping
           [--comment TEXT]    every comment; V is TOML text, or a plain word
                               that is stored as a string

FILE defaults to <prefix>/etc/mads.ini for --check/--get/--set.

Author(s): Paolo Bosetti
*/
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace Mads::Ini {

/// Opens the editor on `file` (nullopt: a new document from the template)
/// and returns the process exit code. When the GUI cannot start (e.g. no
/// display), it prints why, suggests --print/--check, and returns non-zero.
using LaunchGui = std::function<int(std::optional<std::filesystem::path>)>;

/// Runs `mads ini`. `argv[0]` is the command name. `launch_gui` is empty
/// when no GUI is available: a bare invocation then prints the template, and
/// a FILE argument alone is an error.
int run_cli(int argc, char **argv, const LaunchGui &launch_gui = {});

/// Template parameters with their defaults (broker, port_frontend,
/// port_backend, port_settings, mongo_uri).
nlohmann::json default_template_data();

/// Renders <prefix>/share/templates/mads.ini with `data`. Throws on a
/// missing template.
std::string render_template(const nlohmann::json &data =
                                default_template_data());

/// <prefix>/etc/mads.ini.
std::filesystem::path default_ini_path();

} // namespace Mads::Ini
