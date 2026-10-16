#pragma once

#include <filesystem>
#include <string>

// Returns the director.toml template text (MADS_DIRECTOR_EXAMPLE_CONFIG
// selects the variant used by the test suite and the packaged build).
std::string example_config_text();

// Writes example_config_text() to path. Without overwrite, an existing file
// is left untouched and the call fails with "File already exists: <path>".
bool write_example_config(const std::filesystem::path& path, bool overwrite, std::string* out_error);
