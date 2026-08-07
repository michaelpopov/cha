#pragma once

#include <filesystem>

namespace cha {

// Returns the directory containing the running executable without relying on
// argv[0] or the process working directory.
std::filesystem::path executable_directory();

} // namespace cha
