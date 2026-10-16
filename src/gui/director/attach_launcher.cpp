#include "attach_launcher.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <cstdint>
#endif

namespace {

std::string shell_single_quote(const std::string& input) {
  std::string quoted = "'";
  for (char ch : input) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(ch);
    }
  }
  quoted += "'";
  return quoted;
}

#if !defined(_WIN32) && !defined(__APPLE__)
std::string terminal_name(const std::string& terminal_command) {
  const std::size_t slash = terminal_command.find_last_of('/');
  const std::string name = (slash == std::string::npos) ? terminal_command : terminal_command.substr(slash + 1);
  return name;
}

bool prefers_double_dash(const std::string& terminal_command) {
  std::string name = terminal_name(terminal_command);
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return name == "gnome-terminal" || name == "gnome-terminal.real";
}

std::string sanitized_env_prefix_for_terminal_launch() {
  // When launched from Snap-based IDEs (for example VS Code snap), inherited GTK/SNAP
  // variables can force host terminals to load incompatible snap libraries.
  static constexpr const char* kVarsToUnset[] = {
      "LD_LIBRARY_PATH",
      "LD_PRELOAD",
      "GTK_EXE_PREFIX",
      "GTK_PATH",
      "GTK_IM_MODULE_FILE",
      "GNOME_TERMINAL_SCREEN",
      "GNOME_TERMINAL_SERVICE",
      "SNAP",
      "SNAP_ARCH",
      "SNAP_COMMON",
      "SNAP_CONTEXT",
      "SNAP_COOKIE",
      "SNAP_DATA",
      "SNAP_EUID",
      "SNAP_INSTANCE_NAME",
      "SNAP_LAUNCHER_ARCH_TRIPLET",
      "SNAP_LIBRARY_PATH",
      "SNAP_NAME",
      "SNAP_REAL_HOME",
      "SNAP_REVISION",
      "SNAP_UID",
      "SNAP_USER_COMMON",
      "SNAP_USER_DATA",
      "SNAP_VERSION",
  };

  bool needs_sanitize = false;
  for (const char* var : kVarsToUnset) {
    const char* value = std::getenv(var);
    if (value != nullptr && std::strlen(value) > 0) {
      needs_sanitize = true;
      break;
    }
  }

  if (!needs_sanitize) {
    return "";
  }

  std::string prefix = "env";
  for (const char* var : kVarsToUnset) {
    prefix += " -u ";
    prefix += var;
  }
  prefix += " ";
  return prefix;
}
#endif

#ifdef __APPLE__
std::string applescript_escape(const std::string& input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (char ch : input) {
    if (ch == '\\') {
      out += "\\\\";
    } else if (ch == '"') {
      out += "\\\"";
    } else {
      out.push_back(ch);
    }
  }
  return out;
}
#endif

#ifdef _WIN32
std::string powershell_single_quote(const std::string& input) {
  std::string quoted = "'";
  for (char ch : input) {
    if (ch == '\'') {
      quoted += "''";
    } else {
      quoted.push_back(ch);
    }
  }
  quoted += "'";
  return quoted;
}

std::string base64_encode(const std::vector<std::uint8_t>& bytes) {
  static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);

  std::size_t i = 0;
  while (i + 3 <= bytes.size()) {
    const std::uint32_t v = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                            (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
                            static_cast<std::uint32_t>(bytes[i + 2]);
    out.push_back(kTable[(v >> 18) & 0x3f]);
    out.push_back(kTable[(v >> 12) & 0x3f]);
    out.push_back(kTable[(v >> 6) & 0x3f]);
    out.push_back(kTable[v & 0x3f]);
    i += 3;
  }

  const std::size_t rem = bytes.size() - i;
  if (rem == 1) {
    const std::uint32_t v = static_cast<std::uint32_t>(bytes[i]) << 16;
    out.push_back(kTable[(v >> 18) & 0x3f]);
    out.push_back(kTable[(v >> 12) & 0x3f]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    const std::uint32_t v =
        (static_cast<std::uint32_t>(bytes[i]) << 16) | (static_cast<std::uint32_t>(bytes[i + 1]) << 8);
    out.push_back(kTable[(v >> 18) & 0x3f]);
    out.push_back(kTable[(v >> 12) & 0x3f]);
    out.push_back(kTable[(v >> 6) & 0x3f]);
    out.push_back('=');
  }

  return out;
}
#endif

}  // namespace

