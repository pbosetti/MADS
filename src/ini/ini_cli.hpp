/*
 ___       _    ____ _     ___
|_ _|_ __ (_)  / ___| |   |_ _|
 | || '_ \| | | |   | |    | |
 | || | | | | | |___| |___ | |
|___|_| |_|_|  \____|_____|___|

`mads ini` command line, shared by the internal command of the `mads`
dispatcher (builds without GUI) and the mads-ini editor executable.

  mads ini                     GUI on ./mads.ini, else <prefix>/etc/mads.ini,
                               else a new document from the template that
                               saves to ./mads.ini; the GUI runs detached.
                               Without a GUI: print the template (legacy)
  mads ini FILE                GUI on FILE (from the template if missing)
  mads ini -p|--print          print the rendered template
  mads ini -o FILE | -i        write / install the rendered template
           [-b -F -B -s -f -m] template parameters (unchanged)
  mads ini --check [FILE]      parse + lint; exit 0 clean, 1 warnings, 2 errors
  mads ini --get S.K [FILE]    print the raw TOML value of key K in section S
  mads ini --set S.K=V [FILE]  set key K of section S (repeatable), keeping
           [--comment TEXT]    every comment; V is TOML text, or a plain word
                               that is stored as a string
  mads ini --set-password [FILE]
                               set the broker's editing password: prompts
                               without echo on a terminal, else reads one
                               line from stdin
  mads ini --remove-password [FILE]
                               remove the broker's editing password file

FILE defaults to <prefix>/etc/mads.ini for --check/--get/--set and the
password options.

Author(s): Paolo Bosetti
*/
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>

namespace Mads::Ini {

/// Opens the editor on `file` (missing: a new document from the template
/// that saves to `file`; nullopt: the same, unsaved) and returns the process exit code. When the GUI cannot start (e.g. no
/// display), it prints why, suggests --print/--check, and returns non-zero.
using LaunchGui = std::function<int(std::optional<std::filesystem::path>)>;

/// Runs `mads ini`. `argv[0]` is the command name. `launch_gui` is empty
/// when no GUI is available: a bare invocation then prints the template, and
/// a FILE argument alone is an error.
int run_cli(int argc, char **argv, const LaunchGui &launch_gui = {});

/// Creates the editing password verifier of the settings file `settings`
/// (see Remote::password_file_for) from one line read from `in` (trailing
/// \r\n stripped). Progress and errors go to `err`, the result to `out`.
/// Returns 0 when written, 1 on an empty password or a failure. Warns when
/// the password is shorter than 8 characters, but still writes it.
int set_password(const std::filesystem::path &settings, std::istream &in,
                 std::ostream &out, std::ostream &err);

/// Template parameters with their defaults (broker, port_frontend,
/// port_backend, port_settings, mongo_uri).
nlohmann::json default_template_data();

/// Renders <prefix>/share/templates/mads.ini with `data`. Throws on a
/// missing template.
std::string render_template(const nlohmann::json &data =
                                default_template_data());

/// <prefix>/etc/mads.ini.
std::filesystem::path default_ini_path();

/// What a bare `mads ini` edits: ./mads.ini if it exists, else
/// default_ini_path() if it exists, else ./mads.ini (absolute paths).
std::filesystem::path startup_ini_path();

} // namespace Mads::Ini
