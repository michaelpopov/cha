#pragma once

#include <filesystem>
#include <string_view>

namespace cha {

// Updates one process environment variable. On Windows this uses _putenv_s;
// on POSIX it uses setenv. Existing values are preserved when overwrite is
// false.
bool set_environment_variable(
    std::string_view name,
    std::string_view value,
    bool overwrite = true);

// Removes one process environment variable.
bool unset_environment_variable(std::string_view name);

// Loads variables from a dotenv file without replacing values inherited by the process.
void load_dotenv(const std::filesystem::path& path = ".env");

} // namespace cha