bool launch_attach_terminal(const std::string& executable_path, const std::string& socket_path,
                            const std::optional<std::string>& terminal_command, std::string* out_error) {
#ifdef _WIN32
  (void)terminal_command;
  const std::string command = "& " + powershell_single_quote(executable_path) + " --attach-socket " +
                              powershell_single_quote(socket_path);
  const int wide_size = MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, nullptr, 0);
  if (wide_size <= 1) {
    *out_error = "Failed to encode attach command for PowerShell.";
    return false;
  }

  std::vector<wchar_t> wide(static_cast<std::size_t>(wide_size));
  if (MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, wide.data(), wide_size) == 0) {
    *out_error = "Failed to encode attach command for PowerShell.";
    return false;
  }

  std::vector<std::uint8_t> utf16le;
  utf16le.reserve((wide.size() - 1) * 2);
  for (std::size_t i = 0; i + 1 < wide.size(); ++i) {
    const std::uint16_t code = static_cast<std::uint16_t>(wide[i]);
    utf16le.push_back(static_cast<std::uint8_t>(code & 0xff));
    utf16le.push_back(static_cast<std::uint8_t>((code >> 8) & 0xff));
  }

  const std::string encoded = base64_encode(utf16le);
  const std::string args = "-NoLogo -NoProfile -NoExit -EncodedCommand " + encoded;
  const HINSTANCE result =
      ShellExecuteA(nullptr, "open", "powershell.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<std::intptr_t>(result) <= 32) {
    *out_error = "Failed to open PowerShell for attach.";
    return false;
  }
  return true;
#elif defined(__APPLE__)
  (void)terminal_command;
  const std::string command = shell_single_quote(executable_path) + " --attach-socket " + shell_single_quote(socket_path);
  const std::string escaped = applescript_escape(command);
  std::string osascript = "osascript -e \"tell application \\\"Terminal\\\" to activate\" "
                          "-e \"tell application \\\"Terminal\\\" to do script \\\"" +
                          escaped + "\\\"\"";

  if (std::system(osascript.c_str()) != 0) {
    *out_error = "Failed to open macOS Terminal for attach.";
    return false;
  }
  return true;
#else
  const std::string command = shell_single_quote(executable_path) + " --attach-socket " + shell_single_quote(socket_path);
  const std::string wrapped = "sh -lc " + shell_single_quote(command);
  const std::string env_prefix = sanitized_env_prefix_for_terminal_launch();

  std::vector<std::string> candidates;
  if (terminal_command.has_value() && !terminal_command->empty()) {
    const std::string terminal = shell_single_quote(*terminal_command);
    if (prefers_double_dash(*terminal_command)) {
      candidates.push_back(env_prefix + terminal + " -- " + wrapped);
    } else {
      candidates.push_back(env_prefix + terminal + " -e " + wrapped);
      candidates.push_back(env_prefix + terminal + " -- " + wrapped);
    }
  } else {
    candidates = {
        env_prefix + "gnome-terminal -- " + wrapped,
        env_prefix + "x-terminal-emulator -e " + wrapped,
        env_prefix + "konsole -e " + wrapped,
        env_prefix + "xterm -e " + wrapped,
    };
  }

  for (const auto& candidate : candidates) {
    if (std::system(candidate.c_str()) == 0) {
      return true;
    }
  }

  if (terminal_command.has_value() && !terminal_command->empty()) {
    *out_error = "Failed to open configured terminal for attach. Check [director].terminal.";
  } else {
    *out_error = "Failed to open terminal window for attach.";
  }
  return false;
#endif
}
