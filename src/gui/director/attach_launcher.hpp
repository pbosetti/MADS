#pragma once

#include <optional>
#include <string>

bool launch_attach_terminal(const std::string& executable_path, const std::string& socket_path,
                            const std::optional<std::string>& terminal_command, std::string* out_error);
